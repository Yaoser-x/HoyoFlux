#include "platform/win32/registry.hpp"

#include <windows.h>

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hoyoflux::win32 {
namespace {

constexpr std::wstring_view kCnLauncherRoot = L"Software\\miHoYo\\HYP";
constexpr std::wstring_view kGlobalLauncherRoot = L"Software\\Cognosphere\\HYP";
constexpr std::wstring_view kGameInstallPath = L"GameInstallPath";

constexpr std::wstring_view kGenshinCnExe = L"YuanShen.exe";
constexpr std::wstring_view kGenshinGlobalExe = L"GenshinImpact.exe";
constexpr std::wstring_view kStarRailExe = L"StarRail.exe";

std::string utf8(std::wstring_view w) {
    // Narrow only for diagnostics; ASCII in practice.
    return {w.begin(), w.end()};
}

// RAII for a registry key (RegCloseKey, not CloseHandle).
class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY key) noexcept : key_(key) {}
    ~RegKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    RegKey(RegKey&& other) noexcept : key_(std::exchange(other.key_, nullptr)) {}
    RegKey& operator=(RegKey&& other) noexcept {
        if (this != &other) {
            if (key_ != nullptr) {
                RegCloseKey(key_);
            }
            key_ = std::exchange(other.key_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HKEY get() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != nullptr; }

private:
    HKEY key_{nullptr};
};

// Read a REG_SZ value. Returns nullopt when the value is absent; an Error on
// any real failure.
Result<std::optional<std::wstring>> read_registry_string(HKEY root,
                                                         std::wstring_view subkey,
                                                         std::wstring_view value) {
    RegKey key;
    {
        HKEY raw = nullptr;
        const LSTATUS status =
            RegOpenKeyExW(root, std::wstring(subkey).c_str(), 0, KEY_READ, &raw);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
            return std::optional<std::wstring>{};
        }
        if (status != ERROR_SUCCESS) {
            return std::unexpected(Error::make(
                ErrorCode::RegistryReadFailed,
                "RegOpenKeyExW failed for " + utf8(subkey), status));
        }
        key = RegKey(raw);
    }

    DWORD size = 0;
    DWORD type = 0;
    LSTATUS status = RegGetValueW(key.get(), nullptr, std::wstring(value).c_str(),
                                  RRF_RT_REG_SZ, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        return std::optional<std::wstring>{};
    }
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed, "RegGetValueW sizing failed", status));
    }

    std::wstring buffer((size + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    status = RegGetValueW(key.get(), nullptr, std::wstring(value).c_str(),
                          RRF_RT_REG_SZ, &type, buffer.data(), &size);
    if (status != ERROR_SUCCESS) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed, "RegGetValueW read failed", status));
    }
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

// A channel to look for under each HYP\1_N version subkey.
struct Channel {
    std::wstring_view subkey;                      // e.g. L"hk4e_cn"
    std::wstring_view exe_name;                    // e.g. L"YuanShen.exe"
    std::optional<std::filesystem::path>* target;  // where to store the result
};

// Scan one launcher root (CN or Global). Verifies the exe exists before
// storing anything.
void scan_launcher_root(std::wstring_view root, std::span<const Channel> channels,
                        LauncherPaths& /*out*/) {
    for (unsigned v = 0; v < 10; ++v) {
        const std::wstring version_key =
            std::wstring(root) + L"\\1_" + std::to_wstring(v);
        for (const Channel& channel : channels) {
            if (channel.target->has_value()) {
                continue;  // already located under an earlier version subkey
            }
            const std::wstring full_key =
                version_key + L"\\" + std::wstring(channel.subkey);
            auto path = read_registry_string(HKEY_CURRENT_USER, full_key,
                                             kGameInstallPath);
            if (!path.has_value() || !path->has_value()) {
                continue;  // read error or channel not present
            }
            std::filesystem::path exe = **path;
            exe /= std::wstring(channel.exe_name);
            if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                *channel.target = std::move(exe);
            }
        }
    }
}

}  // namespace

Result<LauncherPaths> read_launcher_paths() {
    LauncherPaths paths;

    const Channel cn_channels[] = {
        {L"hk4e_cn", kGenshinCnExe, &paths.genshin_cn},
        {L"hkrpg_cn", kStarRailExe, &paths.starrail_cn},
    };
    scan_launcher_root(kCnLauncherRoot, cn_channels, paths);

    const Channel global_channels[] = {
        {L"hk4e_global", kGenshinGlobalExe, &paths.genshin_global},
        {L"hkrpg_global", kStarRailExe, &paths.starrail_global},
    };
    scan_launcher_root(kGlobalLauncherRoot, global_channels, paths);

    return paths;
}

}  // namespace hoyoflux::win32
