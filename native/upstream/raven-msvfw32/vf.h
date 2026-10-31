/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_MSVFW_H
#define RAVEN_MSVFW_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>

void *vf_alloc(SIZE_T size);
void vf_free(void *block);

#endif
