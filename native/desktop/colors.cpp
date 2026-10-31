#include "api.h"

#include <new>

namespace raven {

uint32_t DesktopObjects::system_color(int32_t index) const
{
	return index>=0 && size_t(index)<system_brushes.size() ? system_brushes[size_t(index)].color : 0;
}

uint64_t DesktopObjects::system_brush(int32_t index) const
{
	return index>=0 && size_t(index)<system_brushes.size() ? desktop_system_brush+uint32_t(index) : 0;
}

bool NativeDesktop::color_dispatch(std::string_view name)
{
	const bool system=name=="GetSysColor" || name=="GetSysColorBrush";
	const bool query=name=="GetTextColor" || name=="GetBkColor" || name=="GetBkMode";
	const bool color=name=="SetTextColor" || name=="SetBkColor";
	if (!system && !query && !color && name!="SetBkMode" && name!="GetObjectType") return false;
	const auto args=process.api_arguments(system || query || name=="GetObjectType" ? 1 : 2);
	uint64_t result=0;
	try {
		if (system) result=name=="GetSysColor" ? objects.system_color(int32_t(uint32_t(args[0]))) : objects.system_brush(int32_t(uint32_t(args[0])));
		else if (name=="GetObjectType") {
			if (const auto *dc=objects.dc(args[0])) result=dc->window ? 3 : 10;
			else if (objects.brush(args[0])) result=2;
			else if (objects.font(args[0])) result=6;
			else if (objects.bitmap(args[0])) result=7;
			else if (objects.region(args[0])) result=8;
			else if (objects.pen(args[0])) result=1;
			else if (objects.palette(args[0])) result=5;
			else throw uint32_t(6);
		} else {
			auto *dc=objects.dc(args[0]);if (!dc) throw uint32_t(6);
			if (query) result=name=="GetTextColor" ? dc->text_color : name=="GetBkColor" ? dc->background_color : dc->background_mode;
			else if (color) {
				const auto value=uint32_t(args[1]);if (value & 0xff000000U) throw uint32_t(120);
				auto &setting=name=="SetBkColor" ? dc->background_color : dc->text_color;
				result=setting;setting=value;
			} else {
				const auto value=uint32_t(args[1]);if (value!=1 && value!=2) throw uint32_t(87);
				result=dc->background_mode;dc->background_mode=value;
			}
		}
	} catch (uint32_t error) {
		environment.set_last_error(error);
		result=color || name=="GetTextColor" || name=="GetBkColor" ? UINT32_MAX : 0;
	}
	process.return_api(result);return true;
}

}
