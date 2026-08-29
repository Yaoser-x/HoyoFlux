#pragma once

// The active-session journal: a small JSON file that makes every launcher
// crash recoverable. Written before the game is spawned, updated as the
// session advances, cleared on clean completion. A later launch that finds a
// stale journal recovers (see session_engine.hpp).
//
// Location: %LOCALAPPDATA%\HoyoFlux\state\active-session.json. Writes are
// atomic (temp file + rename) so a crash mid-write cannot corrupt it.

#include "domain/session.hpp"
#include "platform/win32/display.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hoyoflux::session {

struct JournalDisplay {
    win32::DisplaySettings settings;
};

struct ActiveSessionJournal {
    int schema{1};
    SessionId session_id;
    GameId game{GameId::Genshin};
    uint32_t pid{0};
    SessionStage stage{SessionStage::Idle};
    bool rollback_required{false};
    std::vector<JournalDisplay> displays;
};

[[nodiscard]] std::filesystem::path journal_path();

Result<void> save_journal(const ActiveSessionJournal& journal);
// std::nullopt when no journal exists; a JournalCorrupt error when one does
// but cannot be parsed (kept on disk for diagnosis, never auto-deleted).
Result<std::optional<ActiveSessionJournal>> load_journal();
Result<void> clear_journal();

[[nodiscard]] std::string_view to_string(SessionStage stage);

}  // namespace hoyoflux::session
