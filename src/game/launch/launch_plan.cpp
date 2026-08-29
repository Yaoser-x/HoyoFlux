#include "game/launch/launch_plan.hpp"

#include <cwctype>
#include <string>

namespace hoyoflux::game {

Result<std::vector<std::wstring>> build_render_arguments(
    const RenderPolicy& render) {
    std::vector<std::wstring> args;

    if (render.resolution.has_value()) {
        args.push_back(L"-screen-width");
        args.push_back(std::to_wstring(render.resolution->width));
        args.push_back(L"-screen-height");
        args.push_back(std::to_wstring(render.resolution->height));
    }

    if (render.fullscreen.has_value()) {
        switch (*render.fullscreen) {
        case FullscreenMode::Windowed:
            args.push_back(L"-screen-fullscreen");
            args.push_back(L"0");
            break;
        case FullscreenMode::Exclusive:
            args.push_back(L"-screen-fullscreen");
            args.push_back(L"1");
            break;
        case FullscreenMode::Borderless:
            // Guarded by the capability contract (F0); refuse anyway so a
            // future caller cannot bypass the gate.
            return std::unexpected(Error::make(
                ErrorCode::NotSupported,
                "borderless fullscreen cannot be set via launch arguments; "
                "use \"windowed\"/\"exclusive\" or set it in-game"));
        }
    }
    return args;
}

bool is_managed_launch_field(std::wstring_view token) {
    // Unity flags are matched case-insensitively; profiles and passthrough
    // lists are tiny, so a normalized copy is fine.
    std::wstring lower(token);
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return lower == L"-screen-width" || lower == L"-screen-height" ||
           lower == L"-screen-fullscreen";
}

Result<std::vector<std::wstring>> merge_passthrough(
    const std::vector<std::wstring>& managed,
    const std::vector<std::wstring>& passthrough, std::string_view context) {
    for (const auto& token : passthrough) {
        if (is_managed_launch_field(token)) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidArgument,
                "passthrough argument '" + std::string(token.begin(), token.end()) +
                    "' conflicts with a profile-managed render field (" +
                    std::string(context) +
                    "); remove it from the passthrough or change the profile"));
        }
    }
    std::vector<std::wstring> merged = managed;
    merged.insert(merged.end(), passthrough.begin(), passthrough.end());
    return merged;
}

}  // namespace hoyoflux::game
