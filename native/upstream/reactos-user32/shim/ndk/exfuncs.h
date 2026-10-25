/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#pragma once
/* The message box asks the kernel to raise a hard error only for service sessions; the component has no such path. */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#endif
#ifndef STATUS_SERVICE_NOTIFICATION
#define STATUS_SERVICE_NOTIFICATION ((NTSTATUS)0x40000018)
#endif
#ifndef HARDERROR_OVERRIDE_ERRORMODE
#define HARDERROR_OVERRIDE_ERRORMODE 0x10000000
#endif
NTSTATUS NTAPI NtRaiseHardError(NTSTATUS error, ULONG parameter_count, ULONG unicode_mask, PULONG_PTR parameters, ULONG options, PULONG response);
typedef enum _HARDERROR_RESPONSE_OPTION { OptionAbortRetryIgnore, OptionOk, OptionOkCancel, OptionRetryCancel, OptionYesNo, OptionYesNoCancel,
	OptionShutdownSystem, OptionOkNoWait, OptionCancelTryContinue } HARDERROR_RESPONSE_OPTION;
typedef enum _HARDERROR_RESPONSE { ResponseReturnToCaller, ResponseNotHandled, ResponseAbort, ResponseCancel, ResponseIgnore, ResponseNo,
	ResponseOk, ResponseRetry, ResponseYes, ResponseTryAgain, ResponseContinue } HARDERROR_RESPONSE, *PHARDERROR_RESPONSE;
