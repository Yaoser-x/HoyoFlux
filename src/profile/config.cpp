#include "profile/config.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <sstream>

namespace hoyoflux::profile {
namespace {

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

Result<Resolution> parse_resolution(std::string_view text, std::string_view context) {
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= text.size()) {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid,
            std::string(context) + ": resolution must look like \"2560x1440\""));
    }
    Resolution resolution;
    resolution.width = static_cast<uint32_t>(std::stoi(std::string(text.substr(0, x))));
    resolution.height =
        static_cast<uint32_t>(std::stoi(std::string(text.substr(x + 1))));
    if (resolution.empty()) {
        return std::unexpected(Error::make(ErrorCode::ProfileInvalid,
                                           std::string(context) +
                                               ": resolution is zero"));
    }
    return resolution;
}

Result<Profile> parse_profile(std::string id, const toml::table& body) {
    const std::string context = "profile '" + id + "'";

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

    if (const auto match = opt_string(body, "match")) {
        if (*match == "manual") {
            profile.match = MatchPolicy::Manual;
        } else if (*match == "auto") {
            profile.match = MatchPolicy::Auto;
        } else {
            return std::unexpected(Error::make(
                ErrorCode::ProfileInvalid,
                context + ": match must be \"manual\" or \"auto\""));
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
        profile.render.monitor = opt_int(table, "monitor")
                                     .transform([](int64_t v) {
                                         return static_cast<uint32_t>(v);
                                     });
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
        if (const auto* power_save = table.get("power_save");
            power_save && power_save->is_table()) {
            const auto& ps = *power_save->as_table();
            if (const auto enabled = opt_bool(ps, "enabled")) {
                profile.runtime.power_save =
                    *enabled ? PowerSavePolicy::Enabled : PowerSavePolicy::Disabled;
            }
            if (const auto fps = opt_int(ps, "fps")) {
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
            if (*dpi < 0.25 || *dpi > 4.0) {
                return std::unexpected(Error::make(
                    ErrorCode::ProfileInvalid,
                    context + ": dpi_scale must be within [0.25, 4.0]"));
            }
            profile.ui.dpi_scale = static_cast<float>(*dpi);
        }
    }

    return profile;
}

}  // namespace

std::string default_config_toml() {
    return R"(# HoyoFlux configuration. See `hoyoflux profile list` for the parsed view.
default_profile = "desktop"

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
match = "auto"

[profiles.ipad.render]
resolution = "1080x1920"
persistence = "session"

[profiles.ipad.runtime]
fps = 60

[profiles.ipad.ui]
mobile_ui = true
dpi_scale = 2.0

[profiles.xiaomi]
game = "genshin"

[profiles.xiaomi.render]
resolution = "1220x2712"
persistence = "session"

[profiles.xiaomi.runtime]
fps = 60

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
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& error) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "TOML parse error: " + std::string(error.description())));
    }

    Config config;
    if (const auto* profiles = root.get("profiles"); profiles && profiles->is_table()) {
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
    if (const auto* default_profile = root.get("default_profile");
        default_profile && default_profile->is_string()) {
        config.default_profile = *default_profile->value<std::string>();
    }
    return config;
}

Result<Config> load_config(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // First run: parse the built-in default document so the caller sees
        // the same shape a user file has.
        return parse_config(default_config_toml());
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(
            Error::make(ErrorCode::ConfigParseFailed, "cannot open config file"));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse_config(buffer.str());
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

Result<Profile> match_auto_profile(
    const Config& config, GameId game,
    const std::vector<win32::DisplayInfo>& displays) {
    bool portrait_attached = false;
    for (const auto& display : displays) {
        if (display.is_attached &&
            display.right - display.left > 0 &&
            display.bottom - display.top > display.right - display.left) {
            portrait_attached = true;
            break;
        }
    }

    const Profile* fallback = nullptr;
    for (const auto& profile : config.profiles) {
        if (profile.game != game) {
            continue;
        }
        if (portrait_attached && profile.ui.mobile_ui) {
            return profile;
        }
        if (!portrait_attached && !profile.ui.mobile_ui && fallback == nullptr) {
            fallback = &profile;
        }
    }
    if (fallback != nullptr) {
        return *fallback;
    }
    return std::unexpected(Error::make(
        ErrorCode::ProfileNotFound,
        "no matching profile for game '" + std::string(to_string(game)) +
            "' (add one to config.toml)"));
}

}  // namespace hoyoflux::profile
