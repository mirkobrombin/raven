#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>
#include <mfapi.h>

/* The multimedia components a media library takes its threads and its buffers through: avrt.dll registers a thread with the scheduler and
   mfplat.dll makes the samples, types and buffers a pipeline carries. Nothing here schedules threads or carries media, so the task is a
   stand-in handle and each factory names its interface as unsupported instead of handing back an object every later call fails on. */
int raven_entry(void)
{
	DWORD task_index = 0xdead;
	HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
	if (!task || task_index != 0)
		return 1;
	if (!AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH))
		return 2;
	ULONG responsiveness = 0xdead;
	if (AvQuerySystemResponsiveness(task, &responsiveness) != S_OK || responsiveness != 0)
		return 3;
	if (!AvRevertMmThreadCharacteristics(task))
		return 4;

	IMFSample *sample = (IMFSample *)0xdead;
	if (MFCreateSample(&sample) != E_NOTIMPL || sample)
		return 5;
	IMFMediaType *type = (IMFMediaType *)0xdead;
	if (MFCreateMediaType(&type) != E_NOTIMPL || type)
		return 6;
	IMFMediaBuffer *buffer = (IMFMediaBuffer *)0xdead;
	if (MFCreateMemoryBuffer(4096, &buffer) != E_NOTIMPL || buffer)
		return 7;
	buffer = (IMFMediaBuffer *)0xdead;
	if (MFCreateAlignedMemoryBuffer(4096, 16, &buffer) != E_NOTIMPL || buffer)
		return 8;
	/* No transform is registered, so an enumeration succeeds and finds none, as paint.net asks whether a video codec is installed. */
	static const GUID video_decoders = {0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
	IMFActivate **activates = (IMFActivate **)0xdead;
	UINT32 count = 0xdead;
	if (MFTEnumEx(video_decoders, MFT_ENUM_FLAG_ALL, NULL, NULL, &activates, &count) != S_OK || activates || count)
		return 9;
	return 0;
}
