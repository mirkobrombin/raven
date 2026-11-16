/* SPDX-License-Identifier: MIT
 *
 * raven-winmm.dll for each width: the parts of winmm.dll that call a program back or move samples, Raven code on the public headers.
 * The runtime's winmm.dll forwards these exports here, so the callbacks run on guest threads of this component: the multimedia timers on
 * one timer thread as on Windows, each wave device's buffers on a thread of its own. Samples go through the shared page of a stream of
 * the runtime's sound engine (raven_audio.h) without leaving the guest. What stays with the runtime is the clock (timeGetTime and its
 * period), the MIDI and auxiliary devices the machine lacks, and the mmio file functions.
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WINMM_
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <stddef.h>
#include <string.h>

#include "../../nt/raven_audio.h"

#define EXPORT __declspec(dllexport)

/* The heap functions need the guest RTL component, which a build may not provision; LocalAlloc is always served. */
#define ALLOC(size) LocalAlloc(LPTR, (size))
#define FREE(block) LocalFree(block)

#ifndef DRVM_MAPPER_PREFERRED_GET
#define DRVM_MAPPER_PREFERRED_GET 0x2015
#endif
#ifndef DRVM_MAPPER_CONSOLEVOICECOM_GET
#define DRVM_MAPPER_CONSOLEVOICECOM_GET 0x2017
#endif

/* What a device calls back with: a window or a thread is posted the message, a function is called, an event is set. */
typedef struct notify {
	DWORD kind;
	DWORD_PTR target, instance;
} notify;

/* Checks a waveOutOpen-style callback and its flags; MMSYSERR_NOERROR when they can be used. */
MMRESULT notify_init(notify *notify, DWORD_PTR callback, DWORD_PTR instance, DWORD flags);
void notify_send(const notify *notify, HANDLE handle, UINT message, DWORD_PTR first, DWORD_PTR second);

/* user32 is the program's: it is looked up when a window or a thread is to be posted to, so the component imports nothing beyond the
 * kernel and the runtime. */
FARPROC user32_function(const char *name);

/* The scale the speakers line of the mixer sets for every wave output, 0 to 1; changing it updates the open devices. */
float master_volume(void);
void wave_volumes_changed(void);

void ascii_from_wide(char *target, size_t size, const WCHAR *source);
