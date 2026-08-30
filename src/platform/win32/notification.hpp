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

// Notifications never participate in authoritative session control flow.
void notify_best_effort(const NotificationFunction& function,
                        std::wstring_view title, std::wstring_view body,
                        NotificationKind kind);

}  // namespace hoyoflux::win32
