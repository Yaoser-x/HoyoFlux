#pragma once

// Read-only access to the HoYoPlay launcher install paths in HKCU.
//
// This replaces the legacy RegGetpath() (main.cpp:869-1116). Unlike the
// legacy code, the game channel subkeys are opened properly instead of being
// passed as part of the value name (the hk4e_global bug).

#include "domain/error.hpp"

#include <filesystem>
#include <optional>

namespace hoyoflux::win32 {

// Absolute paths to each located game executable, verified to exist on disk.
// Empty optionals mean the channel was not found.
struct LauncherPaths {
    std::optional<std::filesystem::path> genshin_cn;       // YuanShen.exe
    std::optional<std::filesystem::path> genshin_global;   // GenshinImpact.exe
    std::optional<std::filesystem::path> starrail_cn;      // StarRail.exe
    std::optional<std::filesystem::path> starrail_global;  // StarRail.exe
};

// Scan both launcher trees (miHoYo for CN, Cognosphere for Global), probing
// the HYP\1_0..1_9 version subkeys, then each game channel, reading the
// GameInstallPath value and verifying the game executable exists.
Result<LauncherPaths> read_launcher_paths();

}  // namespace hoyoflux::win32
