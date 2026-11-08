#include <windows.h>

// Single-line edit controls place and hit-test text from the measured advances of their characters.
static int edit_x(HWND edit,unsigned index)
{
	return (short)LOWORD(SendMessageW(edit,EM_POSFROMCHAR,index,0));
}

int raven_entry(void)
{
	HWND parent=CreateWindowExW(0,L"STATIC",L"edit parent",WS_POPUP,0,0,200,100,NULL,NULL,NULL,NULL);
	HWND centered=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_BORDER|ES_NUMBER|ES_CENTER,5,5,60,20,parent,(HMENU)5,NULL,NULL);
	HWND right=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_BORDER|ES_RIGHT,80,5,60,20,parent,(HMENU)6,NULL,NULL);
	HWND left=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_BORDER,5,40,60,20,parent,(HMENU)7,NULL,NULL);
	WCHAR text[16];
	int start,second,third,fourth;
	if (!parent || !centered || !right || !left) return 1;
	if (!SetDlgItemInt(parent,5,1000,FALSE) || !SetDlgItemInt(parent,6,1000,FALSE) || !SetDlgItemInt(parent,7,1000,FALSE)) return 2;
	if (GetDlgItemTextW(parent,5,text,16)!=4 || lstrcmpW(text,L"1000")) return 3;
	start=edit_x(left,0);second=edit_x(left,1);third=edit_x(left,2);fourth=edit_x(left,3);
	// Every digit has the same advance, so the positions step evenly and the first one is the format rectangle margin.
	if (start<0 || second<=start || third-second!=second-start || fourth-third!=third-second) return 4;
	// The same text sits further right the more the control pushes it towards the right edge.
	if (edit_x(centered,0)<=start+2 || edit_x(right,0)<=edit_x(centered,0)) return 5;
	// Offsets across the string are the character advances, so hit testing maps a point back to its character.
	if ((int)LOWORD(SendMessageW(left,EM_CHARFROMPOS,0,MAKELPARAM(start+1,5)))!=0) return 6;
	if ((int)LOWORD(SendMessageW(left,EM_CHARFROMPOS,0,MAKELPARAM(third+1,5)))!=2) return 7;
	if ((int)LOWORD(SendMessageW(left,EM_CHARFROMPOS,0,MAKELPARAM(58,5)))!=4) return 8;
	// An MDI client erases what no child covers with the application workspace colour.
	CLIENTCREATESTRUCT creation={0};
	HWND client=CreateWindowExW(0,L"MDICLIENT",NULL,WS_CHILD,0,0,40,30,parent,NULL,NULL,&creation);
	if (!client || GetClassLongPtrW(client,GCLP_HBRBACKGROUND)!=(ULONG_PTR)(COLOR_APPWORKSPACE+1)) return 9;
	HDC screen=GetDC(NULL),memory=CreateCompatibleDC(screen);
	HBITMAP bitmap=CreateCompatibleBitmap(screen,40,30),previous=SelectObject(memory,bitmap);
	SetPixel(memory,10,10,RGB(1,2,3));
	SendMessageW(client,WM_ERASEBKGND,(WPARAM)memory,0);
	if (GetPixel(memory,10,10)!=GetSysColor(COLOR_APPWORKSPACE)) return 10;
	SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(NULL,screen);
	DestroyWindow(parent);
	return 0;
}
