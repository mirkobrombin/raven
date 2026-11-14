#include "api.h"

#include <algorithm>
#include <cstdlib>
#include <new>

namespace raven {

uint8_t combine_raster(uint32_t mode,uint8_t pen,uint8_t destination);

const DesktopPen *DesktopObjects::pen(uint64_t handle) const
{
	if (handle>=desktop_stock_pen && handle<desktop_stock_pen+stock_pens.size()) return &stock_pens[size_t(handle-desktop_stock_pen)];
	const auto found=objects.find(handle);return found==objects.end() ? nullptr : std::get_if<DesktopPen>(&found->second);
}

bool DesktopObjects::pen_selected(uint64_t handle) const
{
	for(const auto &[identity,object]:objects)
		if(const auto *device=std::get_if<DesktopDC>(&object);device && device->pen==handle)return true;
	return false;
}

uint64_t DesktopObjects::select_pen(uint64_t handle,uint64_t selected)
{
	auto *device=dc(handle);if(!device || !pen(selected))return 0;
	const auto previous=device->pen;device->pen=selected;
	if(const auto *old=pen(previous);old && old->pending_delete && !pen_selected(previous))erase(previous);
	return previous;
}

bool DesktopObjects::delete_pen(uint64_t handle)
{
	if(handle>=desktop_stock_pen && handle<desktop_stock_pen+stock_pens.size())return true;
	const auto found=objects.find(handle);if(found==objects.end())return false;
	auto *value=std::get_if<DesktopPen>(&found->second);if(!value)return false;
	if(!pen_selected(handle))return erase(handle);
	value->pending_delete=true;return true;
}

uint8_t combine_raster(uint32_t mode,uint8_t pen,uint8_t destination)
{
	switch(mode){
	case 1: return 0;
	case 2: return uint8_t(~(pen|destination));
	case 3: return uint8_t(~pen&destination);
	case 4: return uint8_t(~pen);
	case 5: return uint8_t(pen&~destination);
	case 6: return uint8_t(~destination);
	case 7: return uint8_t(pen^destination);
	case 8: return uint8_t(~(pen&destination));
	case 9: return uint8_t(pen&destination);
	case 10: return uint8_t(~(pen^destination));
	case 11: return destination;
	case 12: return uint8_t(~pen|destination);
	case 14: return uint8_t(pen|~destination);
	case 15: return uint8_t(pen|destination);
	case 16: return 0xff;
	default: return pen;
	}
}

bool NativeDesktop::pen_dispatch(std::string_view name)
{
	const bool move=name=="MoveToEx",line=name=="LineTo",position=name=="GetCurrentPositionEx";
	const bool extents=name=="GetWindowExtEx" || name=="GetViewportExtEx";
	if(!move && !line && !position && !extents && name!="CreatePen" && name!="GetGraphicsMode" && name!="GdiFlush")return false;
	const auto args=process.api_arguments(name=="GdiFlush" ? 0 : name=="CreatePen" || line ? 3 : move ? 4 : name=="GetGraphicsMode" ? 1 : 2);
	uint64_t result=0;
	try {
		if(name=="GdiFlush")result=1;
		else if(name=="CreatePen"){
			const auto style=uint32_t(args[0]);const auto width=int32_t(args[1]);const auto color=uint32_t(args[2]);
			if((style!=0 && style!=5) || width<0 || width>1 || (color & 0xff000000U))throw uint32_t(120);
			result=objects.add(DesktopPen{color,style==5});if(!result)throw uint32_t(8);
		}else{
			auto *dc=objects.dc(args[0]);if(!dc)throw uint32_t(6);
			const auto point=[&](uint64_t output,int32_t x,int32_t y){
				std::array<uint8_t,8> bytes{};for(size_t i=0;i<4;++i){bytes[i]=uint8_t(uint32_t(x)>>(i*8));bytes[i+4]=uint8_t(uint32_t(y)>>(i*8));}process.write_memory(output,bytes);
			};
			if(name=="GetGraphicsMode")result=1;
			else if(extents){point(args[1],1,1);result=1;}
			else if(position){point(args[1],dc->position_x,dc->position_y);result=1;}
			else if(move){
				if(args[3])point(args[3],dc->position_x,dc->position_y);
				dc->position_x=int32_t(args[1]);dc->position_y=int32_t(args[2]);result=1;
			}else{
				const auto *pen=objects.pen(dc->pen);if(!pen)throw uint32_t(6);
				const auto target_x=int32_t(args[1]),target_y=int32_t(args[2]);
				bool changed=false;
				if(!pen->hollow){
					auto *bitmap=objects.bitmap(dc->bitmap);if(!bitmap || bitmap->depth!=32)throw uint32_t(120);
					auto x=int64_t(dc->position_x),y=int64_t(dc->position_y);
					const auto dx=std::abs(int64_t(target_x)-x),dy=std::abs(int64_t(target_y)-y);
					if(std::max(dx,dy)>1048576)throw uint32_t(120);
					const int64_t sx=x<target_x ? 1 : -1,sy=y<target_y ? 1 : -1;
					auto error=dx-dy;
					const auto pixels=bitmap->pixels();
					while(x!=target_x || y!=target_y){
						const auto px=x+dc->origin_x,py=y+dc->origin_y;
						if(px>=0 && py>=0 && px<bitmap->width && py<bitmap->height && dc->contains(px,py)){
							const auto row=bitmap->top_down ? py : bitmap->height-1-py;const auto at=(size_t(row)*bitmap->width+size_t(px))*4;
							// Pen channels combine with the destination through the DC's binary raster operation.
							const uint8_t source[3]={uint8_t(pen->color>>16),uint8_t(pen->color>>8),uint8_t(pen->color)};
							for(size_t channel=0;channel<3;++channel)pixels[at+channel]=combine_raster(dc->rop2,source[channel],pixels[at+channel]);
							pixels[at+3]=0;changed=true;
						}
						const auto twice=2*error;
						if(twice> -dy || (twice== -dy && sx<0)){error-=dy;x+=sx;}
						if(twice<dx || (twice==dx && sy<0)){error+=dx;y+=sy;}
					}
				}
				dc->position_x=target_x;dc->position_y=target_y;result=1;
				if(changed && dc->window && dc->leased && updated)drew(*dc);
			}
		}
	}catch(uint32_t error){environment.set_last_error(error);}
	catch(const std::bad_alloc &){environment.set_last_error(8);}
	process.return_api(result);return true;
}

}
