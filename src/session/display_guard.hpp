#pragma once

// DisplayGuard (A8), current scope.
//
// The plan requires live monitoring of whatever storage Genshin uses to
// persist the session resolution (RegNotifyChangeKeyValue or file watch).
// That storage location is unknown until on-device research with a running
// game (plan §A8 first step), so this module ships the plan's documented
// fallback: the session captures a display snapshot up front (it lives in
// the journal) and unconditionally restores it when the session ends.
//
// When the real storage is identified, a watcher gets added here and the
// engine's Restoring stage already provides the integration point.

#include "session/journal.hpp"

#include <cstdint>

namespace hoyoflux::session {

// Restore every display captured in the journal snapshot. Best effort per
// display: a failure on one device does not stop the others.
Result<void> restore_display_snapshot(const std::vector<JournalDisplay>& displays);

}  // namespace hoyoflux::session
