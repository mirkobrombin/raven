#include "objects.h"

namespace raven {

uint64_t DesktopObjects::add_accelerators(std::vector<DesktopAccelerator> table)
{
	const TableHold hold(*this, true);
	constexpr uint64_t first = 0x7a000000ULL;
	if (table.empty() || table.size() > 1024 || accelerator_tables.size() >= 1024)
		return 0;
	const uint64_t handle = accelerator_tables.empty() ? first + 1 : accelerator_tables.rbegin()->first + 1;
	accelerator_tables.emplace(handle, std::move(table));
	++accelerator_version;
	return handle;
}

bool DesktopObjects::remove_accelerators(uint64_t handle)
{
	const TableHold hold(*this, true);
	const bool removed = accelerator_tables.erase(handle) != 0;
	accelerator_version += removed;
	return removed;
}

const std::vector<DesktopAccelerator> *DesktopObjects::accelerators(uint64_t handle) const
{
	const TableHold hold(*this, false);
	const auto found = accelerator_tables.find(handle);
	return found == accelerator_tables.end() ? nullptr : &found->second;
}

}
