#include "platform/win32/registry.hpp"

#include <windows.h>

#include <algorithm>
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

Result<bool> registry_key_exists(std::wstring_view subkey) {
    HKEY raw = nullptr;
    const LSTATUS status =
        RegOpenKeyExW(HKEY_CURRENT_USER, std::wstring(subkey).c_str(), 0,
                      KEY_READ, &raw);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    if (status != ERROR_SUCCESS) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed,
            "RegOpenKeyExW failed for " + utf8(subkey), status));
    }
    RegCloseKey(raw);
    return true;
}

Result<std::vector<RegistryValue>> read_registry_values(
    std::wstring_view subkey) {
    RegKey key;
    {
        HKEY raw = nullptr;
        const LSTATUS status =
            RegOpenKeyExW(HKEY_CURRENT_USER, std::wstring(subkey).c_str(), 0,
                          KEY_READ, &raw);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
            return std::vector<RegistryValue>{};
        }
        if (status != ERROR_SUCCESS) {
            return std::unexpected(Error::make(
                ErrorCode::RegistryReadFailed,
                "RegOpenKeyExW failed for " + utf8(subkey), status));
        }
        key = RegKey(raw);
    }

    DWORD count = 0;
    DWORD max_name_len = 0;
    DWORD max_data_len = 0;
    if (const LSTATUS status = RegQueryInfoKeyW(
            key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &count, &max_name_len, &max_data_len, nullptr, nullptr);
        status != ERROR_SUCCESS) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed, "RegQueryInfoKeyW failed", status));
    }

    std::vector<RegistryValue> values;
    values.reserve(count);
    std::wstring name(max_name_len + 1, L'\0');
    std::vector<std::byte> data(max_data_len + 1);
    for (DWORD i = 0; i < count; ++i) {
        DWORD name_len = static_cast<DWORD>(name.size());
        DWORD data_len = static_cast<DWORD>(data.size());
        DWORD type = 0;
        const LSTATUS status =
            RegEnumValueW(key.get(), i, name.data(), &name_len, nullptr, &type,
                          reinterpret_cast<LPBYTE>(data.data()), &data_len);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            return std::unexpected(Error::make(
                ErrorCode::RegistryReadFailed, "RegEnumValueW failed", status));
        }
        RegistryValue value;
        value.name.assign(name.data(), name_len);
        value.type = type;
        value.data.assign(data.data(), data.data() + data_len);
        values.push_back(std::move(value));
    }
    return values;
}

Result<void> write_registry_values(std::wstring_view subkey,
                                   std::span<const RegistryValue> values) {
    HKEY raw = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                     std::wstring(subkey).c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                     nullptr, &raw, nullptr);
    if (status != ERROR_SUCCESS) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed,
            "RegCreateKeyExW failed for " + utf8(subkey), status));
    }
    RegKey key(raw);
    for (const RegistryValue& value : values) {
        status =
            RegSetValueExW(key.get(), value.name.c_str(), 0, value.type,
                           reinterpret_cast<const BYTE*>(value.data.data()),
                           static_cast<DWORD>(value.data.size()));
        if (status != ERROR_SUCCESS) {
            return std::unexpected(Error::make(
                ErrorCode::RegistryReadFailed,
                "RegSetValueExW failed for '" + utf8(value.name) + "'",
                status));
        }
    }
    return {};
}

Result<void> restore_registry_prefix_exact(
    std::wstring_view subkey, std::wstring_view prefix,
    std::span<const RegistryValue> values) {
    auto current = read_registry_values(subkey);
    if (!current) {
        return std::unexpected(current.error());
    }

    HKEY raw = nullptr;
    const LSTATUS opened = RegCreateKeyExW(
        HKEY_CURRENT_USER, std::wstring(subkey).c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &raw, nullptr);
    if (opened != ERROR_SUCCESS) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed,
            "RegCreateKeyExW(KEY_SET_VALUE) failed for " + utf8(subkey), opened));
    }
    RegKey key(raw);
    for (const auto& existing : *current) {
        if (existing.name.rfind(prefix, 0) != 0) {
            continue;
        }
        const bool retained = std::any_of(
            values.begin(), values.end(), [&](const RegistryValue& expected) {
                return expected.name == existing.name;
            });
        if (!retained) {
            const LSTATUS deleted =
                RegDeleteValueW(key.get(), existing.name.c_str());
            if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
                return std::unexpected(Error::make(
                    ErrorCode::RegistryReadFailed,
                    "RegDeleteValueW failed for '" + utf8(existing.name) + "'",
                    deleted));
            }
        }
    }
    return write_registry_values(subkey, values);
}

}  // namespace hoyoflux::win32
