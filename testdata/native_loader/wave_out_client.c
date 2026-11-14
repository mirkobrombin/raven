// A program plays a buffer through the waveform output and hears through its event that the buffer is done, as the audio endpoint
// MusicBee's 32-bit sound library opens plays through it. The buffer is a tenth of a second of silence. The exit code names the first
// check that failed.
#include <windows.h>
#include <mmsystem.h>

static short samples[4410 * 2];

int raven_entry(void)
{
	if (waveOutGetNumDevs() < 1)
		return 1;
	WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
	HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
	HWAVEOUT device = NULL;
	if (waveOutOpen(&device, WAVE_MAPPER, &format, (DWORD_PTR)done, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR || !device)
		return 2;
	WAVEHDR header = {0};
	header.lpData = (LPSTR)samples;
	header.dwBufferLength = sizeof samples;
	if (waveOutPrepareHeader(device, &header, sizeof header) != MMSYSERR_NOERROR || !(header.dwFlags & WHDR_PREPARED))
		return 3;
	if (waveOutWrite(device, &header, sizeof header) != MMSYSERR_NOERROR)
		return 4;
	const DWORD start = GetTickCount();
	while (!(header.dwFlags & WHDR_DONE) && GetTickCount() - start < 5000)
		WaitForSingleObject(done, 100);
	if (!(header.dwFlags & WHDR_DONE))
		return 5;
	MMTIME position = {TIME_BYTES};
	if (waveOutGetPosition(device, &position, sizeof position) != MMSYSERR_NOERROR || position.wType != TIME_BYTES || !position.u.cb)
		return 6;
	if (waveOutUnprepareHeader(device, &header, sizeof header) != MMSYSERR_NOERROR || waveOutClose(device) != MMSYSERR_NOERROR)
		return 7;
	CloseHandle(done);
	return 0;
}
