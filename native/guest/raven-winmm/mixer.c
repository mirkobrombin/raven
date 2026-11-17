/* SPDX-License-Identifier: MIT
 *
 * The mixer: one device with two destinations, the speakers and the recording, each with one source line (the wave output and the
 * microphone), every line with a volume and a mute control. The speakers' and the wave line's controls scale every wave output.
 */
#include "winmm_private.h"

#define CONTROL_COUNT 8
#define LINE_COUNT 4

typedef struct mixer {
	struct mixer *next;
	notify notify;
} mixer;

typedef struct line {
	DWORD destination, source, id, flags, component, channels, connections, target;
	const WCHAR *short_name, *name;
	DWORD first_control;
} line;

static const line lines[LINE_COUNT] = {
	{0, 0, 0xffff0000, MIXERLINE_LINEF_ACTIVE, MIXERLINE_COMPONENTTYPE_DST_SPEAKERS, 2, 1, MIXERLINE_TARGETTYPE_WAVEOUT, L"Speakers",
		L"Speakers", 0},
	{1, 0, 0xffff0001, MIXERLINE_LINEF_ACTIVE, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, 2, 1, MIXERLINE_TARGETTYPE_WAVEIN, L"Recording",
		L"Recording Control", 2},
	{0, 0, 0x00000000, MIXERLINE_LINEF_ACTIVE | MIXERLINE_LINEF_SOURCE, MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT, 2, 0,
		MIXERLINE_TARGETTYPE_WAVEOUT, L"Wave", L"Wave", 4},
	{1, 0, 0x00010000, MIXERLINE_LINEF_ACTIVE | MIXERLINE_LINEF_SOURCE, MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE, 2, 0,
		MIXERLINE_TARGETTYPE_WAVEIN, L"Microphone", L"Microphone", 6},
};

static CRITICAL_SECTION mixer_lock;
static mixer *mixers;
/* Each line's volume per channel and its mute, indexed by the line's first control. */
static DWORD volumes[LINE_COUNT][2] = {{0xffff, 0xffff}, {0xffff, 0xffff}, {0xffff, 0xffff}, {0xffff, 0xffff}};
static BOOL mutes[LINE_COUNT];

void mixer_init(void) { InitializeCriticalSection(&mixer_lock); }

/* shortcut: the louder channel scales both and the recording lines scale nothing, a stream has one gain and capture none; apply them
 * when the engine takes a gain per channel and for capture. */
static float line_scale(int index)
{
	if (mutes[index])
		return 0;
	const DWORD loudest = volumes[index][0] > volumes[index][1] ? volumes[index][0] : volumes[index][1];
	return (float)loudest / 65535.0f;
}

float master_volume(void)
{
	EnterCriticalSection(&mixer_lock);
	const float scale = line_scale(0) * line_scale(2);
	LeaveCriticalSection(&mixer_lock);
	return scale;
}

static BOOL mixer_valid(void *handle)
{
	BOOL found = FALSE;
	EnterCriticalSection(&mixer_lock);
	for (mixer *each = mixers; each; each = each->next)
		found |= (void *)each == handle;
	LeaveCriticalSection(&mixer_lock);
	return found;
}

/* The object a mixer function names with its MIXER_OBJECTF flag. There is one mixer, and a mixer id or handle is taken whichever of the
 * two the flag says, as programs mix them up and Windows accepts both. */
static MMRESULT object_check(UINT_PTR object, DWORD flags)
{
	switch (flags & 0xf0000000) {
	case MIXER_OBJECTF_MIXER:
	case MIXER_OBJECTF_HMIXER:
		return object == 0 || mixer_valid((void *)object) ? MMSYSERR_NOERROR :
			(flags & MIXER_OBJECTF_HANDLE) ? MMSYSERR_INVALHANDLE : MMSYSERR_BADDEVICEID;
	case MIXER_OBJECTF_WAVEOUT:
	case MIXER_OBJECTF_WAVEIN:
		return object == 0 || object == WAVE_MAPPER || object == (UINT_PTR)(UINT)WAVE_MAPPER ? MMSYSERR_NOERROR : MMSYSERR_BADDEVICEID;
	case MIXER_OBJECTF_HWAVEOUT:
	case MIXER_OBJECTF_HWAVEIN:
		return object ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
	case MIXER_OBJECTF_MIDIOUT:
	case MIXER_OBJECTF_MIDIIN:
	case MIXER_OBJECTF_AUX:
		return MMSYSERR_BADDEVICEID;
	case MIXER_OBJECTF_HMIDIOUT:
	case MIXER_OBJECTF_HMIDIIN:
		return MMSYSERR_INVALHANDLE;
	default:
		return MMSYSERR_INVALFLAG;
	}
}

EXPORT UINT WINAPI mixerGetNumDevs(void) { return 1; }

static void mixer_caps(MIXERCAPSW *caps)
{
	memset(caps, 0, sizeof *caps);
	caps->wMid = MM_MICROSOFT;
	caps->wPid = MM_MSFT_WSS_MIXER;
	caps->vDriverVersion = 0x0500;
	lstrcpynW(caps->szPname, L"Raven Audio Mixer", MAXPNAMELEN);
	caps->cDestinations = 2;
}

EXPORT MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR id, LPMIXERCAPSW caps, UINT size)
{
	if (id != 0 && !mixer_valid((void *)id))
		return MMSYSERR_BADDEVICEID;
	if (!caps)
		return MMSYSERR_INVALPARAM;
	MIXERCAPSW full;
	mixer_caps(&full);
	memcpy(caps, &full, size < sizeof full ? size : sizeof full);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR id, LPMIXERCAPSA caps, UINT size)
{
	if (id != 0 && !mixer_valid((void *)id))
		return MMSYSERR_BADDEVICEID;
	if (!caps)
		return MMSYSERR_INVALPARAM;
	MIXERCAPSW wide;
	mixer_caps(&wide);
	MIXERCAPSA full;
	full.wMid = wide.wMid;
	full.wPid = wide.wPid;
	full.vDriverVersion = wide.vDriverVersion;
	ascii_from_wide(full.szPname, sizeof full.szPname, wide.szPname);
	full.fdwSupport = wide.fdwSupport;
	full.cDestinations = wide.cDestinations;
	memcpy(caps, &full, size < sizeof full ? size : sizeof full);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerOpen(LPHMIXER out, UINT id, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	const MMRESULT checked = object_check(id, flags & 0xf0000000);
	if (checked)
		return checked;
	if (!out)
		return MMSYSERR_INVALPARAM;
	notify notify;
	const MMRESULT usable = notify_init(&notify, callback, instance, flags & CALLBACK_TYPEMASK);
	if (usable)
		return usable;
	mixer *made = ALLOC(sizeof *made);
	if (!made)
		return MMSYSERR_NOMEM;
	made->notify = notify;
	EnterCriticalSection(&mixer_lock);
	made->next = mixers;
	mixers = made;
	LeaveCriticalSection(&mixer_lock);
	*out = (HMIXER)made;
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerClose(HMIXER handle)
{
	EnterCriticalSection(&mixer_lock);
	for (mixer **link = &mixers; *link; link = &(*link)->next)
		if ((void *)*link == (void *)handle) {
			mixer *gone = *link;
			*link = gone->next;
			LeaveCriticalSection(&mixer_lock);
			FREE(gone);
			return MMSYSERR_NOERROR;
		}
	LeaveCriticalSection(&mixer_lock);
	return MMSYSERR_INVALHANDLE;
}

EXPORT MMRESULT WINAPI mixerGetID(HMIXEROBJ object, UINT *id, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	if (!id)
		return MMSYSERR_INVALPARAM;
	*id = 0;
	return MMSYSERR_NOERROR;
}

EXPORT DWORD WINAPI mixerMessage(HMIXER handle, UINT message, DWORD_PTR first, DWORD_PTR second)
{
	return mixer_valid(handle) ? MMSYSERR_NOTSUPPORTED : MMSYSERR_INVALHANDLE;
}

/* ----- Lines ----- */

static const line *line_by_id(DWORD id)
{
	for (int index = 0; index < LINE_COUNT; index++)
		if (lines[index].id == id)
			return &lines[index];
	return NULL;
}

/* Finds the line the query names from the fields it reads, and fills in the rest. */
static MMRESULT line_info(MIXERLINEW *info, DWORD query)
{
	const line *found = NULL;
	switch (query) {
	case MIXER_GETLINEINFOF_DESTINATION:
		if (info->dwDestination < 2)
			found = &lines[info->dwDestination];
		break;
	case MIXER_GETLINEINFOF_SOURCE:
		if (info->dwDestination < 2 && info->dwSource < 1)
			found = &lines[2 + info->dwDestination];
		break;
	case MIXER_GETLINEINFOF_LINEID:
		found = line_by_id(info->dwLineID);
		break;
	case MIXER_GETLINEINFOF_COMPONENTTYPE:
		for (int index = 0; index < LINE_COUNT && !found; index++)
			if (lines[index].component == info->dwComponentType)
				found = &lines[index];
		break;
	case MIXER_GETLINEINFOF_TARGETTYPE:
		for (int index = 0; index < LINE_COUNT && !found; index++)
			if (lines[index].target == info->Target.dwType)
				found = &lines[index];
		break;
	default:
		return MMSYSERR_INVALFLAG;
	}
	if (!found)
		return MIXERR_INVALLINE;
	info->dwDestination = found->destination;
	info->dwSource = found->source;
	info->dwLineID = found->id;
	info->fdwLine = found->flags;
	info->dwUser = 0;
	info->dwComponentType = found->component;
	info->cChannels = found->channels;
	info->cConnections = found->connections;
	info->cControls = 2;
	lstrcpynW(info->szShortName, found->short_name, MIXER_SHORT_NAME_CHARS);
	lstrcpynW(info->szName, found->name, MIXER_LONG_NAME_CHARS);
	info->Target.dwType = found->target;
	info->Target.dwDeviceID = 0;
	info->Target.wMid = MM_MICROSOFT;
	info->Target.wPid = found->target == MIXERLINE_TARGETTYPE_WAVEOUT ? MM_MSFT_WSS_WAVEOUT : MM_MSFT_WSS_WAVEIN;
	info->Target.vDriverVersion = 0x0500;
	lstrcpynW(info->Target.szPname,
		found->target == MIXERLINE_TARGETTYPE_WAVEOUT ? L"Speakers (Raven Audio)" : L"Microphone (Raven Audio)", MAXPNAMELEN);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ object, LPMIXERLINEW info, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	if (!info || info->cbStruct < sizeof *info)
		return MMSYSERR_INVALPARAM;
	return line_info(info, flags & MIXER_GETLINEINFOF_QUERYMASK);
}

EXPORT MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ object, LPMIXERLINEA info, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	if (!info || info->cbStruct < sizeof *info)
		return MMSYSERR_INVALPARAM;
	MIXERLINEW wide;
	memset(&wide, 0, sizeof wide);
	wide.cbStruct = sizeof wide;
	wide.dwDestination = info->dwDestination;
	wide.dwSource = info->dwSource;
	wide.dwLineID = info->dwLineID;
	wide.dwComponentType = info->dwComponentType;
	wide.Target.dwType = info->Target.dwType;
	const MMRESULT result = line_info(&wide, flags & MIXER_GETLINEINFOF_QUERYMASK);
	if (result)
		return result;
	info->dwDestination = wide.dwDestination;
	info->dwSource = wide.dwSource;
	info->dwLineID = wide.dwLineID;
	info->fdwLine = wide.fdwLine;
	info->dwUser = wide.dwUser;
	info->dwComponentType = wide.dwComponentType;
	info->cChannels = wide.cChannels;
	info->cConnections = wide.cConnections;
	info->cControls = wide.cControls;
	ascii_from_wide(info->szShortName, sizeof info->szShortName, wide.szShortName);
	ascii_from_wide(info->szName, sizeof info->szName, wide.szName);
	info->Target.dwType = wide.Target.dwType;
	info->Target.dwDeviceID = wide.Target.dwDeviceID;
	info->Target.wMid = wide.Target.wMid;
	info->Target.wPid = wide.Target.wPid;
	info->Target.vDriverVersion = wide.Target.vDriverVersion;
	ascii_from_wide(info->Target.szPname, sizeof info->Target.szPname, wide.Target.szPname);
	return MMSYSERR_NOERROR;
}

/* ----- Controls ----- */

static void control_info(DWORD id, MIXERCONTROLW *control)
{
	memset(control, 0, sizeof *control);
	control->cbStruct = sizeof *control;
	control->dwControlID = id;
	if (id & 1) {
		control->dwControlType = MIXERCONTROL_CONTROLTYPE_MUTE;
		control->fdwControl = MIXERCONTROL_CONTROLF_UNIFORM;
		lstrcpynW(control->szShortName, L"Mute", MIXER_SHORT_NAME_CHARS);
		lstrcpynW(control->szName, L"Mute", MIXER_LONG_NAME_CHARS);
		control->Bounds.dwMinimum = 0;
		control->Bounds.dwMaximum = 1;
	} else {
		control->dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
		lstrcpynW(control->szShortName, L"Volume", MIXER_SHORT_NAME_CHARS);
		lstrcpynW(control->szName, L"Volume", MIXER_LONG_NAME_CHARS);
		control->Bounds.dwMinimum = 0;
		control->Bounds.dwMaximum = 0xffff;
		control->Metrics.cSteps = 192;
	}
}

static void control_ascii(const MIXERCONTROLW *wide, MIXERCONTROLA *control)
{
	control->cbStruct = sizeof *control;
	control->dwControlID = wide->dwControlID;
	control->dwControlType = wide->dwControlType;
	control->fdwControl = wide->fdwControl;
	control->cMultipleItems = wide->cMultipleItems;
	ascii_from_wide(control->szShortName, sizeof control->szShortName, wide->szShortName);
	ascii_from_wide(control->szName, sizeof control->szName, wide->szName);
	memcpy(&control->Bounds, &wide->Bounds, sizeof control->Bounds);
	memcpy(&control->Metrics, &wide->Metrics, sizeof control->Metrics);
}

/* The controls the query names, as their ids; the line's id is filled in for a query by control. */
static MMRESULT controls_of(MIXERLINECONTROLSW *query, DWORD flags, DWORD *ids, DWORD *count)
{
	switch (flags & MIXER_GETLINECONTROLSF_QUERYMASK) {
	case MIXER_GETLINECONTROLSF_ALL: {
		const line *found = line_by_id(query->dwLineID);
		if (!found)
			return MIXERR_INVALLINE;
		if (query->cControls != 2)
			return MMSYSERR_INVALPARAM;
		ids[0] = found->first_control;
		ids[1] = found->first_control + 1;
		*count = 2;
		return MMSYSERR_NOERROR;
	}
	case MIXER_GETLINECONTROLSF_ONEBYID:
		if (query->dwControlID >= CONTROL_COUNT)
			return MIXERR_INVALCONTROL;
		ids[0] = query->dwControlID;
		query->dwLineID = lines[query->dwControlID / 2].id;
		*count = 1;
		return MMSYSERR_NOERROR;
	case MIXER_GETLINECONTROLSF_ONEBYTYPE: {
		const line *found = line_by_id(query->dwLineID);
		if (!found)
			return MIXERR_INVALLINE;
		if (query->dwControlType == MIXERCONTROL_CONTROLTYPE_VOLUME)
			ids[0] = found->first_control;
		else if (query->dwControlType == MIXERCONTROL_CONTROLTYPE_MUTE)
			ids[0] = found->first_control + 1;
		else
			return MIXERR_INVALCONTROL;
		*count = 1;
		return MMSYSERR_NOERROR;
	}
	default:
		return MMSYSERR_INVALFLAG;
	}
}

EXPORT MMRESULT WINAPI mixerGetLineControlsW(HMIXEROBJ object, LPMIXERLINECONTROLSW query, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	if (!query || query->cbStruct < sizeof *query || query->cbmxctrl < sizeof(MIXERCONTROLW) || !query->pamxctrl)
		return MMSYSERR_INVALPARAM;
	DWORD ids[2], count = 0;
	const MMRESULT result = controls_of(query, flags, ids, &count);
	if (result)
		return result;
	for (DWORD index = 0; index < count; index++)
		control_info(ids[index], (MIXERCONTROLW *)((BYTE *)query->pamxctrl + index * query->cbmxctrl));
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ object, LPMIXERLINECONTROLSA query, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	if (!query || query->cbStruct < sizeof *query || query->cbmxctrl < sizeof(MIXERCONTROLA) || !query->pamxctrl)
		return MMSYSERR_INVALPARAM;
	MIXERLINECONTROLSW wide;
	wide.cbStruct = sizeof wide;
	wide.dwLineID = query->dwLineID;
	wide.dwControlID = query->dwControlID;
	wide.cControls = query->cControls;
	wide.cbmxctrl = sizeof(MIXERCONTROLW);
	wide.pamxctrl = NULL;
	DWORD ids[2], count = 0;
	const MMRESULT result = controls_of(&wide, flags, ids, &count);
	if (result)
		return result;
	query->dwLineID = wide.dwLineID;
	for (DWORD index = 0; index < count; index++) {
		MIXERCONTROLW control;
		control_info(ids[index], &control);
		control_ascii(&control, (MIXERCONTROLA *)((BYTE *)query->pamxctrl + index * query->cbmxctrl));
	}
	return MMSYSERR_NOERROR;
}

static MMRESULT details_check(const MIXERCONTROLDETAILS *details, DWORD flags)
{
	if (!details || details->cbStruct < sizeof *details)
		return MMSYSERR_INVALPARAM;
	if (details->dwControlID >= CONTROL_COUNT)
		return MIXERR_INVALCONTROL;
	if ((flags & MIXER_GETCONTROLDETAILSF_QUERYMASK) != MIXER_GETCONTROLDETAILSF_VALUE)
		return (flags & MIXER_GETCONTROLDETAILSF_QUERYMASK) == MIXER_GETCONTROLDETAILSF_LISTTEXT ? MMSYSERR_INVALPARAM : MMSYSERR_INVALFLAG;
	const BOOL mute = details->dwControlID & 1;
	const DWORD channels = details->cChannels;
	if (!details->paDetails || details->cMultipleItems || (channels != 1 && channels != 2) ||
		details->cbDetails < (mute ? sizeof(MIXERCONTROLDETAILS_BOOLEAN) : sizeof(MIXERCONTROLDETAILS_UNSIGNED)))
		return MMSYSERR_INVALPARAM;
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerGetControlDetailsW(HMIXEROBJ object, LPMIXERCONTROLDETAILS details, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	const MMRESULT valid = details_check(details, flags);
	if (valid)
		return valid;
	const DWORD index = details->dwControlID / 2;
	EnterCriticalSection(&mixer_lock);
	for (DWORD channel = 0; channel < details->cChannels; channel++) {
		void *item = (BYTE *)details->paDetails + channel * details->cbDetails;
		if (details->dwControlID & 1)
			((MIXERCONTROLDETAILS_BOOLEAN *)item)->fValue = mutes[index];
		else
			((MIXERCONTROLDETAILS_UNSIGNED *)item)->dwValue = volumes[index][channel];
	}
	LeaveCriticalSection(&mixer_lock);
	return MMSYSERR_NOERROR;
}

EXPORT MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ object, LPMIXERCONTROLDETAILS details, DWORD flags)
{
	return mixerGetControlDetailsW(object, details, flags);
}

EXPORT MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ object, LPMIXERCONTROLDETAILS details, DWORD flags)
{
	const MMRESULT checked = object_check((UINT_PTR)object, flags);
	if (checked)
		return checked;
	const MMRESULT valid = details_check(details, flags);
	if (valid)
		return valid;
	const DWORD index = details->dwControlID / 2;
	EnterCriticalSection(&mixer_lock);
	if (details->dwControlID & 1) {
		mutes[index] = ((MIXERCONTROLDETAILS_BOOLEAN *)details->paDetails)->fValue != 0;
	} else {
		/* One channel given sets both, as for a uniform control. */
		for (DWORD channel = 0; channel < 2; channel++) {
			const void *item = (BYTE *)details->paDetails + (details->cChannels == 1 ? 0 : channel) * details->cbDetails;
			const DWORD value = ((const MIXERCONTROLDETAILS_UNSIGNED *)item)->dwValue;
			volumes[index][channel] = value > 0xffff ? 0xffff : value;
		}
	}
	/* Every mixer handle hears of the change; they are told after the lock is left.
	 * shortcut: the first 16 handles are told, allocate the list when a program opens more. */
	notify targets[16];
	HMIXER handles[16];
	int count = 0;
	for (mixer *each = mixers; each && count < 16; each = each->next, count++) {
		targets[count] = each->notify;
		handles[count] = (HMIXER)each;
	}
	LeaveCriticalSection(&mixer_lock);
	if (index == 0 || index == 2)
		wave_volumes_changed();
	for (int each = 0; each < count; each++)
		notify_send(&targets[each], handles[each], MM_MIXM_CONTROL_CHANGE, details->dwControlID, 0);
	return MMSYSERR_NOERROR;
}
