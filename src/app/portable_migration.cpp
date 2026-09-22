#include "app/portable_migration.hpp"

#include "platform/win32/file.hpp"
#include "platform/win32/process.hpp"
#include "profile/config.hpp"
#include "session/journal.hpp"
#include "session/session_engine.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace hoyoflux::app {
namespace {

enum class MigrationStage { Prepared, LocalVerified, SourceCleaned };

struct MigrationRecord {
    MigrationStage stage{MigrationStage::Prepared};
    bool imported_config{false};
    std::string source_hash;
    std::string target_hash;
    std::string backup_name;
};

constexpr std::string_view kRecordHeader = "hoyoflux-portable-migration=1";

std::string stage_name(MigrationStage stage) {
    switch (stage) {
    case MigrationStage::Prepared: return "prepared";
    case MigrationStage::LocalVerified: return "local-verified";
    case MigrationStage::SourceCleaned: return "source-cleaned";
    }
    return "prepared";
}

Result<MigrationStage> parse_stage(std::string_view text) {
    if (text == "prepared") return MigrationStage::Prepared;
    if (text == "local-verified") return MigrationStage::LocalVerified;
    if (text == "source-cleaned") return MigrationStage::SourceCleaned;
    return std::unexpected(Error::make(
        ErrorCode::ConfigParseFailed, "迁移记录包含未知阶段: " + std::string(text)));
}

Result<std::optional<MigrationRecord>> read_record(const AppPaths& paths) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(paths.migration_record, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查迁移记录: " + paths.migration_record.string(),
        static_cast<unsigned long>(ec.value())));
    if (!exists) return std::optional<MigrationRecord>{};

    auto bytes = win32::read_file_bytes(paths.migration_record);
    if (!bytes) return std::unexpected(bytes.error());
    std::istringstream input(*bytes);
    std::string line;
    if (!std::getline(input, line) || line != kRecordHeader) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "迁移记录损坏，已保留原始数据: " + paths.migration_record.string()));
    }
    MigrationRecord record;
    bool has_stage = false;
    bool has_imported = false;
    bool has_source = false;
    bool has_target = false;
    bool has_backup = false;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed, "迁移记录格式错误"));
        const auto key = std::string_view(line).substr(0, separator);
        const auto value = std::string_view(line).substr(separator + 1);
        if (key == "stage") {
            auto stage = parse_stage(value);
            if (!stage) return std::unexpected(stage.error());
            record.stage = *stage;
            has_stage = true;
        } else if (key == "imported_config") {
            if (value == "0") record.imported_config = false;
            else if (value == "1") record.imported_config = true;
            else return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed, "迁移记录 imported_config 无效"));
            has_imported = true;
        } else if (key == "source_sha256") {
            record.source_hash = value;
            has_source = true;
        } else if (key == "target_sha256") {
            record.target_hash = value;
            has_target = true;
        } else if (key == "backup") {
            record.backup_name = value;
            has_backup = true;
        } else {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed, "迁移记录包含未知字段"));
        }
    }
    if (!has_stage || !has_imported || !has_source || !has_target || !has_backup ||
        record.source_hash.size() != 64 || record.target_hash.size() != 64 ||
        record.backup_name.empty() ||
        !record.backup_name.starts_with("appdata-config-") ||
        !record.backup_name.ends_with(".toml") ||
        record.backup_name.find_first_of("/\\") != std::string::npos) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed, "迁移记录不完整，已保留原始数据"));
    }
    return std::optional<MigrationRecord>{std::move(record)};
}

Result<void> save_record(const AppPaths& paths, const MigrationRecord& record) {
    const std::string text = std::string(kRecordHeader) + "\n" +
        "stage=" + stage_name(record.stage) + "\n" +
        "imported_config=" + (record.imported_config ? "1" : "0") + "\n" +
        "source_sha256=" + record.source_hash + "\n" +
        "target_sha256=" + record.target_hash + "\n" +
        "backup=" + record.backup_name + "\n";
    return win32::write_file_atomic(paths.migration_record, text);
}

Result<void> clear_record(const AppPaths& paths) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(paths.migration_record, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查迁移记录: " + paths.migration_record.string(),
        static_cast<unsigned long>(ec.value())));
    if (!exists) return {};
    auto current = win32::read_file_bytes(paths.migration_record);
    if (!current) return std::unexpected(current.error());
    return win32::remove_file_if_unchanged(paths.migration_record, *current);
}

Result<std::string> read_bytes(const std::filesystem::path& path) {
    return win32::read_file_bytes(path);
}

Result<std::filesystem::path> archive_exact(const AppPaths& paths,
                                            std::wstring_view prefix,
                                            std::wstring_view extension,
                                            std::string_view bytes) {
    auto digest = win32::sha256_hex(bytes);
    if (!digest) return std::unexpected(digest.error());
    const auto target = paths.backups /
        (std::wstring(prefix) + L"-" + std::wstring(digest->begin(), digest->end()) +
         std::wstring(extension));
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        if (ec) return std::unexpected(Error::make(
            ErrorCode::OsError, "无法检查迁移备份: " + target.string(),
            static_cast<unsigned long>(ec.value())));
        auto existing = read_bytes(target);
        if (!existing) return std::unexpected(existing.error());
        if (*existing != bytes) return std::unexpected(Error::make(
            ErrorCode::OsError, "迁移备份内容冲突: " + target.string()));
        return target;
    }
    auto saved = win32::write_file_atomic(target, bytes);
    if (!saved) return std::unexpected(saved.error());
    return target;
}

Result<void> verify_journal_safe(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) return std::unexpected(Error::make(
            ErrorCode::OsError, "无法检查恢复记录: " + path.string(),
            static_cast<unsigned long>(ec.value())));
        return {};
    }
    auto journal = session::load_journal(path);
    if (!journal) return std::unexpected(Error::make(
        journal.error().code, "恢复记录无法验证，已保留: " + journal.error().message));
    if (!journal->has_value() || (**journal).pid == 0) return {};
    switch (win32::inspect_process_liveness((**journal).pid)) {
    case win32::ProcessLiveness::Exited: return {};
    case win32::ProcessLiveness::Running:
        return std::unexpected(Error::make(
            ErrorCode::SessionAlreadyActive,
            "恢复记录对应的游戏仍在运行，请先正常退出游戏"));
    case win32::ProcessLiveness::Unknown:
        return std::unexpected(Error::make(
            ErrorCode::SessionAlreadyActive,
            "无法确认恢复记录对应的游戏是否已退出，已停止迁移以保护数据"));
    }
    return {};
}

Result<void> preflight_journals(const AppPaths& paths,
                                const std::filesystem::path& legacy_root) {
    const auto old_journal = legacy_root / L"state" / L"active-session.json";
    if (auto checked = verify_journal_safe(old_journal); !checked) return checked;
    if (auto checked = verify_journal_safe(paths.journal); !checked) return checked;

    std::error_code ec;
    const bool old_exists = std::filesystem::exists(old_journal, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查旧版恢复记录: " + old_journal.string(),
        static_cast<unsigned long>(ec.value())));
    const bool local_exists = std::filesystem::exists(paths.journal, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查本地恢复记录: " + paths.journal.string(),
        static_cast<unsigned long>(ec.value())));
    if (!old_exists || !local_exists) return {};
    auto old_bytes = read_bytes(old_journal);
    auto local_bytes = read_bytes(paths.journal);
    if (!old_bytes) return std::unexpected(old_bytes.error());
    if (!local_bytes) return std::unexpected(local_bytes.error());
    if (*old_bytes != *local_bytes) return std::unexpected(Error::make(
        ErrorCode::JournalCorrupt,
        "本地与 AppData 同时存在不同的恢复记录，已全部保留"));
    return {};
}

Result<void> migrate_journal(const AppPaths& paths,
                             const std::filesystem::path& source,
                             bool* imported) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        if (ec) return std::unexpected(Error::make(
            ErrorCode::OsError, "无法检查旧版恢复记录: " + source.string(),
            static_cast<unsigned long>(ec.value())));
        return {};
    }
    auto source_bytes = read_bytes(source);
    if (!source_bytes) return std::unexpected(source_bytes.error());
    const bool local_exists = std::filesystem::exists(paths.journal, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查本地恢复记录: " + paths.journal.string(),
        static_cast<unsigned long>(ec.value())));
    if (local_exists) {
        auto local_bytes = read_bytes(paths.journal);
        if (!local_bytes) return std::unexpected(local_bytes.error());
        if (*local_bytes != *source_bytes) return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "本地与 AppData 同时存在不同的恢复记录，已全部保留"));
    } else {
        if (auto written = win32::write_file_atomic(paths.journal, *source_bytes); !written)
            return written;
        if (auto verified = session::load_journal(paths.journal); !verified)
            return std::unexpected(verified.error());
        *imported = true;
    }
    if (auto archived = archive_exact(paths, L"appdata-active-session", L".json", *source_bytes);
        !archived) return std::unexpected(archived.error());
    return win32::remove_file_if_unchanged(source, *source_bytes);
}

Result<void> complete_config_migration(const AppPaths& paths,
                                       const std::filesystem::path& source,
                                       std::string_view source_bytes,
                                       MigrationRecord* record,
                                       MigrationResult* outcome) {
    auto source_hash = win32::sha256_hex(source_bytes);
    if (!source_hash) return std::unexpected(source_hash.error());
    if (*source_hash != record->source_hash) return std::unexpected(Error::make(
        ErrorCode::ConfigParseFailed,
        "旧版配置在迁移期间已变化，已保留双方文件"));
    const auto backup = paths.backups /
        std::filesystem::path(std::wstring(record->backup_name.begin(),
                                           record->backup_name.end()));
    auto backup_bytes = read_bytes(backup);
    if (!backup_bytes || *backup_bytes != source_bytes) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "迁移记录对应的原始备份缺失或内容不一致，已停止以保护数据"));
    }

    std::error_code ec;
    const bool local_exists = std::filesystem::exists(paths.config, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查本地配置: " + paths.config.string(),
        static_cast<unsigned long>(ec.value())));

    if (record->stage == MigrationStage::Prepared && record->imported_config) {
        if (!local_exists) {
            if (auto copied = win32::write_file_atomic(paths.config, source_bytes); !copied)
                return copied;
        } else {
            auto local_bytes = read_bytes(paths.config);
            if (!local_bytes) return std::unexpected(local_bytes.error());
            auto local_hash = win32::sha256_hex(*local_bytes);
            if (!local_hash) return std::unexpected(local_hash.error());
            if (*local_hash != record->source_hash && *local_hash != record->target_hash) {
                return std::unexpected(Error::make(
                    ErrorCode::ConfigParseFailed,
                    "迁移中断后本地配置已被修改，已保留双方文件"));
            }
        }
    }
    if (!local_exists && !record->imported_config) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "迁移记录表明本地配置原已存在，但现在找不到它，已停止以保护数据"));
    }

    auto local = profile::read_config(paths.config);
    if (!local) return std::unexpected(Error::make(
        local.error().code, "本地配置无法验证，AppData 源文件已保留: " + local.error().message));
    if (record->imported_config && local->schema == 1) {
        auto migrated = profile::migrate_config_file(paths.config, paths.backups);
        if (!migrated) return std::unexpected(migrated.error());
    }
    auto final_bytes = read_bytes(paths.config);
    if (!final_bytes) return std::unexpected(final_bytes.error());
    auto final_hash = win32::sha256_hex(*final_bytes);
    if (!final_hash) return std::unexpected(final_hash.error());
    if (record->imported_config && *final_hash != record->target_hash) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "迁移后的本地配置与预期内容不同，已保留双方文件"));
    }

    record->stage = MigrationStage::LocalVerified;
    if (auto saved = save_record(paths, *record); !saved) return saved;
    if (auto removed = win32::remove_file_if_unchanged(source, source_bytes); !removed)
        return removed;
    record->stage = MigrationStage::SourceCleaned;
    if (auto saved = save_record(paths, *record); !saved) return saved;
    if (auto cleared = clear_record(paths); !cleared) return cleared;
    outcome->removed_legacy_config = true;
    outcome->imported_config = record->imported_config;
    return {};
}

void remove_if_empty(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) &&
        std::filesystem::is_empty(path, ec)) {
        std::filesystem::remove(path, ec);
    }
}

}  // namespace

Result<MigrationResult> migrate_legacy_data(
    const AppPaths& paths,
    const std::filesystem::path& legacy_root) {
    MigrationResult outcome;
    auto lease = session::SessionLease::acquire();
    if (!lease) return std::unexpected(lease.error());

    std::error_code ec;
    const bool legacy_exists = std::filesystem::exists(legacy_root, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查旧版 AppData 目录: " + legacy_root.string(),
        static_cast<unsigned long>(ec.value())));

    auto pending = read_record(paths);
    if (!pending) return std::unexpected(pending.error());
    if (!legacy_exists) {
        if (pending->has_value()) {
            if ((**pending).stage == MigrationStage::SourceCleaned) {
                if (auto cleared = clear_record(paths); !cleared) return std::unexpected(cleared.error());
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::ConfigParseFailed,
                    "迁移记录存在但旧版 AppData 目录已消失，无法确认数据完整性"));
            }
        }
        return outcome;
    }
    if (auto checked = preflight_journals(paths, legacy_root); !checked)
        return std::unexpected(checked.error());

    const auto old_config = legacy_root / L"config.toml";
    const bool source_config_exists = std::filesystem::exists(old_config, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法检查旧版配置: " + old_config.string(),
        static_cast<unsigned long>(ec.value())));
    if (source_config_exists) {
        auto bytes = read_bytes(old_config);
        if (!bytes) return std::unexpected(bytes.error());
        auto source_hash = win32::sha256_hex(*bytes);
        if (!source_hash) return std::unexpected(source_hash.error());

        MigrationRecord record;
        if (pending->has_value()) {
            record = **pending;
            if (record.source_hash != *source_hash) return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                "迁移中断后旧版配置已被修改，已保留双方文件"));
        } else {
            // Preserve the only original copy before any conversion attempt.
            // An invalid legacy document is therefore still recoverable and
            // can be fixed by the user without losing its raw bytes.
            auto archive = archive_exact(paths, L"appdata-config", L".toml", *bytes);
            if (!archive) return std::unexpected(archive.error());
            auto parsed = profile::parse_config(*bytes, paths.root);
            if (!parsed) return std::unexpected(Error::make(
                parsed.error().code, "旧版配置无法验证，未迁移或删除: " + parsed.error().message));
            std::string expected = *bytes;
            if (parsed->schema == 1) {
                const auto converted = profile::serialize_config(*parsed);
                auto reparsed = profile::parse_config(converted, paths.root);
                if (!reparsed || !profile::configs_equivalent(*parsed, *reparsed)) {
                    return std::unexpected(Error::make(
                        ErrorCode::ConfigParseFailed, "无法确认旧版配置转换后的行为一致"));
                }
                expected = converted;
            }
            auto target_hash = win32::sha256_hex(expected);
            if (!target_hash) return std::unexpected(target_hash.error());
            std::error_code local_ec;
            const bool local_exists = std::filesystem::exists(paths.config, local_ec);
            if (local_ec) return std::unexpected(Error::make(
                ErrorCode::OsError, "无法检查本地配置: " + paths.config.string(),
                static_cast<unsigned long>(local_ec.value())));
            record.imported_config = !local_exists;
            record.source_hash = *source_hash;
            record.target_hash = *target_hash;
            record.backup_name = archive->filename().string();
            record.stage = MigrationStage::Prepared;
            if (auto saved = save_record(paths, record); !saved) return std::unexpected(saved.error());
        }
        if (auto completed = complete_config_migration(paths, old_config, *bytes,
                                                        &record, &outcome);
            !completed) return std::unexpected(completed.error());
    } else if (pending->has_value()) {
        if ((**pending).stage == MigrationStage::SourceCleaned) {
            if (auto cleared = clear_record(paths); !cleared) return std::unexpected(cleared.error());
        } else {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                "迁移记录存在但旧版配置消失，已停止以保护本地数据"));
        }
    }

    const auto old_journal = legacy_root / L"state" / L"active-session.json";
    if (auto migrated = migrate_journal(paths, old_journal, &outcome.imported_journal);
        !migrated) return std::unexpected(migrated.error());

    for (std::filesystem::directory_iterator iterator(legacy_root, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (!iterator->is_regular_file(ec)) continue;
        const std::wstring name = iterator->path().filename().wstring();
        if (!name.starts_with(L"config.toml.bak.")) continue;
        auto bytes = read_bytes(iterator->path());
        if (!bytes) return std::unexpected(bytes.error());
        if (auto archived = archive_exact(paths, L"appdata-legacy-backup", L".toml", *bytes);
            !archived) return std::unexpected(archived.error());
        if (auto removed = win32::remove_file_if_unchanged(iterator->path(), *bytes); !removed)
            return std::unexpected(removed.error());
    }
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法枚举旧版 AppData 目录: " + legacy_root.string(),
        static_cast<unsigned long>(ec.value())));

    remove_if_empty(legacy_root / L"state");
    remove_if_empty(legacy_root);
    return outcome;
}

}  // namespace hoyoflux::app
