// Read-only diagnostic report collection.  Report generation intentionally
// shares no configuration migration or session-recovery path with launch.

#include "app/doctor.hpp"

#include "game/game_adapter.hpp"
#include "platform/win32/display.hpp"
#include "platform/win32/file.hpp"
#include "platform/win32/privilege.hpp"
#include "platform/win32/process.hpp"
#include "platform/win32/text.hpp"
#include "profile/config.hpp"
#include "session/journal.hpp"
#include "version.hpp"

#include <windows.h>

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace hoyoflux::app {
namespace {

enum class CheckStatus { Ok, Warn, Unchecked, Fail };

std::string_view label(CheckStatus status) {
    switch (status) {
    case CheckStatus::Ok: return "[通过] ";
    case CheckStatus::Warn: return "[注意] ";
    case CheckStatus::Unchecked: return "[未检查] ";
    case CheckStatus::Fail: return "[失败] ";
    }
    return "[失败] ";
}

class ReportBuilder {
public:
    void section(std::string_view name) { output_ << "\n" << name << "\n"; }
    void item(CheckStatus status, std::string_view name, std::string_view detail) {
        output_ << label(status) << name;
        if (!detail.empty()) output_ << ": " << detail;
        output_ << "\n";
        if (status == CheckStatus::Fail) ++failures_;
    }
    void line(std::string_view value) { output_ << value << "\n"; }
    [[nodiscard]] std::string finish() && { return std::move(output_).str(); }
    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    std::ostringstream output_;
    int failures_{0};
};

std::string path_utf8(const std::filesystem::path& path) {
    return win32::utf8(path.wstring());
}

void report_system(ReportBuilder& report) {
    report.item(win32::is_elevated() ? CheckStatus::Ok : CheckStatus::Warn,
                "管理员权限",
                win32::is_elevated() ? "当前进程已提权" :
                    "诊断无需提权；正常启动时才会请求 UAC");
}

void report_config(ReportBuilder& report, const AppPaths& paths,
                   const DiagnosticContext& context) {
    if (context.error) {
        report.item(CheckStatus::Fail, "本次错误", context.error->message);
    }
    if (context.config) {
        report.item(CheckStatus::Ok, "config.toml",
                    std::to_string(context.config->profiles.size()) +
                        " 个配置档；只读检查结果由启动端提供");
        return;
    }
    auto config = profile::read_config(paths.config);
    if (config) {
        report.item(CheckStatus::Ok, "config.toml",
                    std::to_string(config->profiles.size()) + " 个配置档");
    } else {
        report.item(CheckStatus::Fail, "config.toml", config.error().message);
    }
}

void report_journal(ReportBuilder& report, const std::filesystem::path& path,
                    std::string_view name) {
    auto journal = session::load_journal(path);
    if (!journal) {
        report.item(CheckStatus::Fail, name, journal.error().message);
    } else if (!journal->has_value()) {
        report.item(CheckStatus::Ok, name, "不存在");
    } else {
        const auto& active = **journal;
        const auto state = win32::inspect_process_liveness(active.pid);
        if (state == win32::ProcessLiveness::Running) {
            report.item(CheckStatus::Warn, name,
                        "关联游戏仍在运行，恢复不会执行");
        } else if (state == win32::ProcessLiveness::Unknown) {
            report.item(CheckStatus::Warn, name,
                        "无法确认关联游戏状态，恢复会停止以保护数据");
        } else {
            report.item(CheckStatus::Warn, name,
                        "存在可恢复记录，正常启动时将先执行恢复");
        }
    }
}

void report_migration(ReportBuilder& report, const AppPaths& paths,
                      const DiagnosticContext& context) {
    if (!context.migration_state.empty()) {
        report.item(CheckStatus::Warn, "迁移状态", context.migration_state);
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths.migration_record, ec)) {
        report.item(ec ? CheckStatus::Unchecked : CheckStatus::Ok, "迁移状态",
                    ec ? ec.message() : "无待处理迁移");
        return;
    }
    auto record = win32::read_file_bytes(paths.migration_record);
    report.item(record ? CheckStatus::Warn : CheckStatus::Fail, "迁移状态",
                record ? "发现待处理迁移记录，下一次正常启动会安全续接" :
                         record.error().message);
}

void report_displays(ReportBuilder& report) {
    auto displays = win32::enumerate_displays();
    if (!displays) {
        report.item(CheckStatus::Fail, "显示器", displays.error().message);
        return;
    }
    std::string summary;
    for (const auto& display : *displays) {
        if (!display.is_attached) continue;
        auto settings = win32::query_current_settings(display.device_name);
        if (!settings) {
            summary += path_utf8(display.device_name) + "（权限不足或无法读取） ";
            continue;
        }
        summary += path_utf8(display.device_name) + " " +
            std::to_string(settings->width) + "x" +
            std::to_string(settings->height) + "@" +
            std::to_string(settings->refresh_rate) + " ";
    }
    report.item(CheckStatus::Ok, "显示器", summary.empty() ? "未检测到已连接显示器" : summary);
}

void report_selection(ReportBuilder& report, const AppPaths& paths,
                      const DiagnosticContext& context) {
    const profile::Config* config = context.config ? &*context.config : nullptr;
    std::optional<profile::Config> local;
    if (!config) {
        auto read = profile::read_config(paths.config);
        if (!read) {
            report.item(CheckStatus::Unchecked, "配置选择", "配置无法读取");
            return;
        }
        local = std::move(*read);
        config = &*local;
    }
    if (config->launcher.profile != "auto") {
        report.item(CheckStatus::Ok, "配置选择",
                    "手动配置档：" + config->launcher.profile +
                        "（不会推演 Auto 匹配）");
        return;
    }
    auto displays = win32::enumerate_displays();
    if (!displays) {
        report.item(CheckStatus::Fail, "Auto 选择", displays.error().message);
        return;
    }
    auto decision = profile::resolve_auto_profile(
        *config, config->launcher.game, *displays);
    if (!decision) {
        report.item(CheckStatus::Fail, "Auto 选择", decision.error().message);
        return;
    }
    report.item(CheckStatus::Ok, "Auto 选择",
                decision->profile.id + (decision->used_fallback ? "（回退）" : "（显示器匹配）"));
    for (const auto& candidate : decision->candidates) {
        report.line("  候选 " + candidate.profile_id + "：特异度=" +
                    std::to_string(candidate.specificity) + "，优先级=" +
                    std::to_string(candidate.priority));
    }
}

void report_games(ReportBuilder& report) {
    for (const GameId game : {GameId::Genshin, GameId::StarRail}) {
        auto adapter = game::make_adapter(game);
        const std::string game_name = std::string(to_string(game));
        auto install = adapter->locate_installation(game::Region::Auto);
        if (!install) {
            report.item(install.error().code == ErrorCode::ProcessNotFound
                            ? CheckStatus::Unchecked : CheckStatus::Warn,
                        game_name, install.error().message);
            continue;
        }
        report.item(CheckStatus::Ok, game_name, path_utf8(install->exe_path));
        const Profile probe;
        const auto capabilities = adapter->capabilities(*install, probe);
        for (const auto& entry : capabilities.entries) {
            report.line("  能力 " + std::string(to_string(entry.capability)) + "：" +
                        std::string(to_string(entry.status)) +
                        (entry.reason.empty() ? "" : "（" + entry.reason + "）"));
        }
    }
}

}  // namespace

Result<std::string> build_diagnostic_report(const AppPaths& paths,
                                            const DiagnosticContext& context) {
    ReportBuilder report;
    report.line("HoyoFlux 诊断报告 " HOYOFLUX_VERSION_STRING);
    report.line("此报告只读：不会迁移文件、恢复会话、修改游戏设置或请求管理员权限。\n");
    report.section("路径");
    report.item(CheckStatus::Ok, "可执行文件", path_utf8(paths.executable));
    report.item(CheckStatus::Ok, "配置", path_utf8(paths.config));
    report.item(CheckStatus::Ok, "数据目录", path_utf8(paths.data));
    report.section("系统");
    report_system(report);
    report.section("配置和状态");
    report_config(report, paths, context);
    report_migration(report, paths, context);
    report_journal(report, paths.journal, "本地恢复记录");
    report_displays(report);
    report_selection(report, paths, context);
    if (context.full) {
        report.section("游戏与能力");
        report_games(report);
    } else {
        report.section("游戏与能力");
        report.item(CheckStatus::Unchecked, "深入扫描", "失败诊断未扫描游戏进程");
    }
    report.line("\n报告中的失败项不改变任何文件；修复配置后请再次双击 HoyoFlux。");
    return std::move(report).finish();
}

Result<void> write_diagnostic_report(const AppPaths& paths,
                                     const DiagnosticContext& context) {
    auto report = build_diagnostic_report(paths, context);
    if (!report) return std::unexpected(report.error());
    return win32::write_file_atomic(paths.diagnostics, *report);
}

}  // namespace hoyoflux::app
