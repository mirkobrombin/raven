#include "nt/providers.h"
#include <xkbcommon/xkbcommon.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and modifier fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate); check(bool(file) && file.tellg()>0,"cannot read modifier fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short modifier fixture");
		auto *context=xkb_context_new(XKB_CONTEXT_NO_FLAGS); check(context,"cannot create keyboard context");
		xkb_rule_names names{}; names.layout="us";
		auto *map=xkb_keymap_new_from_names(context,&names,XKB_KEYMAP_COMPILE_NO_FLAGS); check(map,"cannot create keyboard map");
		auto *text=xkb_keymap_get_as_string(map,XKB_KEYMAP_FORMAT_TEXT_V1); check(text,"cannot serialize keyboard map");
		const std::string keymap(text); std::free(text); xkb_keymap_unref(map); xkb_context_unref(context);
		raven::ModuleSet modules; auto &entry=modules.add("modifiers.exe",bytes);
		uint64_t window=0;
		bool typed=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { if (!window) window=handle; };
		presentation.remove=[](uint64_t) {};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!window || typed) return events;
			typed=true;
			raven::DesktopEvent layout{0,0}; layout.kind=raven::DesktopEventKind::keymap; layout.keymap=keymap;
			events.push_back(std::move(layout));
			events.push_back({window,0,0,0,raven::DesktopEventKind::keyboard_focus});
			// Evdev codes: 29 left CONTROL, 100 right ALT, 56 left ALT, 16 Q.
			for (const auto [code,down] : std::initializer_list<std::pair<uint32_t,uint64_t>>{{29,1},{100,1},{16,1},{16,0},{100,0},{29,0},{56,1},{16,1},{16,0},{56,0}})
				events.push_back({window,code,down,0,raven::DesktopEventKind::key});
			events.push_back({window,0x10});
			return events;
		};
		const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value) std::cerr<<"fixture answered "<<result.value<<'\n';
		check(result.stop==raven::NativeStop::returned && !result.value && typed,"modifier keys differ");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
