#include "profile/config.hpp"

#include "platform/win32/text.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#include <windows.h>

namespace hoyoflux::profile {
namespace {

constexpr int kCurrentPresetRevision = 3;

Profile make_profile_template(std::string id, GameId game) {
    Profile profile;
    profile.id = std::move(id);
    profile.game = game;
    return profile;
}

// Optional variants: nullopt when the key is absent.
std::optional<bool> opt_bool(const toml::table& table, std::string_view key) {
    if (const auto* node = table.get(key); node && node->is_boolean()) {
        return node->value<bool>();
    }
    return std::nullopt;
}

std::optional<int64_t> opt_int(const toml::table& table, std::string_view key) {
    if (const auto* node = table.get(key); node && node->is_integer()) {
        return node->value<int64_t>();
    }
    return std::nullopt;
}

std::optional<double> opt_double(const toml::table& table, std::string_view key) {
    if (const auto* node = table.get(key);
        node && (node->is_floating_point() || node->is_integer())) {
        return node->value<double>();
    }
    return std::nullopt;
}

std::optional<std::string> opt_string(const toml::table& table, std::string_view key) {
    if (const auto* node = table.get(key); node && node->is_string()) {
        return node->value<std::string>();
    }
    return std::nullopt;
}

template <typename Predicate>
Result<void> validate_field_type(const toml::table& table,
                                 std::string_view key, Predicate&& predicate,
                                 std::string_view expected,
                                 std::string_view context,
                                 ErrorCode error_code = ErrorCode::ProfileInvalid) {
    const auto* node = table.get(key);
    if (node != nullptr && !predicate(*node)) {
        return std::unexpected(Error::make(
            error_code, std::string(context) + ": " + std::string(key) +
                            " must be " + std::string(expected)));
    }
    return {};
}

Result<void> validate_profile_types(const toml::table& body,
                                    std::string_view context) {
    const auto is_string = [](const toml::node& node) { return node.is_string(); };
    const auto is_bool = [](const toml::node& node) { return node.is_boolean(); };
    const auto is_integer = [](const toml::node& node) { return node.is_integer(); };
    const auto is_number = [](const toml::node& node) {
        return node.is_integer() || node.is_floating_point();
    };
    const auto is_table = [](const toml::node& node) { return node.is_table(); };
    const auto check = [&](const toml::table& table, std::string_view key,
                           auto&& predicate, std::string_view expected) {
        return validate_field_type(table, key, std::forward<decltype(predicate)>(predicate),
                                   expected, context);
    };
    if (auto result = check(body, "game", is_string, "a string"); !result) {
        return result;
    }
    if (auto result = validate_field_type(body, "match",
                                          [&](const toml::node& node) {
                                              return node.is_string() || node.is_table();
                                          },
                                          "a string or table", context);
        !result) {
        return result;
    }
    if (auto result = check(body, "render", is_table, "a table"); !result) {
        return result;
    }
    if (auto result = check(body, "runtime", is_table, "a table"); !result) {
        return result;
    }
    if (auto result = check(body, "ui", is_table, "a table"); !result) {
        return result;
    }

    if (const auto* match = body.get("match"); match && match->is_table()) {
        const auto& table = *match->as_table();
        if (auto result = check(table, "auto_select", is_bool, "a boolean"); !result) return result;
        if (auto result = check(table, "device_name", is_string, "a string"); !result) return result;
        if (auto result = check(table, "resolution", is_string, "a string"); !result) return result;
        if (auto result = check(table, "aspect_ratio", is_number, "a number"); !result) return result;
        if (auto result = check(table, "portrait", is_bool, "a boolean"); !result) return result;
        if (auto result = check(table, "priority", is_integer, "an integer"); !result) return result;
    }
    if (const auto* render = body.get("render"); render && render->is_table()) {
        const auto& table = *render->as_table();
        if (auto result = check(table, "resolution", is_string, "a string"); !result) return result;
        if (auto result = check(table, "fullscreen", is_string, "a string"); !result) return result;
        if (auto result = check(table, "persistence", is_string, "a string"); !result) return result;
        if (auto result = check(table, "monitor", is_integer, "an integer"); !result) return result;
    }
    if (const auto* runtime = body.get("runtime"); runtime && runtime->is_table()) {
        const auto& table = *runtime->as_table();
        if (auto result = check(table, "fps", is_integer, "an integer"); !result) return result;
        if (auto result = check(table, "priority", is_string, "a string"); !result) return result;
        if (auto result = check(table, "hotkeys", is_bool, "a boolean"); !result) return result;
        if (auto result = check(table, "power_save", is_table, "a table"); !result) return result;
        if (const auto* power_save = table.get("power_save");
            power_save && power_save->is_table()) {
            const auto& ps = *power_save->as_table();
            if (auto result = check(ps, "enabled", is_bool, "a boolean"); !result) return result;
            if (auto result = check(ps, "fps", is_integer, "an integer"); !result) return result;
        }
    }
    if (const auto* ui = body.get("ui"); ui && ui->is_table()) {
        const auto& table = *ui->as_table();
        if (auto result = check(table, "mobile_ui", is_bool, "a boolean"); !result) return result;
        if (auto result = check(table, "dpi_scale", is_number, "a number"); !result) return result;
    }
    return {};
}

// std::from_chars throughout: a malformed value is a reported error, never
// an escaping std::invalid_argument (plan 18.1).
Result<Resolution> parse_resolution(std::string_view text,
                                    std::string_view context) {
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= text.size()) {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid,
            std::string(context) + ": resolution must look like \"2560x1440\""));
    }
    Resolution resolution;
    const auto* begin = text.data();
    const auto* mid = begin + x;
    const auto* end = begin + text.size();
    const auto [w_ptr, w_ec] = std::from_chars(begin, mid, resolution.width);
    const auto [h_ptr, h_ec] = std::from_chars(mid + 1, end, resolution.height);
    if (w_ec != std::errc{} || w_ptr != mid || h_ec != std::errc{} ||
        h_ptr != end) {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid,
            std::string(context) + ": resolution must look like \"2560x1440\""));
    }
    if (resolution.empty()) {
        return std::unexpected(Error::make(ErrorCode::ProfileInvalid,
                                           std::string(context) +
                                               ": resolution is zero"));
    }
    return resolution;
}

Result<Profile> parse_profile(std::string id, const toml::table& body) {
    const std::string context = "profile '" + id + "'";

    if (auto valid_types = validate_profile_types(body, context); !valid_types) {
        return std::unexpected(valid_types.error());
    }

    Profile profile = make_profile_template(std::move(id), GameId::Genshin);

    if (const auto game = opt_string(body, "game")) {
        if (*game == "genshin") {
            profile.game = GameId::Genshin;
        } else if (*game == "starrail") {
            profile.game = GameId::StarRail;
        } else {
            return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid,
                context + ": game must be \"genshin\" or \"starrail\""));
        }
    } else {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid, context + ": missing required key 'game'"));
    }

    // match = "manual" | "auto" is the legacy string form and still
    // accepted; the structured [profiles.X.match] table is preferred (F8).
    if (const auto match = opt_string(body, "match")) {
        if (*match == "manual") {
            profile.match.auto_select = false;
        } else if (*match == "auto") {
            profile.match.auto_select = true;
        } else {
            return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid,
                context + ": match must be \"manual\" or \"auto\""));
        }
    }
    if (const auto* match_node = body.get("match");
        match_node && match_node->is_table()) {
        const auto& table = *match_node->as_table();
        if (const auto auto_select = opt_bool(table, "auto_select")) {
            profile.match.auto_select = *auto_select;
        }
        if (const auto device = opt_string(table, "device_name")) {
            auto device_name = win32::utf16(*device);
            if (!device_name) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": match.device_name is not valid UTF-8"));
            }
            profile.match.device_name = std::move(*device_name);
        }
        if (const auto resolution = opt_string(table, "resolution")) {
            auto parsed = parse_resolution(*resolution, context + " match");
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            profile.match.resolution = *parsed;
        }
        if (const auto aspect = opt_double(table, "aspect_ratio")) {
            if (!std::isfinite(*aspect) || *aspect <= 0.0 || *aspect > 10.0) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": match.aspect_ratio must be within (0, 10]"));
            }
            profile.match.aspect_ratio = static_cast<float>(*aspect);
        }
        if (const auto portrait = opt_bool(table, "portrait")) {
            profile.match.portrait = *portrait;
        }
        if (const auto priority = opt_int(table, "priority")) {
            if (*priority < std::numeric_limits<int>::min() ||
                *priority > std::numeric_limits<int>::max()) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": match.priority is outside the int range"));
            }
            profile.match.priority = static_cast<int>(*priority);
        }
    }

    if (const auto* render = body.get("render"); render && render->is_table()) {
        const auto& table = *render->as_table();
        if (const auto resolution = opt_string(table, "resolution")) {
            auto parsed = parse_resolution(*resolution, context);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            profile.render.resolution = *parsed;
        }
        if (const auto fullscreen = opt_string(table, "fullscreen")) {
            if (*fullscreen == "exclusive") {
                profile.render.fullscreen = FullscreenMode::Exclusive;
            } else if (*fullscreen == "borderless") {
                profile.render.fullscreen = FullscreenMode::Borderless;
            } else if (*fullscreen == "windowed") {
                profile.render.fullscreen = FullscreenMode::Windowed;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": fullscreen must be exclusive|borderless|windowed"));
            }
        }
        if (const auto persistence = opt_string(table, "persistence")) {
            if (*persistence == "session") {
                profile.render.persistence = ResolutionPersistence::Session;
            } else if (*persistence == "persistent") {
                profile.render.persistence = ResolutionPersistence::Persistent;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": persistence must be session|persistent"));
            }
        }
        if (const auto monitor = opt_int(table, "monitor")) {
            if (*monitor < 0 || *monitor > 63) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": monitor must be within [0, 63]"));
            }
            profile.render.monitor = static_cast<uint32_t>(*monitor);
        }
    }

    if (const auto* runtime = body.get("runtime"); runtime && runtime->is_table()) {
        const auto& table = *runtime->as_table();
        if (const auto fps = opt_int(table, "fps")) {
            if (*fps < 10 || *fps > 1000) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": fps must be within [10, 1000]"));
            }
            profile.runtime.fps = static_cast<uint32_t>(*fps);
        }
        if (const auto priority = opt_string(table, "priority")) {
            if (*priority == "realtime") {
                profile.runtime.priority = ProcessPriority::Realtime;
            } else if (*priority == "high") {
                profile.runtime.priority = ProcessPriority::High;
            } else if (*priority == "above_normal") {
                profile.runtime.priority = ProcessPriority::AboveNormal;
            } else if (*priority == "normal") {
                profile.runtime.priority = ProcessPriority::Normal;
            } else if (*priority == "below_normal") {
                profile.runtime.priority = ProcessPriority::BelowNormal;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": unknown priority '" + *priority + "'"));
            }
        }
        if (const auto hotkeys = opt_bool(table, "hotkeys")) {
            profile.runtime.hotkeys = *hotkeys;
        }
        if (const auto* power_save = table.get("power_save");
            power_save && power_save->is_table()) {
            const auto& ps = *power_save->as_table();
            if (const auto enabled = opt_bool(ps, "enabled")) {
                profile.runtime.power_save =
                    *enabled ? PowerSavePolicy::Enabled : PowerSavePolicy::Disabled;
            }
            if (const auto fps = opt_int(ps, "fps")) {
                if (*fps < 1 || *fps > 1000) {
                    return std::unexpected(Error::make(
                        ErrorCode::ProfileInvalid,
                        context + ": power_save fps must be within [1, 1000]"));
                }
                profile.runtime.power_save_fps = static_cast<uint32_t>(*fps);
            }
        }
    }

    if (const auto* ui = body.get("ui"); ui && ui->is_table()) {
        const auto& table = *ui->as_table();
        if (const auto mobile = opt_bool(table, "mobile_ui")) {
            profile.ui.mobile_ui = *mobile;
        }
        if (const auto dpi = opt_double(table, "dpi_scale")) {
            if (!std::isfinite(*dpi) || *dpi < 0.25 || *dpi > 4.0) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": dpi_scale must be within [0.25, 4.0]"));
            }
            profile.ui.dpi_scale = static_cast<float>(*dpi);
        }
    }

    return profile;
}

Result<toml::table> parse_document(std::string_view toml_text) {
    try {
        return toml::parse(toml_text);
    } catch (const toml::parse_error& error) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "TOML parse error: " + std::string(error.description())));
    }
}

[[nodiscard]] bool is_legacy_ipad_match(const toml::table& profile) {
    const auto* game = profile.get("game");
    if (game == nullptr || !game->is_string() ||
        game->value<std::string>() != std::optional<std::string>{"genshin"}) {
        return false;
    }

    const auto* match_node = profile.get("match");
    if (match_node == nullptr || !match_node->is_table()) {
        return false;
    }
    const auto& match = *match_node->as_table();
    const auto* auto_select = match.get("auto_select");
    const auto* portrait = match.get("portrait");
    if (auto_select == nullptr || !auto_select->is_boolean() ||
        !auto_select->value<bool>().value_or(false) || portrait == nullptr ||
        !portrait->is_boolean() || portrait->value<bool>().value_or(true)) {
        return false;
    }

    // The generated legacy preset contained only auto_select and portrait,
    // with an optional explicit zero priority. Any extra key is treated as a
    // user customization and is therefore left untouched.
    if (match.size() != 2 && match.size() != 3) {
        return false;
    }
    if (const auto* priority = match.get("priority")) {
        if (!priority->is_integer() || priority->value<int64_t>().value_or(-1) != 0) {
            return false;
        }
    }
    for (const auto& [key, value] : match) {
        (void)value;
        if (key != "auto_select" && key != "portrait" && key != "priority") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_exact_keys(
    const toml::table& table,
    std::initializer_list<std::string_view> expected_keys) {
    if (table.size() != expected_keys.size()) {
        return false;
    }
    for (const auto key : expected_keys) {
        if (table.get(key) == nullptr) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_legacy_xiaomi_profile(const toml::table& profile) {
    // This is the exact revision-2 built-in shape. Any extra key or changed
    // value means the user owns the profile and it must not be overwritten.
    if (!has_exact_keys(profile, {"game", "render", "runtime", "ui"})) {
        return false;
    }
    const auto* game = profile.get("game");
    if (game == nullptr || !game->is_string() ||
        game->value<std::string>() != std::optional<std::string>{"genshin"}) {
        return false;
    }

    const auto* render_node = profile.get("render");
    if (render_node == nullptr || !render_node->is_table() ||
        !has_exact_keys(*render_node->as_table(), {"resolution", "persistence"})) {
        return false;
    }
    const auto& render = *render_node->as_table();
    const auto resolution = opt_string(render, "resolution");
    const auto persistence = opt_string(render, "persistence");
    if (!resolution || *resolution != "1220x2712" || !persistence ||
        *persistence != "session") {
        return false;
    }

    const auto* runtime_node = profile.get("runtime");
    if (runtime_node == nullptr || !runtime_node->is_table() ||
        !has_exact_keys(*runtime_node->as_table(), {"fps"})) {
        return false;
    }
    const auto& runtime = *runtime_node->as_table();
    const auto fps = opt_int(runtime, "fps");
    if (!fps || *fps != 120) {
        return false;
    }

    const auto* ui_node = profile.get("ui");
    if (ui_node == nullptr || !ui_node->is_table() ||
        !has_exact_keys(*ui_node->as_table(), {"mobile_ui", "dpi_scale"})) {
        return false;
    }
    const auto& ui = *ui_node->as_table();
    const auto mobile_ui = opt_bool(ui, "mobile_ui");
    const auto dpi_scale = opt_double(ui, "dpi_scale");
    return mobile_ui && *mobile_ui && dpi_scale && *dpi_scale == 2.75;
}

void add_default_launcher(toml::table& root) {
    toml::table launcher;
    launcher.insert("game", std::string{"genshin"});
    launcher.insert("profile", std::string{"auto"});
    launcher.insert("region", std::string{"auto"});
    launcher.insert("notifications", true);
    root.insert_or_assign("launcher", std::move(launcher));
}

void add_default_game_defaults(toml::table& root) {
    toml::table defaults;
    defaults.insert("genshin", std::string{"desktop"});
    defaults.insert("starrail", std::string{"starrail_desktop"});
    root.insert_or_assign("defaults", std::move(defaults));
}

void migrate_legacy_ipad(toml::table& root) {
    auto* profiles_node = root.get("profiles");
    if (profiles_node == nullptr || !profiles_node->is_table()) {
        return;
    }
    auto* ipad_node = profiles_node->as_table()->get("ipad");
    if (ipad_node == nullptr || !ipad_node->is_table() ||
        !is_legacy_ipad_match(*ipad_node->as_table())) {
        return;
    }

    auto& match = *ipad_node->as_table()->get("match")->as_table();
    match.erase("portrait");
    match.insert_or_assign("resolution", std::string{"2266x1488"});
    match.insert_or_assign("priority", 100);
}

void migrate_legacy_xiaomi(toml::table& root) {
    auto* profiles_node = root.get("profiles");
    if (profiles_node == nullptr || !profiles_node->is_table()) {
        return;
    }
    auto* xiaomi_node = profiles_node->as_table()->get("xiaomi");
    if (xiaomi_node == nullptr || !xiaomi_node->is_table() ||
        !is_legacy_xiaomi_profile(*xiaomi_node->as_table())) {
        return;
    }

    auto& xiaomi = *xiaomi_node->as_table();
    toml::table match;
    match.insert("auto_select", true);
    match.insert("resolution", std::string{"2656x1220"});
    match.insert("priority", 100);
    xiaomi.insert_or_assign("match", std::move(match));

    auto& render = *xiaomi.get("render")->as_table();
    render.insert_or_assign("resolution", std::string{"2656x1220"});
}

[[nodiscard]] std::filesystem::path sibling_path(
    const std::filesystem::path& path, std::wstring_view suffix) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path{"."}
                                                   : path.parent_path();
    return parent / (path.filename().wstring() + std::wstring{suffix});
}

Result<void> create_migration_backup(const std::filesystem::path& path,
                                     int revision) {
    const auto backup = sibling_path(
        path, L".bak.v" + std::to_wstring(revision));
    std::error_code ec;
    if (std::filesystem::exists(backup, ec)) {
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::OsError,
                "cannot inspect config backup '" + backup.string() + "': " +
                    ec.message(),
                static_cast<unsigned long>(ec.value())));
        }
        if (!std::filesystem::is_regular_file(backup, ec) || ec) {
            return std::unexpected(Error::make(
                ErrorCode::OsError,
                "config backup path is not a regular file: " + backup.string(),
                static_cast<unsigned long>(ec.value())));
        }
        return {};
    }
    if (!std::filesystem::copy_file(path, backup,
                                    std::filesystem::copy_options::none, ec)) {
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "cannot create config backup '" + backup.string() + "': " +
                ec.message(),
            static_cast<unsigned long>(ec.value())));
    }
    return {};
}

Result<void> write_migrated_document(const std::filesystem::path& path,
                                     const toml::table& root) {
    const auto temp = sibling_path(path, L".tmp");
    std::ostringstream serialized;
    serialized << root << '\n';
    const auto text = serialized.str();

    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(Error::make(
                ErrorCode::OsError,
                "cannot write migrated config temporary file '" +
                    temp.string() + "'"));
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            return std::unexpected(Error::make(
                ErrorCode::OsError,
                "cannot finish migrated config temporary file '" +
                    temp.string() + "'"));
        }
    }

    if (!ReplaceFileW(path.c_str(), temp.c_str(), nullptr,
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        const auto error = GetLastError();
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "cannot atomically replace config '" + path.string() + "'",
            error));
    }
    return {};
}

Result<Config> parse_config_root(const toml::table& root) {
    Config config;
    const auto is_integer = [](const toml::node& node) { return node.is_integer(); };
    const auto is_string = [](const toml::node& node) { return node.is_string(); };
    const auto is_table = [](const toml::node& node) { return node.is_table(); };
    const auto is_bool = [](const toml::node& node) { return node.is_boolean(); };
    const auto check = [&](const toml::table& table, std::string_view key,
                           auto&& predicate, std::string_view expected) {
        return validate_field_type(table, key,
                                   std::forward<decltype(predicate)>(predicate),
                                   expected, "config", ErrorCode::ConfigParseFailed);
    };
    if (auto result = check(root, "schema", is_integer, "an integer"); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = check(root, "preset_revision", is_integer, "an integer"); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = check(root, "profiles", is_table, "a table"); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = check(root, "launcher", is_table, "a table"); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = check(root, "default_profile", is_string, "a string"); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = check(root, "defaults", is_table, "a table"); !result) {
        return std::unexpected(result.error());
    }
    if (const auto* launcher = root.get("launcher"); launcher && launcher->is_table()) {
        const auto& table = *launcher->as_table();
        if (auto result = check(table, "game", is_string, "a string"); !result) return std::unexpected(result.error());
        if (auto result = check(table, "profile", is_string, "a string"); !result) return std::unexpected(result.error());
        if (auto result = check(table, "region", is_string, "a string"); !result) return std::unexpected(result.error());
        if (auto result = check(table, "notifications", is_bool, "a boolean"); !result) return std::unexpected(result.error());
    }
    if (const auto* defaults = root.get("defaults"); defaults && defaults->is_table()) {
        const auto& table = *defaults->as_table();
        if (auto result = check(table, "genshin", is_string, "a string"); !result) return std::unexpected(result.error());
        if (auto result = check(table, "starrail", is_string, "a string"); !result) return std::unexpected(result.error());
    }
    // Forward-compatibility key (plan 18.4): absent = schema 1. A future
    // schema bumps this and migrates old files on load.
    if (const auto schema = root.get("schema");
        schema && schema->is_integer()) {
        const auto value = schema->value<int64_t>().value_or(0);
        if (value != 1) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                "unsupported config schema " +
                    std::to_string(static_cast<long long>(value)) +
                    " (this build understands schema 1); update HoyoFlux"));
        }
    }
    if (const auto preset_revision = root.get("preset_revision");
        preset_revision != nullptr) {
        if (!preset_revision->is_integer()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                "preset_revision must be an integer"));
        }
        const auto value = preset_revision->value<int64_t>().value_or(0);
        if (value < 1 || value > kCurrentPresetRevision) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                "unsupported preset revision " +
                    std::to_string(static_cast<long long>(value)) +
                    " (this build understands revisions 1-3); update HoyoFlux"));
        }
        config.preset_revision = static_cast<int>(value);
    }
    if (const auto* profiles = root.get("profiles");
        profiles && profiles->is_table()) {
        for (auto&& [key, value] : *profiles->as_table()) {
            if (!value.is_table()) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    "profile '" + std::string(key.str()) + "' must be a table"));
            }
            auto profile =
                parse_profile(std::string(key.str()), *value.as_table());
            if (!profile) {
                return std::unexpected(profile.error());
            }
            config.profiles.push_back(std::move(*profile));
        }
    }
    if (const auto* launcher = root.get("launcher");
        launcher && launcher->is_table()) {
        const auto& table = *launcher->as_table();
        if (const auto game = opt_string(table, "game")) {
            if (*game == "genshin") {
                config.launcher.game = GameId::Genshin;
            } else if (*game == "starrail") {
                config.launcher.game = GameId::StarRail;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::ConfigParseFailed,
                    "launcher.game must be \"genshin\" or \"starrail\""));
            }
        }
        if (const auto profile = opt_string(table, "profile")) {
            config.launcher.profile = *profile;
        }
        if (const auto region = opt_string(table, "region")) {
            if (*region == "auto") {
                config.launcher.region = LauncherRegion::Auto;
            } else if (*region == "cn") {
                config.launcher.region = LauncherRegion::Cn;
            } else if (*region == "global") {
                config.launcher.region = LauncherRegion::Global;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::ConfigParseFailed,
                    "launcher.region must be auto|cn|global"));
            }
        }
        if (const auto notifications = opt_bool(table, "notifications")) {
            config.launcher.notifications = *notifications;
        }
    }
    if (const auto* default_profile = root.get("default_profile");
        default_profile && default_profile->is_string()) {
        config.default_profile = *default_profile->value<std::string>();
    }
    if (const auto* defaults = root.get("defaults");
        defaults && defaults->is_table()) {
        if (const auto value = opt_string(*defaults->as_table(), "genshin")) {
            config.genshin_default = *value;
        }
        if (const auto value = opt_string(*defaults->as_table(), "starrail")) {
            config.starrail_default = *value;
        }
    }
    return config;
}

}  // namespace

std::string default_config_toml() {
    return R"(# HoyoFlux configuration. See `hoyoflux profile list` for the parsed view.
schema = 1
preset_revision = 3
default_profile = "desktop"

[launcher]
game = "genshin"
profile = "auto"
region = "auto"
notifications = true

[defaults]
genshin = "desktop"
starrail = "starrail_desktop"

[profiles.desktop]
game = "genshin"

[profiles.desktop.render]
persistence = "session"

[profiles.desktop.runtime]
fps = 120
priority = "normal"

[profiles.desktop.runtime.power_save]
enabled = false
fps = 30

[profiles.desktop.ui]
mobile_ui = false

[profiles.ipad]
game = "genshin"

[profiles.ipad.match]
auto_select = true
resolution = "2266x1488"
priority = 100

[profiles.ipad.render]
resolution = "2266x1488"
persistence = "session"

[profiles.ipad.runtime]
fps = 60

[profiles.ipad.ui]
mobile_ui = true
dpi_scale = 2.0

[profiles.xiaomi]
game = "genshin"

[profiles.xiaomi.match]
auto_select = true
resolution = "2656x1220"
priority = 100

[profiles.xiaomi.render]
resolution = "2656x1220"
persistence = "session"

[profiles.xiaomi.runtime]
fps = 120

[profiles.xiaomi.ui]
mobile_ui = true
dpi_scale = 2.75

[profiles.starrail_desktop]
game = "starrail"

[profiles.starrail_desktop.runtime]
fps = 120
)";
}

Result<Config> parse_config(std::string_view toml_text) {
    auto parsed = parse_document(toml_text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return parse_config_root(*parsed);
}

Result<Config> load_config(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // First run (plan 18.3): materialize the built-in default document
        // so the user has a real file to edit. A failed write (read-only
        // directory) is not fatal - the defaults still parse and the next
        // run tries again.
        const std::string default_toml = default_config_toml();
        if (std::filesystem::create_directories(path.parent_path(), ec); !ec) {
            const auto temp = path.parent_path() /
                              (path.filename().wstring() + L".tmp");
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (file) {
                    file.write(default_toml.data(),
                               static_cast<std::streamsize>(default_toml.size()));
                }
            }
            if (!ec) {
                std::filesystem::rename(temp, path, ec);
                if (ec) {
                    std::filesystem::remove(temp, ec);
                    ec.clear();
                }
            }
        }
        ec.clear();
        return parse_config(default_toml);
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(
            Error::make(ErrorCode::ConfigParseFailed, "cannot open config file"));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    file.close();
    const auto original = buffer.str();
    auto document = parse_document(original);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto parsed = parse_config_root(*document);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->preset_revision >= kCurrentPresetRevision) {
        return parsed;
    }

    auto& root = *document;
    const int source_revision = parsed->preset_revision;
    if (source_revision < 2) {
        if (root.get("launcher") == nullptr) {
            add_default_launcher(root);
        }
        if (root.get("defaults") == nullptr) {
            add_default_game_defaults(root);
        }
        migrate_legacy_ipad(root);
    }
    if (source_revision < 3) {
        migrate_legacy_xiaomi(root);
    }
    root.insert_or_assign("preset_revision", kCurrentPresetRevision);

    auto backup = create_migration_backup(path, source_revision);
    if (!backup) {
        return std::unexpected(backup.error());
    }
    auto written = write_migrated_document(path, root);
    if (!written) {
        return std::unexpected(written.error());
    }
    return parse_config_root(root);
}

Result<Profile> find_profile(const Config& config, std::string_view id) {
    for (const auto& profile : config.profiles) {
        if (profile.id == id) {
            return profile;
        }
    }
    return std::unexpected(Error::make(ErrorCode::ProfileNotFound,
                                       "profile not found: " + std::string(id)));
}

namespace {

[[nodiscard]] float display_aspect(const DisplayFacts& display) {
    return display.aspect_ratio;
}

[[nodiscard]] bool aspect_close(float a, float b) {
    constexpr float kEpsilon = 0.01f;
    return a > 0.0f && b > 0.0f &&
           (a - b < kEpsilon && b - a < kEpsilon);
}

// Specificity tiers: identity beats geometry (plan section 17.2). A profile
// scores a tier only when every predicate it declares matches the display.
[[nodiscard]] int match_score(const Profile& profile,
                              const DisplayFacts& display) {
    const auto& match = profile.match;
    int score = 0;
    bool matched_any_predicate = false;

    if (match.device_name.has_value()) {
        if (*match.device_name != display.info.device_name) {
            return -1;
        }
        score += 1000;
        matched_any_predicate = true;
    }
    if (match.resolution.has_value()) {
        if (*match.resolution != display.resolution) {
            return -1;
        }
        score += 100;
        matched_any_predicate = true;
    }
    if (match.aspect_ratio.has_value()) {
        if (!aspect_close(*match.aspect_ratio, display_aspect(display))) {
            return -1;
        }
        score += 10;
        matched_any_predicate = true;
    }
    if (match.portrait.has_value()) {
        if (*match.portrait != display.portrait) {
            return -1;
        }
        score += 1;
        matched_any_predicate = true;
    }
    return matched_any_predicate ? score : -1;
}

}  // namespace

Result<AutoProfileDecision> resolve_auto_profile(
    const Config& config, GameId game,
    const std::vector<win32::DisplayInfo>& displays) {
    // Gather current modes once: geometry alone cannot answer resolution
    // or aspect queries.
    std::vector<DisplayFacts> facts;
    for (const auto& display : displays) {
        if (!display.is_attached) {
            continue;
        }
        DisplayFacts entry;
        entry.info = display;
        if (auto settings = win32::query_current_settings(display.device_name);
            settings) {
            entry.resolution = Resolution{settings->width, settings->height};
            entry.refresh_rate = settings->refresh_rate;
        } else if (display.right > display.left &&
                   display.bottom > display.top) {
            // No queryable mode (headless/virtual display): the geometry is
            // the best available statement of the current resolution.
            entry.resolution = Resolution{
                static_cast<uint32_t>(display.right - display.left),
                static_cast<uint32_t>(display.bottom - display.top)};
        }
        if (entry.resolution.height != 0) {
            entry.aspect_ratio = static_cast<float>(entry.resolution.width) /
                                 static_cast<float>(entry.resolution.height);
            entry.portrait = entry.resolution.height > entry.resolution.width;
        }
        facts.push_back(std::move(entry));
    }

    // Plan section 17.3: manual profiles are NEVER auto-selected. The
    // default profile is the final fallback (the user designated it).
    const Profile* best = nullptr;
    const DisplayFacts* best_display = nullptr;
    int best_specificity = -1;
    int best_priority = 0;
    std::vector<const Profile*> tied_profiles;
    const Profile* fallback = nullptr;
    std::vector<AutoCandidateDecision> candidates;
    const std::string& per_game_default =
        game == GameId::Genshin ? config.genshin_default
                                : config.starrail_default;
    const std::string& fallback_id =
        !per_game_default.empty() ? per_game_default : config.default_profile;
    for (const auto& profile : config.profiles) {
        if (profile.game != game) {
            continue;
        }
        if (profile.id == fallback_id) {
            fallback = &profile;
        }
        if (!profile.match.auto_select) {
            continue;
        }
        AutoCandidateDecision candidate{profile.id, std::nullopt, -1,
                                        profile.match.priority};
        for (const auto& display : facts) {
            const int score = match_score(profile, display);
            if (score > candidate.specificity) {
                candidate.display_index = display.info.index;
                candidate.specificity = score;
            }
        }
        candidates.push_back(candidate);
        if (candidate.specificity >= 0) {
            const bool outranks = best == nullptr ||
                candidate.specificity > best_specificity ||
                (candidate.specificity == best_specificity &&
                 candidate.priority > best_priority);
            if (outranks) {
                best = &profile;
                best_specificity = candidate.specificity;
                best_priority = candidate.priority;
                best_display = nullptr;
                for (const auto& display : facts) {
                    if (display.info.index == *candidate.display_index) {
                        best_display = &display;
                        break;
                    }
                }
                tied_profiles.clear();
                tied_profiles.push_back(&profile);
            } else if (candidate.specificity == best_specificity &&
                       candidate.priority == best_priority) {
                tied_profiles.push_back(&profile);
            }
        }
    }
    if (tied_profiles.size() > 1) {
        std::string message = "auto profile is ambiguous:";
        for (const auto* profile : tied_profiles) {
            message += "\n  " + profile->id;
        }
        message += "\nall match at specificity=" +
                   std::to_string(best_specificity) + " priority=" +
                   std::to_string(best_priority);
        return std::unexpected(Error::make(ErrorCode::AutoProfileAmbiguous,
                                           std::move(message)));
    }
    if (best != nullptr) {
        return AutoProfileDecision{*best, false,
                                   best_display ? std::optional(best_display->info.index)
                                                : std::nullopt,
                                   best_specificity, best_priority,
                                   std::move(facts), std::move(candidates)};
    }
    if (fallback != nullptr) {
        return AutoProfileDecision{*fallback, true, std::nullopt, 0, 0,
                                   std::move(facts), std::move(candidates)};
    }
    return std::unexpected(Error::make(
        ErrorCode::ProfileNotFound,
        "no matching auto profile for game '" + std::string(to_string(game)) +
            "' (add one to config.toml)"));
}

Result<Profile> match_auto_profile(
    const Config& config, GameId game,
    const std::vector<win32::DisplayInfo>& displays) {
    auto decision = resolve_auto_profile(config, game, displays);
    if (!decision) {
        return std::unexpected(decision.error());
    }
    return decision->profile;
}

}  // namespace hoyoflux::profile
