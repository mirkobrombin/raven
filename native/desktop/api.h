#pragma once

#include "objects.h"
#include "nt/api.h"
#include "nt/environment.h"
#include "nt/resources.h"
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef RAVEN_SKIA
#include "graphics/text.h"
#endif

namespace raven {

class NativeDesktop {
public:
	NativeDesktop(NativeProcess &process, NativeEnvironment &environment, DesktopObjects &objects, NativeResources &resources);
	bool dispatch(std::string_view name);
	// GDI entry points that follow the shared registry conventions; they take precedence over the older dispatchers.
	void register_gdi_api(ApiRegistry &registry);
	void register_text_api(ApiRegistry &registry);
	void register_image_api(ApiRegistry &registry);
	void set_host_files(std::function<std::optional<std::vector<uint8_t>>(std::u16string_view)> read) { read_host_file=std::move(read); }
	static std::span<const std::string_view> user_exports();
	static std::span<const std::string_view> gdi_exports();
	static std::span<const std::string_view> control_exports();
	// Loads an RT_MENU resource of a module by integer identifier; zero when the resource does not exist.
	uint64_t menu_resource(uint64_t instance,uint64_t identifier);
	void set_updated(std::function<void(uint64_t)> notify) { updated=std::move(notify); }
	// Lets GDI services call guest callbacks such as font enumeration procedures.
	void set_call(std::function<NativeResult(uint64_t,std::span<const uint64_t>)> call) { invoke=std::move(call); }
	// Module whose resources answer LoadBitmap/LoadImage requests that name no module (the OEM bitmaps).
	void set_oem_module(uint64_t base) { oem_module=base; }
	// The descriptor and size of the section a program created, for DIB sections that live in one.
	void set_section_source(std::function<std::optional<std::pair<int, uint64_t>>(uint64_t)> find) { section_source=std::move(find); }
	void set_source(std::function<std::optional<ModuleBytes>(std::string_view)> read) { source=std::move(read); }
	// Carries out a release the guest already answered (see NativeWindows::take_deferred): 1 DestroyIcon, 2 DeleteDC, 3 DeleteObject.
	void apply_deferred(uint32_t operation, uint64_t handle);

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	DesktopObjects &objects;
	NativeResources &resources;
	std::function<void(uint64_t)> updated;
	std::function<NativeResult(uint64_t,std::span<const uint64_t>)> invoke;
	std::function<std::optional<std::vector<uint8_t>>(std::u16string_view)> read_host_file;
	uint64_t oem_module=0;
	std::function<std::optional<std::pair<int, uint64_t>>(uint64_t)> section_source;
	std::function<std::optional<ModuleBytes>(std::string_view)> source;
	// What ScriptShape gave each glyph of a logical font, keyed by its definition: the character behind each of the face's glyphs, and
	// the characters the face lacks, named by identifiers from the face's glyph count up so that ScriptPlace and ScriptTextOut measure
	// and draw them by character, through the fallback faces.
	struct ScriptGlyphs {
		uint32_t base = 0;
		std::vector<std::u16string> missing;
		std::unordered_map<uint16_t, std::u16string> characters;
	};
	std::map<std::string, ScriptGlyphs> script_glyphs;
#ifdef RAVEN_SKIA
	bool text_dispatch(std::string_view name);
	uint64_t text_output(std::span<const uint64_t> arguments);
#endif
	uint64_t load_icon(std::span<const uint64_t> arguments);
	uint64_t load_string(std::span<const uint64_t> arguments);
	bool menu_dispatch(std::string_view name);
	bool color_dispatch(std::string_view name);
	bool clip_dispatch(std::string_view name);
	bool pen_dispatch(std::string_view name);
	bool caps_dispatch(std::string_view name);
	bool load_dispatch(std::string_view name);
	uint64_t scratch=0;
	uint64_t lookup(std::span<const uint64_t> arguments);
	uint64_t create(std::span<const uint64_t> arguments);
	uint64_t create_icon(std::span<const uint8_t> bytes,uint32_t width,uint32_t height,uint32_t flags);
	bool extract_dispatch(std::string_view name);
	bool info(uint64_t handle, uint64_t output);
	uint64_t get_object(std::span<const uint64_t> arguments, bool ansi);
	uint64_t create_font(uint64_t definition);
	uint64_t get_bits(std::span<const uint64_t> arguments);
	uint64_t create_dc(uint64_t source);
	bool delete_dc(uint64_t handle);
	uint64_t select(uint64_t dc, uint64_t bitmap);
	uint64_t create_section(std::span<const uint64_t> arguments);
	uint64_t compatible_bitmap(std::span<const uint64_t> arguments);
	uint64_t allocate_bitmap(uint32_t width,uint32_t height,uint32_t depth,bool section,bool top_down);
	uint64_t allocate_dib(uint32_t width,uint32_t height,uint32_t bits,bool top_down,std::vector<uint32_t> palette,uint64_t section=0,uint64_t section_offset=0);
	bool delete_bitmap(uint64_t handle);
	bool draw(std::span<const uint64_t> arguments);
	bool fill(std::span<const uint64_t> arguments);
	bool frame(std::span<const uint64_t> arguments);
};

// Stretches an icon's color and masks to a new size, as loading one at another size and drawing it stretched do.
void resize_icon(IconBitmap &image, uint32_t width, uint32_t height, bool monochrome);

}
