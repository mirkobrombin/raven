#include <windows.h>
#ifdef RAVEN_ORACLE
#include <stdio.h>
#endif

__declspec(dllexport) DWORD clip_rows[128][16];
__declspec(dllexport) DWORD clip_pixels[32][16*12];
__declspec(dllexport) DWORD clip_errors[16][2];
static DWORD *pixels;
static unsigned row,images,errors;

static void snapshot(HDC dc,unsigned stage,int result,DWORD error)
{
	HRGN saved=CreateRectRgn(90,91,92,93);RECT box={0},visible={0};
	int has=GetClipRgn(dc,saved),type=GetRgnBox(saved,&box),kind=GetClipBox(dc,&visible);
	if(row>=128){++errors;return;}
	DWORD *out=clip_rows[row++];out[0]=stage;out[1]=result;out[2]=error;out[3]=has;out[4]=type;
	out[5]=box.left;out[6]=box.top;out[7]=box.right;out[8]=box.bottom;out[9]=kind;
	out[10]=visible.left;out[11]=visible.top;out[12]=visible.right;out[13]=visible.bottom;out[14]=GetLayout(dc);
	for(unsigned i=0;i<16*12;++i)pixels[i]=0x00112233;
	HBRUSH brush=CreateSolidBrush(RGB(0x90,0xa0,0xb0));RECT area={-32,-32,48,44};
	if(!FillRect(dc,&area,brush) || !DeleteObject(brush))++errors;
	DWORD digest=2166136261U;
	for(unsigned i=0;i<16*12;++i){digest=(digest^pixels[i])*16777619U;if(images<32)clip_pixels[images][i]=pixels[i];}
	out[15]=digest;++images;
	if(!DeleteObject(saved))++errors;
}

static void state(HDC dc,unsigned stage,int result)
{
	snapshot(dc,stage,result,GetLastError());
}

int raven_entry(void)
{
	HDC dc=CreateCompatibleDC(NULL);BITMAPINFO bi={0};bi.bmiHeader.biSize=40;bi.bmiHeader.biWidth=16;bi.bmiHeader.biHeight=-12;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
	HBITMAP bitmap=CreateDIBSection(dc,&bi,0,(void **)&pixels,NULL,0);if(!dc || !bitmap || !pixels)return 1;
	HGDIOBJ old=SelectObject(dc,bitmap);if(!old)return 2;
	HRGN a=CreateRectRgn(10,9,2,1),b=CreateRectRgn(6,4,14,11),out=CreateRectRgn(0,0,0,0);
	if(!a || !b || !out || GetObjectType(a)!=OBJ_REGION)return 3;
	SetLastError(71);state(dc,0,0);
	for(unsigned mode=1;mode<=5;++mode) {
		SetLastError(71);int result=CombineRgn(out,a,mode==5 ? (HRGN)123 : b,mode);DWORD error=GetLastError();
		if(!result)return 4;
		for(int y=-1;y<14;++y)for(int x=-1;x<18;++x){
			BOOL first=x>=2 && x<10 && y>=1 && y<9,second=x>=6 && x<14 && y>=4 && y<11;
			BOOL expected=mode==1 ? first && second : mode==2 ? first || second : mode==3 ? first!=second : mode==4 ? first && !second : first;
			if(PtInRegion(out,x,y)!=expected)++errors;
		}
		if(SelectClipRgn(dc,out)!=result)return 5;
		snapshot(dc,mode,result,error);
	}
	SetLastError(71);state(dc,6,(int)(INT_PTR)SelectObject(dc,out));
	if(!SetRectRgn(out,40,45,50,60))return 6;
	SetLastError(71);state(dc,7,SelectClipRgn(dc,out));
	SetLastError(71);state(dc,8,SelectClipRgn(dc,NULL));
	SetLastError(71);state(dc,9,IntersectClipRect(dc,-5,-4,25,24));
	SetLastError(71);state(dc,10,ExcludeClipRect(dc,2,3,6,7));
	SetLastError(71);state(dc,11,SelectClipRgn(dc,NULL));
	SetLastError(71);state(dc,12,ExcludeClipRect(dc,2,3,6,7));
	if(!SetRectRgn(out,-4,-4,20,16) || !SetRectRgn(b,-1,-1,17,13) || CombineRgn(out,out,b,RGN_DIFF)!=COMPLEXREGION)return 7;
	SetLastError(71);state(dc,13,SelectClipRgn(dc,out));
	if(!SetRectRgn(out,0,0,0,0))return 8;
	SetLastError(71);state(dc,14,SelectClipRgn(dc,out));
	SetLastError(71);state(dc,15,SelectClipRgn(dc,NULL));
	if(!SetRectRgn(out,1,2,9,10) || OffsetRgn(out,3,-1)!=SIMPLEREGION)return 9;
	RECT inverse={8,8,4,1};if(!RectInRegion(out,&inverse))return 10;
	if(CombineRgn(a,out,NULL,RGN_COPY)!=SIMPLEREGION || !EqualRgn(a,out))return 11;
	SetLastError(71);state(dc,16,SelectClipRgn(dc,out));
	if(!SetRectRgn(out,0,0,1,1) || EqualRgn(a,out) || !DeleteObject(out))return 12;
	SetLastError(71);state(dc,17,0);
	if(SelectClipRgn(dc,NULL)!=SIMPLEREGION || GetClipRgn(dc,a)!=0)return 13;
	// A ring of bands: rectangles meeting the hole are outside, those meeting a band inside, and every change is seen at once.
	HRGN ring=CreateRectRgn(0,0,30,30),hole=CreateRectRgn(10,10,20,20);RECT probe,rbox;
	if(!ring || !hole || CombineRgn(ring,ring,hole,RGN_DIFF)!=COMPLEXREGION)return 18;
	SetRect(&probe,12,12,18,18);if(RectInRegion(ring,&probe))return 19;
	SetRect(&probe,18,18,12,12);if(RectInRegion(ring,&probe))return 19;
	SetRect(&probe,5,12,11,13);if(!RectInRegion(ring,&probe))return 20;
	SetRect(&probe,12,12,12,18);if(RectInRegion(ring,&probe))return 20;
	SetRect(&probe,30,0,40,10);if(RectInRegion(ring,&probe))return 20;
	if(GetRgnBox(ring,&rbox)!=COMPLEXREGION || rbox.left!=0 || rbox.top!=0 || rbox.right!=30 || rbox.bottom!=30)return 21;
	if(OffsetRgn(ring,100,0)!=COMPLEXREGION || GetRgnBox(ring,&rbox)!=COMPLEXREGION || rbox.left!=100 || rbox.right!=130)return 22;
	SetRect(&probe,5,12,11,13);if(RectInRegion(ring,&probe))return 22;
	SetRect(&probe,105,12,111,13);if(!RectInRegion(ring,&probe))return 22;
	// Many bands, more than a record of the table holds.
	if(!SetRectRgn(ring,0,0,1,1))return 23;
	for(int i=1;i<40;++i){if(!SetRectRgn(hole,i*4,i*3,i*4+2,i*3+2) || CombineRgn(ring,ring,hole,RGN_OR)!=COMPLEXREGION)return 23;}
	SetRect(&probe,40*4-4,39*3,40*4-3,39*3+1);if(!RectInRegion(ring,&probe))return 24;
	SetRect(&probe,2,2,4,3);if(RectInRegion(ring,&probe))return 24;
	if(GetRgnBox(ring,&rbox)!=COMPLEXREGION || rbox.right!=39*4+2 || rbox.bottom!=39*3+2)return 24;
	if(!SetRectRgn(ring,0,0,0,0) || GetRgnBox(ring,&rbox)!=NULLREGION || rbox.left || rbox.right)return 25;
	SetRect(&probe,0,0,5,5);if(RectInRegion(ring,&probe))return 25;
	if(!DeleteObject(ring) || !DeleteObject(hole))return 26;
	SetLastError(71);if(GetRgnBox(ring,&rbox) || GetLastError()!=71)return 26;
	POINT points[3]={{-3,7},{0,0},{14,-9}};
	if(!DPtoLP(dc,points,3) || !LPtoDP(dc,points,3) || points[0].x!=-3 || points[0].y!=7 || points[2].x!=14 || points[2].y!=-9 || !DPtoLP(dc,NULL,0))return 14;
	unsigned negative=0;
#define NEGATIVE(expr,want,err) do {SetLastError(71);DWORD result=(DWORD)(expr),error=GetLastError();clip_errors[negative][0]=result;clip_errors[negative++][1]=error;if(result!=(DWORD)(want) || error!=(err))++errors;}while(0)
	NEGATIVE(GetClipRgn(dc,NULL),0,71);
	NEGATIVE(GetClipRgn(dc,(HRGN)123),0,71);
	NEGATIVE(GetClipRgn((HDC)123,a),-1,71);
	NEGATIVE(CombineRgn(a,a,b,0),0,71);
	NEGATIVE(SelectClipRgn(dc,(HRGN)123),0,71);
	snapshot(dc,19,0,71);
	RECT box={0};NEGATIVE(GetClipBox((HDC)123,&box),0,71);
	NEGATIVE(GetLayout((HDC)123),GDI_ERROR,6);
	NEGATIVE(SetRectRgn((HRGN)123,0,0,2,2),0,71);
	NEGATIVE(PtInRegion((HRGN)123,0,0),0,71);
	NEGATIVE(OffsetRgn((HRGN)123,0,0),0,71);
	NEGATIVE(DPtoLP((HDC)123,points,1),0,71);
	NEGATIVE(DPtoLP(dc,points,-1),1,71);
	SetLastError(71);state(dc,18,IntersectClipRect(dc,9,8,2,1));
	if(GetClipRgn(dc,a)!=1 || GetRgnBox(a,&box)!=SIMPLEREGION || box.left!=2 || box.top!=1 || box.right!=9 || box.bottom!=8)return 17;
	if(!DeleteObject(a) || !DeleteObject(b) || SelectObject(dc,old)!=bitmap || !DeleteObject(bitmap) || !DeleteDC(dc))return 15;
	return errors ? 16 : 0;
}

#ifdef RAVEN_ORACLE
int main(void)
{
	int result=raven_entry();FILE *f=fopen("wine-clip-rows.bin","wb");if(!f)return 90;
	if(fwrite(clip_rows,1,sizeof(clip_rows),f)!=sizeof(clip_rows) || fclose(f))return 91;
	f=fopen("wine-clip-pixels.bin","wb");if(!f)return 92;
	if(fwrite(clip_pixels,1,sizeof(clip_pixels),f)!=sizeof(clip_pixels) || fclose(f))return 93;
	f=fopen("wine-clip-errors.bin","wb");if(!f)return 94;
	if(fwrite(clip_errors,1,sizeof(clip_errors),f)!=sizeof(clip_errors) || fclose(f))return 95;
	printf("result=%d rows=%u images=%u errors=%u\n",result,row,images,errors);return result;
}
#endif
