#include "game/game_adapter.hpp"

#include "game/genshin/genshin_adapter.hpp"
#include "game/starrail/starrail_adapter.hpp"

#include "scan/pattern_scanner.hpp"

namespace hoyoflux::game {

std::unique_ptr<GameAdapter> make_adapter(GameId game) {
    switch (game) {
    case GameId::Genshin:
        return std::make_unique<GenshinAdapter>();
    case GameId::StarRail:
        return std::make_unique<StarRailAdapter>();
    }
    return nullptr;
}

const ResolvedSignature* find_resolved(const std::vector<ResolvedSignature>& resolved,
                                       std::string_view id) {
    for (const auto& entry : resolved) {
        if (entry.resolved && entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

// Shared resolver loop: both games scan their signatures against every
// provided snapshot and keep the first match. A signature is resolved when
// every one of its resolvers produced a value.
Result<std::vector<ResolvedSignature>> resolve_all_signatures(
    const std::vector<scan::Signature>& signatures,
    const std::vector<scan::ModuleSnapshot>& snapshots) {
    std::vector<ResolvedSignature> out;
    out.reserve(signatures.size());
    for (const auto& sig : signatures) {
        ResolvedSignature resolved;
        resolved.id = sig.id;
        resolved.fields.resize(sig.resolvers.size(), 0);
        bool matched = false;
        bool all_resolved = true;
        for (const auto& snapshot : snapshots) {
            const auto* section = snapshot.find_section(sig.section);
            if (section == nullptr) {
                continue;
            }
            auto match = scan::scan_first(sig.pattern, section->bytes);
            if (!match) {
                continue;
            }
            matched = true;
            for (size_t r = 0; r < sig.resolvers.size(); ++r) {
                auto address = scan::resolve_match(sig, *section, *match, r);
                if (!address) {
                    all_resolved = false;
                    continue;
                }
                resolved.fields[r] = *address;
            }
            break;  // first snapshot hosting a match wins
        }
        resolved.resolved = matched && all_resolved;
        out.push_back(std::move(resolved));
    }
    return out;
}

}  // namespace hoyoflux::game
