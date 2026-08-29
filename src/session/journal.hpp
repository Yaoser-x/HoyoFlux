#pragma once

// The active-session journal: a small JSON file (schema 2) that makes every
// launcher crash recoverable. Written before the game is spawned, updated as
// the session advances, cleared ONLY after a verified clean completion (or a
// verified recovery - plan section 10.3: a failed restore never clears the
// file).
//
// Location: %LOCALAPPDATA%\HoyoFlux\state\active-session.json. Writes are
// atomic (temp file + rename) so a crash mid-write cannot corrupt it.

#include "domain/persistent_state.hpp"
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

// Everything a future recovery needs to undo this session. `required` stays
// true from the first recorded action until the journal is cleared: even a
// session that reached Running (patches live, memory rollback impossible)
// still owes the persistent-state / display restore if the launcher dies.
struct JournalRollback {
    bool required{false};
    std::vector<JournalDisplay> displays;  // physical mode snapshot
    std::optional<PersistentDisplayState> persistent_state;  // F2 snapshot
};

struct ActiveSessionJournal {
    int schema{2};
    SessionId session_id;
    GameId game{GameId::Genshin};
    uint32_t pid{0};
    SessionStage stage{SessionStage::Idle};
    JournalRollback rollback;
};

[[nodiscard]] std::filesystem::path journal_path();

Result<void> save_journal(const ActiveSessionJournal& journal);
// std::nullopt when no journal exists; a JournalCorrupt error when one does
// but cannot be parsed (kept on disk for diagnosis, never auto-deleted).
Result<std::optional<ActiveSessionJournal>> load_journal();
Result<void> clear_journal();

[[nodiscard]] std::string_view to_string(SessionStage stage);

}  // namespace hoyoflux::session
