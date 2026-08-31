#include "session/display_guard.hpp"

#include "platform/win32/display.hpp"

namespace hoyoflux::session {

Result<void> restore_display_snapshot(const std::vector<JournalDisplay>& displays) {
    Error first_failure{};
    bool any_failed = false;
    for (const auto& entry : displays) {
        // Skip displays that vanished or changed identity since the capture.
        if (entry.settings.device_name.empty() ||
            entry.settings.width == 0 || entry.settings.height == 0) {
            continue;
        }
        auto current = win32::query_current_settings(entry.settings.device_name);
        if (current &&
            win32::display_settings_equal(*current, entry.settings)) {
            continue;
        }
        if (auto restored =
                win32::restore_display_settings(entry.settings);
            !restored) {
            if (!any_failed) {
                first_failure = restored.error();
                any_failed = true;
            }
        }
    }
    if (any_failed) {
        return std::unexpected(first_failure);
    }
    return {};
}

}  // namespace hoyoflux::session
