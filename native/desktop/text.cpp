#include "api.h"
#include <algorithm>

namespace raven {

static void put(std::span<uint8_t> bytes,size_t at,uint64_t value,size_t count=4)
{
	for (size_t i=0;i<count;++i) bytes[at+i]=uint8_t(value>>(i*8));
}

bool NativeDesktop::text_dispatch(std::string_view name)
{
	const bool output=name=="TextOutW" || name=="ExtTextOutW";
	const bool extent_query=name=="GetTextExtentPoint32W" || name=="GetTextExtentPointW" || name=="GetTextExtentExPointW";
	if (name!="GetTextMetricsW" && !extent_query && !output) return false;
	const auto args=process.api_arguments(name=="ExtTextOutW" ? 8 : name=="TextOutW" ? 5 : name=="GetTextExtentExPointW" ? 7 : extent_query ? 4 : 2);
	uint64_t result=0;
	try {
		if(output){
			if(name=="ExtTextOutW")result=text_output(args);
			else{const std::array<uint64_t,8> extended{args[0],args[1],args[2],0,0,args[3],args[4],0};result=text_output(extended);}
			process.return_api(result);return true;
		}
		auto *dc=objects.dc(args[0]); if (!dc) throw uint32_t(6);
		if(extent_query && !uint32_t(args[2])){
			std::array<uint8_t,8> zero{};const auto output=name=="GetTextExtentExPointW" ? args[6] : args[3];
			process.validate_write_memory(output,8);
			if(name=="GetTextExtentExPointW" && args[4])process.validate_write_memory(args[4],4);
			process.write_memory(output,zero);
			if(name=="GetTextExtentExPointW" && args[4])process.write_memory(args[4],std::span(zero).first(4));
			process.return_api(1);return true;
		}
		{
			const auto *font=objects.font(dc->font); if (!font) throw uint32_t(6);
			if (name=="GetTextMetricsW") {
				const auto info=objects.text().metrics(font->definition); std::array<uint8_t,60> bytes{};
				put(bytes,0,uint32_t(info.height)); put(bytes,4,uint32_t(info.ascent)); put(bytes,8,uint32_t(info.descent));
				put(bytes,12,uint32_t(info.internal_leading)); put(bytes,16,uint32_t(info.external_leading));
				put(bytes,20,uint32_t(info.average_width)); put(bytes,24,uint32_t(info.maximum_width)); put(bytes,28,400);
				put(bytes,36,96); put(bytes,40,96); put(bytes,44,info.first,2); put(bytes,46,info.last,2);
				put(bytes,48,info.default_character,2); put(bytes,50,info.break_character,2); bytes[55]=info.pitch;
				process.write_memory(args[1],bytes); result=1;
			} else {
				const auto count=int32_t(uint32_t(args[2]));
				if (count<0 || count>4096) throw uint32_t(87);
				const auto source=count ? process.read_memory(args[1],size_t(count)*2) : std::vector<uint8_t>{};
				std::vector<uint16_t> text(size_t(count),0);
				for (size_t i=0;i<text.size();++i) text[i]=uint16_t(source[i*2]|(uint16_t(source[i*2+1])<<8));
				const auto extent=objects.text().extent(font->definition,text);
				if (name!="GetTextExtentExPointW") {
					std::array<uint8_t,8> bytes{}; put(bytes,0,uint32_t(extent.width)); put(bytes,4,uint32_t(extent.height));
					process.write_memory(args[3],bytes); result=1;
				}else{
					const auto widths=objects.text().advances(font->definition,text);size_t fit=widths.size();
					if(args[4]){fit=0;while(fit<widths.size() && uint32_t(widths[fit])<=uint32_t(args[3]))++fit;}
					process.validate_write_memory(args[6],8);if(args[4])process.validate_write_memory(args[4],4);
					if(args[5] && !widths.empty())process.validate_write_memory(args[5],widths.size()*4);
					std::array<uint8_t,8> bytes{};put(bytes,0,uint32_t(extent.width));put(bytes,4,uint32_t(extent.height));process.write_memory(args[6],bytes);
					if(args[4]){put(bytes,0,fit);process.write_memory(args[4],std::span(bytes).first(4));}
					if(args[5] && !widths.empty()){std::vector<uint8_t> output(widths.size()*4);for(size_t i=0;i<widths.size();++i)put(output,i*4,uint32_t(widths[i]));process.write_memory(args[5],output);}
					result=1;
				}
			}
		}
	} catch (uint32_t error) { environment.set_last_error(error); result=0; }
	catch (const std::bad_alloc &) { environment.set_last_error(8); result=0; }
	process.return_api(result); return true;
}

uint64_t NativeDesktop::text_output(std::span<const uint64_t> args)
{
	auto *dc=objects.dc(args[0]);if(!dc)throw uint32_t(6);
	const auto options=uint32_t(args[3]),count=uint32_t(args[6]);
	if(options & ~6U)throw uint32_t(120);
	if(count>4096)throw uint32_t(87);
	std::optional<DesktopRect> rect;
	if(args[4] && options){
		const auto bytes=process.read_memory(args[4],16);std::array<int32_t,4> fields{};
		for(size_t i=0;i<4;++i){uint32_t value=0;for(size_t b=0;b<4;++b)value|=uint32_t(bytes[i*4+b])<<(b*8);fields[i]=int32_t(value);}
		rect=DesktopRect{std::min(fields[0],fields[2]),std::min(fields[1],fields[3]),std::max(fields[0],fields[2]),std::max(fields[1],fields[3])};
	}
	if(!count && (!(options & 2) || !rect))return 1;
	auto *bitmap=objects.bitmap(dc->bitmap);if(!bitmap || bitmap->depth!=32)throw uint32_t(120);
	const auto x=int64_t(int32_t(args[1]))+dc->origin_x,y=int64_t(int32_t(args[2]))+dc->origin_y;
	std::vector<uint8_t> mask(size_t(bitmap->width)*bitmap->height);
	int64_t advance=0;int32_t height=0;
	if(count){
		const auto *font=objects.font(dc->font);if(!font)throw uint32_t(6);
		const auto bytes=process.read_memory(args[5],size_t(count)*2);std::vector<uint16_t> text(count);
		for(size_t i=0;i<text.size();++i)text[i]=uint16_t(bytes[i*2]|(uint16_t(bytes[i*2+1])<<8));
		const auto extent=objects.text().extent(font->definition,text);advance=extent.width;height=extent.height;
		std::vector<std::array<int32_t,2>> positions;
		if(args[7]){
			const auto steps=process.read_memory(args[7],size_t(count)*4);positions.reserve(count);advance=0;
			for(size_t i=0;i<count;++i){
				if(advance<INT32_MIN || advance>INT32_MAX)throw uint32_t(120);
				positions.push_back({int32_t(advance),0});uint32_t value=0;
				for(size_t b=0;b<4;++b)value|=uint32_t(steps[i*4+b])<<(b*8);
				advance+=int32_t(value);
			}
			if(advance<INT32_MIN || advance>INT32_MAX)throw uint32_t(120);
		}
		if(x>=INT32_MIN && x<=INT32_MAX && y>=INT32_MIN && y<=INT32_MAX)
			mask=objects.text().mask(font->definition,text,bitmap->width,bitmap->height,int32_t(x),int32_t(y),positions);
	}
	const auto pixels=bitmap->pixels();
	bool changed=false;
	for(uint32_t row=0;row<bitmap->height;++row)for(uint32_t column=0;column<bitmap->width;++column){
		if(!dc->contains(column,row))continue;
		const auto logical_x=int64_t(column)-dc->origin_x,logical_y=int64_t(row)-dc->origin_y;
		const bool inside=rect && logical_x>=rect->left && logical_x<rect->right && logical_y>=rect->top && logical_y<rect->bottom;
		if((options & 4) && rect && !inside)continue;
		const bool ink=mask[size_t(row)*bitmap->width+column]!=0;
		const bool background=((options & 2) && inside) || (count && dc->background_mode==2 &&
			int64_t(column)>=std::min(x,x+advance) && int64_t(column)<std::max(x,x+advance) && int64_t(row)>=y && int64_t(row)<y+height);
		if(!ink && !background)continue;
		const auto color=ink ? dc->text_color : dc->background_color;
		const auto at=(size_t(bitmap->top_down ? row : bitmap->height-1-row)*bitmap->width+column)*4;
		pixels[at]=uint8_t(color>>16);pixels[at+1]=uint8_t(color>>8);pixels[at+2]=uint8_t(color);pixels[at+3]=0;changed=true;
	}
	if(changed){
		if(dc->window && dc->leased && updated)drew(*dc);
	}
	return 1;
}

}
