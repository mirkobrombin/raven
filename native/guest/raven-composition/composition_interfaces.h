/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Interfaces of Windows.UI.Composition that the MinGW headers do not declare, in the method order of the Windows metadata: the
 * controller that commits a compositor's changes when the program says so, the desktop window target that shows a visual tree in a
 * window, and the compositor's interface for making such targets. */
#pragma once

DEFINE_GUID(IID_ICompositorController, 0x2d75f35a, 0x70a7, 0x4395, 0xba, 0x2d, 0xce, 0xf0, 0xb1, 0x83, 0x99, 0xf9);
DEFINE_GUID(IID_ICompositorDesktopInterop, 0x29e691fa, 0x4567, 0x4dca, 0xb3, 0x19, 0xd0, 0xf2, 0x07, 0xeb, 0x68, 0x07);
DEFINE_GUID(IID_IDesktopWindowTarget, 0x6329d6ca, 0x3366, 0x490e, 0x9d, 0xb3, 0x25, 0x31, 0x29, 0x29, 0xac, 0x51);

typedef struct ICompositorControllerVtbl {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, REFIID riid, void **object);
	ULONG (STDMETHODCALLTYPE *AddRef)(void *This);
	ULONG (STDMETHODCALLTYPE *Release)(void *This);
	HRESULT (STDMETHODCALLTYPE *GetIids)(void *This, ULONG *count, IID **iids);
	HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *This, HSTRING *name);
	HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *This, TrustLevel *level);
	HRESULT (STDMETHODCALLTYPE *get_Compositor)(void *This, void **compositor);
	HRESULT (STDMETHODCALLTYPE *Commit)(void *This);
	HRESULT (STDMETHODCALLTYPE *EnsurePreviousCommitCompletedAsync)(void *This, void **operation);
	HRESULT (STDMETHODCALLTYPE *add_CommitNeeded)(void *This, void *handler, EventRegistrationToken *token);
	HRESULT (STDMETHODCALLTYPE *remove_CommitNeeded)(void *This, EventRegistrationToken token);
} ICompositorControllerVtbl;

typedef struct ICompositorDesktopInteropVtbl {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, REFIID riid, void **object);
	ULONG (STDMETHODCALLTYPE *AddRef)(void *This);
	ULONG (STDMETHODCALLTYPE *Release)(void *This);
	HRESULT (STDMETHODCALLTYPE *CreateDesktopWindowTarget)(void *This, HWND window, BOOL topmost, void **target);
	HRESULT (STDMETHODCALLTYPE *EnsureOnThread)(void *This, DWORD thread);
} ICompositorDesktopInteropVtbl;

typedef struct IDesktopWindowTargetVtbl {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, REFIID riid, void **object);
	ULONG (STDMETHODCALLTYPE *AddRef)(void *This);
	ULONG (STDMETHODCALLTYPE *Release)(void *This);
	HRESULT (STDMETHODCALLTYPE *GetIids)(void *This, ULONG *count, IID **iids);
	HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *This, HSTRING *name);
	HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *This, TrustLevel *level);
	HRESULT (STDMETHODCALLTYPE *get_IsTopmost)(void *This, boolean *topmost);
} IDesktopWindowTargetVtbl;
