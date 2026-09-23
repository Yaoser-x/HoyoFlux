#include "profile/config.hpp"

#include "platform/win32/text.hpp"
#include "platform/win32/file.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>

namespace hoyoflux::profile {
namespace {

constexpr int kCurrentPresetRevision = 4;

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
        Error::Location location;
        location.field = key;
        const auto source = node->source();
        location.line = source.begin.line;
        location.column = source.begin.column;
        return std::unexpected(Error::make(
            error_code, std::string(context) + ": " + std::string(key) +
                            " must be " + std::string(expected), 0,
            std::move(location)));
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
        Error::Location location;
        location.line = error.source().begin.line;
        location.column = error.source().begin.column;
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "TOML parse error: " + std::string(error.description()) +
                " (line " + std::to_string(*location.line) + ", column " +
                std::to_string(*location.column) + ")",
            0, std::move(location)));
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

[[maybe_unused]] void add_default_launcher(toml::table& root) {
    toml::table launcher;
    launcher.insert("game", std::string{"genshin"});
    launcher.insert("profile", std::string{"auto"});
    launcher.insert("region", std::string{"auto"});
    launcher.insert("notifications", true);
    root.insert_or_assign("launcher", std::move(launcher));
}

[[maybe_unused]] void add_default_game_defaults(toml::table& root) {
    toml::table defaults;
    defaults.insert("genshin", std::string{"desktop"});
    defaults.insert("starrail", std::string{"starrail_desktop"});
    root.insert_or_assign("defaults", std::move(defaults));
}

[[maybe_unused]] void migrate_legacy_ipad(toml::table& root) {
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

[[maybe_unused]] void migrate_legacy_xiaomi(toml::table& root) {
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

[[maybe_unused]] [[nodiscard]] std::filesystem::path sibling_path(
    const std::filesystem::path& path, std::wstring_view suffix) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path{"."}
                                                   : path.parent_path();
    return parent / (path.filename().wstring() + std::wstring{suffix});
}

Result<Config> parse_legacy_config_root(const toml::table& root) {
    Config config;
    config.schema = 1;
    config.genshin_default.clear();
    config.starrail_default.clear();
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
                    " (this build understands revisions 1-4); update HoyoFlux"));
        }
        (void)value;
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

std::string source_suffix(const toml::node& node) {
    const auto source = node.source();
    return " (line " + std::to_string(source.begin.line) + ", column " +
           std::to_string(source.begin.column) + ")";
}

Result<void> reject_unknown_keys(
    const toml::table& table,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    for (const auto& [key, value] : table) {
        const bool known = std::find(allowed.begin(), allowed.end(),
                                     key.str()) != allowed.end();
        if (!known) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                std::string(context) + ": unknown key '" +
                    std::string(key.str()) + "'" + source_suffix(value)));
        }
    }
    return {};
}

Result<GameId> parse_game(std::string_view value, std::string_view context) {
    if (value == "genshin") return GameId::Genshin;
    if (value == "starrail") return GameId::StarRail;
    return std::unexpected(Error::make(
        ErrorCode::ConfigParseFailed,
        std::string(context) + " must be \"genshin\" or \"starrail\""));
}

Result<ProcessPriority> parse_priority(std::string_view value,
                                       std::string_view context) {
    if (value == "realtime") return ProcessPriority::Realtime;
    if (value == "high") return ProcessPriority::High;
    if (value == "above_normal") return ProcessPriority::AboveNormal;
    if (value == "normal") return ProcessPriority::Normal;
    if (value == "below_normal") return ProcessPriority::BelowNormal;
    return std::unexpected(Error::make(
        ErrorCode::ProfileInvalid,
        std::string(context) +
            " must be realtime|high|above_normal|normal|below_normal"));
}

Result<Profile> parse_v2_profile(std::string id, const toml::table& body,
                                 const std::filesystem::path& base_directory) {
    const std::string context = "profile '" + id + "'";
    const std::string profile_id = id;
    if (auto known = reject_unknown_keys(
            body,
            {"game", "fps", "resolution", "fullscreen", "persistence",
             "monitor", "priority", "power_save", "power_save_fps",
             "hotkeys", "mobile_ui", "dpi_scale", "match", "exe", "args"},
            context);
        !known) {
        return std::unexpected(known.error());
    }
    const auto* game_node = body.get("game");
    if (game_node == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid, context + ": missing required field game"));
    }
    if (!game_node->is_string()) {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid,
            context + ".game must be a string" + source_suffix(*game_node)));
    }
    auto game = parse_game(*game_node->value<std::string>(), context + ".game");
    if (!game) return std::unexpected(Error::make(
        game.error().code, game.error().message + source_suffix(*game_node)));
    Profile profile = make_profile_template(std::move(id), *game);

    const auto field_error = [&](std::string_view key, std::string message) {
        Error::Location location;
        location.profile = profile_id;
        location.field = key;
        if (const auto* node = body.get(key)) {
            message += source_suffix(*node);
            const auto source = node->source();
            location.line = source.begin.line;
            location.column = source.begin.column;
        }
        return Error::make(ErrorCode::ProfileInvalid, std::move(message), 0,
                           std::move(location));
    };

    const auto require_type = [&](std::string_view key, auto predicate,
                                  std::string_view expected) -> Result<void> {
        const auto* node = body.get(key);
        if (node != nullptr && !predicate(*node)) {
            Error::Location location;
            location.profile = profile_id;
            location.field = key;
            const auto source = node->source();
            location.line = source.begin.line;
            location.column = source.begin.column;
            return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid,
                context + "." + std::string(key) + " must be " +
                    std::string(expected) + source_suffix(*node), 0,
                std::move(location)));
        }
        return {};
    };
    const auto is_string = [](const toml::node& n) { return n.is_string(); };
    const auto is_integer = [](const toml::node& n) { return n.is_integer(); };
    const auto is_bool = [](const toml::node& n) { return n.is_boolean(); };
    const auto is_number = [](const toml::node& n) {
        return n.is_integer() || n.is_floating_point();
    };
    for (const auto& result : {
             require_type("fps", is_integer, "an integer"),
             require_type("resolution", is_string, "a string"),
             require_type("fullscreen", is_string, "a string"),
             require_type("persistence", is_string, "a string"),
             require_type("monitor", is_integer, "an integer"),
             require_type("priority", is_string, "a string"),
             require_type("power_save", is_bool, "a boolean"),
             require_type("power_save_fps", is_integer, "an integer"),
             require_type("hotkeys", is_bool, "a boolean"),
             require_type("mobile_ui", is_bool, "a boolean"),
             require_type("dpi_scale", is_number, "a number"),
             require_type("exe", is_string, "a string")}) {
        if (!result) return std::unexpected(result.error());
    }

    if (const auto fps = opt_int(body, "fps")) {
        if (*fps < 10 || *fps > 1000) {
            return std::unexpected(field_error(
                "fps", context + ".fps must be within [10, 1000]"));
        }
        profile.runtime.fps = static_cast<uint32_t>(*fps);
    }
    if (const auto resolution = opt_string(body, "resolution")) {
        auto parsed = parse_resolution(*resolution, context);
        if (!parsed) return std::unexpected(field_error(
            "resolution", parsed.error().message));
        profile.render.resolution = *parsed;
    }
    if (const auto fullscreen = opt_string(body, "fullscreen")) {
        if (*fullscreen == "exclusive") profile.render.fullscreen = FullscreenMode::Exclusive;
        else if (*fullscreen == "borderless") profile.render.fullscreen = FullscreenMode::Borderless;
        else if (*fullscreen == "windowed") profile.render.fullscreen = FullscreenMode::Windowed;
        else return std::unexpected(field_error(
            "fullscreen",
            context + ".fullscreen must be exclusive|borderless|windowed"));
    }
    if (const auto persistence = opt_string(body, "persistence")) {
        if (*persistence == "session") profile.render.persistence = ResolutionPersistence::Session;
        else if (*persistence == "persistent") profile.render.persistence = ResolutionPersistence::Persistent;
        else return std::unexpected(field_error(
            "persistence", context + ".persistence must be session|persistent"));
    }
    if (const auto monitor = opt_int(body, "monitor")) {
        if (*monitor < 0 || *monitor > 63) {
            return std::unexpected(field_error(
                "monitor", context + ".monitor must be within [0, 63]"));
        }
        profile.render.monitor = static_cast<uint32_t>(*monitor);
    }
    if (const auto priority = opt_string(body, "priority")) {
        auto parsed = parse_priority(*priority, context + ".priority");
        if (!parsed) return std::unexpected(field_error(
            "priority", parsed.error().message));
        profile.runtime.priority = *parsed;
    }
    if (const auto power_save = opt_bool(body, "power_save")) {
        profile.runtime.power_save = *power_save ? PowerSavePolicy::Enabled
                                                : PowerSavePolicy::Disabled;
    }
    if (const auto fps = opt_int(body, "power_save_fps")) {
        if (*fps < 1 || *fps > 1000) {
            return std::unexpected(field_error(
                "power_save_fps",
                context + ".power_save_fps must be within [1, 1000]"));
        }
        profile.runtime.power_save_fps = static_cast<uint32_t>(*fps);
    }
    if (const auto hotkeys = opt_bool(body, "hotkeys")) profile.runtime.hotkeys = *hotkeys;
    if (const auto mobile_ui = opt_bool(body, "mobile_ui")) profile.ui.mobile_ui = *mobile_ui;
    if (const auto dpi = opt_double(body, "dpi_scale")) {
        if (!std::isfinite(*dpi) || *dpi < 0.25 || *dpi > 4.0) {
            return std::unexpected(field_error(
                "dpi_scale", context + ".dpi_scale must be within [0.25, 4.0]"));
        }
        profile.ui.dpi_scale = static_cast<float>(*dpi);
    }

    if (const auto* match_node = body.get("match")) {
        if (!match_node->is_table()) {
            return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid,
                context + ".match must be an inline table" + source_suffix(*match_node)));
        }
        const auto& match = *match_node->as_table();
        const auto match_error = [&](std::string_view key, std::string message) {
            if (const auto* node = match.get(key)) message += source_suffix(*node);
            return Error::make(ErrorCode::ProfileInvalid, std::move(message));
        };
        if (auto known = reject_unknown_keys(
                match, {"auto_select", "device_name", "resolution",
                        "aspect_ratio", "portrait", "priority"},
                context + ".match");
            !known) return std::unexpected(known.error());
        profile.match.auto_select = true;
        if (const auto enabled = opt_bool(match, "auto_select")) {
            profile.match.auto_select = *enabled;
        }
        if (const auto device = opt_string(match, "device_name")) {
            auto wide = win32::utf16(*device);
            if (!wide) return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid, context + ".match.device_name is not valid UTF-8"));
            profile.match.device_name = std::move(*wide);
        }
        if (const auto resolution = opt_string(match, "resolution")) {
            auto parsed = parse_resolution(*resolution, context + ".match");
            if (!parsed) return std::unexpected(match_error(
                "resolution", parsed.error().message));
            profile.match.resolution = *parsed;
        }
        if (const auto aspect = opt_double(match, "aspect_ratio")) {
            if (!std::isfinite(*aspect) || *aspect <= 0.0 || *aspect > 10.0) {
                return std::unexpected(match_error(
                    "aspect_ratio",
                    context + ".match.aspect_ratio must be within (0, 10]"));
            }
            profile.match.aspect_ratio = static_cast<float>(*aspect);
        }
        if (const auto portrait = opt_bool(match, "portrait")) profile.match.portrait = *portrait;
        if (const auto priority = opt_int(match, "priority")) {
            if (*priority < std::numeric_limits<int>::min() ||
                *priority > std::numeric_limits<int>::max()) {
                return std::unexpected(match_error(
                    "priority", context + ".match.priority is outside the int range"));
            }
            profile.match.priority = static_cast<int>(*priority);
        }
        for (const auto& [key, value] : match) {
            const auto key_view = key.str();
            const bool valid =
                (key_view == "auto_select" && value.is_boolean()) ||
                ((key_view == "device_name" || key_view == "resolution") && value.is_string()) ||
                (key_view == "aspect_ratio" && (value.is_integer() || value.is_floating_point())) ||
                (key_view == "portrait" && value.is_boolean()) ||
                (key_view == "priority" && value.is_integer());
            if (!valid) return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid,
                context + ".match." + std::string(key_view) + " has the wrong type" +
                    source_suffix(value)));
        }
    }

    if (const auto executable = opt_string(body, "exe")) {
        auto wide = win32::utf16(*executable);
        if (!wide) return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid, context + ".exe is not valid UTF-8"));
        std::filesystem::path path(*wide);
        profile.executable = path.is_relative() ? base_directory / path : path;
    }
    if (const auto* args = body.get("args")) {
        if (!args->is_array()) return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid, context + ".args must be an array of strings" +
                source_suffix(*args)));
        for (const auto& item : *args->as_array()) {
            if (!item.is_string()) return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid, context + ".args must contain only strings" +
                    source_suffix(item)));
            auto wide = win32::utf16(*item.value<std::string>());
            if (!wide) return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid, context + ".args contains invalid UTF-8"));
            profile.arguments.push_back(std::move(*wide));
        }
    }
    return profile;
}

Result<Config> parse_v2_config_root(
    const toml::table& root,
    const std::filesystem::path& base_directory) {
    if (auto known = reject_unknown_keys(
            root, {"schema", "launcher", "defaults", "profiles"}, "config");
        !known) return std::unexpected(known.error());
    Config config;
    config.schema = 2;
    const auto* launcher_node = root.get("launcher");
    if (launcher_node == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed, "config.launcher is required"));
    }
    if (!launcher_node->is_table()) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "config.launcher must be a table" + source_suffix(*launcher_node)));
    }
    const auto& launcher = *launcher_node->as_table();
    const auto launcher_error = [&](std::string_view key, std::string message) {
        if (const auto* node = launcher.get(key)) message += source_suffix(*node);
        return Error::make(ErrorCode::ConfigParseFailed, std::move(message));
    };
    if (auto known = reject_unknown_keys(
            launcher, {"game", "profile", "region", "action", "notifications"},
            "launcher"); !known) return std::unexpected(known.error());
    if (const auto game = opt_string(launcher, "game")) {
        auto parsed = parse_game(*game, "launcher.game");
        if (!parsed) return std::unexpected(launcher_error(
            "game", parsed.error().message));
        config.launcher.game = *parsed;
    } else if (launcher.get("game") != nullptr) {
        return std::unexpected(launcher_error(
            "game", "launcher.game must be a string"));
    }
    if (const auto profile = opt_string(launcher, "profile")) config.launcher.profile = *profile;
    else if (launcher.get("profile") != nullptr) return std::unexpected(launcher_error(
        "profile", "launcher.profile must be a string"));
    if (const auto region = opt_string(launcher, "region")) {
        if (*region == "auto") config.launcher.region = LauncherRegion::Auto;
        else if (*region == "cn") config.launcher.region = LauncherRegion::Cn;
        else if (*region == "global") config.launcher.region = LauncherRegion::Global;
        else return std::unexpected(launcher_error(
            "region", "launcher.region must be auto|cn|global"));
    } else if (launcher.get("region") != nullptr) return std::unexpected(launcher_error(
        "region", "launcher.region must be a string"));
    if (const auto action = opt_string(launcher, "action")) {
        if (*action == "launch") config.launcher.action = LauncherAction::Launch;
        else if (*action == "diagnose") config.launcher.action = LauncherAction::Diagnose;
        else return std::unexpected(launcher_error(
            "action", "launcher.action must be launch|diagnose"));
    } else if (launcher.get("action") != nullptr) return std::unexpected(launcher_error(
        "action", "launcher.action must be a string"));
    if (const auto notifications = opt_bool(launcher, "notifications")) {
        config.launcher.notifications = *notifications;
    } else if (launcher.get("notifications") != nullptr) return std::unexpected(launcher_error(
        "notifications", "launcher.notifications must be a boolean"));

    if (const auto* defaults = root.get("defaults")) {
        if (!defaults->is_table()) return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "config.defaults must be a table" + source_suffix(*defaults)));
        const auto& table = *defaults->as_table();
        const auto defaults_error = [&](std::string_view key, std::string message) {
            if (const auto* node = table.get(key)) message += source_suffix(*node);
            return Error::make(ErrorCode::ConfigParseFailed, std::move(message));
        };
        if (auto known = reject_unknown_keys(table, {"genshin", "starrail"}, "defaults");
            !known) return std::unexpected(known.error());
        if (const auto value = opt_string(table, "genshin")) config.genshin_default = *value;
        else if (table.get("genshin") != nullptr) return std::unexpected(defaults_error(
            "genshin", "defaults.genshin must be a string"));
        if (const auto value = opt_string(table, "starrail")) config.starrail_default = *value;
        else if (table.get("starrail") != nullptr) return std::unexpected(defaults_error(
            "starrail", "defaults.starrail must be a string"));
    }

    const auto* profiles = root.get("profiles");
    if (profiles == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed, "config.profiles is required"));
    }
    if (!profiles->is_table()) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "config.profiles must be a table" + source_suffix(*profiles)));
    }
    for (const auto& [key, value] : *profiles->as_table()) {
        if (!value.is_table()) return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid,
            "profile '" + std::string(key.str()) + "' must be a table" +
                source_suffix(value)));
        auto profile = parse_v2_profile(std::string(key.str()), *value.as_table(),
                                        base_directory);
        if (!profile) return std::unexpected(profile.error());
        config.profiles.push_back(std::move(*profile));
    }
    if (config.profiles.empty()) return std::unexpected(Error::make(
        ErrorCode::ConfigParseFailed, "config must contain at least one profile"));
    return config;
}

Result<void> write_text_atomic(const std::filesystem::path& path,
                               std::string_view text) {
    return win32::write_file_atomic(path, text);
}

Result<std::string> read_text(const std::filesystem::path& path) {
    auto text = win32::read_file_bytes(path);
    if (!text) {
        Error error = text.error();
        error.code = ErrorCode::ConfigParseFailed;
        error.message = "cannot read config file: " + path.string() +
                        " (" + error.message + ")";
        Error::Location location;
        location.file = win32::utf8(path.wstring());
        error.location = std::move(location);
        return std::unexpected(std::move(error));
    }
    return text;
}

std::string toml_quote(std::string_view text) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char character : text) {
        switch (character) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\t': out << "\\t"; break;
        case '\n': out << "\\n"; break;
        case '\f': out << "\\f"; break;
        case '\r': out << "\\r"; break;
        default:
            if (character < 0x20 || character == 0x7f) {
                out << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            } else {
                out << static_cast<char>(character);
            }
        }
    }
    out << '"';
    return out.str();
}

const char* priority_name(ProcessPriority value) {
    switch (value) {
    case ProcessPriority::Realtime: return "realtime";
    case ProcessPriority::High: return "high";
    case ProcessPriority::AboveNormal: return "above_normal";
    case ProcessPriority::Normal: return "normal";
    case ProcessPriority::BelowNormal: return "below_normal";
    }
    return "normal";
}

const char* fullscreen_name(FullscreenMode value) {
    switch (value) {
    case FullscreenMode::Exclusive: return "exclusive";
    case FullscreenMode::Borderless: return "borderless";
    case FullscreenMode::Windowed: return "windowed";
    }
    return "windowed";
}

Result<void> preserve_schema1_backup(
    const std::filesystem::path& backup_directory,
    std::string_view original) {
    std::error_code ec;
    std::filesystem::create_directories(backup_directory, ec);
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "cannot create config backup directory: " +
            backup_directory.string(), static_cast<unsigned long>(ec.value())));
    const auto backup = backup_directory / L"config.schema1.toml";
    if (std::filesystem::exists(backup, ec)) {
        if (ec) return std::unexpected(Error::make(
            ErrorCode::OsError, "cannot inspect config backup: " + backup.string(),
            static_cast<unsigned long>(ec.value())));
        auto existing = read_text(backup);
        if (!existing) return std::unexpected(existing.error());
        if (*existing != original) return std::unexpected(Error::make(
            ErrorCode::OsError,
            "a different schema-1 backup already exists: " + backup.string()));
        return {};
    }
    return write_text_atomic(backup, original);
}

}  // namespace

std::string default_config_toml() {
    return R"(# HoyoFlux 便携配置
# 第一次使用：修改后保存，再次双击 HoyoFlux.exe。
# 诊断模式：把 action 改成 "diagnose"，双击后会打开 data/diagnostics.txt。
schema = 2

[launcher]
game = "genshin"
profile = "auto"
action = "launch"

[profiles.desktop]
game = "genshin"
fps = 120

[profiles.starrail_desktop]
game = "starrail"
fps = 120

# 移动端配置示例（删除每行开头的 # 后，按实际设备修改）：
# [profiles.example_mobile]
# game = "genshin"
# fps = 60
# mobile_ui = true
# dpi_scale = 2.0
# match = { resolution = "1920x1080" }
)";
}

std::string serialize_config(const Config& config) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "# HoyoFlux portable configuration\n"
        << "schema = 2\n\n[launcher]\n"
        << "game = \"" << to_string(config.launcher.game) << "\"\n"
        << "profile = " << toml_quote(config.launcher.profile) << "\n"
        << "action = \""
        << (config.launcher.action == LauncherAction::Diagnose ? "diagnose" : "launch")
        << "\"\n";
    if (config.launcher.region != LauncherRegion::Auto) {
        out << "region = \""
            << (config.launcher.region == LauncherRegion::Cn ? "cn" : "global")
            << "\"\n";
    }
    if (!config.launcher.notifications) out << "notifications = false\n";

    const std::string genshin_default = !config.genshin_default.empty()
        ? config.genshin_default : config.default_profile;
    const std::string starrail_default = !config.starrail_default.empty()
        ? config.starrail_default : config.default_profile;
    if (genshin_default != "desktop" || starrail_default != "starrail_desktop") {
        out << "\n[defaults]\n"
            << "genshin = " << toml_quote(genshin_default) << "\n"
            << "starrail = " << toml_quote(starrail_default) << "\n";
    }

    for (const auto& profile : config.profiles) {
        out << "\n[profiles." << toml_quote(profile.id) << "]\n"
            << "game = \"" << to_string(profile.game) << "\"\n"
            << "fps = " << profile.runtime.fps << "\n";
        if (profile.render.resolution) {
            out << "resolution = \"" << profile.render.resolution->width << "x"
                << profile.render.resolution->height << "\"\n";
        }
        if (profile.render.fullscreen) {
            out << "fullscreen = \"" << fullscreen_name(*profile.render.fullscreen)
                << "\"\n";
        }
        if (profile.render.persistence == ResolutionPersistence::Persistent)
            out << "persistence = \"persistent\"\n";
        if (profile.render.monitor) out << "monitor = " << *profile.render.monitor << "\n";
        if (profile.runtime.priority != ProcessPriority::Normal)
            out << "priority = \"" << priority_name(profile.runtime.priority) << "\"\n";
        if (profile.runtime.power_save == PowerSavePolicy::Enabled)
            out << "power_save = true\n";
        if (profile.runtime.power_save_fps != 30)
            out << "power_save_fps = " << profile.runtime.power_save_fps << "\n";
        if (profile.runtime.hotkeys) out << "hotkeys = true\n";
        if (profile.ui.mobile_ui) out << "mobile_ui = true\n";
        if (profile.ui.dpi_scale) out << "dpi_scale = " << *profile.ui.dpi_scale << "\n";
        if (profile.executable) {
            out << "exe = " << toml_quote(win32::utf8(profile.executable->wstring())) << "\n";
        }
        if (!profile.arguments.empty()) {
            out << "args = [";
            for (size_t i = 0; i < profile.arguments.size(); ++i) {
                if (i != 0) out << ", ";
                out << toml_quote(win32::utf8(profile.arguments[i]));
            }
            out << "]\n";
        }
        const auto& match = profile.match;
        const bool has_match = match.auto_select || match.device_name ||
            match.resolution || match.aspect_ratio || match.portrait ||
            match.priority != 0;
        if (has_match) {
            out << "match = { ";
            bool first = true;
            const auto separator = [&] {
                if (!first) out << ", ";
                first = false;
            };
            if (!match.auto_select) { separator(); out << "auto_select = false"; }
            if (match.device_name) { separator(); out << "device_name = "
                << toml_quote(win32::utf8(*match.device_name)); }
            if (match.resolution) { separator(); out << "resolution = \""
                << match.resolution->width << "x" << match.resolution->height << "\""; }
            if (match.aspect_ratio) { separator(); out << "aspect_ratio = " << *match.aspect_ratio; }
            if (match.portrait) { separator(); out << "portrait = "
                << (*match.portrait ? "true" : "false"); }
            if (match.priority != 0) { separator(); out << "priority = " << match.priority; }
            out << " }\n";
        }
    }
    return out.str();
}

Result<Config> parse_config(
    std::string_view toml_text,
    const std::filesystem::path& base_directory) {
    auto parsed = parse_document(toml_text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto* schema = parsed->get("schema");
    const int64_t version = schema && schema->is_integer()
        ? schema->value<int64_t>().value_or(0) : 1;
    if (version == 1) return parse_legacy_config_root(*parsed);
    if (version == 2) return parse_v2_config_root(*parsed, base_directory);
    return std::unexpected(Error::make(
        ErrorCode::ConfigParseFailed,
        "unsupported config schema " + std::to_string(version) +
            " (this build understands schema 1 and 2)"));
}

Result<void> write_default_config(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return {};
    if (ec) return std::unexpected(Error::make(
        ErrorCode::OsError, "cannot inspect config path: " + path.string(),
        static_cast<unsigned long>(ec.value())));
    return write_text_atomic(path, default_config_toml());
}

Result<Config> read_config(const std::filesystem::path& path) {
    auto original = read_text(path);
    if (!original) return std::unexpected(original.error());
    auto parsed = parse_config(*original, path.parent_path());
    if (!parsed) {
        Error error = parsed.error();
        if (!error.location) error.location = Error::Location{};
        error.location->file = win32::utf8(path.wstring());
        return std::unexpected(std::move(error));
    }
    return parsed;
}

Result<LauncherAction> read_declared_launcher_action(
    const std::filesystem::path& path) {
    auto original = read_text(path);
    if (!original) return std::unexpected(original.error());
    auto document = parse_document(*original);
    if (!document) {
        Error error = document.error();
        if (!error.location) error.location = Error::Location{};
        error.location->file = win32::utf8(path.wstring());
        return std::unexpected(std::move(error));
    }
    const auto* launcher = document->get("launcher");
    if (launcher == nullptr || !launcher->is_table()) return LauncherAction::Launch;
    const auto* action = launcher->as_table()->get("action");
    if (action == nullptr) return LauncherAction::Launch;
    if (!action->is_string()) return std::unexpected(Error::make(
        ErrorCode::ConfigParseFailed,
        "launcher.action must be a string" + source_suffix(*action)));
    const auto value = action->value<std::string>();
    if (*value == "diagnose") return LauncherAction::Diagnose;
    return LauncherAction::Launch;
}

Result<ConfigMigrationPreparation> prepare_config_migration(
    const std::filesystem::path& path) {
    auto original = read_text(path);
    if (!original) return std::unexpected(original.error());
    auto source = parse_config(*original, path.parent_path());
    if (!source) {
        Error error = source.error();
        if (!error.location) error.location = Error::Location{};
        error.location->file = win32::utf8(path.wstring());
        return std::unexpected(std::move(error));
    }
    if (source->schema != 1) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument,
            "only schema-1 configs require migration: " + path.string()));
    }
    const std::string migrated = serialize_config(*source);
    auto converted = parse_config(migrated, path.parent_path());
    if (!converted) return std::unexpected(converted.error());
    if (!configs_equivalent(*source, *converted)) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "schema-1 conversion changed effective configuration behavior"));
    }
    return ConfigMigrationPreparation{
        .source = std::move(*source), .converted = std::move(*converted),
        .source_text = std::move(*original), .converted_text = migrated};
}

Result<Config> commit_config_migration(
    const std::filesystem::path& path,
    const std::filesystem::path& backup_directory,
    const ConfigMigrationPreparation& preparation) {
    const auto backups = backup_directory.empty()
        ? path.parent_path() / L"data" / L"backups" : backup_directory;
    auto current = read_text(path);
    if (!current) return std::unexpected(current.error());
    if (*current != preparation.source_text) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "配置在迁移期间已被修改，已保留原文件: " + path.string()));
    }
    if (auto backup = preserve_schema1_backup(backups, preparation.source_text); !backup)
        return std::unexpected(backup.error());
    if (auto written = write_text_atomic(path, preparation.converted_text); !written)
        return std::unexpected(written.error());
    auto verified = read_config(path);
    if (!verified) return std::unexpected(verified.error());
    if (!configs_equivalent(preparation.source, *verified)) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "迁移后重新读取的配置行为不一致，原始备份已保留"));
    }
    return verified;
}

Result<Config> migrate_config_file(
    const std::filesystem::path& path,
    const std::filesystem::path& backup_directory) {
    auto inspected = read_config(path);
    if (!inspected) return std::unexpected(inspected.error());
    if (inspected->schema == 2) return inspected;
    auto prepared = prepare_config_migration(path);
    if (!prepared) return std::unexpected(prepared.error());
    return commit_config_migration(path, backup_directory, *prepared);
}

bool configs_equivalent(const Config& left, const Config& right) {
    // Serializing both typed configurations removes schema-only bookkeeping
    // and compares every supported setting that affects a launch.  The
    // serializer is deterministic, uses the classic locale, and emits float
    // values at max_digits10 precision.
    return serialize_config(left) == serialize_config(right);
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
