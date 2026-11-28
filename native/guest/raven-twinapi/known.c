/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Storage.KnownFolders: the user's libraries and the folders inside them as storage folders. Each is the known folder of the
 * shell with the same meaning, made if it is not there yet: the Documents, Music, Pictures and Videos libraries are those folders,
 * Camera Roll, Saved Pictures and Screenshots are in Pictures, Playlists in Music, 3D Objects in the profile. Every user is the user the
 * process runs as. HomeGroup, removable devices and media servers are views of other machines and devices, and App Captures and
 * Recorded Calls have no place the shell publishes; asking for them is named in Raven's trace. */
#include "object.h"
#include "../raven-winrt/winrt.h"
/* The folder ids are defined here, not taken from uuid.lib. */
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

static const IID iid_known_statics = {0x5a2a7520, 0x4802, 0x452d, {0x9a, 0xd9, 0x43, 0x51, 0xad, 0xa7, 0xec, 0x35}};
static const IID iid_known_statics2 = {0x194bd0cd, 0xcf6e, 0x4d07, {0x9d, 0x53, 0xe9, 0x16, 0x3a, 0x25, 0x36, 0xe9}};
static const IID iid_known_statics3 = {0xc5194341, 0x9742, 0x4ed5, {0x82, 0x3d, 0xfc, 0x14, 0x01, 0x14, 0x87, 0x64}};
static const IID iid_camera_roll_statics = {0x5d115e66, 0x27e8, 0x492f, {0xb8, 0xe5, 0x2f, 0x90, 0x89, 0x6c, 0xd4, 0xcd}};
static const IID iid_playlists_statics = {0xdad5ecd6, 0x306f, 0x4d6a, {0xb4, 0x96, 0x46, 0xba, 0x8e, 0xb1, 0x06, 0xce}};
static const IID iid_saved_pictures_statics = {0x055c93ea, 0x253d, 0x467c, {0xb6, 0xca, 0xa9, 0x7d, 0xa1, 0xe9, 0xa1, 0x8d}};

/* Windows.Storage.KnownFolderId, in order. */
enum known {
	APP_CAPTURES, CAMERA_ROLL, DOCUMENTS, HOME_GROUP, MEDIA_SERVERS, MUSIC, OBJECTS_3D, PICTURES, PLAYLISTS, RECORDED_CALLS,
	REMOVABLE_DEVICES, SAVED_PICTURES, SCREENSHOTS, VIDEOS, KNOWN_COUNT
};

/* The shell's folder for each id, or what the trace names when there is none. */
static const struct {
	const KNOWNFOLDERID *id;
	const char *missing;
} known_folders[KNOWN_COUNT] = {
	[APP_CAPTURES] = {NULL, "KnownFolders.AppCaptures"},
	[CAMERA_ROLL] = {&FOLDERID_CameraRoll, NULL},
	[DOCUMENTS] = {&FOLDERID_Documents, NULL},
	[HOME_GROUP] = {NULL, "KnownFolders.HomeGroup"},
	[MEDIA_SERVERS] = {NULL, "KnownFolders.MediaServerDevices"},
	[MUSIC] = {&FOLDERID_Music, NULL},
	[OBJECTS_3D] = {&FOLDERID_Objects3D, NULL},
	[PICTURES] = {&FOLDERID_Pictures, NULL},
	[PLAYLISTS] = {&FOLDERID_Playlists, NULL},
	[RECORDED_CALLS] = {NULL, "KnownFolders.RecordedCalls"},
	[REMOVABLE_DEVICES] = {NULL, "KnownFolders.RemovableDevices"},
	[SAVED_PICTURES] = {&FOLDERID_SavedPictures, NULL},
	[SCREENSHOTS] = {&FOLDERID_Screenshots, NULL},
	[VIDEOS] = {&FOLDERID_Videos, NULL},
};

static HRESULT known_folder(int which, void **out)
{
	WCHAR *path = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (which < 0 || which >= KNOWN_COUNT)
		return E_INVALIDARG;
	if (!known_folders[which].id)
		return missing(known_folders[which].missing);
	HRESULT result = SHGetKnownFolderPath(known_folders[which].id, KF_FLAG_CREATE, NULL, &path);
	if (SUCCEEDED(result))
		result = make_folder(path, out);
	CoTaskMemFree(path);
	return result;
}

static HRESULT WINAPI music(void *iface, void **out) { return known_folder(MUSIC, out); }
static HRESULT WINAPI pictures(void *iface, void **out) { return known_folder(PICTURES, out); }
static HRESULT WINAPI videos(void *iface, void **out) { return known_folder(VIDEOS, out); }
static HRESULT WINAPI documents(void *iface, void **out) { return known_folder(DOCUMENTS, out); }
static HRESULT WINAPI home_group(void *iface, void **out) { return known_folder(HOME_GROUP, out); }
static HRESULT WINAPI removable_devices(void *iface, void **out) { return known_folder(REMOVABLE_DEVICES, out); }
static HRESULT WINAPI media_servers(void *iface, void **out) { return known_folder(MEDIA_SERVERS, out); }
static HRESULT WINAPI objects_3d(void *iface, void **out) { return known_folder(OBJECTS_3D, out); }
static HRESULT WINAPI app_captures(void *iface, void **out) { return known_folder(APP_CAPTURES, out); }
static HRESULT WINAPI recorded_calls(void *iface, void **out) { return known_folder(RECORDED_CALLS, out); }
static HRESULT WINAPI camera_roll(void *iface, void **out) { return known_folder(CAMERA_ROLL, out); }
static HRESULT WINAPI playlists(void *iface, void **out) { return known_folder(PLAYLISTS, out); }
static HRESULT WINAPI saved_pictures(void *iface, void **out) { return known_folder(SAVED_PICTURES, out); }

/* GetFolderForUserAsync: the folder for any user, the process's own, as an operation already done. */
static HRESULT WINAPI folder_for_user(void *iface, IInspectable *user, int which, void **out)
{
	union winrt_value folder = {0};
	if (!out)
		return E_POINTER;
	*out = NULL;
	/* What failed is the operation's error. */
	const HRESULT result = known_folder(which, &folder.object);
	const HRESULT made = winrt_async_done(&iid_folder_operation, WINRT_RESULT_OBJECT, result, &folder, out);
	release(folder.object);
	return made;
}

TABLE(known_factory_table, 0, FACTORY);
TABLE(known_statics_table, 1, INSPECTABLE, (const void *)music, (const void *)pictures, (const void *)videos, (const void *)documents,
	(const void *)home_group, (const void *)removable_devices, (const void *)media_servers);
TABLE(known_statics2_table, 2, INSPECTABLE, (const void *)objects_3d, (const void *)app_captures, (const void *)recorded_calls);
TABLE(known_statics3_table, 3, INSPECTABLE, (const void *)folder_for_user);
TABLE(camera_roll_table, 4, INSPECTABLE, (const void *)camera_roll);
TABLE(playlists_table, 5, INSPECTABLE, (const void *)playlists);
TABLE(saved_pictures_table, 6, INSPECTABLE, (const void *)saved_pictures);
static const struct face_iid known_class_iids[] = {{&iid_factory, 0}, {&iid_known_statics, 1}, {&iid_known_statics2, 2},
	{&iid_known_statics3, 3}, {&iid_camera_roll_statics, 4}, {&iid_playlists_statics, 5}, {&iid_saved_pictures_statics, 6}, {NULL, 0}};
static const struct kind known_class_kind = {"KnownFolders", L"Windows.Storage.KnownFolders", known_class_iids, NULL};
struct object known_folders_class = {{known_factory_table.slots, known_statics_table.slots, known_statics2_table.slots,
	known_statics3_table.slots, camera_roll_table.slots, playlists_table.slots, saved_pictures_table.slots}, &known_class_kind, 1};
