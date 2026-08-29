// SessionEngine + journal tests. The lifecycle runs against a real trivial
// process (cmd.exe) with a stub adapter, so no game is needed: the journal
// round trip, the suspended-spawn -> resume flow, the module-wait
// (resume/wait/re-suspend) flow and crash recovery are all covered.

#include "game/game_adapter.hpp"
#include "platform/win32/process.hpp"
#include "session/journal.hpp"
#include "session/session_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace hoyoflux;
namespace session = hoyoflux::session;

namespace {

// cmd.exe: present on every Windows test host.
std::filesystem::path cmd_path() {
    wchar_t comspec[MAX_PATH];
    const DWORD size = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    REQUIRE(size > 0);
    return std::filesystem::path(std::wstring(comspec, size));
}

// Adapter stub: install = cmd.exe, module requirements configurable,
// signatures empty, plan empty. The session flow never writes anything into
// the target process.
class StubAdapter final : public game::GameAdapter {
public:
    explicit StubAdapter(std::vector<game::ModuleRequirement> modules)
        : modules_(std::move(modules)) {}

    GameId id() const override { return GameId::Genshin; }
    CapabilityReport capabilities(const GameInstall&,
                                  const Profile&) const override {
        // Permissive stub: every capability supported so validation passes.
        CapabilityReport report;
        for (int i = 0; i <= static_cast<int>(Capability::PersistentStateGuard);
             ++i) {
            report.entries.push_back({static_cast<Capability>(i),
                                      CapabilityStatus::Supported, "stub"});
        }
        return report;
    }
    Result<GameInstall> locate_installation(game::Region) const override {
        return GameInstall{GameId::Genshin, true, cmd_path()};
    }
    Result<bool> is_old_version(const GameInstall&) const override { return false; }
    Result<game::ModuleRequirements> module_requirements(
        const GameInstall&, const Profile&) const override {
        return game::ModuleRequirements{modules_};
    }
    Result<std::vector<game::ResolvedSignature>> resolve_signatures(
        const std::vector<scan::ModuleSnapshot>&) const override {
        return std::vector<game::ResolvedSignature>{};
    }
    Result<PatchPlan> build_patch_plan(const game::PatchContext&) const override {
        return PatchPlan{};
    }

private:
    std::vector<game::ModuleRequirement> modules_;
};

LaunchRequest make_request() {
    LaunchRequest request;
    request.game = GameId::Genshin;
    request.profile.id = "test";
    request.profile.runtime.fps = 120;
    request.game_args = {L"/c", L"ping", L"-n", L"2", L"127.0.0.1"};
    return request;
}

session::SessionConfig fast_config() {
    session::SessionConfig config;
    config.module_wait_timeout_ms = 15000;
    config.module_poll_interval_ms = 20;
    return config;
}

// A pid that is guaranteed dead: spawn cmd /c exit and let it finish.
uint32_t dead_pid() {
    auto path = cmd_path();
    auto exited = win32::spawn_suspended(path, {L"/c", L"exit"}, path.parent_path(), 0);
    REQUIRE(exited.has_value());
    ResumeThread(exited->thread.get());
    WaitForSingleObject(exited->process.get(), 10000);
    return exited->pid;
}

}  // namespace

TEST_CASE("journal round trip keeps every field", "[session][journal]") {
    session::ActiveSessionJournal journal;
    journal.session_id = "session-123";
    journal.game = GameId::StarRail;
    journal.pid = 4242;
    journal.stage = SessionStage::Patching;
    journal.rollback_required = true;
    journal.displays.push_back(
        session::JournalDisplay{win32::DisplaySettings{
            L"\\\\.\\DISPLAY1", 2560, 1440, 144, 32, 0, 0, false}});
    journal.displays.push_back(
        session::JournalDisplay{win32::DisplaySettings{
            L"\\\\.\\DISPLAY2", 1080, 1920, 60, 32, 2560, -300, true}});

    REQUIRE(session::save_journal(journal).has_value());
    auto loaded = session::load_journal();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->session_id == "session-123");
    CHECK((*loaded)->game == GameId::StarRail);
    CHECK((*loaded)->pid == 4242);
    CHECK((*loaded)->stage == SessionStage::Patching);
    CHECK((*loaded)->rollback_required);
    REQUIRE((*loaded)->displays.size() == 2);
    CHECK((*loaded)->displays[0].settings.device_name == L"\\\\.\\DISPLAY1");
    CHECK((*loaded)->displays[0].settings.width == 2560);
    CHECK((*loaded)->displays[0].settings.refresh_rate == 144);
    CHECK((*loaded)->displays[1].settings.position_x == 2560);
    CHECK((*loaded)->displays[1].settings.position_y == -300);
    CHECK((*loaded)->displays[1].settings.interlaced);

    REQUIRE(session::clear_journal().has_value());
    auto gone = session::load_journal();
    REQUIRE(gone.has_value());
    CHECK_FALSE(gone->has_value());
}

TEST_CASE("corrupt journal is reported, not silently ignored", "[session][journal]") {
    session::ActiveSessionJournal journal;
    journal.pid = 1;
    REQUIRE(session::save_journal(journal).has_value());
    {
        std::ofstream out(session::journal_path(), std::ios::binary | std::ios::trunc);
        out << "{\"schema\": 1, \"pid\": 12";  // truncated
    }
    auto loaded = session::load_journal();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == ErrorCode::JournalCorrupt);
    REQUIRE(session::clear_journal().has_value());
}

TEST_CASE("recover reacts to absent, stale and live journals", "[session][recover]") {
    auto adapter = std::make_unique<StubAdapter>(std::vector<game::ModuleRequirement>{});
    session::SessionEngine engine(*adapter, fast_config());

    REQUIRE(session::clear_journal().has_value());
    auto none = engine.recover();
    REQUIRE(none.has_value());
    CHECK(*none == session::RecoveryAction::None);

    // Stale journal: dead pid -> cleared.
    session::ActiveSessionJournal stale;
    stale.pid = dead_pid();
    REQUIRE(session::save_journal(stale).has_value());
    auto cleaned = engine.recover();
    REQUIRE(cleaned.has_value());
    CHECK(*cleaned == session::RecoveryAction::CleanedStaleJournal);
    auto gone = session::load_journal();
    REQUIRE(gone.has_value());
    CHECK_FALSE(gone->has_value());

    // Live journal: current test process pid -> untouched.
    session::ActiveSessionJournal live;
    live.pid = GetCurrentProcessId();
    REQUIRE(session::save_journal(live).has_value());
    auto running = engine.recover();
    REQUIRE(running.has_value());
    CHECK(*running == session::RecoveryAction::GameStillRunning);
    auto still_there = session::load_journal();
    REQUIRE(still_there.has_value());
    CHECK(still_there->has_value());
    REQUIRE(session::clear_journal().has_value());
}

TEST_CASE("full session lifecycle with main-module-only requirements",
          "[session][engine]") {
    StubAdapter adapter({game::ModuleRequirement{"", {".text"}, false}});
    session::SessionEngine engine(adapter, fast_config());

    auto context = engine.run(make_request());
    REQUIRE(context.has_value());
    CHECK(context->stage == SessionStage::Completed);
    CHECK(context->pid != 0);
    CHECK_FALSE(context->id.empty());

    // Clean completion leaves no journal behind.
    auto journal = session::load_journal();
    REQUIRE(journal.has_value());
    CHECK_FALSE(journal->has_value());
}

TEST_CASE("session waits for lazily loaded modules via resume/suspend",
          "[session][engine]") {
    // kernel32.dll is not mapped in a CREATE_SUSPENDED process; requiring it
    // forces the engine through resume -> poll -> re-suspend.
    StubAdapter adapter({game::ModuleRequirement{"kernel32.dll", {".text"}, false}});
    session::SessionEngine engine(adapter, fast_config());

    auto context = engine.run(make_request());
    REQUIRE(context.has_value());
    CHECK(context->stage == SessionStage::Completed);
}

TEST_CASE("failed module wait fails the session and cleans the journal",
          "[session][engine]") {
    // A DLL that cmd.exe will never load.
    StubAdapter adapter(
        {game::ModuleRequirement{"hoyoflux_nonexistent.dll", {".text"}, false}});
    session::SessionEngine engine(adapter, [] {
        session::SessionConfig config;
        config.module_wait_timeout_ms = 800;
        config.module_poll_interval_ms = 20;
        return config;
    }());

    auto context = engine.run(make_request());
    REQUIRE_FALSE(context.has_value());
    CHECK(context.error().code == ErrorCode::ModuleNotFound);

    auto journal = session::load_journal();
    REQUIRE(journal.has_value());
    CHECK_FALSE(journal->has_value());  // cleaned up, not left behind
}
