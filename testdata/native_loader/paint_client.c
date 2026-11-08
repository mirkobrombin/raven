#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>

static BYTE data[40+32*32*4+128];
static HICON icon;
#ifdef RAVEN_BITBLT_PAINT
static HDC back_dc;
static HBITMAP back_bitmap;
static HGDIOBJ back_stock;
#endif
static unsigned painted,failed;
#ifdef RAVEN_REPAINT_LOOP
static unsigned repaint,erased;
static HICON replacement;
#endif
#ifdef RAVEN_KEYBOARD_LOOP
static unsigned keydown,keyup,characters;
#ifdef RAVEN_FOCUS_LOOP
static unsigned held_release,super_down,super_up;
#endif
#endif
#ifdef RAVEN_POINTER_LOOP
static unsigned moved,pressed;
#endif
__declspec(dllexport) volatile unsigned phase;

static void put(unsigned at,DWORD value)
{
	for (unsigned byte=0;byte<4;++byte) data[at+byte]=(BYTE)(value>>(byte*8));
}

static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
	(void)wparam; (void)lparam;
	if (message==WM_NCCREATE) return DefWindowProcW(window,message,wparam,lparam);
	if (message==WM_NCACTIVATE || message==WM_ACTIVATE) return DefWindowProcW(window,message,wparam,lparam);
	if (message==WM_SETTEXT || message==WM_GETTEXT || message==WM_GETTEXTLENGTH) return DefWindowProcW(window,message,wparam,lparam);
#ifdef RAVEN_REPAINT_LOOP
	if (message==WM_ERASEBKGND) { ++erased; return repaint==3; }
#endif
#ifdef RAVEN_KEYBOARD_LOOP
	if (message==WM_KEYDOWN || message==WM_KEYUP) {
#ifdef RAVEN_FOCUS_LOOP
		if (wparam==VK_LWIN) {
			if ((DWORD)lparam!=(message==WM_KEYDOWN ? 0x415b0001U : 0xc15b0001U)) failed=27;
			if (message==WM_KEYDOWN) ++super_down; else ++super_up;
			return 0;
		}
		if (message==WM_KEYUP && wparam=='G' && !keydown) {
			if ((DWORD)lparam!=0xc0220001U) failed=28;
			++held_release; return 0;
		}
#endif
		if (wparam!='G' || (DWORD)lparam!=(message==WM_KEYDOWN ? 0x220001U : 0xc0220001U)) failed=7;
		if (message==WM_KEYDOWN) ++keydown; else ++keyup;
		if (keydown && keyup && characters) phase=3;
		return 0;
	}
	if (message==WM_CHAR) {
		if (wparam!='g' || (DWORD)lparam!=0x220001U || !keydown) failed=8;
		++characters;
		if (!SetWindowTextW(window,L"Keyboard: g")) failed=9;
		if (keydown && keyup) phase=3;
		return 0;
	}
#endif
#ifdef RAVEN_POINTER_LOOP
	if (message==WM_NCHITTEST || message==WM_SETCURSOR) return DefWindowProcW(window,message,wparam,lparam);
	if (message==WM_MOUSEACTIVATE) return MA_ACTIVATE;
	if (message==WM_MOUSEMOVE) { ++moved; return 0; }
	if (message==WM_LBUTTONDOWN || message==WM_LBUTTONUP) {
		if ((SHORT)LOWORD(lparam)!=16 || (SHORT)HIWORD(lparam)!=16 ||
			wparam!=(message==WM_LBUTTONDOWN ? MK_LBUTTON : 0)) failed=4;
		if (message==WM_LBUTTONDOWN) pressed=1;
		else { if (!moved || !pressed) failed=5; DestroyWindow(window); }
		return 0;
	}
#endif
#ifdef RAVEN_CLOSE_LOOP
	if (message==WM_CLOSE) return DefWindowProcW(window,message,wparam,lparam);
	if (message==WM_DESTROY) { PostQuitMessage(23); return 0; }
#endif
	if (message==WM_PAINT) {
		PAINTSTRUCT paint;
		HDC dc=BeginPaint(window,&paint);
#ifdef RAVEN_REPAINT_LOOP
		RECT expected={0,0,32,32};
		if (repaint==1) expected=(RECT){2,3,28,29};
		if (repaint==2) expected=(RECT){12,13,14,15};
		if (repaint==3) expected=(RECT){0,0,1,1};
		if (repaint==4) expected=(RECT){30,30,32,32};
		if (!dc || paint.hdc!=dc || paint.rcPaint.left!=expected.left || paint.rcPaint.top!=expected.top ||
			paint.rcPaint.right!=expected.right || paint.rcPaint.bottom!=expected.bottom || (repaint && !!paint.fErase!=(repaint==4))) failed=10;
		if (!DrawIconEx(dc,0,0,repaint ? replacement : icon,0,0,0,NULL,DI_NORMAL)) failed=11;
		if (repaint==1) { RECT next={12,13,14,15}; if (!InvalidateRect(window,&next,FALSE)) failed=12; }
#else
		if (!dc || paint.hdc!=dc || paint.rcPaint.left || paint.rcPaint.top || paint.rcPaint.right!=32 || paint.rcPaint.bottom!=32)
			failed=1;
		{
			/* A paint begun inside this one, as MFC's paint contexts nest, gets a context of its own and nothing left to update. */
			PAINTSTRUCT inner;
			HDC nested=BeginPaint(window,&inner);
			if (!nested || nested==dc || inner.hdc!=nested || !IsRectEmpty(&inner.rcPaint)) failed=14;
			if (!EndPaint(window,&inner)) failed=15;
		}
#ifdef RAVEN_BITBLT_PAINT
		if (!BitBlt(dc,0,0,32,32,back_dc,0,0,SRCCOPY)) failed=2;
#else
		if (!DrawIconEx(dc,0,0,icon,0,0,0,NULL,DI_NORMAL)) failed=2;
#endif
#endif
		if (!EndPaint(window,&paint)) failed=3;
		++painted;
	}
	return 0;
}

int raven_entry(void)
{
	put(0,40); put(4,32); put(8,64); put(12,1|(32U<<16));
	for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x) {
		unsigned at=40+(y*32+x)*4;
		data[at]=(BYTE)(x*7); data[at+1]=(BYTE)(y*7); data[at+2]=0xc0; data[at+3]=255;
	}
	icon=CreateIconFromResourceEx(data,sizeof(data),TRUE,0x30000,0,0,0);
	if (!icon) return 1;
#ifdef RAVEN_BITBLT_PAINT
	BITMAPINFO bitmap_info={0}; bitmap_info.bmiHeader.biSize=40;
	bitmap_info.bmiHeader.biWidth=32; bitmap_info.bmiHeader.biHeight=32;
	bitmap_info.bmiHeader.biPlanes=1; bitmap_info.bmiHeader.biBitCount=32;
	BYTE *back_pixels;
	back_dc=CreateCompatibleDC(NULL);
	back_bitmap=CreateDIBSection(back_dc,&bitmap_info,0,(void **)&back_pixels,NULL,0);
	if (!back_dc || !back_bitmap || !(back_stock=SelectObject(back_dc,back_bitmap))) return 29;
	for (unsigned i=0;i<32*32*4;++i) back_pixels[i]=data[40+i];
#endif
	HINSTANCE instance=GetModuleHandleW(NULL);
	WNDCLASSEXW definition={0}; definition.cbSize=sizeof(definition); definition.hInstance=instance;
	definition.lpfnWndProc=procedure; definition.lpszClassName=L"RavenPaintContract";
#ifdef RAVEN_POINTER_LOOP
	definition.hCursor=LoadCursorW(NULL,IDC_HAND);
#endif
	ATOM atom=RegisterClassExW(&definition);
	if (!atom) return 2;
	HWND window=CreateWindowExW(0,MAKEINTATOM(atom),L"Raven native paint",WS_POPUP,12,23,32,32,NULL,NULL,instance,NULL);
	if (!window) return 3;
#ifdef RAVEN_COMPATIBLE_PAINT
	HDC window_dc=GetDC(window),next_dc=CreateCompatibleDC(window_dc);
	HBITMAP next_bitmap=CreateCompatibleBitmap(window_dc,32,32);
	BITMAP compatible_info;
	if (!window_dc || !next_dc || !next_bitmap || GetObjectW(next_bitmap,sizeof(compatible_info),&compatible_info)!=sizeof(compatible_info) ||
		compatible_info.bmWidth!=32 || compatible_info.bmHeight!=32 || compatible_info.bmBitsPixel!=32 || compatible_info.bmBits) return 31;
	HGDIOBJ next_stock=SelectObject(next_dc,next_bitmap);
	if (!next_stock || !BitBlt(next_dc,0,0,32,32,back_dc,0,0,SRCCOPY) || ReleaseDC(window,window_dc)!=1 ||
		SelectObject(back_dc,back_stock)!=back_bitmap || !DeleteObject(back_bitmap) || !DeleteDC(back_dc)) return 32;
	back_dc=next_dc; back_bitmap=next_bitmap; back_stock=next_stock;
#endif
	ShowWindow(window,SW_SHOW);
	RECT rect;
	if (!GetClientRect(window,&rect) || rect.left || rect.top || rect.right!=32 || rect.bottom!=32 ||
		!GetWindowRect(window,&rect) || rect.left!=12 || rect.top!=23 || rect.right!=44 || rect.bottom!=55 ||
		SetFocus(window)!=window || GetFocus()!=window || GetActiveWindow()!=window) return 8;
	MSG message;
	if (!IsWindowVisible(window) || !PeekMessageW(&message,window,WM_PAINT,WM_PAINT,PM_NOREMOVE) || message.message!=WM_PAINT ||
		GetMessageW(&message,window,WM_PAINT,WM_PAINT)!=1 || TranslateMessage(&message) || DispatchMessageW(&message) || !painted || failed) return 4;
	HDC dc=GetDC(window);
	if (!dc || ReleaseDC(window,dc)!=1) return 5;
	phase=1;
#ifdef RAVEN_REPAINT_LOOP
	erased=0;
	for (unsigned index=0;index<32*32;++index) data[40+index*4+2]=64;
	replacement=CreateIconFromResourceEx(data,sizeof(data),TRUE,0x30000,0,0,0);
	if (!replacement) return 10;
	RECT a={2,3,10,11},b={20,21,28,29},hole={4,5,6,7};
	if (!InvalidateRect(window,&a,FALSE) || !InvalidateRect(window,&b,FALSE) || !ValidateRect(window,&hole) ||
		!GetUpdateRect(window,&rect,FALSE) || rect.left!=2 || rect.top!=3 || rect.right!=28 || rect.bottom!=29) return 11;
	repaint=1;
	if (!UpdateWindow(window) || failed || !GetUpdateRect(window,&rect,FALSE) || rect.left!=12 || rect.top!=13 || rect.right!=14 || rect.bottom!=15) return 12;
	repaint=2;
	if (!UpdateWindow(window) || failed || GetUpdateRect(window,&rect,FALSE)) return 13;
	if (!InvalidateRect(window,NULL,TRUE) || !ValidateRect(window,NULL) || GetUpdateRect(window,&rect,FALSE)) return 14;
	a=(RECT){0,0,1,1}; repaint=3;
	if (!InvalidateRect(window,&a,TRUE) || !UpdateWindow(window) || failed || erased!=1) return 15;
	a=(RECT){30,30,32,32}; repaint=4;
	if (!InvalidateRect(window,&a,TRUE) || !UpdateWindow(window) || failed || erased!=2) return 16;
	if (!SetWindowTextW(window,L"Repaint: ready")) return 17;
	phase=4;
#endif
#ifdef RAVEN_CLOSE_LOOP
	BOOL received;
	while ((received=GetMessageW(&message,NULL,0,0))>0) {
#ifdef RAVEN_POINTER_LOOP
		if (message.message>=WM_MOUSEMOVE && message.message<=WM_MBUTTONUP &&
			(message.pt.x!=12+(SHORT)LOWORD(message.lParam) || message.pt.y!=23+(SHORT)HIWORD(message.lParam))) failed=6;
#endif
		TranslateMessage(&message); DispatchMessageW(&message);
	}
	if (received || message.wParam!=23 || IsWindow(window) || failed) return 6;
#ifdef RAVEN_KEYBOARD_LOOP
	if (keydown!=1 || keyup!=1 || characters!=1) return 9;
#ifdef RAVEN_FOCUS_LOOP
	if (held_release!=1 || super_down!=1 || super_up!=1) return 29;
#endif
#endif
	if (!UnregisterClassW(MAKEINTATOM(atom),instance) || !DestroyIcon(icon)) return 7;
#ifdef RAVEN_REPAINT_LOOP
	if (!DestroyIcon(replacement)) return 18;
#endif
#else
	HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);
	if (!event || WaitForSingleObject(event,2000)!=WAIT_TIMEOUT || !CloseHandle(event)) return 6;
	if (!DestroyWindow(window) || IsWindowVisible(window) || !UnregisterClassW(MAKEINTATOM(atom),instance) || !DestroyIcon(icon)) return 7;
#endif
#ifdef RAVEN_BITBLT_PAINT
	if (SelectObject(back_dc,back_stock)!=back_bitmap || !DeleteObject(back_bitmap) || !DeleteDC(back_dc)) return 30;
#endif
	phase=2;
	return 0;
}
