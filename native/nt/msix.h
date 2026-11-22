#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace raven {

class ApiRegistry;
class NativePrefix;
struct KernelContext;

inline constexpr std::u16string_view windows_apps = u"C:\\Program Files\\WindowsApps";

struct MsixApplication {
	std::u16string full_name, family, name, publisher, resource, publisher_id;
	std::u16string aumid, image, directory;
	uint64_t version = 0;
	uint32_t architecture = 0;
};

std::filesystem::path msix_store(const NativePrefix &prefix);
std::optional<MsixApplication> find_msix_application(const NativePrefix &prefix, std::u16string_view aumid, std::u16string_view image = {});
void register_msix_api(ApiRegistry &registry, KernelContext &context);
void remember_msix_verification(uint32_t thread, std::u16string_view publisher, std::span<const uint8_t> hashes);

}
