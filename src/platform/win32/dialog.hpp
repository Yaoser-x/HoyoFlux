#pragma once

#include "domain/error.hpp"

#include <string_view>

namespace hoyoflux::win32 {

enum class ErrorDialogAction { Close, OpenConfig, ViewDiagnostics };

struct ErrorDialogOptions {
    std::wstring_view title{L"HoyoFlux"};
    std::wstring_view instruction;
    std::wstring_view content;
    bool can_open_config{false};
    bool can_view_diagnostics{false};
};

// A fixed native error dialog. TaskDialogIndirect gives users the applicable
// file actions; MessageBoxW is retained only for platforms where the common
// controls dialog cannot be created.
Result<ErrorDialogAction> show_error_dialog(const ErrorDialogOptions& options);

}  // namespace hoyoflux::win32
