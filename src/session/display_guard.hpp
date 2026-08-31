#pragma once

// DisplayGuard (A8), current scope.
//
// The plan requires live monitoring of whatever storage Genshin uses to
// persist the session resolution (RegNotifyChangeKeyValue or file watch).
// That storage location is unknown until on-device research with a running
// game (plan §A8 first step), so this module ships the plan's documented
// compatibility fallback for journals created by older builds. Current v1
// sessions do not alter the Windows physical display mode and therefore do
// not record one.
//
// When the real storage is identified, a watcher gets added here and the
// engine's Restoring stage already provides the integration point.

#include "session/journal.hpp"

#include <cstdint>

namespace hoyoflux::session {

// Restore displays captured in a legacy journal. A snapshot that already
// equals the current mode is a successful no-op.
Result<void> restore_display_snapshot(const std::vector<JournalDisplay>& displays);

}  // namespace hoyoflux::session
