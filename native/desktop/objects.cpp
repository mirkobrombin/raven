#include "objects.h"
#include "foreign_surface.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace raven {

static size_t object_size(const DesktopObjects::Object &object)
{
	if (const auto *menu=std::get_if<DesktopMenu>(&object)) {
		size_t size=sizeof(DesktopMenu)+menu->items.size()*sizeof(DesktopMenuItem);
		for (const auto &item:menu->items) size+=item.text.size()*2;
		return size;
	}
	if (const auto *icon = std::get_if<DesktopIcon>(&object))
		return icon->image.color.rgba.size() + icon->image.and_mask.size() + icon->image.xor_mask.size();
	if (const auto *bitmap = std::get_if<DesktopBitmap>(&object))
		return bitmap->address ? (size_t(bitmap->width) * bitmap->height * 4 + 4095) & ~size_t(4095) : bitmap->bits.size();
	if (const auto *window = std::get_if<DesktopWindow>(&object))
		return sizeof(DesktopWindow)+window->title.size()*2+window->update.bytes()+window->extra.size();
	if (std::holds_alternative<DesktopFont>(object))
		return sizeof(DesktopFont);
	if (std::holds_alternative<DesktopBrush>(object)) return sizeof(DesktopBrush);
	if (std::holds_alternative<DesktopPen>(object)) return sizeof(DesktopPen);
	if (const auto *region=std::get_if<DesktopRegion>(&object)) return sizeof(DesktopRegion)+region->bytes();
	const auto &dc=std::get<DesktopDC>(object);
	return sizeof(DesktopDC)+(dc.clip ? dc.clip->bytes() : 0)+(dc.application_clip ? dc.application_clip->bytes() : 0);
}

DesktopObjects::DesktopObjects(size_t limit, IconDisplay display,DesktopMetrics metrics,DesktopColors colors) : limit(limit), icon_display(display), window_metrics(metrics)
{
	if (!display.width || !display.height || !display.depth)
		throw std::runtime_error("invalid icon display state");
	for (const int index:{5,6,7,8,32,33,4,51,15,30,31,52,53})
		if (metrics.get(index)<0 || metrics.get(index)>8192) throw std::runtime_error("invalid nonclient metric");
	if (metrics.frame_x<metrics.dialog_x || metrics.frame_y<metrics.dialog_y)
		throw std::runtime_error("frame metric is smaller than its dialog border");
	for (size_t i=0;i<colors.size();++i) {
		if (colors[i] & 0xff000000U) throw std::runtime_error("invalid system color profile");
		system_brushes[i].color=colors[i];
	}
	system_font.definition[0]=16;
	system_font.definition[4]=7;
	system_font.definition[16]=0xbc; system_font.definition[17]=2;
	system_font.definition[27]=34;
	const std::string_view name="System";
	for (size_t i=0;i<name.size();++i) system_font.definition[28+i*2]=uint8_t(name[i]);
	// DEFAULT_GUI_FONT is the 8 point dialog face; the fixed stock font is a 16 pixel monospace cell.
	const auto font_face=[](DesktopFont &font,const char *face,int32_t height,int32_t weight,uint8_t pitch) {
		for (auto &byte:font.definition) byte=0;
		for (size_t i=0;i<4;++i) font.definition[i]=uint8_t(uint32_t(height)>>(i*8));
		font.definition[16]=uint8_t(weight); font.definition[17]=uint8_t(weight>>8);
		font.definition[23]=1; font.definition[27]=pitch;
		for (size_t i=0;face[i];++i) font.definition[28+i*2]=uint8_t(face[i]);
	};
	font_face(gui_font,"MS Shell Dlg",-11,400,0x22);
	font_face(fixed_font,"Courier New",-16,400,0x31);
}

const DesktopMetrics &DesktopObjects::nonclient_metrics() const { return window_metrics; }

bool DesktopObjects::set_screen_size(int32_t width,int32_t height)
{
	if (width<=0 || height<=0 || width>8192 || height>8192) throw std::runtime_error("invalid screen dimensions");
	if (window_metrics.screen_x==width && window_metrics.screen_y==height) return false;
	window_metrics.screen_x=width;
	window_metrics.screen_y=height;
	return true;
}

const DesktopMenu *DesktopObjects::menu(uint64_t handle) const
{
	const auto found=objects.find(handle);
	return found==objects.end() ? nullptr : std::get_if<DesktopMenu>(&found->second);
}

DesktopMenu *DesktopObjects::edit_menu(uint64_t handle)
{
	const auto found=objects.find(handle);
	auto *record=found==objects.end() ? nullptr : std::get_if<DesktopMenu>(&found->second);
	if (record) menus_touched.touch(handle);
	return record;
}

std::vector<uint64_t> DesktopObjects::accelerator_handles() const
{
	std::vector<uint64_t> result;
	for (const auto &[handle,table] : accelerator_tables) result.push_back(handle);
	return result;
}

std::vector<uint64_t> DesktopObjects::bitmap_handles() const
{
	std::vector<uint64_t> result;
	for (const auto &[handle,object] : objects)
		if (std::holds_alternative<DesktopBitmap>(object)) result.push_back(handle);
	return result;
}

std::vector<uint64_t> DesktopObjects::menu_handles() const
{
	std::vector<uint64_t> result;
	for (const auto &[handle,object] : objects)
		if (std::holds_alternative<DesktopMenu>(object)) result.push_back(handle);
	return result;
}

uint64_t DesktopObjects::load_menu(const MenuTemplate &definition)
{
	const auto load=[&](auto &&self,const std::vector<MenuTemplateItem> &items,uint32_t help,unsigned depth)->uint64_t {
		if (depth>64 || items.size()>4096) throw std::runtime_error("menu owner limit exceeded");
		DesktopMenu record{{},help};
		std::vector<uint64_t> children;
		try {
			for (const auto &item:items) {
				uint64_t child=0;
				if (item.submenu_help) {
					child=self(self,item.children,*item.submenu_help,depth+1);
					if (!child) throw uint32_t(8);
					try { children.push_back(child); } catch (...) { erase(child); throw; }
				}
				record.items.push_back({item.type,item.state,item.id,item.text,child});
			}
			const auto handle=add(std::move(record));
			if (!handle) throw uint32_t(8);
			return handle;
		} catch (...) {
			for (const auto child:children) erase(child);
			throw;
		}
	};
	return load(load,definition.items,definition.help,0);
}

uint64_t DesktopObjects::add(Object object)
{
	if (const auto *record=std::get_if<DesktopWindow>(&object); record && record->parent) {
		const auto *parent=window(record->parent);
		if (!parent || parent->destroying) return 0;
		unsigned depth=1;
		while (parent->parent) {
			if (++depth>=64) return 0;
			parent=window(parent->parent); if (!parent) return 0;
		}
	}
	if (const auto *menu=std::get_if<DesktopMenu>(&object)) {
		if (menu->parent) return 0;
		std::vector<uint64_t> children;
		for (const auto &item:menu->items) if (item.submenu) {
			const auto *child=this->menu(item.submenu);
			if (!child || child->parent || std::find(children.begin(),children.end(),item.submenu)!=children.end()) return 0;
			children.push_back(item.submenu);
		}
	}
	const auto size = object_size(object);
	if (size > limit - used || objects.size()+classes.size() >= 10000 || next == UINT32_MAX)
		return 0;
	uint64_t handle = 0;
	if (std::holds_alternative<DesktopWindow>(object)) {
		// A window handle is unique among the processes of the host: its number sits under the tag of this process.
		const auto base = window_handle_base();
		for (uint32_t tries = 0; tries < 0xffff && !handle; ++tries) {
			next_window = next_window % 0xfffe + 1;
			if (!objects.contains(base + next_window)) handle = base + next_window;
		}
		if (!handle) return 0;
	} else {
		handle = next + 1;
		while (objects.contains(handle)) ++handle;
		next = handle;
	}
	objects.emplace(handle, std::move(object));
	if (const auto *record=menu(handle))
		for (const auto &item:record->items) if (item.submenu)
			std::get<DesktopMenu>(objects.at(item.submenu)).parent=handle;
	used += size;
	if (auto *record=std::get_if<DesktopWindow>(&objects.at(handle))) {
		record->order=++next_order;
		window_set.insert(handle);
		windows_touched.touch(handle);
	}
	if (std::holds_alternative<DesktopMenu>(objects.at(handle))) menus_touched.touch(handle);
	if (std::holds_alternative<DesktopBitmap>(objects.at(handle))) bitmaps_touched.touch(handle);
	if (const auto *device = std::get_if<DesktopDC>(&objects.at(handle))) {
		dcs_touched.touch(handle);
		count_selection(device->bitmap, 1);
	}
	if (std::holds_alternative<DesktopIcon>(objects.at(handle))) icons_touched.touch(handle);
	return handle;
}

bool DesktopObjects::erase(uint64_t handle)
{
	const auto found = objects.find(handle);
	if (found == objects.end())
		return false;
	if (std::holds_alternative<DesktopWindow>(found->second) && !child_windows(handle).empty()) return false;
	if (std::holds_alternative<DesktopWindow>(found->second)) {
		windows_touched.touch(handle);
		window_set.erase(handle);
	}
	if (std::holds_alternative<DesktopMenu>(found->second)) menus_touched.touch(handle);
	if (std::holds_alternative<DesktopBitmap>(found->second)) bitmaps_touched.touch(handle);
	if (std::holds_alternative<DesktopDC>(found->second)) dcs_touched.touch(handle);
	if (std::holds_alternative<DesktopIcon>(found->second)) icons_touched.touch(handle);
	if (const auto *menu=std::get_if<DesktopMenu>(&found->second)) {
		if (menu->parent) return false;
		for (const auto &item:menu->items) if (item.submenu) {
			std::get<DesktopMenu>(objects.at(item.submenu)).parent=0;
			erase(item.submenu);
		}
	}
	const auto *device=std::get_if<DesktopDC>(&found->second);
	if (device) count_selection(device->bitmap, -1);
	const auto previous_font=device ? device->font : 0;
	const auto previous_brush=device ? device->brush : 0;
	const auto previous_pen=device ? device->pen : 0;
	used -= object_size(found->second);
	objects.erase(found);
	if (const auto *previous=font(previous_font); previous && previous->pending_delete && !font_selected(previous_font))
		erase(previous_font);
	if (const auto *previous=brush(previous_brush); previous && previous->pending_delete && !brush_selected(previous_brush)) erase(previous_brush);
	if (const auto *previous=pen(previous_pen); previous && previous->pending_delete && !pen_selected(previous_pen)) erase(previous_pen);
	return true;
}

const DesktopFont *DesktopObjects::font(uint64_t handle) const
{
	if (handle==desktop_system_font) return &system_font;
	if (handle==desktop_gui_font) return &gui_font;
	if (handle==desktop_fixed_font) return &fixed_font;
	const auto found=objects.find(handle);
	return found==objects.end() ? nullptr : std::get_if<DesktopFont>(&found->second);
}

bool DesktopObjects::font_selected(uint64_t handle) const
{
	for (const auto &[identity,object] : objects)
		if (const auto *device=std::get_if<DesktopDC>(&object); device && device->font==handle) return true;
	return false;
}

uint64_t DesktopObjects::select_font(uint64_t handle,uint64_t selected_font)
{
	auto *device=dc(handle);
	if (!device || !font(selected_font)) return 0;
	const auto previous=device->font;
	device->font=selected_font;
	if (const auto *record=font(previous); record && record->pending_delete && !font_selected(previous)) erase(previous);
	return previous;
}

uint32_t DesktopObjects::set_dc_brush_color(uint32_t color)
{
	const auto previous=stock_brushes[6].color;
	stock_brushes[6].color=color;
	return previous;
}

uint32_t DesktopObjects::set_dc_pen_color(uint32_t color)
{
	const auto previous=stock_pens[3].color;
	stock_pens[3].color=color;
	return previous;
}

bool DesktopObjects::delete_font(uint64_t handle)
{
	if (handle==desktop_system_font || handle==desktop_gui_font || handle==desktop_fixed_font) return true;
	const auto found=objects.find(handle);
	if (found==objects.end()) return false;
	auto *record=std::get_if<DesktopFont>(&found->second);
	if (!record) return false;
	if (!font_selected(handle)) return erase(handle);
	record->pending_delete=true;
	return true;
}

void DesktopObjects::update_window_dcs(uint64_t handle)
{
	if (!window(handle)) return;
	for (auto &[identity,object] : objects)
		if (auto *device=std::get_if<DesktopDC>(&object); device && (device->window==handle || is_child(handle,device->window)) && (device->bounds || device->window_frame))
			configure_window_dc(device->window,*device,device->window_frame);
}

void DesktopObjects::detach_window_dcs(uint64_t handle)
{
	for (auto &[identity,object] : objects)
		if (auto *device=std::get_if<DesktopDC>(&object); device && device->window==handle && device->leased) {
			const auto previous=object_size(object);
			// A retained DC keeps selected objects, but no longer owns a drawable surface.
			count_selection(device->bitmap, -1);
			device->bitmap=device->default_bitmap=0;
			device->bounds=DesktopRect{}; device->clip.reset();
			used-=previous-object_size(object);
		}
}

// A retained device context follows its window onto the surface that replaces a released one.
void DesktopObjects::attach_window_dcs(uint64_t root,uint64_t bitmap)
{
	for (auto &[identity,object] : objects)
		if (auto *device=std::get_if<DesktopDC>(&object); device && device->leased && !device->bitmap && device->window && window(device->window) && window_root(device->window)==root) {
			device->bitmap=device->default_bitmap=bitmap;
			count_selection(bitmap, 1);
			configure_window_dc(device->window,*device,device->window_frame);
		}
}

const DesktopBrush *DesktopObjects::brush(uint64_t handle) const
{
	if (handle>=desktop_stock_brush && handle<desktop_stock_brush+stock_brushes.size()) return &stock_brushes[size_t(handle-desktop_stock_brush)];
	if (handle>=desktop_system_brush && handle<desktop_system_brush+system_brushes.size()) return &system_brushes[size_t(handle-desktop_system_brush)];
	const auto found=objects.find(handle);
	return found==objects.end() ? nullptr : std::get_if<DesktopBrush>(&found->second);
}

bool DesktopObjects::brush_selected(uint64_t handle) const
{
	for (const auto &[identity,object] : objects)
		if (const auto *device=std::get_if<DesktopDC>(&object); device && device->brush==handle) return true;
	return false;
}

uint64_t DesktopObjects::select_brush(uint64_t handle,uint64_t selected_brush)
{
	auto *device=dc(handle); if (!device || !brush(selected_brush)) return 0;
	const auto previous=device->brush; device->brush=selected_brush;
	if (const auto *record=brush(previous); record && record->pending_delete && !brush_selected(previous)) erase(previous);
	return previous;
}

bool DesktopObjects::delete_brush(uint64_t handle)
{
	if (handle>=desktop_stock_brush && handle<desktop_stock_brush+stock_brushes.size()) return true;
	if (handle>=desktop_system_brush && handle<desktop_system_brush+system_brushes.size()) return true;
	const auto found=objects.find(handle); if (found==objects.end()) return false;
	auto *record=std::get_if<DesktopBrush>(&found->second); if (!record) return false;
	if (!brush_selected(handle)) return erase(handle);
	record->pending_delete=true; return true;
}

const DesktopIcon *DesktopObjects::icon(uint64_t handle) const
{
	const auto found = objects.find(handle);
	return found == objects.end() ? nullptr : std::get_if<DesktopIcon>(&found->second);
}

DesktopIcon *DesktopObjects::icon(uint64_t handle)
{
	return const_cast<DesktopIcon *>(std::as_const(*this).icon(handle));
}

const DesktopBitmap *DesktopObjects::bitmap(uint64_t handle) const
{
	if (handle == 0x10000)
		return &default_bitmap;
	const auto found = objects.find(handle);
	return found == objects.end() ? nullptr : std::get_if<DesktopBitmap>(&found->second);
}

DesktopBitmap *DesktopObjects::bitmap(uint64_t handle)
{
	if (handle == 0x10000)
		return &default_bitmap;
	const auto found = objects.find(handle);
	auto *record = found == objects.end() ? nullptr : std::get_if<DesktopBitmap>(&found->second);
	if (record)
		bitmaps_touched.touch(handle);
	return record;
}

DesktopDC *DesktopObjects::dc(uint64_t handle)
{
	const auto found = objects.find(handle);
	auto *record = found == objects.end() ? nullptr : std::get_if<DesktopDC>(&found->second);
	if (record)
		dcs_touched.touch(handle);
	return record;
}

const DesktopDC *DesktopObjects::dc(uint64_t handle) const
{
	const auto found = objects.find(handle);
	return found == objects.end() ? nullptr : std::get_if<DesktopDC>(&found->second);
}

std::vector<uint64_t> DesktopObjects::icon_handles() const
{
	std::vector<uint64_t> result;
	for (const auto &[handle,object] : objects)
		if (std::holds_alternative<DesktopIcon>(object)) result.push_back(handle);
	return result;
}

std::vector<uint64_t> DesktopObjects::dc_handles() const
{
	std::vector<uint64_t> result;
	for (const auto &[handle,object] : objects)
		if (std::holds_alternative<DesktopDC>(object)) result.push_back(handle);
	return result;
}

DesktopWindow *DesktopObjects::window(uint64_t handle)
{
	const auto found=objects.find(handle);
	auto *record=found==objects.end() ? nullptr : std::get_if<DesktopWindow>(&found->second);
	if (record) windows_touched.touch(handle);
	return record;
}

std::vector<uint64_t> DesktopObjects::window_handles() const
{
	return std::vector<uint64_t>(window_set.begin(),window_set.end());
}

uint64_t DesktopObjects::invalid_window(uint64_t filter) const
{
	for (const auto handle : window_set)
		if (const auto *window=this->window(handle); window && window_visible(handle) && !window->update.empty() &&
			(!filter || filter==handle || is_child(filter,handle))) return handle;
	return 0;
}

bool DesktopObjects::set_title(uint64_t handle,std::u16string title)
{
	auto *record=window(handle);
	if (!record || title.size()>(limit-used)/2+record->title.size()) return false;
	const auto previous=record->title.size()*2;
	record->title=std::move(title);
	used=used-previous+record->title.size()*2;
	return true;
}

bool DesktopObjects::invalidate(uint64_t handle,DesktopRect rect,bool erase)
{
	auto *record=window(handle); if (!record) return false;
	DesktopRegion update(record->update); update.add(rect);
	const auto previous=record->update.bytes(),current=update.bytes();
	if (current>limit-used+previous) return false;
	if (record->update.empty() && !update.empty()) ++record->update_generation;
	record->update=std::move(update); used=used-previous+current;
	if (!record->update.empty()) record->erase_pending|=erase;
	return true;
}

bool DesktopObjects::validate(uint64_t handle,DesktopRect rect)
{
	auto *record=window(handle); if (!record) return false;
	DesktopRegion update(record->update); update.subtract(rect);
	const auto previous=record->update.bytes(),current=update.bytes();
	if (current>limit-used+previous) return false;
	record->update=std::move(update); used=used-previous+current;
	if (record->update.empty()) record->erase_pending=false;
	return true;
}

bool DesktopObjects::start_paint(uint64_t handle)
{
	auto *record=window(handle);
	auto *device=record ? dc(record->dc) : nullptr;
	if (!record || !device || record->painting || device->clip) throw std::runtime_error("invalid paint owner transition");
	device->clip.emplace(std::move(record->update)); record->painting=true;
	const bool erase=record->erase_pending; record->erase_pending=false;
	return erase;
}

void DesktopObjects::finish_paint(uint64_t handle)
{
	auto *record=window(handle); auto *device=record ? dc(record->dc) : nullptr;
	if (!record || !device || !record->painting || !device->clip) throw std::runtime_error("invalid paint completion");
	used-=device->clip->bytes(); device->clip.reset(); record->painting=false; record->painted=true;
	select_clip(record->dc,std::nullopt);
	select_font(record->dc,desktop_system_font);
	select_brush(record->dc,desktop_stock_brush);
	select_pen(record->dc,desktop_stock_pen+1);device->position_x=device->position_y=0;
	device->text_color=0; device->background_color=0xffffff; device->background_mode=2;
}

static bool same_name(std::string_view a,std::string_view b)
{
	if (a.size()!=b.size()) return false;
	const auto lower=[](char c) { return c>='A' && c<='Z' ? char(c+('a'-'A')) : c; };
	for (size_t i=0;i<a.size();++i) if (lower(a[i])!=lower(b[i])) return false;
	return true;
}

bool DesktopObjects::class_registered(uint64_t instance,std::string_view name,bool global) const
{
	for (const auto &[id,definition] : classes)
		if (same_name(name,definition.name) && bool(definition.style & 0x4000)==global && (global || definition.instance==instance)) return true;
	return false;
}

uint16_t DesktopObjects::register_class(DesktopClass definition)
{
	const auto size=sizeof(DesktopClass)+definition.name.size()+definition.extra.size();
	if (definition.name.empty() || !definition.procedure || (definition.background && definition.background>31 && !brush(definition.background)) || class_registered(definition.instance,definition.name,(definition.style & 0x4000)!=0) ||
		next_class>UINT16_MAX ||
		definition.window_extra>4096 || definition.extra.size()>4096 ||
		objects.size()+classes.size()>=10000 || size>limit-used)
		return 0;
	uint16_t atom=0;
	for (const auto &[id,other] : classes) if (same_name(other.name,definition.name)) { atom=other.atom; break; }
	if (!atom && next_atom>UINT16_MAX) return 0;
	const bool new_name=!atom;
	definition.atom=atom ? atom : uint16_t(next_atom);
	const auto id=uint16_t(next_class);
	classes.emplace(id,std::move(definition));
	++next_class; if (new_name) ++next_atom;
	used+=size;
	return id;
}

uint32_t DesktopObjects::unregister_class(uint16_t atom)
{
	const auto found=classes.find(atom);
	if (found==classes.end()) return 1411;
	for (const auto &[handle,object] : objects)
		if (const auto *window=std::get_if<DesktopWindow>(&object); window && window->class_id==atom)
			return 1412;
	used-=sizeof(DesktopClass)+found->second.name.size()+found->second.extra.size();
	const auto background=found->second.background;
	classes.erase(found);
	if (background) delete_brush(background);
	return 0;
}

uint16_t DesktopObjects::find_class(uint64_t instance, std::string_view name) const
{
	uint16_t global=0,any=0;
	// A window created without a module handle finds the class of whichever module registered it.
	for (const auto &[id,definition] : classes)
		if (same_name(definition.name,name)) {
			if (definition.style & 0x4000) global=id;
			else if (definition.instance==instance) return id;
			else if (!instance && !any) any=id;
		}
	return global ? global : any;
}

uint16_t DesktopObjects::find_class(uint64_t instance,uint16_t atom) const
{
	uint16_t global=0,any=0;
	for (const auto &[id,definition] : classes)
		if (definition.atom==atom) {
			if (definition.style & 0x4000) global=id;
			else if (definition.instance==instance) return id;
			else if (!instance && !any) any=id;
		}
	return global ? global : any;
}

const DesktopClass *DesktopObjects::window_class(uint16_t atom) const
{
	const auto found=classes.find(atom);
	return found==classes.end() ? nullptr : &found->second;
}

DesktopClass *DesktopObjects::window_class(uint16_t atom)
{
	const auto found=classes.find(atom);
	return found==classes.end() ? nullptr : &found->second;
}

bool DesktopObjects::selected(uint64_t bitmap) const
{
	return selections.contains(bitmap);
}

void DesktopObjects::reselect(DesktopDC &dc, uint64_t bitmap)
{
	count_selection(dc.bitmap, -1);
	dc.bitmap = bitmap;
	count_selection(bitmap, 1);
}

size_t DesktopObjects::count() const { return objects.size(); }
size_t DesktopObjects::bytes() const { return used; }
IconDisplay DesktopObjects::display() const { return icon_display; }

#ifdef RAVEN_SKIA
TextRenderer &DesktopObjects::text()
{
	if (!text_engine) {
		const auto bundled=std::filesystem::read_symlink("/proc/self/exe").parent_path()/"fonts";
		text_engine=std::make_unique<TextRenderer>(std::filesystem::is_directory(bundled) ? bundled.string() : RAVEN_FONT_DIRECTORY);
	}
	return *text_engine;
}
#endif

}
