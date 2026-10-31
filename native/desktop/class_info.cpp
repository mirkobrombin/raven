#include "windows.h"
#include <algorithm>

namespace raven {

static void put(std::span<uint8_t> bytes,size_t at,uint64_t value,size_t width)
{
	for (size_t i=0;i<width;++i) bytes[at+i]=uint8_t(value>>(i*8));
}

uint64_t NativeWindows::class_info(std::string_view name,std::span<const uint64_t> args)
{
	if (name=="GetClassNameW" || name=="GetClassNameA") {
		const auto *window=objects.window(args[0]); if (!window) throw uint32_t(1400);
		const auto count=uint32_t(args[2]); if (count<=1) throw uint32_t(122);
		std::string_view value=objects.window_class(window->class_id)->name;
		// A class registered inside an activation context keeps its plain name for the program: the version before '!' stays internal.
		if (const auto mark=value.find('!'); mark!=value.npos && value.find_first_not_of("0123456789.")==mark) value.remove_prefix(mark+1);
		const auto length=std::min(size_t(count-1),value.size()),width=name=="GetClassNameW" ? size_t(2) : size_t(1);
		std::vector<uint8_t> bytes((length+1)*width);
		for (size_t i=0;i<length;++i) bytes[i*width]=uint8_t(value[i]);
		process.write_memory(args[1],bytes); return length;
	}
	return fill_class_info(atom(args[1],args[0]),name=="GetClassInfoExW",args);
}

uint64_t NativeWindows::fill_class_info(uint16_t id,bool extended,std::span<const uint64_t> args)
{
	const auto *definition=objects.window_class(id); if (!definition) throw uint32_t(1411);
	const bool narrow=process.mode()==NativeMode::x86;
	const size_t P=narrow ? 4 : 8,shift=narrow && extended ? 4 : 0;
	const auto old=process.read_memory(args[2],narrow ? (extended ? 48 : 40) : (extended ? 80 : 72));
	std::vector<uint8_t> bytes(old.size());
	if (extended) std::copy_n(old.begin(),4,bytes.begin());
	put(bytes,extended ? 4 : 0,definition->style,4);
	put(bytes,P+shift,definition->procedure,P);
	put(bytes,2*P+shift,definition->extra.size(),4); put(bytes,2*P+4+shift,definition->window_extra,4);
	put(bytes,2*P+8+shift,args[0],P); put(bytes,5*P+8+shift,definition->background,P); put(bytes,7*P+8+shift,args[1],P);
	process.write_memory(args[2],bytes);
	return definition->atom;
}

}
