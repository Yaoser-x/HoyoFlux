#include "profile/config.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <system_error>
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
            profile.match.device_name =
                std::wstring(device->begin(), device->end());
        }
        if (const auto resolution = opt_string(table, "resolution")) {
            auto parsed = parse_resolution(*resolution, context + " match");
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            profile.match.resolution = *parsed;
        }
        if (const auto aspect = opt_double(table, "aspect_ratio")) {
            if (*aspect <= 0.0 || *aspect > 10.0) {
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
schema = 1
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

[profiles.xiaomi.render]
resolution = "1220x2712"
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
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& error) {
        return std::unexpected(Error::make(
            ErrorCode::ConfigParseFailed,
            "TOML parse error: " + std::string(error.description())));
    }

    Config config;
    // Forward-compatibility key (plan 18.4): absent = schema 1. A future
    // schema bumps this and migrates old files on load.
    if (const auto schema = root.get("schema");
        schema && schema->is_integer()) {
        const auto value = schema->value<int64_t>().value_or(0);
        if (value != 1) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseFailed,
                "unsupported config schema " + std::to_string(static_cast<long long>(value)) +
                    " (this build understands schema 1); update HoyoFlux"));
        }
    }
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
