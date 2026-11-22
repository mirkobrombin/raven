#pragma once

#include <string>
#include <string_view>

namespace raven {

// Windows API set contracts ("api-ms-win-core-synch-l1-2-0") are virtual names for functions that live in
// system libraries. Returns the library that Raven serves the contract from, or empty when the name is no contract.
std::string apiset_host(std::string_view name);
bool apiset_implemented(std::string_view name);

}
