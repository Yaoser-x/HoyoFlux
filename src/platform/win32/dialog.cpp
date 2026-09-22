#include "platform/win32/dialog.hpp"

#include <windows.h>
#include <commctrl.h>

#include <array>

namespace hoyoflux::win32 {
namespace {

constexpr int kOpenConfig = 1001;
constexpr int kViewDiagnostics = 1002;

}  // namespace

Result<ErrorDialogAction> show_error_dialog(const ErrorDialogOptions& options) {
    std::array<TASKDIALOG_BUTTON, 3> buttons{};
    UINT count = 0;
    if (options.can_open_config) {
        buttons[count++] = TASKDIALOG_BUTTON{kOpenConfig, L"打开配置"};
    }
    if (options.can_view_diagnostics) {
        buttons[count++] = TASKDIALOG_BUTTON{kViewDiagnostics, L"查看诊断"};
    }
    buttons[count++] = TASKDIALOG_BUTTON{IDCLOSE, L"关闭"};

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = nullptr;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = options.title.data();
    config.pszMainInstruction = options.instruction.data();
    config.pszContent = options.content.data();
    config.cButtons = count;
    config.pButtons = buttons.data();
    config.nDefaultButton = IDCLOSE;
    config.pszMainIcon = TD_ERROR_ICON;

    int selected = IDCLOSE;
    const HRESULT result = TaskDialogIndirect(&config, &selected, nullptr, nullptr);
    if (FAILED(result)) {
        MessageBoxW(nullptr, options.content.data(), options.title.data(),
                    MB_OK | MB_ICONERROR);
        return ErrorDialogAction::Close;
    }
    if (selected == kOpenConfig) return ErrorDialogAction::OpenConfig;
    if (selected == kViewDiagnostics) return ErrorDialogAction::ViewDiagnostics;
    return ErrorDialogAction::Close;
}

}  // namespace hoyoflux::win32
