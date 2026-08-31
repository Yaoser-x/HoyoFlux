#pragma once

// `hoyoflux doctor` (F12): strictly read-only environment inspection.
// It never patches a game, never writes memory, and never changes state;
// when a game process happens to be running, its signatures are resolved
// live for the freshness report (read access only).

namespace hoyoflux::app {

// Runs every doctor check and returns the process exit code (0 = all green).
int run_doctor(bool verbose);

}  // namespace hoyoflux::app
