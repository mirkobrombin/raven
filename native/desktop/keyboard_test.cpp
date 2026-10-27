#include "keyboard.h"
#include <xkbcommon/xkbcommon.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main()
{
	try {
		auto *context=xkb_context_new(XKB_CONTEXT_NO_FLAGS); check(context,"cannot create reference XKB context");
		for (const auto *layout : {"us","de"}) {
			xkb_rule_names names{}; names.layout=layout;
			auto *map=xkb_keymap_new_from_names(context,&names,XKB_KEYMAP_COMPILE_NO_FLAGS); check(map,"cannot build reference layout");
			auto *text=xkb_keymap_get_as_string(map,XKB_KEYMAP_FORMAT_TEXT_V1); check(text,"cannot serialize reference layout");
			raven::KeyboardMap keyboard; keyboard.load(text); std::free(text);
			const uint32_t events[]{125,126,127,69,70},virtual_keys[]{0x5b,0x5c,0x5d,0x90,0x91},scans[]{91,92,93,69,70};
			for (size_t i=0;i<5;++i) {
				const auto key=keyboard.decode(events[i]);
				check(key.code==virtual_keys[i] && key.scan==scans[i] && !key.character && key.extended==(i<3),"system key translation differs");
			}
			// Keys the layout gives no symbol (the Japanese Ro key, a vendor key) pass with no virtual key instead of failing.
			const auto ro=keyboard.decode(89),vendor=keyboard.decode(141);
			check(!ro.code && ro.scan==89 && !ro.character && !vendor.code && !vendor.scan && !vendor.character,"key without a symbol differs");
			const auto g=keyboard.decode(34),y=keyboard.decode(21);
			check(g.code==71 && g.scan==34 && g.character=='g' && !g.extended,"G keyboard contract differs");
			check(y.code==(layout[0]=='u' ? 89U : 90U) && y.character==(layout[0]=='u' ? 'y' : 'z'),"layout-dependent virtual key differs");
			const auto shift=xkb_keymap_mod_get_index(map,XKB_MOD_NAME_SHIFT),control=xkb_keymap_mod_get_index(map,XKB_MOD_NAME_CTRL);
			check(shift<32 && control<32,"reference modifier index exceeds ABI");
			keyboard.modifiers(1U<<shift,0,0,0);
			check(keyboard.decode(34).character=='G' && keyboard.mouse_modifiers()==4,"shift translation differs");
			keyboard.modifiers(1U<<control,0,0,0);
			check(keyboard.decode(34).character==7 && keyboard.mouse_modifiers()==8,"control translation differs");
			keyboard.modifiers(0,0,0,0);
			bool rejected=false;
			try { keyboard.load("invalid keymap"); } catch (const std::runtime_error &) { rejected=true; }
			check(rejected && keyboard.decode(34).character=='g',"failed keymap reload damaged state");
			rejected=false;
			try { keyboard.decode(999); } catch (const std::runtime_error &) { rejected=true; }
			check(rejected,"out-of-range keyboard code succeeded");
			xkb_keymap_unref(map);
		}
		xkb_context_unref(context);
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
