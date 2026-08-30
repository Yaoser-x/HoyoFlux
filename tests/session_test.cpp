// SessionEngine + journal tests. The lifecycle runs against a real trivial
// process (cmd.exe) with a stub adapter, so no game is needed: the journal
// round trip, the suspended-spawn -> resume flow, the module-wait
// (resume/wait/re-suspend) flow and crash recovery are all covered.

#include "game/game_adapter.hpp"
#include "platform/win32/process.hpp"
#include "platform/win32/registry.hpp"
#include "session/journal.hpp"
#include "session/persistent_state_guard.hpp"
#include "session/session_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_assertion_result.hpp>

namespace w32 = hoyoflux::win32;

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
    Result<hoyoflux::game::GameLaunchPlan> build_launch_plan(
        const GameInstall&, const LaunchRequest& request) const override {
        // The stub's game_args are cmd.exe arguments, passed through as-is.
        hoyoflux::game::GameLaunchPlan plan;
        plan.executable = cmd_path();
        plan.working_directory = cmd_path().parent_path();
        plan.arguments = request.game_args;
        return plan;
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

TEST_CASE("journal v2 round trip keeps every field", "[session][journal]") {
    session::ActiveSessionJournal journal;
    journal.session_id = "session-123";
    journal.game = GameId::StarRail;
    journal.pid = 4242;
    journal.stage = SessionStage::Patching;
    journal.rollback.required = true;
    journal.rollback.displays.push_back(
        session::JournalDisplay{win32::DisplaySettings{
            L"\\\\.\\DISPLAY1", 2560, 1440, 144, 32, 0, 0, false}});
    journal.rollback.displays.push_back(
        session::JournalDisplay{win32::DisplaySettings{
            L"\\\\.\\DISPLAY2", 1080, 1920, 60, 32, 2560, -300, true}});

    const std::array<uint32_t, 1> width{2560};
    const std::array<uint32_t, 1> height{144};
    hoyoflux::PersistentDisplayState persistent;
    hoyoflux::PersistentSettingSet set;
    set.root = L"Software\\miHoYo\\Genshin Impact";
    set.settings.push_back(hoyoflux::PersistentSetting{
        L"Screenmanager Resolution Width H907608738", REG_DWORD,
        std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(width.data()),
            reinterpret_cast<const std::byte*>(width.data() + 1))});
    set.settings.push_back(hoyoflux::PersistentSetting{
        L"Screenmanager Resolution Height H907608738", REG_DWORD,
        std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(height.data()),
            reinterpret_cast<const std::byte*>(height.data() + 1))});
    persistent.sets.push_back(std::move(set));
    journal.rollback.persistent_state = std::move(persistent);

    REQUIRE(session::save_journal(journal).has_value());
    auto loaded = session::load_journal();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->session_id == "session-123");
    CHECK((*loaded)->game == GameId::StarRail);
    CHECK((*loaded)->pid == 4242);
    CHECK((*loaded)->stage == SessionStage::Patching);
    CHECK((*loaded)->rollback.required);
    REQUIRE((*loaded)->rollback.displays.size() == 2);
    CHECK((*loaded)->rollback.displays[0].settings.device_name ==
          L"\\\\.\\DISPLAY1");
    CHECK((*loaded)->rollback.displays[0].settings.width == 2560);
    CHECK((*loaded)->rollback.displays[0].settings.refresh_rate == 144);
    CHECK((*loaded)->rollback.displays[1].settings.position_x == 2560);
    CHECK((*loaded)->rollback.displays[1].settings.position_y == -300);
    CHECK((*loaded)->rollback.displays[1].settings.interlaced);

    // The persistent state survives the JSON round trip byte-exact.
    REQUIRE((*loaded)->rollback.persistent_state.has_value());
    REQUIRE(game::persistent_state_equals(*journal.rollback.persistent_state,
                                          *(*loaded)->rollback.persistent_state));

    REQUIRE(session::clear_journal().has_value());
    auto gone = session::load_journal();
    REQUIRE(gone.has_value());
    CHECK_FALSE(gone->has_value());
}

TEST_CASE("schema 1 journals still parse (physical displays only)",
          "[session][journal]") {
    session::ActiveSessionJournal modern;
    modern.session_id = "legacy";
    modern.pid = 7;
    REQUIRE(session::save_journal(modern).has_value());
    {
        std::ofstream out(session::journal_path(),
                          std::ios::binary | std::ios::trunc);
        out << R"({"schema": 1, "session_id": "legacy", "game": "genshin",)"
               R"( "pid": 7, "stage": "patching", "rollback_required": true,)"
               R"( "displays": [{"device_name": "\\\\.\\DISPLAY1", "width": 2560,)"
               R"( "height": 1440, "refresh_rate": 144, "bits_per_pixel": 32,)"
               R"( "position_x": 0, "position_y": 0, "interlaced": false}]})";
    }
    auto loaded = session::load_journal();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->schema == 1);
    CHECK((*loaded)->rollback.required);
    REQUIRE((*loaded)->rollback.displays.size() == 1);
    CHECK((*loaded)->rollback.displays[0].settings.width == 2560);
    CHECK_FALSE((*loaded)->rollback.persistent_state.has_value());
    REQUIRE(session::clear_journal().has_value());
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

    // Stale journal: dead pid -> real recovery (nothing recorded beyond the
    // physical snapshot, so the restore is trivially complete -> cleared).
    session::ActiveSessionJournal stale;
    stale.pid = dead_pid();
    REQUIRE(session::save_journal(stale).has_value());
    auto recovered = engine.recover();
    REQUIRE(recovered.has_value());
    CHECK(*recovered == session::RecoveryAction::Recovered);
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

TEST_CASE("recovery restores game persistent state before clearing the journal",
          "[session][recover][f4]") {
    auto adapter = std::make_unique<StubAdapter>(std::vector<game::ModuleRequirement>{});
    session::SessionEngine engine(*adapter, fast_config());

    const std::wstring root = L"Software\\HoyoFluxTest\\" +
                              std::to_wstring(GetCurrentProcessId()) +
                              L"\\recover";
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
    const std::array<uint32_t, 1> desktop{2560};
    const w32::RegistryValue desktop_state[] = {
        {L"Screenmanager Resolution Width H123", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(desktop.data()),
             reinterpret_cast<const std::byte*>(desktop.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, desktop_state).has_value());

    std::vector<w32::RegistryValue> unrelated;
    for (uint32_t i = 0; i < 20; ++i) {
        unrelated.push_back({
            L"UnrelatedSetting" + std::to_wstring(i), REG_DWORD,
            std::vector<std::byte>(reinterpret_cast<const std::byte*>(&i),
                                   reinterpret_cast<const std::byte*>(&i + 1))});
    }
    REQUIRE(w32::write_registry_values(root, unrelated).has_value());

    // Journal records the desktop snapshot; then the "game" rewrites it.
    const std::array<uint32_t, 1> ipad{1920};
    const w32::RegistryValue game_wrote[] = {
        {L"Screenmanager Resolution Width H123", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(ipad.data()),
             reinterpret_cast<const std::byte*>(ipad.data() + 1))},
    };

    session::ActiveSessionJournal journal;
    journal.pid = dead_pid();
    journal.rollback.required = true;
    hoyoflux::PersistentDisplayState persistent;
    hoyoflux::PersistentSettingSet set;
    set.root = root;
    set.settings.push_back(hoyoflux::PersistentSetting{
        L"Screenmanager Resolution Width H123", REG_DWORD,
        std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(desktop.data()),
            reinterpret_cast<const std::byte*>(desktop.data() + 1))});
    persistent.sets.push_back(std::move(set));
    journal.rollback.persistent_state = std::move(persistent);
    REQUIRE(session::save_journal(journal).has_value());
    REQUIRE(w32::write_registry_values(root, game_wrote).has_value());

    auto action = engine.recover();
    REQUIRE(action.has_value());
    CHECK(*action == session::RecoveryAction::Recovered);

    // The recorded value is back, and the journal is gone (plan 10.3: only
    // after a verified restore).
    auto values = w32::read_registry_values(root);
    REQUIRE(values.has_value());
    bool restored = false;
    for (const auto& value : *values) {
        if (value.name == L"Screenmanager Resolution Width H123") {
            restored =
                *reinterpret_cast<const uint32_t*>(value.data.data()) == 2560;
        }
    }
    CHECK(restored);
    auto gone = session::load_journal();
    REQUIRE(gone.has_value());
    CHECK_FALSE(gone->has_value());
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
}

TEST_CASE("failed recovery keeps the journal for a retry",
          "[session][recover][f4]") {
    auto adapter = std::make_unique<StubAdapter>(std::vector<game::ModuleRequirement>{});
    session::SessionEngine engine(*adapter, fast_config());

    // A root whose key name exceeds the registry's per-key length limit:
    // the restore write must fail, so the journal must survive.
    session::ActiveSessionJournal journal;
    journal.pid = dead_pid();
    journal.rollback.required = true;
    hoyoflux::PersistentDisplayState persistent;
    hoyoflux::PersistentSettingSet set;
    set.root = L"Software\\HoyoFluxTest\\" + std::wstring(300, L'x');
    set.settings.push_back(hoyoflux::PersistentSetting{
        L"Screenmanager Resolution Width H123", REG_DWORD,
        std::vector<std::byte>(4, std::byte{0})});
    persistent.sets.push_back(std::move(set));
    journal.rollback.persistent_state = std::move(persistent);
    REQUIRE(session::save_journal(journal).has_value());

    auto action = engine.recover();
    REQUIRE(action.has_value());
    CHECK(*action == session::RecoveryAction::RecoveryFailed);

    auto still_there = session::load_journal();
    REQUIRE(still_there.has_value());
    REQUIRE(still_there->has_value());
    REQUIRE(session::clear_journal().has_value());
}

TEST_CASE("persistent state guard snaps changed values back event-driven",
          "[session][guard]") {
    const std::wstring root = L"Software\\HoyoFluxTest\\" +
                              std::to_wstring(GetCurrentProcessId()) +
                              L"\\guard";
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());

    const std::array<uint32_t, 1> width{2560};
    const w32::RegistryValue desktop_state[] = {
        {L"Screenmanager Resolution Width H123", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(width.data()),
             reinterpret_cast<const std::byte*>(width.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, desktop_state).has_value());

    std::vector<w32::RegistryValue> guard_unrelated;
    for (uint32_t i = 0; i < 20; ++i) {
        guard_unrelated.push_back({
            L"UnrelatedSetting" + std::to_wstring(i), REG_DWORD,
            std::vector<std::byte>(reinterpret_cast<const std::byte*>(&i),
                                   reinterpret_cast<const std::byte*>(&i + 1))});
    }
    REQUIRE(w32::write_registry_values(root, guard_unrelated).has_value());

    hoyoflux::PersistentDisplayState snapshot;
    hoyoflux::PersistentSettingSet set;
    set.root = root;
    set.settings.push_back(hoyoflux::PersistentSetting{
        L"Screenmanager Resolution Width H123", REG_DWORD,
        std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(width.data()),
            reinterpret_cast<const std::byte*>(width.data() + 1))});
    snapshot.sets.push_back(std::move(set));

    session::PersistentStateGuard guard;
    REQUIRE(guard.start(snapshot).has_value());

    // An unrelated write wakes the registry notification but must not be
    // mistaken for protected-state drift or cause a restore loop.
    const std::array<uint32_t, 1> unrelated_update{99};
    const w32::RegistryValue unrelated_wrote[] = {
        {L"UnrelatedSetting0", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(unrelated_update.data()),
             reinterpret_cast<const std::byte*>(unrelated_update.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, unrelated_wrote).has_value());
    Sleep(100);
    CHECK(guard.restore_count() == 0);

    // Simulate the game rewriting its persistent value.
    const std::array<uint32_t, 1> ipad{1920};
    const w32::RegistryValue game_wrote[] = {
        {L"Screenmanager Resolution Width H123", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(ipad.data()),
             reinterpret_cast<const std::byte*>(ipad.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, game_wrote).has_value());

    // The guard restores without any polling from the test thread.
    bool restored = false;
    for (int i = 0; i < 100 && !restored; ++i) {
        Sleep(20);
        auto values = w32::read_registry_values(root);
        REQUIRE(values.has_value());
        for (const auto& value : *values) {
            if (value.name == L"Screenmanager Resolution Width H123") {
                restored = *reinterpret_cast<const uint32_t*>(value.data.data()) == 2560;
            }
        }
    }
    CHECK(restored);
    CHECK(guard.restore_count() >= 1);

    // A protected value created during the session is removed exactly.
    const w32::RegistryValue created[] = {
        {L"Screenmanager SessionOnly H456", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(ipad.data()),
             reinterpret_cast<const std::byte*>(ipad.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, created).has_value());
    bool removed = false;
    for (int i = 0; i < 100 && !removed; ++i) {
        Sleep(20);
        auto values = w32::read_registry_values(root);
        REQUIRE(values.has_value());
        removed = std::none_of(values->begin(), values->end(), [](const auto& value) {
            return value.name == L"Screenmanager SessionOnly H456";
        });
    }
    CHECK(removed);

    guard.stop();
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
}
