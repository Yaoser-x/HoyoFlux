#include "session/journal.hpp"

#include "domain/game.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include <shlobj.h>
#include <windows.h>

namespace hoyoflux::session {
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON model + parser. The journal schema is flat and produced by
// this same module; the parser is strict enough to reject truncated files
// (which is the failure mode we care about) without being a general JSON
// library.
// ---------------------------------------------------------------------------

struct JsonValue;

using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind{Kind::Null};
    bool boolean{false};
    double number{0};
    std::string text;
    std::vector<JsonValue> items;
    JsonObject members;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : in_(input) {}

    Result<JsonValue> parse() {
        try {
            skip_ws();
            JsonValue value = parse_value();
            skip_ws();
            if (pos_ != in_.size()) {
                fail("trailing bytes after JSON document");
            }
            return value;
        } catch (const ParseAbort& abort) {
            return std::unexpected(Error::make(ErrorCode::JournalCorrupt, abort.what));
        }
    }

private:
    [[noreturn]] void fail(const char* what) {
        throw ParseAbort{what};
    }
    struct ParseAbort {
        const char* what;
    };

    void skip_ws() {
        while (pos_ < in_.size() &&
               (in_[pos_] == ' ' || in_[pos_] == '\t' || in_[pos_] == '\n' ||
                in_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool consume(char c) {
        if (pos_ < in_.size() && in_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!consume(c)) {
            fail("expected character");
        }
    }

    JsonValue parse_value() {
        skip_ws();
        if (consume('{')) {
            JsonValue value;
            value.kind = JsonValue::Kind::Object;
            value.members = parse_object();
            return value;
        }
        if (consume('[')) {
            JsonValue value;
            value.kind = JsonValue::Kind::Array;
            value.items = parse_array();
            return value;
        }
        if (consume('"')) {
            return parse_string_value();
        }
        if (consume('t')) {
            expect_literal("rue");
            return JsonValue{.kind = JsonValue::Kind::Bool, .boolean = true};
        }
        if (consume('f')) {
            expect_literal("alse");
            return JsonValue{.kind = JsonValue::Kind::Bool, .boolean = false};
        }
        if (consume('n')) {
            expect_literal("ull");
            return {};
        }
        return parse_number();
    }

    void expect_literal(std::string_view literal) {
        if (in_.substr(pos_, literal.size()) != literal) {
            fail("expected literal");
        }
        pos_ += literal.size();
    }

    JsonValue parse_number() {
        const size_t start = pos_;
        while (pos_ < in_.size() &&
               (in_[pos_] == '-' || in_[pos_] == '+' || in_[pos_] == '.' ||
                (in_[pos_] >= '0' && in_[pos_] <= '9') || in_[pos_] == 'e' ||
                in_[pos_] == 'E')) {
            ++pos_;
        }
        if (pos_ == start) {
            fail("expected value");
        }
        double number = 0;
        const auto [ptr, ec] = std::from_chars(in_.data() + start,
                                               in_.data() + pos_, number);
        if (ec != std::errc{} || ptr != in_.data() + pos_) {
            fail("malformed number");
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number = number;
        return value;
    }

    std::string parse_string() {
        std::string out;
        while (pos_ < in_.size() && in_[pos_] != '"') {
            if (in_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= in_.size()) {
                    fail("unterminated escape");
                }
                switch (in_[pos_]) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4 >= in_.size()) {
                        fail("truncated \\u escape");
                    }
                    unsigned code = 0;
                    for (int i = 1; i <= 4; ++i) {
                        const char hex = in_[pos_ + i];
                        code *= 16;
                        if (hex >= '0' && hex <= '9') {
                            code += static_cast<unsigned>(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            code += static_cast<unsigned>(hex - 'a' + 10);
                        } else if (hex >= 'A' && hex <= 'F') {
                            code += static_cast<unsigned>(hex - 'A' + 10);
                        } else {
                            fail("bad \\u escape");
                        }
                    }
                    pos_ += 4;
                    // Encode as UTF-8 (surrogates unsupported: journal
                    // strings are paths/names, never astral plane).
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: fail("unknown escape");
                }
                ++pos_;
                continue;
            }
            out.push_back(in_[pos_]);
            ++pos_;
        }
        expect('"');
        return out;
    }

    JsonValue parse_string_value() {
        JsonValue value;
        value.kind = JsonValue::Kind::String;
        value.text = parse_string();
        return value;
    }

    std::vector<JsonValue> parse_array() {
        std::vector<JsonValue> items;
        skip_ws();
        if (consume(']')) {
            return items;
        }
        while (true) {
            items.push_back(parse_value());
            skip_ws();
            if (consume(']')) {
                return items;
            }
            expect(',');
        }
    }

    JsonObject parse_object() {
        JsonObject members;
        skip_ws();
        if (consume('}')) {
            return members;
        }
        while (true) {
            skip_ws();
            expect('"');
            std::string key = parse_string();
            skip_ws();
            expect(':');
            JsonValue value = parse_value();
            members.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (consume('}')) {
                return members;
            }
            expect(',');
        }
    }

    std::string_view in_;
    size_t pos_{0};
};

// ---------------------------------------------------------------------------
// Serialization of the journal document.
// ---------------------------------------------------------------------------

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), size);
    return wide;
}

std::string to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::array<char, 8> buf{};
                std::snprintf(buf.data(), buf.size(), "\\u%04X", c);
                out += buf.data();
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

// Base64 (RFC 4648) for PersistentSetting data bytes.
void append_base64(std::string& out, const std::vector<std::byte>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    size_t i = 0;
    while (i + 3 <= data.size()) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                               (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                               static_cast<uint32_t>(bytes[i + 2]);
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += kAlphabet[chunk & 0x3F];
        i += 3;
    }
    if (i + 1 == data.size()) {
        const uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16;
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                               (static_cast<uint32_t>(bytes[i + 1]) << 8);
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += kAlphabet[(chunk >> 6) & 0x3F];
        out += '=';
    }
}

std::optional<std::vector<std::byte>> parse_base64(std::string_view text) {
    const auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const int value = value_of(c);
        if (value < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

void append_display(std::ostringstream& out, const JournalDisplay& display) {
    const auto& s = display.settings;
    out << "{\"device_name\":\"" << json_escape(to_utf8(s.device_name))
        << "\",\"width\":" << s.width << ",\"height\":" << s.height
        << ",\"refresh_rate\":" << s.refresh_rate << ",\"bits_per_pixel\":" << s.bits_per_pixel
        << ",\"position_x\":" << s.position_x << ",\"position_y\":" << s.position_y
        << ",\"interlaced\":" << (s.interlaced ? "true" : "false") << "}";
}

void append_persistent_state(
    std::ostringstream& out,
    const std::optional<PersistentDisplayState>& state) {
    if (!state.has_value()) {
        out << "null";
        return;
    }
    out << "{\"sets\":[";
    for (size_t i = 0; i < state->sets.size(); ++i) {
        const auto& set = state->sets[i];
        if (i != 0) {
            out << ",";
        }
        out << "{\"root\":\"" << json_escape(to_utf8(set.root))
            << "\",\"settings\":[";
        for (size_t j = 0; j < set.settings.size(); ++j) {
            const auto& setting = set.settings[j];
            if (j != 0) {
                out << ",";
            }
            out << "{\"name\":\"" << json_escape(to_utf8(setting.name))
                << "\",\"type\":" << setting.type << ",\"data\":\"";
            std::string encoded;
            append_base64(encoded, setting.data);
            out << encoded;
            out << "\"}";
        }
        out << "]}";
    }
    out << "]}";
}

template <typename T>
std::optional<T> member_number(const JsonObject& members, std::string_view key) {
    for (const auto& [k, v] : members) {
        if (k == key && v.kind == JsonValue::Kind::Number) {
            return static_cast<T>(v.number);
        }
    }
    return std::nullopt;
}

std::optional<std::string> member_string(const JsonObject& members,
                                         std::string_view key) {
    for (const auto& [k, v] : members) {
        if (k == key && v.kind == JsonValue::Kind::String) {
            return v.text;
        }
    }
    return std::nullopt;
}

std::optional<bool> member_bool(const JsonObject& members, std::string_view key) {
    for (const auto& [k, v] : members) {
        if (k == key && v.kind == JsonValue::Kind::Bool) {
            return v.boolean;
        }
    }
    return std::nullopt;
}

const JsonValue* member_object(const JsonObject& members, std::string_view key) {
    for (const auto& [k, v] : members) {
        if (k == key && v.kind == JsonValue::Kind::Object) {
            return &v;
        }
    }
    return nullptr;
}

std::optional<GameId> parse_game(std::string_view text) {
    if (text == "genshin") {
        return GameId::Genshin;
    }
    if (text == "starrail") {
        return GameId::StarRail;
    }
    return std::nullopt;
}

std::optional<SessionStage> parse_stage(std::string_view text) {
    for (const auto stage : {SessionStage::Idle, SessionStage::Preparing,
                             SessionStage::Launching, SessionStage::Resolving,
                             SessionStage::Patching, SessionStage::Running,
                             SessionStage::Restoring, SessionStage::Completed,
                             SessionStage::Failed}) {
        if (to_string(stage) == text) {
            return stage;
        }
    }
    return std::nullopt;
}

JournalDisplay parse_display(const JsonValue& item) {
    JournalDisplay display;
    if (auto device = member_string(item.members, "device_name")) {
        display.settings.device_name = to_wide(*device);
    }
    display.settings.width =
        member_number<uint32_t>(item.members, "width").value_or(0);
    display.settings.height =
        member_number<uint32_t>(item.members, "height").value_or(0);
    display.settings.refresh_rate =
        member_number<uint32_t>(item.members, "refresh_rate").value_or(0);
    display.settings.bits_per_pixel =
        member_number<uint32_t>(item.members, "bits_per_pixel").value_or(0);
    display.settings.position_x =
        member_number<int32_t>(item.members, "position_x").value_or(0);
    display.settings.position_y =
        member_number<int32_t>(item.members, "position_y").value_or(0);
    display.settings.interlaced =
        member_bool(item.members, "interlaced").value_or(false);
    return display;
}

std::optional<PersistentDisplayState> parse_persistent_state(
    const JsonValue& node) {
    const JsonValue* sets = nullptr;
    for (const auto& [key, value] : node.members) {
        if (key == "sets" && value.kind == JsonValue::Kind::Array) {
            sets = &value;
        }
    }
    if (sets == nullptr) {
        return std::nullopt;
    }
    PersistentDisplayState state;
    for (const auto& set_item : sets->items) {
        if (set_item.kind != JsonValue::Kind::Object) {
            continue;
        }
        PersistentSettingSet set;
        if (auto root = member_string(set_item.members, "root")) {
            set.root = to_wide(*root);
        }
        for (const auto& [key, value] : set_item.members) {
            if (key != "settings" || value.kind != JsonValue::Kind::Array) {
                continue;
            }
            for (const auto& setting_item : value.items) {
                if (setting_item.kind != JsonValue::Kind::Object) {
                    continue;
                }
                PersistentSetting setting;
                if (auto name = member_string(setting_item.members, "name")) {
                    setting.name = to_wide(*name);
                }
                setting.type =
                    member_number<uint32_t>(setting_item.members, "type")
                        .value_or(0);
                if (auto data = member_string(setting_item.members, "data")) {
                    auto bytes = parse_base64(*data);
                    if (!bytes) {
                        return std::nullopt;
                    }
                    setting.data = std::move(*bytes);
                }
                set.settings.push_back(std::move(setting));
            }
        }
        state.sets.push_back(std::move(set));
    }
    return state;
}

}  // namespace

std::string_view to_string(SessionStage stage) {
    switch (stage) {
    case SessionStage::Idle: return "idle";
    case SessionStage::Preparing: return "preparing";
    case SessionStage::Launching: return "launching";
    case SessionStage::Resolving: return "resolving";
    case SessionStage::Patching: return "patching";
    case SessionStage::Running: return "running";
    case SessionStage::Restoring: return "restoring";
    case SessionStage::Completed: return "completed";
    case SessionStage::Failed: return "failed";
    }
    return "unknown";
}

std::filesystem::path journal_path() {
    wchar_t override_path[32768];
    const DWORD override_size = GetEnvironmentVariableW(
        L"HOYOFLUX_STATE_DIR", override_path, std::size(override_path));
    if (override_size > 0 && override_size < std::size(override_path)) {
        return std::filesystem::path(
                   std::wstring_view(override_path, override_size)) /
               L"active-session.json";
    }
    wchar_t* raw = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                             &raw) != S_OK) {
        return {};
    }
    std::filesystem::path base(raw);
    CoTaskMemFree(raw);
    return base / L"HoyoFlux" / L"state" / L"active-session.json";
}

Result<void> save_journal(const ActiveSessionJournal& journal) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": " << journal.schema << ",\n";
    out << "  \"session_id\": \"" << json_escape(journal.session_id) << "\",\n";
    out << "  \"game\": \""
        << json_escape(std::string(to_string(journal.game))) << "\",\n";
    out << "  \"pid\": " << journal.pid << ",\n";
    out << "  \"stage\": \"" << json_escape(std::string(to_string(journal.stage)))
        << "\",\n";
    out << "  \"rollback\": {\n";
    out << "    \"required\": "
        << (journal.rollback.required ? "true" : "false") << ",\n";
    out << "    \"physical_displays\": [";
    for (size_t i = 0; i < journal.rollback.displays.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        append_display(out, journal.rollback.displays[i]);
    }
    out << "],\n";
    out << "    \"persistent_state\": ";
    append_persistent_state(out, journal.rollback.persistent_state);
    out << "\n  }\n}\n";
    const std::string body = out.str();

    const auto path = journal_path();
    if (path.empty()) {
        return std::unexpected(
            Error::make(ErrorCode::SessionFailed, "cannot resolve LOCALAPPDATA"));
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected(Error::make(ErrorCode::SessionFailed,
                                           "cannot create journal directory",
                                           ec.value()));
    }

    // Atomic-ish write: temp file in the same directory, then rename over.
    const auto temp = path.parent_path() / (path.filename().wstring() + L".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(
                Error::make(ErrorCode::SessionFailed, "cannot open journal temp file"));
        }
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!file) {
            return std::unexpected(
                Error::make(ErrorCode::SessionFailed, "cannot write journal"));
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(
            Error::make(ErrorCode::SessionFailed, "cannot replace journal", ec.value()));
    }
    return {};
}

Result<std::optional<ActiveSessionJournal>> load_journal() {
    const auto path = journal_path();
    if (path.empty()) {
        return std::unexpected(
            Error::make(ErrorCode::SessionFailed, "cannot resolve LOCALAPPDATA"));
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::optional<ActiveSessionJournal>{};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(
            Error::make(ErrorCode::SessionFailed, "cannot open journal"));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    auto parsed = JsonParser(text).parse();
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->kind != JsonValue::Kind::Object) {
        return std::unexpected(
            Error::make(ErrorCode::JournalCorrupt, "journal is not a JSON object"));
    }
    const auto& members = parsed->members;

    ActiveSessionJournal journal;
    const auto schema = member_number<int>(members, "schema");
    if (!schema || (*schema != 1 && *schema != 2)) {
        return std::unexpected(Error::make(ErrorCode::JournalCorrupt,
                                           "unknown journal schema"));
    }
    journal.schema = *schema;
    if (auto id = member_string(members, "session_id")) {
        journal.session_id = *id;
    }
    if (auto game = member_string(members, "game")) {
        if (auto parsed_game = parse_game(*game)) {
            journal.game = *parsed_game;
        } else {
            return std::unexpected(
                Error::make(ErrorCode::JournalCorrupt, "unknown game in journal"));
        }
    }
    if (auto pid = member_number<uint32_t>(members, "pid")) {
        journal.pid = *pid;
    }
    if (auto stage = member_string(members, "stage")) {
        if (auto parsed_stage = parse_stage(*stage)) {
            journal.stage = *parsed_stage;
        } else {
            return std::unexpected(
                Error::make(ErrorCode::JournalCorrupt, "unknown stage in journal"));
        }
    }

    // Schema 1: flat rollback_required + displays. Schema 2: rollback object.
    if (*schema == 1) {
        journal.rollback.required =
            member_bool(members, "rollback_required").value_or(false);
        for (const auto& [key, value] : members) {
            if (key != "displays" || value.kind != JsonValue::Kind::Array) {
                continue;
            }
            for (const auto& item : value.items) {
                if (item.kind == JsonValue::Kind::Object) {
                    journal.rollback.displays.push_back(parse_display(item));
                }
            }
        }
    } else if (const auto* rollback = member_object(members, "rollback");
               rollback != nullptr) {
        journal.rollback.required =
            member_bool(rollback->members, "required").value_or(false);
        for (const auto& [key, value] : rollback->members) {
            if (key == "physical_displays" && value.kind == JsonValue::Kind::Array) {
                for (const auto& item : value.items) {
                    if (item.kind == JsonValue::Kind::Object) {
                        journal.rollback.displays.push_back(parse_display(item));
                    }
                }
            }
            if (key == "persistent_state" && value.kind == JsonValue::Kind::Object) {
                journal.rollback.persistent_state = parse_persistent_state(value);
            }
        }
    }
    return std::optional<ActiveSessionJournal>{std::move(journal)};
}

Result<void> clear_journal() {
    std::error_code ec;
    const auto path = journal_path();
    std::filesystem::remove(path, ec);
    if (ec) {
        return std::unexpected(
            Error::make(ErrorCode::SessionFailed, "cannot delete journal", ec.value()));
    }
    return {};
}

}  // namespace hoyoflux::session
