#include "session/journal.hpp"

#include "domain/game.hpp"
#include "platform/win32/text.hpp"

#include <array>
#include <atomic>
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

std::atomic<size_t> g_save_call_count{0};
std::atomic<size_t> g_fail_on_save{0};

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
    std::string number_text;
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
            JsonValue value;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = true;
            return value;
        }
        if (consume('f')) {
            expect_literal("alse");
            JsonValue value;
            value.kind = JsonValue::Kind::Bool;
            value.boolean = false;
            return value;
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
        if (consume('-')) {
            // The sign is part of the strict JSON number grammar below.
        }
        if (pos_ >= in_.size()) {
            fail("malformed number");
        }
        if (in_[pos_] == '0') {
            ++pos_;
            if (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') {
                fail("leading zero in number");
            }
        } else if (in_[pos_] >= '1' && in_[pos_] <= '9') {
            do {
                ++pos_;
            } while (pos_ < in_.size() && in_[pos_] >= '0' &&
                     in_[pos_] <= '9');
        } else {
            fail("malformed number");
        }
        if (consume('.')) {
            if (pos_ >= in_.size() || in_[pos_] < '0' || in_[pos_] > '9') {
                fail("malformed number fraction");
            }
            while (pos_ < in_.size() && in_[pos_] >= '0' &&
                   in_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < in_.size() && (in_[pos_] == 'e' || in_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < in_.size() &&
                (in_[pos_] == '+' || in_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= in_.size() || in_[pos_] < '0' || in_[pos_] > '9') {
                fail("malformed number exponent");
            }
            while (pos_ < in_.size() && in_[pos_] >= '0' &&
                   in_[pos_] <= '9') {
                ++pos_;
            }
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number_text = std::string(in_.substr(start, pos_ - start));
        return value;
    }

    std::string parse_string() {
        std::string out;
        while (pos_ < in_.size() && in_[pos_] != '"') {
            if (static_cast<unsigned char>(in_[pos_]) < 0x20) {
                fail("unescaped control character in string");
            }
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
            for (const auto& [existing, value] : members) {
                (void)value;
                if (existing == key) {
                    fail("duplicate object key");
                }
            }
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
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    for (size_t offset = 0; offset < text.size(); offset += 4) {
        const char a = text[offset];
        const char b = text[offset + 1];
        const char c = text[offset + 2];
        const char d = text[offset + 3];
        const int va = value_of(a);
        const int vb = value_of(b);
        if (va < 0 || vb < 0) {
            return std::nullopt;
        }
        const bool pad_c = c == '=';
        const bool pad_d = d == '=';
        const int vc = pad_c ? 0 : value_of(c);
        const int vd = pad_d ? 0 : value_of(d);
        if (vc < 0 || vd < 0 || (pad_c && !pad_d) ||
            (offset + 4 != text.size() && (pad_c || pad_d))) {
            return std::nullopt;
        }
        if ((pad_c && (vb & 0x0F) != 0) ||
            (pad_d && !pad_c && (vc & 0x03) != 0)) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::byte>((va << 2) | (vb >> 4)));
        if (!pad_c) {
            out.push_back(static_cast<std::byte>((vb << 4) | (vc >> 2)));
        }
        if (!pad_d) {
            out.push_back(static_cast<std::byte>((vc << 6) | vd));
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

const JsonValue* find_member(const JsonObject& members, std::string_view key) {
    for (const auto& [k, v] : members) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

template <typename T>
Result<std::optional<T>> optional_integer(const JsonObject& members,
                                          std::string_view key) {
    const auto* value = find_member(members, key);
    if (value == nullptr) {
        return std::optional<T>{};
    }
    if (value->kind != JsonValue::Kind::Number) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal field '" + std::string(key) + "' must be an integer"));
    }
    T parsed{};
    const auto [ptr, ec] = std::from_chars(
        value->number_text.data(),
        value->number_text.data() + value->number_text.size(), parsed);
    if (ec != std::errc{} || ptr != value->number_text.data() +
                                      value->number_text.size()) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal field '" + std::string(key) +
                "' must be an in-range integer"));
    }
    return std::optional<T>{parsed};
}

Result<std::optional<std::string>> optional_string(const JsonObject& members,
                                                   std::string_view key) {
    const auto* value = find_member(members, key);
    if (value == nullptr) {
        return std::optional<std::string>{};
    }
    if (value->kind != JsonValue::Kind::String) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal field '" + std::string(key) + "' must be a string"));
    }
    return std::optional<std::string>{value->text};
}

Result<std::optional<bool>> optional_bool(const JsonObject& members,
                                          std::string_view key) {
    const auto* value = find_member(members, key);
    if (value == nullptr) {
        return std::optional<bool>{};
    }
    if (value->kind != JsonValue::Kind::Bool) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal field '" + std::string(key) + "' must be a boolean"));
    }
    return std::optional<bool>{value->boolean};
}

Result<const JsonValue*> required_member(const JsonObject& members,
                                         std::string_view key) {
    const auto* value = find_member(members, key);
    if (value == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal is missing required field '" + std::string(key) + "'"));
    }
    return value;
}

template <typename T>
Result<T> required_integer(const JsonObject& members, std::string_view key) {
    auto value = optional_integer<T>(members, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal is missing required integer field '" + std::string(key) + "'"));
    }
    return **value;
}

Result<std::string> required_string(const JsonObject& members,
                                    std::string_view key) {
    auto value = optional_string(members, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal is missing required string field '" + std::string(key) + "'"));
    }
    return **value;
}

Result<bool> required_bool(const JsonObject& members, std::string_view key) {
    auto value = optional_bool(members, key);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt,
            "journal is missing required boolean field '" + std::string(key) + "'"));
    }
    return **value;
}

// Legacy schema-1 parsing uses these non-throwing accessors. Schema 2 uses
// the typed Result helpers above so missing and wrong-typed fields cannot be
// turned into defaults.
std::optional<std::string> member_string(const JsonObject& members,
                                         std::string_view key) {
    const auto* value = find_member(members, key);
    if (value != nullptr && value->kind == JsonValue::Kind::String) {
        return value->text;
    }
    return std::nullopt;
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

Result<JournalDisplay> parse_display(const JsonValue& item) {
    if (item.kind != JsonValue::Kind::Object) {
        return std::unexpected(
            Error::make(ErrorCode::JournalCorrupt, "display entry must be an object"));
    }
    JournalDisplay display;
    auto device = optional_string(item.members, "device_name");
    auto width = optional_integer<uint32_t>(item.members, "width");
    auto height = optional_integer<uint32_t>(item.members, "height");
    auto refresh = optional_integer<uint32_t>(item.members, "refresh_rate");
    auto bits = optional_integer<uint32_t>(item.members, "bits_per_pixel");
    auto x = optional_integer<int32_t>(item.members, "position_x");
    auto y = optional_integer<int32_t>(item.members, "position_y");
    auto interlaced = optional_bool(item.members, "interlaced");
    if (!device) return std::unexpected(device.error());
    if (!width) return std::unexpected(width.error());
    if (!height) return std::unexpected(height.error());
    if (!refresh) return std::unexpected(refresh.error());
    if (!bits) return std::unexpected(bits.error());
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!interlaced) return std::unexpected(interlaced.error());
    if (!*device || !*width || !*height || !*refresh || !*bits || !*x ||
        !*y || !*interlaced) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt, "display entry is missing a required field"));
    }
    auto wide_device = win32::utf16(**device);
    if (!wide_device) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt, "display device_name is not valid UTF-8"));
    }
    display.settings.device_name = std::move(*wide_device);
    display.settings.width = **width;
    display.settings.height = **height;
    display.settings.refresh_rate = **refresh;
    display.settings.bits_per_pixel = **bits;
    display.settings.position_x = **x;
    display.settings.position_y = **y;
    display.settings.interlaced = **interlaced;
    return display;
}

Result<PersistentDisplayState> parse_persistent_state(const JsonValue& node) {
    if (node.kind != JsonValue::Kind::Object) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt, "persistent_state must be an object"));
    }
    auto sets_member = required_member(node.members, "sets");
    if (!sets_member) {
        return std::unexpected(sets_member.error());
    }
    if ((*sets_member)->kind != JsonValue::Kind::Array) {
        return std::unexpected(Error::make(
            ErrorCode::JournalCorrupt, "persistent_state.sets must be an array"));
    }
    PersistentDisplayState state;
    for (const auto& set_item : (*sets_member)->items) {
        if (set_item.kind != JsonValue::Kind::Object) {
            return std::unexpected(Error::make(
                ErrorCode::JournalCorrupt, "persistent_state set must be an object"));
        }
        PersistentSettingSet set;
        auto root = required_member(set_item.members, "root");
        auto settings = required_member(set_item.members, "settings");
        if (!root) return std::unexpected(root.error());
        if (!settings) return std::unexpected(settings.error());
        if ((*root)->kind != JsonValue::Kind::String ||
            (*root)->text.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::JournalCorrupt,
                "persistent_state set root must be a non-empty string"));
        }
        if ((*settings)->kind != JsonValue::Kind::Array) {
            return std::unexpected(Error::make(
                ErrorCode::JournalCorrupt, "persistent_state.settings must be an array"));
        }
        auto wide_root = win32::utf16((*root)->text);
        if (!wide_root) {
            return std::unexpected(Error::make(
                ErrorCode::JournalCorrupt, "persistent_state root is not valid UTF-8"));
        }
        set.root = std::move(*wide_root);
        for (const auto& setting_item : (*settings)->items) {
            if (setting_item.kind != JsonValue::Kind::Object) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "persistent setting must be an object"));
            }
            auto name = required_member(setting_item.members, "name");
            auto type = required_member(setting_item.members, "type");
            auto data = required_member(setting_item.members, "data");
            if (!name) return std::unexpected(name.error());
            if (!type) return std::unexpected(type.error());
            if (!data) return std::unexpected(data.error());
            if ((*name)->kind != JsonValue::Kind::String ||
                (*name)->text.empty()) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt,
                    "persistent setting name must be a non-empty string"));
            }
            if ((*type)->kind != JsonValue::Kind::Number) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "persistent setting type must be an integer"));
            }
            auto parsed_type = optional_integer<uint32_t>(
                setting_item.members, "type");
            if (!parsed_type || !*parsed_type) {
                return std::unexpected(parsed_type
                                           ? Error::make(ErrorCode::JournalCorrupt,
                                                         "persistent setting type is missing")
                                           : parsed_type.error());
            }
            if ((*data)->kind != JsonValue::Kind::String) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "persistent setting data must be base64 text"));
            }
            auto bytes = parse_base64((*data)->text);
            if (!bytes) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "persistent setting data is invalid base64"));
            }
                PersistentSetting setting;
                auto wide_name = win32::utf16((*name)->text);
                if (!wide_name) {
                    return std::unexpected(Error::make(
                        ErrorCode::JournalCorrupt,
                        "persistent setting name is not valid UTF-8"));
                }
                setting.name = std::move(*wide_name);
                setting.type = **parsed_type;
                setting.data = std::move(*bytes);
                set.settings.push_back(std::move(setting));
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

    const size_t save_call = ++g_save_call_count;
    if (const size_t fail_on = g_fail_on_save.load();
        fail_on != 0 && save_call == fail_on) {
        return std::unexpected(Error::make(
            ErrorCode::SessionFailed,
            "injected journal save failure for durability test"));
    }

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
        file.flush();
        if (!file) {
            return std::unexpected(
                Error::make(ErrorCode::SessionFailed, "cannot write journal"));
        }
        file.close();
        if (!file) {
            return std::unexpected(
                Error::make(ErrorCode::SessionFailed, "cannot close journal temp file"));
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

void set_journal_save_failure_for_testing(std::optional<size_t> fail_on_save) {
    g_save_call_count.store(0);
    g_fail_on_save.store(fail_on_save.value_or(0));
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

    auto schema = required_integer<int>(members, "schema");
    if (!schema) {
        return std::unexpected(schema.error());
    }
    if (*schema != 1 && *schema != 2) {
        return std::unexpected(Error::make(ErrorCode::JournalCorrupt,
                                           "unknown journal schema"));
    }
    ActiveSessionJournal journal;
    journal.schema = *schema;

    // Schema 1 remains readable for existing journals. Schema 2 is strict:
    // every recovery-critical field is required and correctly typed.
    if (*schema == 2) {
        auto id = required_string(members, "session_id");
        auto game = required_string(members, "game");
        auto pid = required_integer<uint32_t>(members, "pid");
        auto stage = required_string(members, "stage");
        auto rollback_member = required_member(members, "rollback");
        if (!id) return std::unexpected(id.error());
        if (!game) return std::unexpected(game.error());
        if (!pid) return std::unexpected(pid.error());
        if (!stage) return std::unexpected(stage.error());
        if (!rollback_member) return std::unexpected(rollback_member.error());
        auto parsed_game = parse_game(*game);
        auto parsed_stage = parse_stage(*stage);
        if (!parsed_game) {
            return std::unexpected(
                Error::make(ErrorCode::JournalCorrupt, "unknown game in journal"));
        }
        if (!parsed_stage) {
            return std::unexpected(
                Error::make(ErrorCode::JournalCorrupt, "unknown stage in journal"));
        }
        if ((*rollback_member)->kind != JsonValue::Kind::Object) {
            return std::unexpected(Error::make(
                ErrorCode::JournalCorrupt, "journal rollback must be an object"));
        }
        const auto& rollback = (*rollback_member)->members;
        auto required = required_bool(rollback, "required");
        if (!required) return std::unexpected(required.error());
        journal.session_id = *id;
        journal.game = *parsed_game;
        journal.pid = *pid;
        journal.stage = *parsed_stage;
        journal.rollback.required = *required;

        if (const auto* displays = find_member(rollback, "physical_displays")) {
            if (displays->kind != JsonValue::Kind::Array) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt,
                    "rollback.physical_displays must be an array"));
            }
            for (const auto& item : displays->items) {
                auto display = parse_display(item);
                if (!display) return std::unexpected(display.error());
                journal.rollback.displays.push_back(std::move(*display));
            }
        }
        if (const auto* persistent = find_member(rollback, "persistent_state")) {
            if (persistent->kind == JsonValue::Kind::Null) {
                journal.rollback.persistent_state.reset();
            } else {
                auto state = parse_persistent_state(*persistent);
                if (!state) return std::unexpected(state.error());
                journal.rollback.persistent_state = std::move(*state);
            }
        }
    } else {
        if (auto id = member_string(members, "session_id")) {
            journal.session_id = *id;
        }
        if (auto game = member_string(members, "game")) {
            if (auto parsed_game = parse_game(*game)) {
                journal.game = *parsed_game;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "unknown game in journal"));
            }
        }
        auto pid = optional_integer<uint32_t>(members, "pid");
        if (!pid) return std::unexpected(pid.error());
        if (*pid) journal.pid = **pid;
        if (auto stage = member_string(members, "stage")) {
            if (auto parsed_stage = parse_stage(*stage)) {
                journal.stage = *parsed_stage;
            } else {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "unknown stage in journal"));
            }
        }
        auto required = optional_bool(members, "rollback_required");
        if (!required) return std::unexpected(required.error());
        if (*required) journal.rollback.required = **required;
        if (const auto* displays = find_member(members, "displays")) {
            if (displays->kind != JsonValue::Kind::Array) {
                return std::unexpected(Error::make(
                    ErrorCode::JournalCorrupt, "legacy displays must be an array"));
            }
            for (const auto& item : displays->items) {
                auto display = parse_display(item);
                if (!display) return std::unexpected(display.error());
                journal.rollback.displays.push_back(std::move(*display));
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
