#include "windows.h"
#include "nt/text.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace raven {

namespace {

constexpr uint32_t by_position = 0x400, popup_flag = 0x10;
constexpr uint32_t type_string = 0, type_bitmap = 4, type_menubar_break = 0x20, type_menu_break = 0x40, type_owner_draw = 0x100,
	type_radio = 0x200, type_separator = 0x800, type_right_justify = 0x4000, type_mask = type_bitmap | type_menubar_break | type_menu_break |
	type_owner_draw | type_radio | type_separator | type_right_justify;
constexpr uint32_t state_grayed = 3, state_disabled = 2, state_checked = 8, state_hilite = 0x80, state_default = 0x1000;

// MENUITEMINFOW: size, mask, type, state, id, submenu, bitmaps, item data, type data, length, item bitmap.
struct ItemInfo {
	uint32_t size = 0, mask = 0, type = 0, state = 0, id = 0;
	uint64_t submenu = 0, bitmap_checked = 0, bitmap_unchecked = 0, data = 0, type_data = 0;
	uint32_t length = 0;
	uint64_t bitmap_item = 0;
};

// Where the fields after the five leading words sit: a 32-bit program packs its handles and pointers in 4 bytes, a 64-bit one aligns
// them to 8.
struct ItemLayout {
	size_t width, submenu, bitmap_checked, bitmap_unchecked, data, type_data, length, bitmap_item, size;
};

ItemLayout item_layout(const ApiCall &call)
{
	return call.pointer_width() == 4 ? ItemLayout{4, 20, 24, 28, 32, 36, 40, 44, 48} : ItemLayout{8, 24, 32, 40, 48, 56, 64, 72, 80};
}

ItemInfo read_item_info(ApiCall &call, uint64_t address)
{
	const auto layout = item_layout(call);
	const auto bytes = call.read(address, layout.size);
	ItemInfo info;
	std::memcpy(&info.size, bytes.data(), 4);
	std::memcpy(&info.mask, bytes.data() + 4, 4);
	std::memcpy(&info.type, bytes.data() + 8, 4);
	std::memcpy(&info.state, bytes.data() + 12, 4);
	std::memcpy(&info.id, bytes.data() + 16, 4);
	std::memcpy(&info.submenu, bytes.data() + layout.submenu, layout.width);
	std::memcpy(&info.bitmap_checked, bytes.data() + layout.bitmap_checked, layout.width);
	std::memcpy(&info.bitmap_unchecked, bytes.data() + layout.bitmap_unchecked, layout.width);
	std::memcpy(&info.data, bytes.data() + layout.data, layout.width);
	std::memcpy(&info.type_data, bytes.data() + layout.type_data, layout.width);
	std::memcpy(&info.length, bytes.data() + layout.length, 4);
	std::memcpy(&info.bitmap_item, bytes.data() + layout.bitmap_item, layout.width);
	return info;
}

void write_item_info(ApiCall &call, uint64_t address, const ItemInfo &info)
{
	const auto layout = item_layout(call);
	std::vector<uint8_t> bytes(layout.size);
	std::memcpy(bytes.data(), &info.size, 4);
	std::memcpy(bytes.data() + 4, &info.mask, 4);
	std::memcpy(bytes.data() + 8, &info.type, 4);
	std::memcpy(bytes.data() + 12, &info.state, 4);
	std::memcpy(bytes.data() + 16, &info.id, 4);
	std::memcpy(bytes.data() + layout.submenu, &info.submenu, layout.width);
	std::memcpy(bytes.data() + layout.bitmap_checked, &info.bitmap_checked, layout.width);
	std::memcpy(bytes.data() + layout.bitmap_unchecked, &info.bitmap_unchecked, layout.width);
	std::memcpy(bytes.data() + layout.data, &info.data, layout.width);
	std::memcpy(bytes.data() + layout.type_data, &info.type_data, layout.width);
	std::memcpy(bytes.data() + layout.length, &info.length, 4);
	std::memcpy(bytes.data() + layout.bitmap_item, &info.bitmap_item, layout.width);
	call.write(address, std::span<const uint8_t>(bytes.data(), std::min<size_t>(info.size ? info.size : layout.size, layout.size)));
}

}

void NativeWindows::register_menu_api(ApiRegistry &registry)
{
	// An item is found by command id (searching submenus too) or by position in one menu.
	struct Location {
		uint64_t menu = 0;
		size_t index = 0;
		bool found = false;
	};
	const auto locate = [this](uint64_t menu, uint32_t id, uint32_t flags) -> Location {
		const auto *top = objects.menu(menu);
		if (!top)
			return {};
		if (flags & by_position)
			return id < top->items.size() ? Location{menu, id, true} : Location{};
		const auto search = [&](auto &&self, uint64_t handle) -> Location {
			const auto *level = objects.menu(handle);
			if (!level)
				return {};
			for (size_t index = 0; index < level->items.size(); ++index)
				if (!level->items[index].submenu && level->items[index].id == id)
					return {handle, index, true};
			for (const auto &item : level->items)
				if (item.submenu)
					if (const auto inner = self(self, item.submenu); inner.found)
						return inner;
			return {};
		};
		return search(search, menu);
	};
	// Builds an item from AppendMenu/InsertMenu/ModifyMenu arguments.
	const auto make_item = [this](ApiCall &call, uint32_t flags, uint64_t id, uint64_t data) -> std::optional<DesktopMenuItem> {
		DesktopMenuItem item{};
		item.type = flags & type_mask;
		item.state = (flags & 1 ? state_grayed : 0) | (flags & 2 ? state_disabled : 0) | (flags & state_checked) | (flags & state_hilite) | (flags & state_default);
		if (flags & popup_flag) {
			if (!objects.menu(id))
				return std::nullopt;
			item.submenu = id;
		} else {
			item.id = uint32_t(id);
		}
		if (item.type & type_separator)
			item.state = 0;
		else if (item.type & type_owner_draw)
			item.data = data;
		else if (item.type & type_bitmap)
			item.bitmap_item = data;
		else if (data)
			item.text = call.wide(data, 4096);
		return item;
	};
	const auto adopt = [this](uint64_t parent, const DesktopMenuItem &item) {
		if (item.submenu)
			if (auto *child = objects.edit_menu(item.submenu))
				child->parent = parent;
	};
	registry.add("CreateMenu", 0, [this](ApiCall &call) {
		const auto handle = objects.add(DesktopMenu{});
		if (!handle) {
			call.fail(8);
			return;
		}
		call.ret(handle);
	});
	registry.add("CreatePopupMenu", 0, [this](ApiCall &call) {
		const auto handle = objects.add(DesktopMenu{});
		if (!handle) {
			call.fail(8);
			return;
		}
		call.ret(handle);
	});
	const auto append = [this, make_item, adopt](ApiCall &call) {
		auto *menu = objects.edit_menu(call[0]);
		if (!menu) {
			call.fail(1401);
			return;
		}
		const auto item = make_item(call, call.u32(1), call[2], call[3]);
		if (!item) {
			call.fail(1401);
			return;
		}
		menu->items.push_back(*item);
		adopt(call[0], *item);
		call.ret(1);
	};
	registry.add("AppendMenuW", 4, append);
	registry.add("AppendMenuA", 4, [this, make_item, adopt](ApiCall &call) {
		auto *menu = objects.edit_menu(call[0]);
		if (!menu) {
			call.fail(1401);
			return;
		}
		const uint32_t flags = call.u32(1);
		DesktopMenuItem item{};
		if (!(flags & (type_separator | type_owner_draw | type_bitmap)) && call[3]) {
			// ANSI item text is widened through the Windows code page.
			const auto text = from_ansi(call.narrow(call[3], 4096), 1252);
			const auto stored = objects.add(DesktopMenu{});
			objects.erase(stored);
			const auto made = make_item(call, flags, call[2], 0);
			if (!made) {
				call.fail(1401);
				return;
			}
			item = *made;
			item.text = text;
		} else {
			const auto made = make_item(call, flags, call[2], call[3]);
			if (!made) {
				call.fail(1401);
				return;
			}
			item = *made;
		}
		menu->items.push_back(item);
		adopt(call[0], item);
		call.ret(1);
	});
	registry.add("InsertMenuW", 5, [this, locate, make_item, adopt](ApiCall &call) {
		auto *menu = objects.edit_menu(call[0]);
		if (!menu) {
			call.fail(1401);
			return;
		}
		const uint32_t flags = call.u32(2);
		const auto item = make_item(call, flags, call[3], call[4]);
		if (!item) {
			call.fail(1401);
			return;
		}
		size_t position = menu->items.size();
		if (call.u32(1) != UINT32_MAX) {
			const auto where = locate(call[0], call.u32(1), flags);
			if (!where.found || where.menu != call[0]) {
				call.fail(1456);
				return;
			}
			position = where.index;
		}
		menu->items.insert(menu->items.begin() + ptrdiff_t(position), *item);
		adopt(call[0], *item);
		call.ret(1);
	});
	registry.add("ModifyMenuW", 5, [this, locate, make_item, adopt](ApiCall &call) {
		const uint32_t flags = call.u32(2);
		const auto where = locate(call[0], call.u32(1), flags);
		if (!where.found) {
			call.fail(1456);
			return;
		}
		auto *menu = objects.edit_menu(where.menu);
		const auto item = make_item(call, flags, call[3], call[4]);
		if (!item) {
			call.fail(1401);
			return;
		}
		// Without MF_POPUP the item keeps its submenu and only its text, look and id change, as on Windows, where programs rewrite a
		// submenu's title this way. A new popup replaces the old one, which stays alive as a free menu.
		auto replacement = *item;
		const auto previous = menu->items[where.index].submenu;
		if (!(flags & popup_flag))
			replacement.submenu = previous;
		else if (previous && previous != replacement.submenu)
			if (auto *old = objects.edit_menu(previous))
				old->parent = 0;
		// Replacing an item's text, bitmap or owner keeps its placement at the right edge of the menu bar; only a new MF_HELP adds it.
		replacement.type |= menu->items[where.index].type & type_right_justify;
		menu->items[where.index] = replacement;
		adopt(where.menu, replacement);
		call.ret(1);
	});
	const auto remove = [this, locate](ApiCall &call, bool destroy) {
		const auto where = locate(call[0], call.u32(1), call.u32(2));
		if (!where.found) {
			call.fail(1456);
			return;
		}
		auto *menu = objects.edit_menu(where.menu);
		const auto item = menu->items[where.index];
		menu->items.erase(menu->items.begin() + ptrdiff_t(where.index));
		if (item.submenu) {
			if (auto *child = objects.edit_menu(item.submenu))
				child->parent = 0;
			if (destroy)
				objects.erase(item.submenu);
		}
		call.ret(1);
	};
	registry.add("DeleteMenu", 3, [remove](ApiCall &call) { remove(call, true); });
	registry.add("RemoveMenu", 3, [remove](ApiCall &call) { remove(call, false); });
	registry.add("EnableMenuItem", 3, [this, locate](ApiCall &call) {
		const uint32_t flags = call.u32(2);
		const auto where = locate(call[0], call.u32(1), flags);
		if (!where.found) {
			call.ret(UINT32_MAX);
			return;
		}
		auto &item = objects.edit_menu(where.menu)->items[where.index];
		const uint32_t previous = (item.state & state_grayed) == state_grayed ? 1 : (item.state & state_disabled) ? 2 : 0;
		item.state &= ~state_grayed;
		item.state |= (flags & 1 ? state_grayed : 0) | (flags & 2 ? state_disabled : 0);
		call.ret(previous);
	});
	registry.add("CheckMenuRadioItem", 5, [this, locate](ApiCall &call) {
		const uint32_t flags = call.u32(4);
		const auto first = locate(call[0], call.u32(1), flags), last = locate(call[0], call.u32(2), flags), checked = locate(call[0], call.u32(3), flags);
		if (!first.found || !last.found || !checked.found || first.menu != last.menu) {
			call.fail(1456);
			return;
		}
		auto *menu = objects.edit_menu(first.menu);
		for (size_t index = first.index; index <= last.index && index < menu->items.size(); ++index) {
			auto &item = menu->items[index];
			const bool selected = checked.menu == first.menu && index == checked.index;
			item.state = selected ? item.state | state_checked : item.state & ~state_checked;
			item.type = selected ? item.type | type_radio : item.type & ~type_radio;
		}
		call.ret(1);
	});
	// One item of a menu is its default (MFS_DEFAULT), drawn bold and chosen by a double click; -1 leaves the menu without one.
	registry.add("SetMenuDefaultItem", 3, [this, locate](ApiCall &call) {
		auto *menu = objects.edit_menu(call[0]);
		if (!menu) {
			call.fail(1401);
			return;
		}
		const auto where = call.u32(1) == UINT32_MAX ? Location{} : locate(call[0], call.u32(1), call.u32(2) ? by_position : 0);
		if (call.u32(1) != UINT32_MAX && (!where.found || where.menu != call[0])) {
			call.fail(1456);
			return;
		}
		for (auto &item : menu->items)
			item.state &= ~state_default;
		if (where.found)
			menu->items[where.index].state |= state_default;
		call.ret(1);
	});
	registry.add("GetMenuDefaultItem", 3, [this](ApiCall &call) {
		const auto *menu = objects.menu(call[0]);
		if (!menu) {
			call.fail(1401);
			call.ret(UINT32_MAX);
			return;
		}
		for (size_t index = 0; index < menu->items.size(); ++index) {
			const auto &item = menu->items[index];
			// GMDI_USEDISABLED (1) admits a disabled default item.
			if (!(item.state & state_default) || ((item.state & state_disabled) && !(call.u32(2) & 1)))
				continue;
			call.ret(call.u32(1) ? index : item.id);
			return;
		}
		call.ret(UINT32_MAX);
	});
	registry.add("GetMenuState", 3, [this, locate](ApiCall &call) {
		const auto where = locate(call[0], call.u32(1), call.u32(2));
		if (!where.found) {
			call.ret(UINT32_MAX);
			return;
		}
		const auto &item = objects.menu(where.menu)->items[where.index];
		uint32_t value = item.state | (item.type & (type_separator | type_menu_break | type_menubar_break | type_owner_draw | type_bitmap));
		if (item.submenu) {
			const auto *child = objects.menu(item.submenu);
			value = (value & 0xff) | popup_flag | (uint32_t(child ? child->items.size() : 0) << 8);
		}
		call.ret(value);
	});
	registry.add("GetMenuStringW", 5, [this, locate](ApiCall &call) {
		const auto where = locate(call[0], call.u32(1), call.u32(4));
		if (!where.found) {
			call.ret(0);
			return;
		}
		const auto &text = objects.menu(where.menu)->items[where.index].text;
		const int32_t capacity = call.i32(3);
		if (!capacity || !call[2]) {
			call.ret(text.size());
			return;
		}
		call.put_wide(call[2], text.substr(0, size_t(capacity) - 1), size_t(capacity));
		call.ret(std::min<size_t>(text.size(), size_t(capacity) - 1));
	});
	registry.add("GetMenu", 1, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		// A child window keeps its control identifier in the menu slot, which is what GetMenu reports for it.
		call.ret(window->parent ? window->child_id : window->menu);
	});
	registry.add("SetMenuItemBitmaps", 5, [this, locate](ApiCall &call) {
		const auto where = locate(call[0], call.u32(1), call.u32(2));
		if (!where.found) {
			call.fail(1456);
			return;
		}
		auto &item = objects.edit_menu(where.menu)->items[where.index];
		item.bitmap_unchecked = call[3];
		item.bitmap_checked = call[4];
		call.ret(1);
	});
	const auto to_item = [this, adopt](ApiCall &call, const ItemInfo &info, DesktopMenuItem &item, uint64_t owner) {
		if (info.mask & 0x100) item.type = (item.type & ~type_mask) | (info.type & type_mask);
		if (info.mask & 0x10) item.type = (item.type & ~type_mask) | (info.type & type_mask);
		if (info.mask & 1) item.state = info.state;
		if (info.mask & 2) item.id = info.id;
		if (info.mask & 4) {
			item.submenu = info.submenu;
			adopt(owner, item);
		}
		if (info.mask & 8) {
			item.bitmap_checked = info.bitmap_checked;
			item.bitmap_unchecked = info.bitmap_unchecked;
		}
		if (info.mask & 0x20) item.data = info.data;
		if (info.mask & 0x80) item.bitmap_item = info.bitmap_item;
		if ((info.mask & 0x40) || ((info.mask & 0x10) && !(item.type & (type_separator | type_bitmap | type_owner_draw))))
			if (info.type_data && !(item.type & (type_separator | type_bitmap | type_owner_draw)))
				item.text = call.wide(info.type_data, 4096);
	};
	registry.add("InsertMenuItemW", 4, [this, locate, to_item](ApiCall &call) {
		auto *menu = objects.edit_menu(call[0]);
		if (!menu) {
			call.fail(1401);
			return;
		}
		const auto info = read_item_info(call, call[3]);
		if (info.size < 48) {
			call.fail(87);
			return;
		}
		DesktopMenuItem item{};
		to_item(call, info, item, call[0]);
		size_t position = menu->items.size();
		if (call.u32(2) ? call.u32(1) != UINT32_MAX : true) {
			const auto where = locate(call[0], call.u32(1), call.u32(2) ? by_position : 0);
			if (call.u32(1) != UINT32_MAX && where.found && where.menu == call[0])
				position = where.index;
			else if (call.u32(1) != UINT32_MAX && call.u32(2)) {
				call.fail(1456);
				return;
			} else if (where.found && where.menu == call[0]) {
				position = where.index;
			}
		}
		menu->items.insert(menu->items.begin() + ptrdiff_t(position), item);
		call.ret(1);
	});
	registry.add("SetMenuItemInfoW", 4, [this, locate, to_item](ApiCall &call) {
		const auto where = locate(call[0], call.u32(1), call.u32(2) ? by_position : 0);
		if (!where.found) {
			call.fail(1456);
			return;
		}
		to_item(call, read_item_info(call, call[3]), objects.edit_menu(where.menu)->items[where.index], where.menu);
		call.ret(1);
	});
	registry.add("GetMenuItemInfoW", 4, [this, locate](ApiCall &call) {
		const auto where = locate(call[0], call.u32(1), call.u32(2) ? by_position : 0);
		if (!where.found) {
			call.fail(1456);
			return;
		}
		auto info = read_item_info(call, call[3]);
		const auto &item = objects.menu(where.menu)->items[where.index];
		if (info.mask & 0x100) info.type = item.type;
		if (info.mask & 0x10) info.type = item.type;
		if (info.mask & 1) info.state = item.state;
		if (info.mask & 2) info.id = item.id;
		if (info.mask & 4) info.submenu = item.submenu;
		if (info.mask & 8) {
			info.bitmap_checked = item.bitmap_checked;
			info.bitmap_unchecked = item.bitmap_unchecked;
		}
		if (info.mask & 0x20) info.data = item.data;
		if (info.mask & 0x80) info.bitmap_item = item.bitmap_item;
		if (info.mask & 0x40) {
			if (info.type_data) {
				const size_t capacity = info.length;
				call.put_wide(info.type_data, item.text.substr(0, capacity ? capacity - 1 : 0), capacity);
				info.length = uint32_t(std::min(item.text.size(), capacity ? capacity - 1 : 0));
			} else {
				info.length = uint32_t(item.text.size());
			}
		}
		write_item_info(call, call[3], info);
		call.ret(1);
	});
	registry.add("GetMenuBarInfo", 4, [this](ApiCall &call) {
		const auto *window = std::as_const(objects).window(call[0]);
		if (!window || !window->menu) {
			call.fail(window ? 1401 : 1400);
			return;
		}
		const auto layout = frame_layout(*window);
		const int32_t values[4] = {window->outer.left + layout.menu_bar.left, window->outer.top + layout.menu_bar.top,
			window->outer.left + layout.menu_bar.right, window->outer.top + layout.menu_bar.bottom};
		std::vector<uint8_t> bytes(48);
		const uint32_t size = 48;
		std::memcpy(bytes.data(), &size, 4);
		std::memcpy(bytes.data() + 4, values, 16);
		std::memcpy(bytes.data() + 24, &window->menu, 8);
		const uint64_t owner = call[0];
		std::memcpy(bytes.data() + 32, &owner, 8);
		call.write(call[3], bytes);
		call.ret(1);
	});
	registry.add("CreateAcceleratorTableW", 2, [this](ApiCall &call) {
		const int32_t count = call.i32(1);
		if (!call[0] || count <= 0 || count > 1024) {
			call.fail(87);
			return;
		}
		std::vector<DesktopAccelerator> table;
		const auto bytes = call.read(call[0], size_t(count) * 6);
		for (int32_t index = 0; index < count; ++index) {
			const uint8_t *entry = bytes.data() + size_t(index) * 6;
			uint16_t key, command;
			std::memcpy(&key, entry + 2, 2);
			std::memcpy(&command, entry + 4, 2);
			table.push_back({entry[0], key, command});
		}
		const auto handle = objects.add_accelerators(std::move(table));
		if (!handle)
			call.fail(8);
		else
			call.ret(handle);
	});
	registry.add("DestroyAcceleratorTable", 1, [this](ApiCall &call) { call.ret(objects.remove_accelerators(call[0])); });
}

}
