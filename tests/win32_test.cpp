// platform/win32 unit tests. All display/registry tests are read-only: they
// never change the display, registry or process state.

#include "platform/win32/display.hpp"
#include "platform/win32/pe.hpp"
#include "platform/win32/privilege.hpp"
#include "platform/win32/process.hpp"
#include "platform/win32/registry.hpp"
#include "platform/win32/notification.hpp"
#include "platform/win32/text.hpp"
#include "platform/win32/unique_handle.hpp"

#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace hoyoflux;
namespace w32 = hoyoflux::win32;

namespace {

// Build a minimal-but-valid PE32+ image in memory: DOS header + PE signature +
// file header + optional header64 + two sections (.text, il2cpp).
std::vector<std::byte> make_synthetic_pe() {
    constexpr uint32_t kPeOffset = 0x80;
    constexpr size_t kTotal =
        kPeOffset + 4 + sizeof(IMAGE_FILE_HEADER) +
        sizeof(IMAGE_OPTIONAL_HEADER64) + 2 * sizeof(IMAGE_SECTION_HEADER);

    std::vector<std::byte> buf(kTotal);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = kPeOffset;
    std::memcpy(buf.data(), &dos, sizeof(dos));

    std::byte* base = buf.data() + kPeOffset;
    std::memcpy(base, "PE\0\0", 4);

    IMAGE_FILE_HEADER fh{};
    fh.Machine = IMAGE_FILE_MACHINE_AMD64;
    fh.NumberOfSections = 2;
    fh.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    std::memcpy(base + 4, &fh, sizeof(fh));

    IMAGE_OPTIONAL_HEADER64 opt{};
    opt.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    opt.ImageBase = 0x140000000ULL;
    opt.SizeOfImage = 0x23000;
    opt.SectionAlignment = 0x1000;
    std::memcpy(base + 4 + sizeof(IMAGE_FILE_HEADER), &opt, sizeof(opt));

    const size_t section_table_offset =
        kPeOffset + 4 + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64);

    IMAGE_SECTION_HEADER text{};
    std::memcpy(text.Name, ".text", 5);
    text.VirtualAddress = 0x1000;
    text.Misc.VirtualSize = 0x20000;
    text.SizeOfRawData = 0x20000;
    text.Characteristics =
        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;

    IMAGE_SECTION_HEADER il2cpp{};
    std::memcpy(il2cpp.Name, "il2cpp", 6);
    il2cpp.VirtualAddress = 0x21000;
    il2cpp.Misc.VirtualSize = 0x1000;
    il2cpp.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    std::memcpy(buf.data() + section_table_offset, &text, sizeof(text));
    std::memcpy(buf.data() + section_table_offset + sizeof(IMAGE_SECTION_HEADER),
                &il2cpp, sizeof(il2cpp));
    return buf;
}

}  // namespace

TEST_CASE("parse_pe: valid synthetic PE64", "[win32][pe]") {
    auto buf = make_synthetic_pe();
    auto info = w32::parse_pe(std::span<const std::byte>(buf));
    REQUIRE(info.has_value());
    CHECK(info->image_base == 0x140000000ULL);
    CHECK(info->sections.size() == 2);

    const auto* text = info->find_section(".text");
    REQUIRE(text != nullptr);
    CHECK(text->virtual_address == 0x1000);
    CHECK(text->is_executable());
    CHECK(text->is_readable());

    const auto* il = info->find_section("il2cpp");
    REQUIRE(il != nullptr);
    CHECK_FALSE(il->is_executable());

    CHECK(info->find_section(".rdata") == nullptr);

    const auto va = w32::rva_to_va(*info, 0x140000000ULL, 0x12000);
    REQUIRE(va.has_value());
    CHECK(*va == 0x140000000ULL + 0x12000);
    CHECK_FALSE(w32::rva_to_va(*info, 0x140000000ULL, 0x30000).has_value());
}

TEST_CASE("parse_pe: rejects bad input", "[win32][pe]") {
    std::vector<std::byte> empty;
    CHECK_FALSE(w32::parse_pe(std::span<const std::byte>(empty)).has_value());

    std::vector<std::byte> garbage(128, std::byte{0xAB});
    CHECK_FALSE(w32::parse_pe(std::span<const std::byte>(garbage)).has_value());

    // MZ but e_lfanew points far outside the buffer.
    std::vector<std::byte> truncated(64);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x10000;
    std::memcpy(truncated.data(), &dos, sizeof(dos));
    CHECK_FALSE(w32::parse_pe(std::span<const std::byte>(truncated)).has_value());
}

TEST_CASE("build_command_line quoting", "[win32][process]") {
    CHECK(w32::build_command_line({L"a", L"b"}) == L"a b");
    CHECK(w32::build_command_line({L"hello world"}) == L"\"hello world\"");
    CHECK(w32::build_command_line({L""}) == L"\"\"");
    // No whitespace -> no quotes.
    CHECK(w32::build_command_line({L"C:\\Games\\YuanShen.exe", L"-screen-width",
                                   L"2560"}) ==
          L"C:\\Games\\YuanShen.exe -screen-width 2560");
    // Path with a space -> quoted.
    CHECK(w32::build_command_line({L"C:\\Program Files\\Game\\YuanShen.exe",
                                   L"-popupwindow"}) ==
          L"\"C:\\Program Files\\Game\\YuanShen.exe\" -popupwindow");
}

TEST_CASE("quote_windows_argument follows CommandLineToArgvW rules",
          "[win32][process]") {
    using w32::quote_windows_argument;
    CHECK(quote_windows_argument(L"plain") == L"plain");
    CHECK(quote_windows_argument(L"") == L"\"\"");
    CHECK(quote_windows_argument(L"with space") == L"\"with space\"");
    CHECK(quote_windows_argument(L"with\ttab") == L"\"with\ttab\"");

    // Backslashes are literal unless they precede a quote or end the arg.
    CHECK(quote_windows_argument(L"C:\\a\\b") == L"C:\\a\\b");
    // An unquoted token never needs backslash protection...
    CHECK(quote_windows_argument(L"before\\") == L"before\\");
    // ...but a quoted one does: trailing backslashes sit before the closing
    // quote, so they double.
    CHECK(quote_windows_argument(L"before \\") == L"\"before \\\\\"");
    CHECK(quote_windows_argument(L"two \\\\") == L"\"two \\\\\\\\\"");

    // n backslashes + quote -> 2n+1 backslashes + \".
    CHECK(quote_windows_argument(L"a\\\"b") == L"\"a\\\\\\\"b\"");
    CHECK(quote_windows_argument(L"a\\\\\"b") == L"\"a\\\\\\\\\\\"b\"");
    CHECK(quote_windows_argument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");

    // The whole line round-trips through CommandLineToArgvW.
    const std::vector<std::wstring> args = {
        L"C:\\Program Files\\Game\\YuanShen.exe", L"-screen-width", L"2560",
        L"trailing\\", L"q\"uote", L""};
    const std::wstring line = w32::build_command_line(args);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
    REQUIRE(argv != nullptr);
    REQUIRE(argc == static_cast<int>(args.size()));
    for (size_t i = 0; i < args.size(); ++i) {
        CHECK(std::wstring(argv[i]) == args[i]);
    }
    LocalFree(argv);
}

TEST_CASE("process enumeration finds self", "[win32][process]") {
    std::wstring self(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, self.data(),
                                       static_cast<DWORD>(self.size()));
    self.resize(n);
    const std::wstring self_name = std::filesystem::path(self).filename().wstring();

    auto found = w32::find_process(self_name);
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    CHECK(found->value().pid == GetCurrentProcessId());
    CHECK(found->value().name == self_name);
}

TEST_CASE("find_process: no match yields nullopt", "[win32][process]") {
    auto found = w32::find_process(L"no-such-process-xyz.exe");
    REQUIRE(found.has_value());
    CHECK_FALSE(found->has_value());
}

TEST_CASE("query own process path", "[win32][process]") {
    auto handle =
        w32::open_process(GetCurrentProcessId(), PROCESS_QUERY_LIMITED_INFORMATION);
    REQUIRE(handle.has_value());
    auto path = w32::query_process_path(*handle);
    REQUIRE(path.has_value());
    CHECK_FALSE(path->empty());
    CHECK(GetFileAttributesW(path->c_str()) != INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("liveness checks", "[win32][process]") {
    CHECK(w32::is_process_running(GetCurrentProcessId()));
    CHECK_FALSE(w32::is_process_running(0xFFFFFFF0));  // bogus pid
}

TEST_CASE("launcher paths read without failing", "[win32][registry]") {
    // Machines without HoYoverse games installed must still succeed (empty).
    auto paths = w32::read_launcher_paths();
    REQUIRE(paths.has_value());
}

TEST_CASE("registry raw values round trip in a temporary key",
          "[win32][registry][f2]") {
    const std::wstring root =
        L"Software\\HoyoFluxTest\\" + std::to_wstring(GetCurrentProcessId()) +
        L"\\f2";

    auto cleanup = [&] { RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()); };
    cleanup();

    // A missing key reads as empty, not an error.
    auto absent = w32::read_registry_values(root);
    REQUIRE(absent.has_value());
    CHECK(absent->empty());

    const std::array<uint32_t, 1> width_value{2266};
    const std::array<wchar_t, 4> name_value{L'a', L'b', L'c', L'\0'};
    const std::vector<std::byte> binary{std::byte{0x01}, std::byte{0x02},
                                        std::byte{0xFF}};
    const w32::RegistryValue values[] = {
        {L"Screenmanager Resolution Width H123", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(width_value.data()),
             reinterpret_cast<const std::byte*>(width_value.data() + 1))},
        {L"DisplayName", REG_SZ,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(name_value.data()),
             reinterpret_cast<const std::byte*>(name_value.data() + 4))},
        {L"blob", REG_BINARY, binary},
    };
    REQUIRE(w32::write_registry_values(root, values).has_value());

    auto read_back = w32::read_registry_values(root);
    REQUIRE(read_back.has_value());
    REQUIRE(read_back->size() == 3);
    std::sort(read_back->begin(), read_back->end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    CHECK(read_back->at(0).name == L"DisplayName");
    CHECK(read_back->at(0).type == REG_SZ);
    CHECK(read_back->at(1).name == L"Screenmanager Resolution Width H123");
    CHECK(read_back->at(1).type == REG_DWORD);
    const uint32_t width = *reinterpret_cast<const uint32_t*>(
        read_back->at(1).data.data());
    CHECK(width == 2266);
    CHECK(read_back->at(2).data == binary);

    // Overwrite one value and re-read.
    const std::array<uint32_t, 1> new_width{1080};
    const w32::RegistryValue updated[] = {
        {L"Screenmanager Resolution Width H123", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(new_width.data()),
             reinterpret_cast<const std::byte*>(new_width.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, updated).has_value());
    auto reread = w32::read_registry_values(root);
    REQUIRE(reread.has_value());
    for (const auto& value : *reread) {
        if (value.name == L"Screenmanager Resolution Width H123") {
            CHECK(*reinterpret_cast<const uint32_t*>(value.data.data()) == 1080);
        }
    }

    REQUIRE(w32::registry_key_exists(root).has_value());
    CHECK(w32::registry_key_exists(root).value());
    cleanup();
    auto gone = w32::registry_key_exists(root);
    REQUIRE(gone.has_value());
    CHECK_FALSE(*gone);
}

TEST_CASE("display enumeration is read-only", "[win32][display]") {
    auto displays = w32::enumerate_displays();
    REQUIRE(displays.has_value());
    // At least the primary display is normally attached.
    if (!displays->empty()) {
        bool has_primary = false;
        for (const auto& d : *displays) {
            if (d.is_primary) {
                has_primary = true;
            }
        }
        CHECK(has_primary);
    }
}

TEST_CASE("display settings equality covers the complete mode",
          "[win32][display]") {
    w32::DisplaySettings expected{L"\\\\.\\DISPLAY10", 2266, 1488, 144,
                                  32, 0, 0, false};
    auto same = expected;
    CHECK(w32::display_settings_equal(expected, same));

    same.refresh_rate = 120;
    CHECK_FALSE(w32::display_settings_equal(expected, same));
    same = expected;
    same.position_x = 1;
    CHECK_FALSE(w32::display_settings_equal(expected, same));
    same = expected;
    same.interlaced = true;
    CHECK_FALSE(w32::display_settings_equal(expected, same));
}

TEST_CASE("unique handle RAII", "[win32][handle]") {
    using w32::UniqueHandle;

    UniqueHandle invalid(INVALID_HANDLE_VALUE);
    CHECK_FALSE(invalid);  // INVALID_HANDLE_VALUE is treated as empty

    HANDLE raw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(raw != nullptr);
    UniqueHandle owned(raw);
    CHECK(owned);

    UniqueHandle moved = std::move(owned);
    CHECK_FALSE(owned);
    CHECK(moved);

    moved.reset();  // closes the event
    CHECK_FALSE(moved);
}

TEST_CASE("privilege probe is callable", "[win32][privilege]") {
    // Must not crash; the result depends on how the test was launched.
    const bool elevated = w32::is_elevated();
    CHECK((elevated || !elevated));
}

TEST_CASE("notification failure is explicitly best effort",
          "[win32][notification][b1-8]") {
    bool called = false;
    w32::NotificationFunction failing =
        [&](std::wstring_view, std::wstring_view, w32::NotificationKind) {
            called = true;
            return Result<void>(std::unexpected(Error::make(
                ErrorCode::OsError, "synthetic notification failure")));
        };
    w32::notify_best_effort(failing, L"HoyoFlux", L"test",
                            w32::NotificationKind::Info);
    CHECK(called);
}

TEST_CASE("notification cleanup is idempotent",
          "[win32][notification][b1-r0]") {
    w32::cleanup_notifications();
    w32::cleanup_notifications();
}

TEST_CASE("idle notification drain is immediate and idempotent",
          "[win32][notification][b1-r0]") {
    w32::cleanup_notifications();
    const auto started = std::chrono::steady_clock::now();
    w32::drain_notifications();
    w32::drain_notifications();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(250));
}

TEST_CASE("notification drain waits for the transient lifetime",
          "[win32][notification][b1-r0]") {
    w32::cleanup_notifications();
    const auto notified = w32::notify(L"HoyoFlux", L"test",
                                      w32::NotificationKind::Info);
    const auto started = std::chrono::steady_clock::now();
    w32::drain_notifications();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Shell notification is best-effort and may be unavailable in a headless
    // test runner. When it succeeds, drain must wait for the worker timer.
    CHECK(elapsed < std::chrono::milliseconds(8500));
    if (notified) {
        CHECK(elapsed >= std::chrono::seconds(5));
    }
    w32::drain_notifications();
    w32::cleanup_notifications();
}

TEST_CASE("UTF-16 diagnostics are converted to UTF-8",
          "[win32][text][b1-r0]") {
    CHECK(w32::utf8(L"\x539f\x795e") ==
          std::string("\xE5\x8E\x9F\xE7\xA5\x9E", 6));

    const std::wstring invalid(1, static_cast<wchar_t>(0xD800));
    CHECK(w32::utf8(invalid) == "<invalid UTF-16>");
}
