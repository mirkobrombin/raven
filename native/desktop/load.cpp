#include "api.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <stdexcept>
#include <sys/mman.h>

namespace raven {

// Legacy LoadIcon/LoadCursor/LoadAccelerators entry points over the module resource tables.
bool NativeDesktop::load_dispatch(std::string_view name)
{
	if (name == "LoadCursorW") {
		const auto args = process.api_arguments(2);
		uint64_t result = 0;
		// Only the system cursors exist so far; a cursor resource of a module shows as the arrow, so the pointer does not vanish over it.
		if (!args[0] && args[1] >= 32512 && args[1] < 32512 + desktop_stock_cursor_count)
			result = desktop_stock_cursor + (args[1] - 32512);
		else if (args[0] && resources.view(args[0], 12, args[1]))
			result = desktop_stock_cursor;
		else
			environment.set_last_error(args[0] ? 120 : 1813);
		process.return_api(result);
		return true;
	}
	if (name == "LoadAcceleratorsW") {
		const auto args = process.api_arguments(2);
		uint64_t result = 0;
		try {
			const auto view = resources.view(args[0], 9, args[1]);
			if (view) {
				if (view->size < 8 || view->size > 8192)
					throw uint32_t(13);
				const auto bytes = process.read_memory(view->address, view->size);
				std::vector<DesktopAccelerator> table;
				// ACCELTABLEENTRY: flags word, key word, command word, padding word; bit 0x80 ends the table.
				for (size_t at = 0; at + 8 <= bytes.size(); at += 8) {
					table.push_back({uint8_t(bytes[at] & 0x7f), uint16_t(bytes[at + 2] | bytes[at + 3] << 8), uint16_t(bytes[at + 4] | bytes[at + 5] << 8)});
					if (bytes[at] & 0x80)
						break;
				}
				result = objects.add_accelerators(std::move(table));
				if (!result)
					throw uint32_t(8);
			}
		} catch (uint32_t error) { environment.set_last_error(error); }
		process.return_api(result);
		return true;
	}
	// SHGetStockIconInfo(stock icon, flags, SHSTOCKICONINFO): the shell's standard icons, which message boxes and file dialogs show,
	// as the system icon each stands for: the handle when SHGSI_ICON asks for it (small with SHGSI_SMALLICON), and where it lives.
	if (name == "SHGetStockIconInfo") {
		const auto args = process.api_arguments(3);
		const uint32_t stock = uint32_t(args[0]), flags = uint32_t(args[1]);
		// A 32-bit program's structure holds a 4-byte handle right after the size; a 64-bit one aligns its 8-byte handle.
		const size_t handle = process.mode() == NativeMode::x86 ? 4 : 8;
		const uint32_t structure_size = handle == 4 ? 0x218 : 0x220;
		uint32_t result = 0;
		try {
			if (!args[2] || process.read_memory(args[2], 4) != std::vector<uint8_t>{uint8_t(structure_size), uint8_t(structure_size >> 8), 0, 0})
				throw uint32_t(0x80070057U);
			// IDI_ identifiers: warning, information, error, help (the question icon), shield, and the application icon for the rest.
			const uint32_t system_icon = stock == 78 ? 32515 : stock == 79 ? 32516 : stock == 80 ? 32513 : stock == 23 ? 32514 : stock == 77 ? 32518 : 32512;
			uint64_t icon = 0;
			if (flags & 0x100) {
				if (!oem_module)
					throw uint32_t(0x80004005U);
				static constexpr uint32_t resource[] = {100, 103, 102, 101, 104, 105, 100};
				if (!scratch)
					scratch = process.map_memory(4096, PROT_READ | PROT_WRITE);
				const uint64_t size = (flags & 1) ? 16 : 32;
				const uint64_t forwarded[] = {oem_module, resource[system_icon - 32512], size, size, scratch};
				if (load_icon(forwarded))
					throw uint32_t(0x80004005U);
				const auto bytes = process.read_memory(scratch, handle);
				for (size_t index = 0; index < handle; ++index)
					icon |= uint64_t(bytes[index]) << (index * 8);
			}
			// SHSTOCKICONINFO: size, icon handle, system image list index, icon index in the file, then the file's path.
			std::vector<uint8_t> info(structure_size - handle, 0);
			std::memcpy(info.data(), &icon, handle);
			const int32_t list_index = -1, file_index = -int32_t(system_icon);
			std::memcpy(info.data() + handle, &list_index, 4);
			std::memcpy(info.data() + handle + 4, &file_index, 4);
			const std::u16string path = u"C:\\Windows\\System32\\user32.dll";
			std::memcpy(info.data() + handle + 8, path.data(), path.size() * 2);
			process.write_memory(args[2] + handle, info);
		} catch (uint32_t error) { result = error; }
		catch (const std::bad_alloc &) { result = 0x8007000eU; }
		process.return_api(result);
		return true;
	}
	if (name != "LoadIconW")
		return false;
	const auto args = process.api_arguments(2);
	uint64_t result = 0;
	try {
		uint64_t instance = args[0], icon_name = args[1];
		if (!instance) {
			// The system icons come from the user32 component: application, hand, question, exclamation, asterisk, logo.
			static constexpr uint32_t stock[] = {100, 103, 102, 101, 104, 105, 100};
			if (!oem_module || icon_name < 32512 || icon_name > 32518)
				throw uint32_t(120);
			instance = oem_module;
			icon_name = stock[icon_name - 32512];
		}
		if (!scratch)
			scratch = process.map_memory(4096, PROT_READ | PROT_WRITE);
		// LoadIcon picks the default icon size of the display, exactly like a zero-sized LoadImage.
		const uint64_t forwarded[] = {instance, icon_name, 0, 0, scratch};
		const auto status = load_icon(forwarded);
		if (status) {
			if ((status & 0xffff0000U) == 0x80070000U)
				environment.set_last_error(uint32_t(status & 0xffffU));
			else
				environment.set_last_error(120);
		} else {
			const auto bytes = process.read_memory(scratch, 8);
			for (size_t index = 0; index < 8; ++index)
				result |= uint64_t(bytes[index]) << (index * 8);
		}
	} catch (uint32_t error) { environment.set_last_error(error); }
	catch (const std::bad_alloc &) { environment.set_last_error(8); }
	process.return_api(result);
	return true;
}

}
