#pragma once

// Read-only access to the HoYoPlay launcher install paths in HKCU.
//
// This replaces the legacy RegGetpath() (main.cpp:869-1116). Unlike the
// legacy code, the game channel subkeys are opened properly instead of being
// passed as part of the value name (the hk4e_global bug).

#include "domain/error.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hoyoflux::win32 {

// Absolute paths to each located game executable, verified to exist on disk.
// Empty optionals mean no usable executable was found; the registered flags
// distinguish an absent channel from a launcher entry with a missing file.
struct LauncherPaths {
    std::optional<std::filesystem::path> genshin_cn;       // YuanShen.exe
    std::optional<std::filesystem::path> genshin_global;   // GenshinImpact.exe
    std::optional<std::filesystem::path> starrail_cn;      // StarRail.exe
    std::optional<std::filesystem::path> starrail_global;  // StarRail.exe
    // True when a channel has a GameInstallPath value even if its executable
    // is missing. Doctor uses this to distinguish absent from corrupt.
    bool genshin_registered{false};
    bool starrail_registered{false};
};

// Scan both launcher trees (miHoYo for CN, Cognosphere for Global), probing
// the HYP\1_0..1_9 version subkeys, then each game channel, reading the
// GameInstallPath value and verifying the game executable exists.
Result<LauncherPaths> read_launcher_paths();

// ---------------------------------------------------------------------------
// F2/F3: raw value access for the persistent-state machinery. All paths are
// subkeys under HKCU. A missing key is not an error (empty result / no-op) -
// a game that never stored settings has nothing to protect.
// ---------------------------------------------------------------------------

// One value exactly as stored.
struct RegistryValue {
    std::wstring name;
    uint32_t type{0};           // REG_* constant
    std::vector<std::byte> data;
};

// Every value directly under HKCU\<subkey> (no subkeys of its own).
Result<std::vector<RegistryValue>> read_registry_values(std::wstring_view subkey);

// Create-or-replace each value under HKCU\<subkey>, creating the subkey when
// missing (restore path). Writes happen under the caller's privilege.
Result<void> write_registry_values(std::wstring_view subkey,
                                   std::span<const RegistryValue> values);

// Make the values whose names start with `prefix` exactly match `values`:
// delete protected values absent from the snapshot, then replay the snapshot.
// Values outside the prefix are never changed.
Result<void> restore_registry_prefix_exact(
    std::wstring_view subkey, std::wstring_view prefix,
    std::span<const RegistryValue> values);

// True when HKCU\<subkey> exists.
Result<bool> registry_key_exists(std::wstring_view subkey);

}  // namespace hoyoflux::win32
