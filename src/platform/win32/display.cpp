#include "platform/win32/display.hpp"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <utility>

namespace hoyoflux::win32 {
namespace {

Error win32_error(ErrorCode code, std::string_view what) {
    return Error::make(code, std::string(what), GetLastError());
}

DEVMODEW to_devmode(const DisplaySettings& settings) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFREQUENCY | DM_POSITION | DM_DISPLAYFLAGS;
    if (settings.device_name.size() < std::size(mode.dmDeviceName)) {
        std::copy(settings.device_name.begin(), settings.device_name.end(),
                  mode.dmDeviceName);
    }
    mode.dmPelsWidth = settings.width;
    mode.dmPelsHeight = settings.height;
    mode.dmBitsPerPel = settings.bits_per_pixel;
    mode.dmDisplayFrequency = settings.refresh_rate;
    mode.dmPosition.x = settings.position_x;
    mode.dmPosition.y = settings.position_y;
    mode.dmDisplayFlags = settings.interlaced ? DM_INTERLACED : 0;
    return mode;
}

}  // namespace

Result<std::vector<DisplayInfo>> enumerate_displays() {
    std::vector<DisplayInfo> out;
    for (uint32_t i = 0;; ++i) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) {
            break;  // no more display adapters
        }

        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        bool attached = (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        EnumDisplayDevicesW(device.DeviceName, 0, &monitor, 0);

        DisplayInfo info;
        info.index = i;
        info.device_name = device.DeviceName;
        info.friendly_name = monitor.DeviceString[0] != L'\0'
                                 ? monitor.DeviceString
                                 : device.DeviceString;
        info.is_primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        info.is_attached = attached;

        if (attached) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                info.left = mode.dmPosition.x;
                info.top = mode.dmPosition.y;
                info.right = mode.dmPosition.x + static_cast<int>(mode.dmPelsWidth);
                info.bottom = mode.dmPosition.y + static_cast<int>(mode.dmPelsHeight);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

Result<DisplaySettings> query_current_settings(std::wstring_view device_name) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const std::wstring name(device_name);
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        return std::unexpected(win32_error(ErrorCode::OsError,
                                           "EnumDisplaySettingsW failed"));
    }
    DisplaySettings settings;
    settings.device_name = name;
    settings.width = mode.dmPelsWidth;
    settings.height = mode.dmPelsHeight;
    settings.refresh_rate = mode.dmDisplayFrequency;
    settings.bits_per_pixel = mode.dmBitsPerPel;
    settings.position_x = mode.dmPosition.x;
    settings.position_y = mode.dmPosition.y;
    settings.interlaced = (mode.dmDisplayFlags & DM_INTERLACED) != 0;
    return settings;
}

Result<void> restore_display_settings(const DisplaySettings& settings) {
    DEVMODEW mode = to_devmode(settings);
    const LONG result = ChangeDisplaySettingsExW(
        settings.device_name.empty() ? nullptr : settings.device_name.c_str(),
        &mode, nullptr, CDS_UPDATEREGISTRY | CDS_RESET, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "ChangeDisplaySettingsExW failed: " +
                                    std::to_string(result)));
    }
    return {};
}

}  // namespace hoyoflux::win32
