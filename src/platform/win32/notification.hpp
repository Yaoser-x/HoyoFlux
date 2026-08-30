#pragma once

#include "domain/error.hpp"

#include <functional>
#include <string_view>

namespace hoyoflux::win32 {

enum class NotificationKind { Info, Success, Warning, Error };

using NotificationFunction = std::function<Result<void>(
    std::wstring_view, std::wstring_view, NotificationKind)>;

Result<void> notify(std::wstring_view title, std::wstring_view body,
                    NotificationKind kind);

// Remove the transient notification-area icon and stop its hidden owner
// worker. Safe to call when no notification was successfully added.
void cleanup_notifications();

// Notifications never participate in authoritative session control flow.
void notify_best_effort(const NotificationFunction& function,
                        std::wstring_view title, std::wstring_view body,
                        NotificationKind kind);

}  // namespace hoyoflux::win32
