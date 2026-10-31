#pragma once

#include "graphics/icon_bitmap.h"
#include "graphics/icon_group.h"
#include "region.h"
#include "shared_pixels.h"
#include "nonclient.h"
#include "colors.h"
#include "menu.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <memory>
#include <optional>
#ifdef RAVEN_SKIA
#include "graphics/text.h"
#endif

#include <map>
#include <set>
#include <variant>
#include <string>
#include <string_view>

namespace raven {

struct DesktopIcon {
	IconBitmap image;
	bool monochrome;
	bool shared;
	// A cursor's hot spot, the point that follows the mouse; an icon's is its centre.
	bool cursor = false;
	uint32_t hotspot_x = 0, hotspot_y = 0;
};

struct DesktopBitmap {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	// Top-down DDB rows: word-aligned monochrome bits or straight-alpha BGRA32.
	std::vector<uint8_t> bits;
	uint64_t address = 0;
	bool top_down = true;
	bool stock = false;
	// Guest-backed window storage does not imply a public DIB section.
	bool dib_section = false;
	// Host view of the guest-visible pixels; guest-backed bitmaps always carry one.
	std::shared_ptr<SharedPixels> shared{};
	// DIB sections keep the format their creator asked for: bytes per row, and the color table of indexed depths (COLORREF).
	uint32_t stride = 0;
	std::vector<uint32_t> palette{};
	// The channel masks of a BI_BITFIELDS section, as its creator gave them.
	std::array<uint32_t,3> masks{};
	// The physical size a program records with SetBitmapDimensionEx, in 0.1 mm; GDI keeps it and never uses it.
	int32_t dimension_x = 0, dimension_y = 0;
	std::span<uint8_t> pixels() { return shared ? std::span<uint8_t>(shared->data(), shared->size()) : std::span<uint8_t>(bits); }
	std::span<const uint8_t> pixels() const { return shared ? std::span<const uint8_t>(shared->data(), shared->size()) : std::span<const uint8_t>(bits); }
};

// A logical palette: its entries as COLORREF, in the program's order.
struct DesktopPalette {
	std::vector<uint32_t> entries{};
};

struct DesktopFont {
	std::array<uint8_t,92> definition{};
	bool pending_delete = false;
};

inline constexpr uint64_t desktop_system_font = 0xfffe;
inline constexpr uint64_t desktop_gui_font = 0xfffd;
inline constexpr uint64_t desktop_fixed_font = 0xfffc;
inline constexpr uint64_t desktop_stock_brush = 0xfff0;
inline constexpr uint64_t desktop_system_brush = 0xffc0;
inline constexpr uint64_t desktop_stock_pen = 0xffb0;
// IDC_* identifiers 32512..32651 map onto this block of system cursor handles.
inline constexpr uint64_t desktop_stock_cursor = 0xfa00;
inline constexpr uint32_t desktop_stock_cursor_count = 140;

struct DesktopBrush {
	uint32_t color = 0;
	bool hollow = false,pending_delete = false;
	// BS_HATCHED selects a hatch pattern drawn in color; BS_PATTERN and BS_DIBPATTERN carry their own pixels.
	uint32_t style = 0,hatch = 0;
	uint32_t pattern_width = 0,pattern_height = 0;
	// A monochrome pattern stores 0 for the text color and 1 for the background color.
	bool pattern_mono = false;
	std::vector<uint32_t> pattern{};
};

struct DesktopPen {
	uint32_t color=0;
	bool hollow=false,pending_delete=false;
	// PS_* style (0 solid .. 6 inside frame) and width in logical units; geometric pens draw wide lines with round caps.
	uint32_t style=0;
	int32_t width=0;
	bool geometric=false;
};

// Attributes SaveDC keeps and RestoreDC puts back.
struct DesktopDCState {
	uint64_t bitmap=0,font=0,brush=0,pen=0;
	std::optional<DesktopRegion> clip{},application_clip{};
	uint32_t text_color=0,background_color=0,background_mode=0,rop2=13,text_align=0,poly_fill=1;
	int32_t origin_x=0,origin_y=0,position_x=0,position_y=0;
	std::optional<DesktopRect> bounds{};
	int32_t brush_org_x=0,brush_org_y=0,window_org_x=0,window_org_y=0,viewport_org_x=0,viewport_org_y=0;
	uint32_t graphics_mode=1;
	std::array<float,6> world{1,0,0,1,0,0};
};

struct DesktopDC {
	uint64_t bitmap;
	uint64_t default_bitmap;
	uint64_t window = 0;
	std::optional<DesktopRegion> clip{};
	uint64_t font = desktop_system_font;
	bool leased = false;
	// A context over the shared pixels of a window another process owns (GetDC of that window); zero otherwise.
	uint64_t foreign = 0;
	bool window_frame = false;
	uint32_t text_color = 0,background_color = 0xffffff,background_mode = 2;
	uint64_t brush = desktop_stock_brush;
	int32_t origin_x=0,origin_y=0;
	std::optional<DesktopRect> bounds{};
	// Application clipping is independent of BeginPaint's update region.
	std::optional<DesktopRegion> application_clip{};
	uint64_t pen=desktop_stock_pen+1;
	int32_t position_x=0,position_y=0;
	// R2_COPYPEN until SetROP2 changes how pens combine with the destination.
	uint32_t rop2=13;
	// TA_LEFT|TA_TOP|TA_NOUPDATECP, ALTERNATE polygon filling, and the origins that shift logical coordinates.
	uint32_t text_align=0,poly_fill=1;
	int32_t brush_org_x=0,brush_org_y=0,window_org_x=0,window_org_y=0,viewport_org_x=0,viewport_org_y=0;
	// Window and viewport origins move logical coordinates; this offset is what they contributed to origin_x and origin_y.
	int32_t user_dx=0,user_dy=0;
	// GM_COMPATIBLE until SetGraphicsMode; the world transform (XFORM order) is kept for programs that read it back, and drawing
	// does not apply it.
	uint32_t graphics_mode=1;
	std::array<float,6> world{1,0,0,1,0,0};
	// The path a BeginPath bracket records: its figures as logical points, each closed when it becomes a region. Open while the
	// bracket lasts; EndPath closes it for PathToRegion.
	struct Path {
		bool open=true;
		std::vector<std::vector<std::array<double,2>>> figures;
	};
	std::optional<Path> path{};
	// Parts of the window other windows cover; drawing never reaches them.
	std::optional<DesktopRegion> visible{};
	std::vector<DesktopDCState> saved{};
	// Clip coordinates belong to the DC, not its bitmap backing.
	bool contains(int64_t x,int64_t y) const
	{
		x-=int64_t(origin_x)-user_dx; y-=int64_t(origin_y)-user_dy;
		if (bounds && (x<bounds->left || y<bounds->top || x>=bounds->right || y>=bounds->bottom)) return false;
		if (x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX) return false;
		return (!clip || clip->contains(int32_t(x),int32_t(y))) &&
			(!application_clip || application_clip->contains(int32_t(x),int32_t(y))) &&
			(!visible || visible->contains(int32_t(x),int32_t(y)));
	}
	// The device pixels of [x0,x1) x [y0,y1) that pass contains(), as rectangles, so a caller can skip the per-pixel test.
	std::vector<DesktopRect> paintable(int64_t x0,int64_t y0,int64_t x1,int64_t y1) const
	{
		const int64_t dx=int64_t(origin_x)-user_dx, dy=int64_t(origin_y)-user_dy;
		x0-=dx; x1-=dx; y0-=dy; y1-=dy;
		if (bounds) {
			x0=std::max<int64_t>(x0,bounds->left); y0=std::max<int64_t>(y0,bounds->top);
			x1=std::min<int64_t>(x1,bounds->right); y1=std::min<int64_t>(y1,bounds->bottom);
		}
		x0=std::max<int64_t>(x0,INT32_MIN); y0=std::max<int64_t>(y0,INT32_MIN);
		x1=std::min<int64_t>(x1,INT32_MAX); y1=std::min<int64_t>(y1,INT32_MAX);
		if (x0>=x1 || y0>=y1) return {};
		DesktopRegion area;
		area.add({int32_t(x0),int32_t(y0),int32_t(x1),int32_t(y1)});
		for (const auto *region : {&clip,&application_clip,&visible})
			if (*region) area.combine(area,**region,1);
		auto result=area.rectangles();
		for (auto &rect : result) {
			rect.left=int32_t(rect.left+dx); rect.right=int32_t(rect.right+dx);
			rect.top=int32_t(rect.top+dy); rect.bottom=int32_t(rect.bottom+dy);
		}
		return result;
	}
};

struct DesktopClass {
	std::string name;
	uint64_t instance;
	uint64_t procedure;
	uint64_t background = 0;
	uint32_t window_extra = 0;
	std::vector<uint8_t> extra{};
	uint32_t style = 0;
	uint16_t atom = 0;
	uint64_t icon = 0,cursor = 0;
	// Default menu resource: an integer identifier, or a name when the identifier is zero.
	uint32_t menu_id = 0;
	std::u16string menu_name{};
};

struct DesktopAccelerator {
	uint8_t flags;
	uint16_t key,command;
};

struct DesktopMenuItem {
	uint32_t type,state,id;
	std::u16string text;
	uint64_t submenu=0;
	// Owner-draw payload and the check-mark and item bitmaps an application attached.
	uint64_t data=0,bitmap_checked=0,bitmap_unchecked=0,bitmap_item=0;
};

struct DesktopMenu {
	std::vector<DesktopMenuItem> items;
	uint32_t help=0;
	uint64_t parent=0;
};

// One scroll bar of a window: range, page size, position and which arrows are disabled.
struct DesktopScroll {
	int32_t min=0,max=100,pos=0,track=0;
	uint32_t page=0,disabled=0;
};

struct DesktopWindow {
	uint16_t class_id;
	uint64_t procedure;
	std::u16string title;
	int32_t x, y, width, height;
	bool destroying = false;
	uint64_t bitmap = 0, dc = 0;
	bool visible = false;
	bool painting = false, erase_pending = false;
	DesktopRegion update{};
	uint64_t update_generation=0;
	// Since when a top-level window has waited for its controls' paint before the compositor shows it (see NativeWindows::present).
	std::optional<std::chrono::steady_clock::time_point> present_waiting{};
	// Whether the window has been shown on the screen once: until then it waits longer for its first paint.
	bool presented = false;
	// Whether the window has ever finished a WM_PAINT: one that has not shows nothing of its own yet.
	bool painted = false;
	// The frame extended into the client area by DwmExtendFrameIntoClientArea: left, right, top and bottom widths, a negative one for all of it.
	std::optional<std::array<int32_t,4>> glass{};
	// The dark caption and border DWMWA_USE_IMMERSIVE_DARK_MODE asks for.
	bool dark_frame = false;
	// A child whose program answered its last WM_NCPAINT without the default handling, drawing its border itself.
	bool own_frame = false;
	uint64_t instance = 0,userdata = 0;
	// The guest thread that created the window: its messages go to that thread's queue and its procedure runs on it.
	uint32_t thread = 0;
	uint32_t style = 0x84000000;
	std::vector<uint8_t> extra{};
	// Client origin and extent stay distinct from the outer window rectangle.
	DesktopRect outer{x,y,int32_t(int64_t(x)+width),int32_t(int64_t(y)+height)};
	uint64_t parent = 0,child_id = 0;
	uint32_t ex_style = 0;
	// SetWindowDisplayAffinity: whether the window asks to be left out of screen captures.
	uint32_t display_affinity = 0;
	// An overlapped window created hidden receives WM_SIZE and WM_MOVE when it is first shown rather than while it is created.
	bool need_size = false;
	uint64_t menu = 0;
	bool client_ready = true;
	// Owner of a top-level window (a different relation from the parent of a child) and the sibling order, bottom to top.
	uint64_t owner = 0;
	// A child moved to the desktop (SetParent with no parent, as a drop-down list is) keeps no owner, but is shown on the host window
	// of the root it came from: a host window of its own would be placed by the compositor rather than where the program put it.
	uint64_t shown_on = 0;
	uint64_t order = 0;
	// Outer rectangle to return to when a maximized or minimized window is restored.
	DesktopRect restore{};
	std::map<std::u16string,uint64_t> properties{};
	// Horizontal and vertical scroll bars (SB_HORZ, SB_VERT).
	std::array<DesktopScroll,2> scroll{};
};

class DesktopObjects {
public:
	explicit DesktopObjects(size_t limit = 256 * 1024 * 1024, IconDisplay display = {},DesktopMetrics metrics = {},DesktopColors colors = desktop_colors);
	DesktopObjects(const DesktopObjects &) = delete;
	DesktopObjects &operator=(const DesktopObjects &) = delete;
	using Object = std::variant<DesktopIcon, DesktopBitmap, DesktopDC, DesktopWindow, DesktopFont, DesktopBrush, DesktopMenu, DesktopRegion, DesktopPen, DesktopPalette>;
	uint64_t add(Object object);
	bool erase(uint64_t handle);
	const DesktopMenu *menu(uint64_t handle) const;
	DesktopMenu *edit_menu(uint64_t handle);
	// Accelerator tables live outside the object map; their handles never collide with GDI or window handles.
	uint64_t add_accelerators(std::vector<DesktopAccelerator> table);
	const std::vector<DesktopAccelerator> *accelerators(uint64_t handle) const;
	bool remove_accelerators(uint64_t handle);
	std::vector<uint64_t> accelerator_handles() const;
	// Changes whenever an accelerator table is made or destroyed.
	uint64_t accelerators_version() const { return accelerator_version; }
	uint64_t load_menu(const MenuTemplate &definition);
	const DesktopIcon *icon(uint64_t handle) const;
	DesktopIcon *icon(uint64_t handle);
	const DesktopBitmap *bitmap(uint64_t handle) const;
	DesktopBitmap *bitmap(uint64_t handle);
	DesktopDC *dc(uint64_t handle);
	const DesktopDC *dc(uint64_t handle) const;
	const DesktopRegion *region(uint64_t handle) const;
	bool replace_region(uint64_t handle,DesktopRegion region);
	bool select_clip(uint64_t dc,std::optional<DesktopRegion> region);
	const DesktopFont *font(uint64_t handle) const;
	uint64_t select_font(uint64_t dc, uint64_t font);
	// SetDCBrushColor and SetDCPenColor recolor the shared stock DC brush and pen; the previous color is returned.
	uint32_t set_dc_brush_color(uint32_t color);
	uint32_t set_dc_pen_color(uint32_t color);
	bool delete_font(uint64_t font);
	const DesktopBrush *brush(uint64_t handle) const;
	uint32_t system_color(int32_t index) const;
	uint64_t system_brush(int32_t index) const;
	uint64_t select_brush(uint64_t dc,uint64_t brush);
	bool delete_brush(uint64_t brush);
	const DesktopPen *pen(uint64_t handle) const;
	DesktopPalette *palette(uint64_t handle);
	uint64_t select_pen(uint64_t dc,uint64_t pen);
	bool delete_pen(uint64_t pen);
	void update_window_dcs(uint64_t window);
	void detach_window_dcs(uint64_t window);
	void attach_window_dcs(uint64_t root,uint64_t bitmap);
	// A window handed out for change is recorded, so what the guest reads of it can follow (see NativeWindows::publish_windows).
	DesktopWindow *window(uint64_t handle);
	const DesktopWindow *window(uint64_t handle) const;
	// Windows, menus and bitmaps handed out for change, added or erased since the guest's tables were last brought up to date.
	struct TouchedHandles {
		std::vector<uint64_t> handles{};
		// Set when more were touched than are recorded: every one may have changed.
		bool overflow=false;
		// The handles touched, each once.
		std::vector<uint64_t> &settled()
		{
			std::sort(handles.begin(), handles.end());
			handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
			return handles;
		}
		void touch(uint64_t handle)
		{
			if (!handles.empty() && handles.back()==handle) return;
			if (handles.size()<4096) handles.push_back(handle);
			else overflow=true;
		}
	};
	TouchedHandles &touched_windows() { return windows_touched; }
	TouchedHandles &touched_menus() { return menus_touched; }
	TouchedHandles &touched_bitmaps() { return bitmaps_touched; }
	TouchedHandles &touched_dcs() { return dcs_touched; }
	TouchedHandles &touched_icons() { return icons_touched; }
	std::vector<uint64_t> menu_handles() const;
	std::vector<uint64_t> bitmap_handles() const;
	std::vector<uint64_t> dc_handles() const;
	std::vector<uint64_t> icon_handles() const;
	uint64_t window_root(uint64_t handle) const;
	// Children of a window in z-order, bottom to top. Top-level windows are the children of window zero.
	std::vector<uint64_t> child_windows(uint64_t parent) const;
	// Moves a window to the top or bottom of its siblings.
	void restack(uint64_t window,bool top);
	bool is_child(uint64_t parent,uint64_t child) const;
	bool window_visible(uint64_t handle) const;
	void configure_window_dc(uint64_t window,DesktopDC &device,bool frame) const;
	uint64_t invalid_window(uint64_t filter) const;
	std::vector<uint64_t> window_handles() const;
	bool set_title(uint64_t window,std::u16string title);
	bool invalidate(uint64_t window,DesktopRect rect,bool erase);
	bool validate(uint64_t window,DesktopRect rect);
	bool start_paint(uint64_t window);
	void finish_paint(uint64_t window);
	uint16_t register_class(DesktopClass definition);
	uint32_t unregister_class(uint16_t atom);
	uint16_t find_class(uint64_t instance, std::string_view name) const;
	uint16_t find_class(uint64_t instance,uint16_t atom) const;
	bool class_registered(uint64_t instance,std::string_view name,bool global) const;
	const DesktopClass *window_class(uint16_t atom) const;
	DesktopClass *window_class(uint16_t atom);
	bool selected(uint64_t bitmap) const;
	// Selects a bitmap into a context, keeping count of the contexts each bitmap is selected into.
	void reselect(DesktopDC &dc, uint64_t bitmap);
	size_t count() const;
	size_t bytes() const;
	IconDisplay display() const;
#ifdef RAVEN_SKIA
	// One glyph engine serves every GDI and nonclient text operation of the process.
	TextRenderer &text();
#endif
	const DesktopMetrics &nonclient_metrics() const;
	bool set_screen_size(int32_t width,int32_t height);

private:
	TouchedHandles windows_touched{}, menus_touched{}, bitmaps_touched{}, dcs_touched{}, icons_touched{};
	// How many contexts each bitmap is selected into.
	std::map<uint64_t,uint32_t> selections;
	// The windows among the objects, in handle order.
	std::set<uint64_t> window_set;
	void count_selection(uint64_t bitmap, int change)
	{
		if (!bitmap) return;
		auto &count = selections[bitmap];
		count = uint32_t(int64_t(count) + change);
		if (!count) selections.erase(bitmap);
	}
	std::map<uint64_t, Object> objects;
	std::map<uint16_t,DesktopClass> classes;
	std::map<uint64_t,std::vector<DesktopAccelerator>> accelerator_tables;
	uint64_t accelerator_version=0;
#ifdef RAVEN_SKIA
	std::unique_ptr<TextRenderer> text_engine;
#endif
	uint32_t next_atom = 0xc000;
	uint32_t next_class = 1;
	uint64_t next = 0x10000;
	// Windows are numbered apart from other objects, under a tag of this process (see foreign_surface.h).
	uint64_t next_window = 0;
	uint64_t next_order = 0;
	size_t used = 0;
	size_t limit;
	IconDisplay icon_display;
	DesktopMetrics window_metrics;
	// Immutable default bitmap identity, shared by all memory DCs in this owner.
	DesktopBitmap default_bitmap{1,1,1,{0,0},0,true,true};
	DesktopFont system_font,gui_font,fixed_font;
	std::array<DesktopBrush,31> system_brushes{};
	std::array<DesktopBrush,7> stock_brushes{{{0xffffff},{0xc0c0c0},{0x808080},{0x404040},{0},{0,true},{0xffffff}}};
	std::array<DesktopPen,4> stock_pens{{{0xffffff},{0},{0,true},{0}}};
	bool font_selected(uint64_t handle) const;
	bool brush_selected(uint64_t handle) const;
	bool pen_selected(uint64_t handle) const;
};

}
