#include "api.h"

#include <algorithm>
#include <array>
#include <new>

namespace raven {

struct ClipError { uint32_t error=0; };

const DesktopRegion *DesktopObjects::region(uint64_t handle) const
{
	const auto found=objects.find(handle);
	return found==objects.end() ? nullptr : std::get_if<DesktopRegion>(&found->second);
}

bool DesktopObjects::replace_region(uint64_t handle,DesktopRegion value)
{
	const auto found=objects.find(handle);
	if (found==objects.end() || !std::holds_alternative<DesktopRegion>(found->second)) return false;
	const auto previous=std::get<DesktopRegion>(found->second).bytes(),current=value.bytes();
	if (current>limit-used+previous) return false;
	std::get<DesktopRegion>(found->second)=std::move(value);used=used-previous+current;
	regions_touched.touch(handle);
	return true;
}

bool DesktopObjects::select_clip(uint64_t handle,std::optional<DesktopRegion> value)
{
	auto *device=dc(handle);if (!device) return false;
	const auto previous=device->application_clip ? device->application_clip->bytes() : 0,current=value ? value->bytes() : 0;
	if (current>limit-used+previous) return false;
	device->application_clip=std::move(value);used=used-previous+current;return true;
}

static DesktopRegion rectangle(int32_t left,int32_t top,int32_t right,int32_t bottom)
{
	for (const auto coordinate:{left,top,right,bottom})
		if (coordinate< -0x4000000 || coordinate>0x3ffffff) throw uint32_t(120);
	DesktopRegion result;
	result.add({std::min(left,right),std::min(top,bottom),std::max(left,right),std::max(top,bottom)});
	return result;
}

static DesktopRegion visibility(DesktopObjects &objects,const DesktopDC &dc)
{
	DesktopRegion result;const auto *bitmap=objects.bitmap(dc.bitmap);
	if (!bitmap) return result;
	const auto coordinate=[](int64_t value) { return int32_t(std::clamp(value,int64_t(INT32_MIN),int64_t(INT32_MAX))); };
	result.add({coordinate(-int64_t(dc.origin_x)),coordinate(-int64_t(dc.origin_y)),
		coordinate(int64_t(bitmap->width)-dc.origin_x),coordinate(int64_t(bitmap->height)-dc.origin_y)});
	if (dc.bounds) {DesktopRegion bound;bound.add(*dc.bounds);result.combine(result,bound,1);}
	return result;
}

bool NativeDesktop::clip_dispatch(std::string_view name)
{
	const bool rect=name=="CreateRectRgn" || name=="SetRectRgn" || name=="IntersectClipRect" || name=="ExcludeClipRect";
	const bool points=name=="DPtoLP" || name=="LPtoDP";
	const bool query=name=="GetRgnBox" || name=="GetClipBox" || name=="GetClipRgn" || name=="GetLayout";
	if (!rect && !points && !query && name!="CombineRgn" && name!="EqualRgn" && name!="OffsetRgn" &&
		name!="PtInRegion" && name!="RectInRegion" && name!="SelectClipRgn" && name!="ExtSelectClipRgn" && name!="GetRegionData") return false;
#ifndef RAVEN_PIXMAN
	if (!points && name!="GetLayout") {
		environment.set_last_error(120);process.return_api(name=="GetClipRgn" ? UINT32_MAX : 0);return true;
	}
#endif
	const auto args=process.api_arguments(name=="GetLayout" ? 1 : rect ? (name=="CreateRectRgn" ? 4 : 5) :
		name=="CombineRgn" ? 4 : points || name=="OffsetRgn" || name=="PtInRegion" || name=="ExtSelectClipRgn" || name=="GetRegionData" ? 3 : 2);
	uint64_t result=0;
	try {
		const auto put_rect=[&](uint64_t output,DesktopRect box) {
			std::array<uint8_t,16> bytes{};
			const int32_t fields[]{box.left,box.top,box.right,box.bottom};
			for(size_t i=0;i<4;++i)for(size_t b=0;b<4;++b)bytes[i*4+b]=uint8_t(uint32_t(fields[i])>>(b*8));
			process.write_memory(output,bytes);
		};
		const auto read_rect=[&](uint64_t input) {
			const auto bytes=process.read_memory(input,16);std::array<int32_t,4> fields{};
			for(size_t i=0;i<4;++i){uint32_t value=0;for(size_t b=0;b<4;++b)value|=uint32_t(bytes[i*4+b])<<(b*8);fields[i]=int32_t(value);}
			return fields;
		};
		const auto required_region=[&](uint64_t handle) -> const DesktopRegion & {
			const auto *value=objects.region(handle);if (!value) throw ClipError{};return *value;
		};
		if (name=="CreateRectRgn" || name=="SetRectRgn") {
			const auto first=name=="CreateRectRgn" ? 0U : 1U;
			if (first) required_region(args[0]);
			auto value=rectangle(int32_t(args[first]),int32_t(args[first+1]),int32_t(args[first+2]),int32_t(args[first+3]));
			if (first) result=objects.replace_region(args[0],std::move(value));
			else result=objects.add(std::move(value));
			if (!result) throw uint32_t(8);
		} else if (name=="CombineRgn") {
			required_region(args[0]);const auto &first=required_region(args[1]);
			const auto mode=uint32_t(args[3]);if(mode<1 || mode>5)throw ClipError{};
			DesktopRegion empty,value;value.combine(first,mode==5 ? empty : required_region(args[2]),mode);
			result=value.complexity();if (!objects.replace_region(args[0],std::move(value))) throw uint32_t(8);
		} else if (name=="OffsetRgn") {
			auto value=required_region(args[0]);const auto box=value.bounds();
			const auto x=int32_t(args[1]),y=int32_t(args[2]);
			if (!value.empty() && (int64_t(box.left)+x< -0x4000000 || int64_t(box.right)+x>0x3ffffff ||
				int64_t(box.top)+y< -0x4000000 || int64_t(box.bottom)+y>0x3ffffff)) throw uint32_t(120);
			value.offset(x,y);result=value.complexity();if (!objects.replace_region(args[0],std::move(value))) throw uint32_t(8);
		} else if (name=="EqualRgn") result=required_region(args[0]).equals(required_region(args[1]));
		else if (name=="PtInRegion") result=required_region(args[0]).contains(int32_t(args[1]),int32_t(args[2]));
		else if (name=="RectInRegion") {
			const auto &source=required_region(args[0]);const auto box=read_rect(args[1]);
			auto value=rectangle(box[0],box[1],box[2],box[3]);value.combine(value,source,1);result=!value.empty();
		} else if (name=="GetRegionData") {
			// RGNDATA: the header (its size, RDH_RECTANGLES, the count, the rectangles' size, the bounds), then the rectangles. Without a
			// buffer the size needed; a buffer too small gets nothing.
			const auto &value=required_region(args[0]);const auto rectangles=value.rectangles();
			const uint64_t size=32+16*uint64_t(rectangles.size());
			if (!args[2]) result=size;
			else if (uint32_t(args[1])<size) throw uint32_t(234);
			else {
				std::vector<uint8_t> bytes(size);
				const auto put=[&](size_t at,uint32_t word){for(size_t b=0;b<4;++b)bytes[at+b]=uint8_t(word>>(b*8));};
				const auto box=value.empty() ? DesktopRect{0,0,0,0} : value.bounds();
				put(0,32);put(4,1);put(8,uint32_t(rectangles.size()));put(12,uint32_t(16*rectangles.size()));
				put(16,uint32_t(box.left));put(20,uint32_t(box.top));put(24,uint32_t(box.right));put(28,uint32_t(box.bottom));
				for(size_t i=0;i<rectangles.size();++i){
					put(32+i*16,uint32_t(rectangles[i].left));put(36+i*16,uint32_t(rectangles[i].top));
					put(40+i*16,uint32_t(rectangles[i].right));put(44+i*16,uint32_t(rectangles[i].bottom));
				}
				process.write_memory(args[2],bytes);result=size;
			}
		} else if (name=="GetRgnBox") {
			const auto &value=required_region(args[0]);put_rect(args[1],value.bounds());result=value.complexity();
		} else {
			auto *device=objects.dc(args[0]);if (!device) throw ClipError{name=="GetLayout" ? 6U : 0U};
			if (name=="GetLayout") result=0;
			else if (points) {
				const auto count=int32_t(args[2]);if (count>65536) throw uint32_t(120);
				if (count>0) {const auto bytes=process.read_memory(args[1],size_t(count)*8);process.write_memory(args[1],bytes);}
				result=1;
			} else if (name=="GetClipRgn") {
				if (device->application_clip) {
					required_region(args[1]);
					if (!objects.replace_region(args[1],*device->application_clip)) throw uint32_t(8);
					result=1;
				}
			} else if (name=="GetClipBox") {
				const auto visible=visibility(objects,*device);auto box=visible.bounds();result=visible.complexity();
				auto combined=device->application_clip;
				if (device->clip) {
					if (combined) combined->combine(*combined,*device->clip,1);
					else combined=*device->clip;
				}
				if (combined) {
					const auto clip=combined->bounds();
					box={std::max(box.left,clip.left),std::max(box.top,clip.top),std::min(box.right,clip.right),std::min(box.bottom,clip.bottom)};
					result=box.right<=box.left || box.bottom<=box.top ? 1 : combined->complexity();
				}
				put_rect(args[1],box);
			} else if (name=="ExtSelectClipRgn") {
				// RGN_AND, RGN_OR, RGN_XOR and RGN_DIFF combine the region with the current clipping, the whole visible surface when
				// there is none; RGN_COPY selects it as SelectClipRgn does, and no region with RGN_COPY removes the clipping.
				const auto mode=uint32_t(args[2]);
				if (mode<1 || mode>5 || (!args[1] && mode!=5)) throw uint32_t(87);
				std::optional<DesktopRegion> value;
				if (args[1]) {
					const auto &region=required_region(args[1]);
					if (mode==5) value=region;
					else {
						DesktopRegion next;
						next.combine(device->application_clip ? *device->application_clip : visibility(objects,*device),region,mode);
						value=std::move(next);
					}
				}
				result=value ? value->complexity() : 2;
				if (!objects.select_clip(args[0],std::move(value))) throw uint32_t(8);
			} else if (name=="SelectClipRgn") {
				std::optional<DesktopRegion> value;
				if (args[1]) {
					const auto *region=objects.region(args[1]);
					if (!region) {
						if (!device->application_clip && !objects.select_clip(args[0],visibility(objects,*device))) throw uint32_t(8);
						throw ClipError{};
					}
					value=*region;
				}
				result=value ? value->complexity() : 2;
				if (!objects.select_clip(args[0],std::move(value))) throw uint32_t(8);
			} else {
				auto value=rectangle(int32_t(args[1]),int32_t(args[2]),int32_t(args[3]),int32_t(args[4]));
				if (device->application_clip) {
					DesktopRegion next;next.combine(*device->application_clip,value,name=="IntersectClipRect" ? 1 : 4);value=std::move(next);
				} else if (name=="ExcludeClipRect") {
					DesktopRegion next;next.combine(visibility(objects,*device),value,4);value=std::move(next);
				}
				result=value.complexity();if (!objects.select_clip(args[0],std::move(value))) throw uint32_t(8);
			}
		}
	} catch (const ClipError &error) {if(error.error)environment.set_last_error(error.error);result=name=="GetClipRgn" || name=="GetLayout" ? UINT32_MAX : 0;}
	catch (uint32_t error) {environment.set_last_error(error);result=name=="GetClipRgn" || name=="GetLayout" ? UINT32_MAX : 0;}
	catch (const std::bad_alloc &) {environment.set_last_error(8);result=name=="GetClipRgn" || name=="GetLayout" ? UINT32_MAX : 0;}
	process.return_api(result);return true;
}

}
