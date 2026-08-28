// Domain model unit tests.

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/launch_request.hpp"
#include "domain/profile.hpp"
#include "domain/session.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace hoyoflux;

TEST_CASE("GameId stringifies", "[domain][game]") {
    CHECK(to_string(GameId::Genshin) == "genshin");
    CHECK(to_string(GameId::StarRail) == "starrail");
}

TEST_CASE("Resolution empty check", "[domain][profile]") {
    Resolution r;
    CHECK(r.empty());
    Resolution full{2560, 1440};
    CHECK_FALSE(full.empty());
    CHECK(full == Resolution{2560, 1440});
}

TEST_CASE("Profile defaults: session-scoped, mobile UI off, power save off",
          "[domain][profile]") {
    Profile p;
    p.id = "desktop";
    CHECK(p.render.persistence == ResolutionPersistence::Session);
    CHECK_FALSE(p.ui.mobile_ui);
    CHECK(p.runtime.power_save == PowerSavePolicy::Disabled);
    CHECK(p.runtime.fps == 120);
    CHECK(p.match == MatchPolicy::Manual);
}

TEST_CASE("Render policy carries optional resolution and monitor",
          "[domain][profile]") {
    Profile p;
    p.id = "ipad";
    p.render.resolution = Resolution{2266, 1488};
    p.render.monitor = 1;
    REQUIRE(p.render.resolution.has_value());
    CHECK(p.render.resolution->width == 2266);
    CHECK(p.render.monitor.value() == 1);
    CHECK(p.render.fullscreen == FullscreenMode::Borderless);
}

TEST_CASE("Result error path", "[domain][error]") {
    Result<int> ok = 42;
    CHECK(ok.has_value());
    CHECK(*ok == 42);

    Result<int> err = std::unexpected(
        Error::make(ErrorCode::SectionNotFound, "no .text section"));
    CHECK_FALSE(err.has_value());
    CHECK(err.error().code == ErrorCode::SectionNotFound);
    CHECK(err.error().message == "no .text section");
    CHECK(err.error().os_code == 0);
}

TEST_CASE("Error os_code is carried for Win32 failures", "[domain][error]") {
    Error e = Error::make(ErrorCode::OsError, "CreateProcessW failed", 5);
    CHECK(e.code == ErrorCode::OsError);
    CHECK(e.os_code == 5);
    CHECK(to_string(e.code) == "os-error");
}

TEST_CASE("LaunchRequest defaults to non-resident Genshin", "[domain][launch]") {
    LaunchRequest req;
    CHECK(req.game == GameId::Genshin);
    CHECK_FALSE(req.resident);
    CHECK(req.game_args.empty());
}

TEST_CASE("SessionContext starts idle with no pid", "[domain][session]") {
    SessionContext ctx;
    CHECK(ctx.stage == SessionStage::Idle);
    CHECK(ctx.pid == 0);
    CHECK_FALSE(ctx.rollback_required);
}
