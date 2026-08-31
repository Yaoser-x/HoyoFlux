#pragma once

// Display enumeration and settings snapshot/restore.
//
// Provides the data the DisplayGuard needs (A8): which displays exist, and
// how to save / restore a display's current mode. Restoring is deliberately a
// separate call so the session engine decides *when* it happens.

#include "domain/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hoyoflux::win32 {

struct DisplayInfo {
    uint32_t index{0};
    std::wstring device_name;    // e.g. L"\\\\.\\DISPLAY1"
    std::wstring friendly_name;  // monitor/device description
    int left{0};
    int top{0};
    int right{0};
    int bottom{0};
    bool is_primary{false};
    bool is_attached{false};
};

// A single display mode, captured via EnumDisplaySettings(ENUM_CURRENT_SETTINGS).
struct DisplaySettings {
    std::wstring device_name;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refresh_rate{0};  // Hz
    uint32_t bits_per_pixel{0};
    int32_t position_x{0};
    int32_t position_y{0};
    bool interlaced{false};
};

// Enumerate attached displays (EnumDisplayDevicesW).
Result<std::vector<DisplayInfo>> enumerate_displays();

// Capture the current mode of `device_name`.
Result<DisplaySettings> query_current_settings(std::wstring_view device_name);

// Exact comparison used to avoid asking a display driver to re-apply the
// mode it is already using (some virtual drivers reject that no-op).
bool display_settings_equal(const DisplaySettings& lhs,
                            const DisplaySettings& rhs);

// Restore a previously captured mode for this session only. This deliberately
// does not persist the mode to the user's Windows display configuration.
Result<void> restore_display_settings(const DisplaySettings& settings);

}  // namespace hoyoflux::win32
