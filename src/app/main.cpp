#include "app/app_paths.hpp"
#include "app/doctor.hpp"
#include "app/elevated_session.hpp"
#include "app/portable_migration.hpp"
#include "platform/win32/dialog.hpp"
#include "platform/win32/elevation.hpp"
#include "platform/win32/privilege.hpp"
#include "platform/win32/process.hpp"
#include "platform/win32/text.hpp"
#include "profile/config.hpp"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace hoyoflux;

namespace {

std::wstring widen(std::string_view utf8) {
    auto wide = win32::utf16(utf8);
    return wide.value_or(L"无法显示详细错误");
}

std::wstring format_error(const Error& error) {
    std::wstring text = widen(error.message);
    if (error.location) {
        const auto& location = *error.location;
        std::wstring prefix;
        if (!location.profile.empty()) prefix += widen(location.profile) + L" 配置档，";
        if (location.line) prefix += L"第 " + std::to_wstring(*location.line) + L" 行";
        if (location.column) prefix += L"第 " + std::to_wstring(*location.column) + L" 列";
        if (!location.field.empty()) {
            if (!prefix.empty()) prefix += L"：";
            prefix += widen(location.field);
        }
        if (!prefix.empty()) text = prefix + L"\n" + text;
    }
    return text;
}

bool open_file(const std::filesystem::path& path) {
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", path.c_str(), nullptr, path.parent_path().c_str(),
        SW_SHOWNORMAL));
    if (opened > 32) return true;

    std::wstring system_directory(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(system_directory.data(),
                                             static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) return false;
    system_directory.resize(length);
    const auto notepad = std::filesystem::path(system_directory) / L"notepad.exe";
    const std::wstring argument = win32::quote_windows_argument(path.wstring());
    const auto fallback = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", notepad.c_str(), argument.c_str(),
        path.parent_path().c_str(), SW_SHOWNORMAL));
    return fallback > 32;
}

void surface_error(const std::optional<app::AppPaths>& paths,
                   std::wstring instruction, Error error,
                   bool offer_config) {
    bool report_available = false;
    if (paths) {
        app::DiagnosticContext context;
        context.error = error;
        context.full = false;
        report_available = app::write_diagnostic_report(*paths, context).has_value();
    }

    std::wstring body = format_error(error) +
        L"\n\n修复后保存配置，再次双击 HoyoFlux。";
    bool retried_open = false;
    for (;;) {
        auto action = win32::show_error_dialog({
            .title = L"HoyoFlux",
            .instruction = instruction,
            .content = body,
            .can_open_config = offer_config && paths.has_value() &&
                               std::filesystem::exists(paths->config),
            .can_view_diagnostics = report_available,
        });
        if (!action || *action == win32::ErrorDialogAction::Close) return;
        const auto target = *action == win32::ErrorDialogAction::OpenConfig
            ? paths->config : paths->diagnostics;
        if (open_file(target)) return;
        if (retried_open) return;
        retried_open = true;
        body += L"\n\n无法打开：" + target.wstring() + L"。请手动打开该文件。";
    }
}

struct NativeArguments {
    bool unsupported_public_arguments{false};
    std::optional<app::ElevatedSessionArguments> elevated;
};

bool is_token(std::wstring_view token) {
    if (token.size() != 48) return false;
    for (const wchar_t character : token) {
        if (!((character >= L'0' && character <= L'9') ||
              (character >= L'a' && character <= L'f'))) return false;
    }
    return true;
}

Result<NativeArguments> read_native_arguments() {
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (raw == nullptr) return std::unexpected(Error::make(
        ErrorCode::OsError, "CommandLineToArgvW failed", GetLastError()));
    std::vector<std::wstring> arguments(raw, raw + count);
    LocalFree(raw);

    NativeArguments parsed;
    std::optional<std::wstring> token;
    std::optional<std::wstring> pipe;
    const std::wstring elevated_prefix =
        std::wstring(win32::kInternalElevatedArgument) + L"=";
    const std::wstring pipe_prefix = std::wstring(win32::kInternalPipeArgument) + L"=";
    for (int index = 1; index < count; ++index) {
        const auto& argument = arguments[index];
        if (argument.starts_with(elevated_prefix) && !token) {
            token = argument.substr(elevated_prefix.size());
        } else if (argument.starts_with(pipe_prefix) && !pipe) {
            pipe = argument.substr(pipe_prefix.size());
        } else {
            parsed.unsupported_public_arguments = true;
        }
    }
    if (token || pipe) {
        if (!token || !pipe || !is_token(*token) ||
            !pipe->starts_with(L"\\\\.\\pipe\\HoyoFlux-") ||
            !pipe->ends_with(*token)) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidArgument, "内部提权上下文无效"));
        }
        parsed.elevated = app::ElevatedSessionArguments{
            .token = std::move(*token), .pipe_name = std::move(*pipe)};
    }
    return parsed;
}

Result<bool> legacy_config_exists(const std::filesystem::path& root) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(root / L"config.toml", ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查旧版 AppData 配置: " + win32::utf8(root.wstring()),
        static_cast<unsigned long>(ec.value())));
    return exists;
}

int diagnose(const app::AppPaths& paths, const profile::Config* config,
             std::optional<Error> prior_error = std::nullopt) {
    app::DiagnosticContext context;
    if (config) context.config = *config;
    context.error = std::move(prior_error);
    context.full = true;
    auto written = app::write_diagnostic_report(paths, context);
    if (!written) {
        surface_error(paths, L"无法生成诊断报告", written.error(), true);
        return 1;
    }
    if (!open_file(paths.diagnostics)) {
        surface_error(paths, L"诊断报告已生成，但无法打开", Error::make(
            ErrorCode::OsError, "请手动打开：" + win32::utf8(paths.diagnostics.wstring())),
            true);
        return 1;
    }
    return 0;
}

int application_main() {
    auto arguments = read_native_arguments();
    if (!arguments) {
        surface_error(std::nullopt, L"无法启动 HoyoFlux", arguments.error(), false);
        return 1;
    }
    auto paths = app::resolve_app_paths();
    if (!paths) {
        surface_error(std::nullopt, L"无法启动 HoyoFlux", paths.error(), false);
        return 1;
    }

    if (arguments->elevated) {
        auto child = app::run_elevated_session_child(*paths, *arguments->elevated);
        return child ? *child : 1;
    }
    if (win32::is_elevated()) {
        surface_error(*paths, L"请以普通方式启动", Error::make(
            ErrorCode::NotElevated,
            "HoyoFlux 无法从管理员环境安全建立普通启动端。请关闭此窗口后直接双击 EXE。"), false);
        return 1;
    }
    if (auto prepared = app::prepare_portable_directories(*paths); !prepared) {
        surface_error(*paths, L"软件目录不可用", prepared.error(), false);
        return 1;
    }
    if (arguments->unsupported_public_arguments) {
        surface_error(*paths, L"不支持命令行启动", Error::make(
            ErrorCode::InvalidArgument,
            "HoyoFlux 已移除命令行功能。请编辑 EXE 旁边的 config.toml，然后双击启动。"), true);
        return 1;
    }

    std::error_code ec;
    bool config_exists = std::filesystem::exists(paths->config, ec);
    if (ec) {
        surface_error(*paths, L"无法检查本地配置", Error::make(
            ErrorCode::OsError, ec.message(), static_cast<unsigned long>(ec.value())), false);
        return 1;
    }
    auto legacy_root = app::legacy_appdata_root();
    if (!legacy_root) {
        surface_error(*paths, L"无法定位旧版数据", legacy_root.error(), false);
        return 1;
    }
    auto has_legacy_config = legacy_config_exists(*legacy_root);
    if (!has_legacy_config) {
        surface_error(*paths, L"无法检查旧版数据", has_legacy_config.error(), false);
        return 1;
    }
    if (!config_exists && !*has_legacy_config) {
        auto created = profile::write_default_config(paths->config);
        if (!created) {
            surface_error(*paths, L"无法创建首次配置", created.error(), false);
            return 1;
        }
        if (!open_file(paths->config)) {
            surface_error(*paths, L"配置已生成，但无法打开", Error::make(
                ErrorCode::OsError, "请手动打开：" + win32::utf8(paths->config.wstring())), true);
            return 1;
        }
        return 0;
    }

    // A local diagnose setting is deliberately read before any AppData scan,
    // migration, recovery, UAC request, or game capability write path.
    if (config_exists) {
        auto inspected = profile::read_config(paths->config);
        if (!inspected) {
            auto declared_action = profile::read_declared_launcher_action(paths->config);
            if (declared_action && *declared_action == profile::LauncherAction::Diagnose) {
                return diagnose(*paths, nullptr, inspected.error());
            }
            surface_error(*paths, L"无法启动：配置有误", inspected.error(), true);
            return 1;
        }
        if (inspected->launcher.action == profile::LauncherAction::Diagnose) {
            return diagnose(*paths, &*inspected);
        }
    }

    if (auto migration = app::migrate_legacy_data(*paths, *legacy_root); !migration) {
        surface_error(*paths, L"旧版数据迁移未完成", migration.error(), config_exists);
        return 1;
    }
    config_exists = std::filesystem::exists(paths->config, ec);
    if (ec || !config_exists) {
        surface_error(*paths, L"无法读取本地配置", Error::make(
            ErrorCode::ConfigParseFailed, ec ? ec.message() : "迁移后未找到 config.toml"), false);
        return 1;
    }
    auto config = profile::read_config(paths->config);
    if (!config) {
        auto declared_action = profile::read_declared_launcher_action(paths->config);
        if (declared_action && *declared_action == profile::LauncherAction::Diagnose) {
            return diagnose(*paths, nullptr, config.error());
        }
        surface_error(*paths, L"无法启动：配置有误", config.error(), true);
        return 1;
    }
    if (config->schema == 1) {
        auto migrated = profile::migrate_config_file(paths->config, paths->backups);
        if (!migrated) {
            surface_error(*paths, L"配置迁移未完成", migrated.error(), true);
            return 1;
        }
        config = std::move(migrated);
    }
    if (config->launcher.action == profile::LauncherAction::Diagnose) {
        return diagnose(*paths, &*config);
    }

    auto launched = app::launch_elevated_session(*paths, *config);
    if (!launched) {
        if (launched.error().code == ErrorCode::ElevationCancelled) return 0;
        surface_error(*paths, L"启动失败", launched.error(), true);
        return 1;
    }
    return *launched;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return application_main();
}
