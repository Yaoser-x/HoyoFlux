#include "app/elevated_session.hpp"

#include "app/launch_service.hpp"
#include "platform/win32/elevation.hpp"
#include "platform/win32/file.hpp"
#include "platform/win32/notification.hpp"
#include "platform/win32/privilege.hpp"
#include "platform/win32/text.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hoyoflux::app {
namespace {

constexpr DWORD kHandshakeTimeoutMs = 15000;
constexpr uint32_t kMaximumMessageBytes = 16 * 1024;

Error pipe_error(std::string_view message, DWORD code = GetLastError()) {
    return Error::make(ErrorCode::OsError, std::string(message), code);
}

std::wstring ascii_wide(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

Result<std::string> random_token() {
    std::array<unsigned char, 24> bytes{};
    const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(),
                                             static_cast<ULONG>(bytes.size()),
                                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return std::unexpected(Error::make(
        ErrorCode::OsError, "无法生成提权上下文令牌", static_cast<unsigned long>(status)));
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 0x0F]);
    }
    return token;
}

class PipeServer {
public:
    static Result<PipeServer> create(std::wstring name) {
        PSECURITY_DESCRIPTOR security = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;AU)(A;;GA;;;SY)", SDDL_REVISION_1,
                &security, nullptr)) {
            return std::unexpected(pipe_error(
                "无法创建本地提权管道的访问控制"));
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = security;
        attributes.bInheritHandle = FALSE;
        HANDLE raw = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
            kMaximumMessageBytes, kMaximumMessageBytes, 0, &attributes);
        LocalFree(security);
        if (raw == INVALID_HANDLE_VALUE) return std::unexpected(pipe_error(
            "无法创建本地提权管道"));
        return PipeServer{name, raw};
    }

    PipeServer() = default;
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    PipeServer(PipeServer&& other) noexcept
        : name_(std::move(other.name_)), handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    PipeServer& operator=(PipeServer&& other) noexcept {
        if (this != &other) {
            close();
            name_ = std::move(other.name_);
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~PipeServer() { close(); }

    [[nodiscard]] const std::wstring& name() const noexcept { return name_; }
    [[nodiscard]] HANDLE handle() const noexcept { return handle_; }

    Result<void> connect() {
        OVERLAPPED operation{};
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) return std::unexpected(pipe_error("无法创建管道等待事件"));
        operation.hEvent = event;
        const BOOL connected = ConnectNamedPipe(handle_, &operation);
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error == ERROR_PIPE_CONNECTED) {
            CloseHandle(event);
            return {};
        }
        if (!connected && error != ERROR_IO_PENDING) {
            CloseHandle(event);
            return std::unexpected(pipe_error("提升后的进程无法连接本地管道", error));
        }
        const DWORD waited = WaitForSingleObject(event, kHandshakeTimeoutMs);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(handle_, &operation);
            CloseHandle(event);
            return std::unexpected(Error::make(
                ErrorCode::SessionFailed, "提升进程握手超时"));
        }
        DWORD ignored = 0;
        const BOOL completed = GetOverlappedResult(handle_, &operation, &ignored, FALSE);
        const DWORD completed_error = completed ? ERROR_SUCCESS : GetLastError();
        CloseHandle(event);
        if (!completed) return std::unexpected(pipe_error(
            "提升进程无法完成本地握手", completed_error));
        return {};
    }

private:
    PipeServer(std::wstring name, HANDLE handle) : name_(std::move(name)), handle_(handle) {}
    void close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(handle_);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }
    std::wstring name_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

Result<void> transfer_overlapped(HANDLE pipe, void* buffer, DWORD length, bool write) {
    OVERLAPPED operation{};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return std::unexpected(pipe_error("无法创建管道 IO 事件"));
    operation.hEvent = event;
    DWORD processed = 0;
    const BOOL immediate = write
        ? WriteFile(pipe, buffer, length, &processed, &operation)
        : ReadFile(pipe, buffer, length, &processed, &operation);
    const DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
    if (!immediate && error != ERROR_IO_PENDING) {
        CloseHandle(event);
        return std::unexpected(pipe_error("本地提权管道读写失败", error));
    }
    if (!immediate) {
        const DWORD waited = WaitForSingleObject(event, kHandshakeTimeoutMs);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            CloseHandle(event);
            return std::unexpected(Error::make(
                ErrorCode::SessionFailed, "本地提权管道通讯超时"));
        }
        if (!GetOverlappedResult(pipe, &operation, &processed, FALSE)) {
            const auto complete_error = pipe_error("本地提权管道通讯未完成");
            CloseHandle(event);
            return std::unexpected(complete_error);
        }
    }
    CloseHandle(event);
    if (processed != length) return std::unexpected(Error::make(
        ErrorCode::SessionFailed, "本地提权管道传输了不完整的数据"));
    return {};
}

Result<void> transfer_sync(HANDLE pipe, void* buffer, DWORD length, bool write) {
    DWORD processed = 0;
    const BOOL done = write
        ? WriteFile(pipe, buffer, length, &processed, nullptr)
        : ReadFile(pipe, buffer, length, &processed, nullptr);
    if (!done) return std::unexpected(pipe_error("本地提权管道读写失败"));
    if (processed != length) return std::unexpected(Error::make(
        ErrorCode::SessionFailed, "本地提权管道传输了不完整的数据"));
    return {};
}

Result<void> send_message(HANDLE pipe, std::string_view text, bool overlapped) {
    if (text.size() > kMaximumMessageBytes) return std::unexpected(Error::make(
        ErrorCode::InvalidArgument, "本地提权管道消息过长"));
    uint32_t length = static_cast<uint32_t>(text.size());
    auto transfer = [&](void* data, DWORD size) {
        return overlapped ? transfer_overlapped(pipe, data, size, true)
                          : transfer_sync(pipe, data, size, true);
    };
    if (auto prefix = transfer(&length, sizeof(length)); !prefix) return prefix;
    if (length == 0) return {};
    return transfer(const_cast<char*>(text.data()), length);
}

Result<std::string> receive_message(HANDLE pipe, bool overlapped) {
    uint32_t length = 0;
    auto transfer = [&](void* data, DWORD size) {
        return overlapped ? transfer_overlapped(pipe, data, size, false)
                          : transfer_sync(pipe, data, size, false);
    };
    if (auto prefix = transfer(&length, sizeof(length)); !prefix)
        return std::unexpected(prefix.error());
    if (length > kMaximumMessageBytes) return std::unexpected(Error::make(
        ErrorCode::SessionFailed, "本地提权管道消息长度无效"));
    std::string result(length, '\0');
    if (length == 0) return result;
    if (auto body = transfer(result.data(), length); !body)
        return std::unexpected(body.error());
    return result;
}

Result<void> send_child_result(HANDLE pipe, int code, std::string_view detail) {
    const std::string message = "RESULT\n" + std::to_string(code) + "\n" +
                                std::string(detail);
    return send_message(pipe, message, false);
}

Result<void> ensure_valid_child_result(std::string_view message,
                                       std::optional<Error>* child_error) {
    constexpr std::string_view prefix = "RESULT\n";
    if (!message.starts_with(prefix)) return std::unexpected(Error::make(
        ErrorCode::SessionFailed, "提升进程返回了无效结果"));
    const auto separator = message.find('\n', prefix.size());
    if (separator == std::string_view::npos) return std::unexpected(Error::make(
        ErrorCode::SessionFailed, "提升进程结果不完整"));
    const auto code_text = message.substr(prefix.size(), separator - prefix.size());
    if (code_text == "0") return {};
    if (code_text != "1") return std::unexpected(Error::make(
        ErrorCode::SessionFailed, "提升进程结果码无效"));
    *child_error = Error::make(ErrorCode::SessionFailed,
        std::string(message.substr(separator + 1)));
    return {};
}

Result<DWORD> parent_pid_from_pipe_name(std::wstring_view pipe_name,
                                        std::wstring_view token) {
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\HoyoFlux-";
    if (!pipe_name.starts_with(prefix) || !pipe_name.ends_with(token)) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument, "内部提权管道名称无效"));
    }
    const auto separator = pipe_name.rfind(L'-', pipe_name.size() - token.size() - 1);
    if (separator == std::wstring_view::npos || separator <= prefix.size()) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument, "内部提权管道缺少父进程标识"));
    }
    const auto text = pipe_name.substr(prefix.size(), separator - prefix.size());
    DWORD value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return std::unexpected(Error::make(
            ErrorCode::InvalidArgument, "内部提权管道父进程标识无效"));
        const DWORD digit = static_cast<DWORD>(character - L'0');
        if (value > (std::numeric_limits<DWORD>::max() - digit) / 10) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidArgument, "内部提权管道父进程标识超出范围"));
        }
        value = value * 10 + digit;
    }
    if (value == 0) return std::unexpected(Error::make(
        ErrorCode::InvalidArgument, "内部提权管道父进程标识无效"));
    return value;
}

LaunchOptions make_launch_options(const profile::Config& config,
                                  const AppPaths& paths) {
    LaunchOptions options;
    options.game = config.launcher.game;
    options.profile = config.launcher.profile;
    options.journal_path = paths.journal;
    switch (config.launcher.region) {
    case profile::LauncherRegion::Auto: options.region = game::Region::Auto; break;
    case profile::LauncherRegion::Cn: options.region = game::Region::Cn; break;
    case profile::LauncherRegion::Global: options.region = game::Region::Global; break;
    }
    return options;
}

std::wstring launch_notification_body(const ResolvedLaunch& resolved) {
    const auto profile_name = win32::utf16(resolved.profile.id).value_or(L"配置档");
    std::wstring body = resolved.auto_decision ? L"已自动选择 " : L"正在使用 ";
    body += profile_name;
    body += L"\n";
    body += resolved.profile.game == GameId::Genshin ? L"原神" : L"崩坏：星穹铁道";
    body += L" · ";
    body += std::to_wstring(resolved.profile.runtime.fps);
    body += L" FPS";
    if (resolved.profile.render.resolution) {
        body += L" · ";
        body += std::to_wstring(resolved.profile.render.resolution->width);
        body += L"×";
        body += std::to_wstring(resolved.profile.render.resolution->height);
    }
    if (resolved.profile.ui.mobile_ui) {
        body += L" · Mobile UI";
    }
    return body;
}

Result<void> run_session_without_ui(const AppPaths& paths,
                                    const profile::Config& config) {
    auto options = make_launch_options(config, paths);
    auto resolved = resolve_launch(config, options);
    if (!resolved) return std::unexpected(resolved.error());
    const std::wstring body = launch_notification_body(*resolved);
    auto outcome = run_resolved_launch(options, std::move(*resolved), [body] {
        win32::notify_best_effort(win32::notify, L"HoyoFlux", body,
                                  win32::NotificationKind::Info);
    });
    if (!outcome) return std::unexpected(outcome.error());
    win32::notify_best_effort(win32::notify, L"HoyoFlux",
                              L"游戏已结束\n会话设置已恢复",
                              win32::NotificationKind::Success);
    win32::drain_notifications();
    return {};
}

}  // namespace

Result<int> launch_elevated_session(const AppPaths& paths,
                                    const profile::Config& confirmed_config) {
    if (confirmed_config.schema != 2 ||
        confirmed_config.launcher.action != profile::LauncherAction::Launch) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument,
            "只有已确认的 schema 2 启动配置可以请求管理员权限"));
    }
    auto config_bytes = win32::read_file_bytes(paths.config);
    if (!config_bytes) return std::unexpected(config_bytes.error());
    auto config_hash = win32::sha256_hex(*config_bytes);
    if (!config_hash) return std::unexpected(config_hash.error());
    auto expected_sid = win32::current_user_sid();
    if (!expected_sid) return std::unexpected(expected_sid.error());
    auto token = random_token();
    if (!token) return std::unexpected(token.error());
    const std::wstring pipe_name = L"\\\\.\\pipe\\HoyoFlux-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + ascii_wide(*token);
    auto server = PipeServer::create(pipe_name);
    if (!server) return std::unexpected(server.error());

    std::optional<Error> child_error;
    bool child_session_ready = false;
    win32::ElevationResult elevation = win32::ElevationResult::Completed;
    auto child = win32::relaunch_elevated_and_wait(
        {std::wstring(win32::kInternalElevatedArgument) + L"=" + ascii_wide(*token),
         std::wstring(win32::kInternalPipeArgument) + L"=" + pipe_name},
        &elevation,
        [&](DWORD child_pid) -> Result<void> {
            if (auto connected = server->connect(); !connected) return connected;
            DWORD pipe_pid = 0;
            if (!GetNamedPipeClientProcessId(server->handle(), &pipe_pid) ||
                pipe_pid != child_pid) {
                return std::unexpected(Error::make(
                    ErrorCode::SessionFailed, "本地提权管道对端身份不匹配"));
            }
            auto child_sid = win32::process_user_sid(child_pid);
            if (!child_sid || *child_sid != *expected_sid) {
                return std::unexpected(Error::make(
                    ErrorCode::NotElevated,
                    "UAC 使用了其他 Windows 账户。HoyoFlux 只支持同一账户提权，请取消后用当前账户确认。"));
            }
            auto hello = receive_message(server->handle(), true);
            if (!hello) return std::unexpected(hello.error());
            if (*hello != "HELLO\n" + win32::utf8(*expected_sid)) {
                return std::unexpected(Error::make(
                    ErrorCode::SessionFailed, "提升进程身份握手校验失败"));
            }
            if (auto accepted = send_message(server->handle(),
                                             "CONFIG\n" + *config_hash, true);
                !accepted) return accepted;
            auto acknowledgement = receive_message(server->handle(), true);
            if (!acknowledgement) return std::unexpected(acknowledgement.error());
            if (*acknowledgement == "READY\n") {
                child_session_ready = true;
                return {};
            }
            // Configuration verification can fail before a session is ready.
            // That failure is already a structured result, rather than a
            // handshake timeout; wait for the child to exit normally below.
            return ensure_valid_child_result(*acknowledgement, &child_error);
        });
    if (!child) {
        if (elevation == win32::ElevationResult::Cancelled) {
            return std::unexpected(Error::make(ErrorCode::ElevationCancelled,
                                                "elevation cancelled"));
        }
        return std::unexpected(child.error());
    }
    if (child_error) return std::unexpected(std::move(*child_error));
    if (!child_session_ready) return std::unexpected(Error::make(
        ErrorCode::SessionFailed,
        "提升进程未确认会话已接管"));

    // The handshake deadline covers only the connection, SID and immutable
    // configuration checks. The session itself intentionally lasts until the
    // game exits, so its result is read only after relaunch_elevated_and_wait
    // has observed the child process exit.
    auto result = receive_message(server->handle(), true);
    if (!result) return std::unexpected(result.error());
    if (auto valid = ensure_valid_child_result(*result, &child_error); !valid) {
        return std::unexpected(valid.error());
    }
    if (child_error) return std::unexpected(std::move(*child_error));
    return *child;
}

Result<int> run_elevated_session_child(const AppPaths& paths,
                                       const ElevatedSessionArguments& arguments) {
    if (!win32::is_elevated()) return std::unexpected(Error::make(
        ErrorCode::NotElevated, "提升后的进程仍没有管理员权限"));
    if (arguments.token.size() != 48 ||
        !arguments.pipe_name.starts_with(L"\\\\.\\pipe\\HoyoFlux-")) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument, "内部提权上下文无效"));
    }
    auto expected_parent = parent_pid_from_pipe_name(arguments.pipe_name, arguments.token);
    if (!expected_parent) return std::unexpected(expected_parent.error());
    if (!WaitNamedPipeW(arguments.pipe_name.c_str(), kHandshakeTimeoutMs)) {
        return std::unexpected(pipe_error("无法连接普通启动端的本地管道"));
    }
    HANDLE pipe = CreateFileW(arguments.pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return std::unexpected(pipe_error(
        "无法打开普通启动端的本地管道"));
    auto close_pipe = [&] { CloseHandle(pipe); };

    DWORD server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid != *expected_parent) {
        close_pipe();
        return std::unexpected(Error::make(
            ErrorCode::SessionFailed, "内部提权管道的父进程身份不匹配"));
    }

    auto sid = win32::current_user_sid();
    if (!sid) {
        close_pipe();
        return std::unexpected(sid.error());
    }
    auto server_sid = win32::process_user_sid(server_pid);
    if (!server_sid || *server_sid != *sid) {
        close_pipe();
        return std::unexpected(Error::make(
            ErrorCode::NotElevated,
            "内部提权管道不属于同一 Windows 账户"));
    }
    if (auto hello = send_message(pipe, "HELLO\n" + win32::utf8(*sid), false); !hello) {
        close_pipe();
        return std::unexpected(hello.error());
    }
    auto approval = receive_message(pipe, false);
    if (!approval) {
        close_pipe();
        return std::unexpected(approval.error());
    }
    constexpr std::string_view kConfigPrefix = "CONFIG\n";
    if (!approval->starts_with(kConfigPrefix) || approval->size() != kConfigPrefix.size() + 64) {
        close_pipe();
        return std::unexpected(Error::make(
            ErrorCode::SessionFailed, "普通启动端未确认提权上下文"));
    }
    auto bytes = win32::read_file_bytes(paths.config);
    if (!bytes) {
        send_child_result(pipe, 1, bytes.error().message);
        close_pipe();
        return std::unexpected(bytes.error());
    }
    auto hash = win32::sha256_hex(*bytes);
    if (!hash || *hash != approval->substr(kConfigPrefix.size())) {
        const std::string detail = "配置在提权期间发生变化，请重新双击 HoyoFlux。";
        send_child_result(pipe, 1, detail);
        close_pipe();
        return std::unexpected(Error::make(ErrorCode::ConfigParseFailed, detail));
    }
    auto config = profile::read_config(paths.config);
    if (!config || config->schema != 2 ||
        config->launcher.action != profile::LauncherAction::Launch) {
        const std::string detail = !config ? config.error().message :
            "提升后的配置已不再是可启动的 schema 2 配置";
        send_child_result(pipe, 1, detail);
        close_pipe();
        return std::unexpected(Error::make(ErrorCode::ConfigParseFailed, detail));
    }
    if (auto ready = send_message(pipe, "READY\n", false); !ready) {
        close_pipe();
        return std::unexpected(ready.error());
    }
    auto run = run_session_without_ui(paths, *config);
    if (!run) {
        send_child_result(pipe, 1, run.error().message);
        close_pipe();
        return std::unexpected(run.error());
    }
    auto sent = send_child_result(pipe, 0, "");
    close_pipe();
    if (!sent) return std::unexpected(sent.error());
    return 0;
}

}  // namespace hoyoflux::app
