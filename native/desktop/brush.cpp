#include "api.h"
#include "brush.h"
#include <algorithm>

namespace raven {

bool fill_rect(NativeProcess &process,DesktopObjects &objects,uint64_t handle,DesktopRect rect,uint64_t brush_handle)
{
	if (brush_handle && brush_handle<=31) brush_handle=objects.system_brush(int32_t(brush_handle-1));
	const auto *dc=objects.dc(handle); const auto *brush=objects.brush(brush_handle);
	if (!dc || !brush) throw uint32_t(brush_handle>0 && brush_handle<=32 ? 120 : 6);
	auto *bitmap=objects.bitmap(dc->bitmap);
	if (!bitmap || bitmap->depth!=32) throw uint32_t(120);
	const auto left=std::max(int64_t(rect.left)+dc->origin_x,int64_t(0)),top=std::max(int64_t(rect.top)+dc->origin_y,int64_t(0));
	const auto right=std::min(int64_t(rect.right)+dc->origin_x,int64_t(bitmap->width)),bottom=std::min(int64_t(rect.bottom)+dc->origin_y,int64_t(bitmap->height));
	if (brush->hollow || right<=left || bottom<=top) return false;
	const auto pixels=bitmap->pixels();
	bool changed=false;
	for (int64_t y=top;y<bottom;++y) for (int64_t x=left;x<right;++x) {
		if (!dc->contains(x,y)) continue;
		const auto row=bitmap->top_down ? size_t(y) : bitmap->height-1-size_t(y);
		const size_t at=(row*bitmap->width+size_t(x))*4;
		pixels[at]=uint8_t(brush->color>>16); pixels[at+1]=uint8_t(brush->color>>8); pixels[at+2]=uint8_t(brush->color); pixels[at+3]=0; changed=true;
	}
	return changed;
}

bool NativeDesktop::fill(std::span<const uint64_t> args)
{
	try {
		if (!objects.dc(args[0])) throw uint32_t(6);
		const auto bytes=process.read_memory(args[1],16);
		const auto coordinate=[&](size_t at) {
			uint32_t value=0; for (size_t i=0;i<4;++i) value|=uint32_t(bytes[at+i])<<(i*8);
			return int32_t(value);
		};
		const auto changed=fill_rect(process,objects,args[0],{coordinate(0),coordinate(4),coordinate(8),coordinate(12)},args[2]);
		const auto *dc=objects.dc(args[0]);
		if (changed && dc->window && dc->leased && updated) drew(*dc);
		return true;
	} catch (uint32_t error) { environment.set_last_error(error); return false; }
}

bool NativeDesktop::frame(std::span<const uint64_t> args)
{
	if (!objects.dc(args[0]) || !objects.brush(args[2])) return false;
	const auto bytes=process.read_memory(args[1],16);
	const auto coordinate=[&](size_t offset) {
		uint32_t value=0;for(size_t i=0;i<4;++i)value|=uint32_t(bytes[offset+i])<<(i*8);return int32_t(value);
	};
	const auto left=coordinate(0),top=coordinate(4),right=coordinate(8),bottom=coordinate(12);
	if (right<=left || bottom<=top) return false;
	bool changed=false;
	try {
		for(const DesktopRect rect:std::array<DesktopRect,4>{{{left,top,right,top+1},{left,bottom-1,right,bottom},
			{left,top+1,left+1,bottom-1},{right-1,top+1,right,bottom-1}}})
			changed=fill_rect(process,objects,args[0],rect,args[2]) || changed;
		const auto *dc=objects.dc(args[0]);
		if (changed && dc->window && dc->leased && updated) drew(*dc);
		return true;
	} catch (uint32_t error) { environment.set_last_error(error);return false; }
}

}
