#pragma once

// HoyoFlux domain error model.
//
// Every fallible boundary in the domain layer reports through
// `hoyoflux::Result<T>` (std::expected) so that control flow never relies on
// bare bools / sentinels, and RAII-owned resources are always released when
// an error unwinds.

#include <expected>
#include <string>
#include <string_view>

namespace hoyoflux {

// Error codes, grouped by the subsystem that raises them.
enum class ErrorCode {
    None,

    // generic
    InvalidArgument,
    NotSupported,

    // platform / win32
    OsError,               // a Win32 call failed; see Error::os_code
    NotElevated,
    ProcessSpawnFailed,
    ProcessNotFound,
    RegistryReadFailed,

    // pe / snapshot
    InvalidPe,
    SectionNotFound,
    ModuleNotFound,
    ReadProcessMemoryFailed,

    // scan
    SignatureNotFound,

    // patch
    PatchFailed,
    RemoteAllocFailed,
    RemoteWriteFailed,

    // session / config
    SessionFailed,
    ProfileNotFound,
    AutoProfileAmbiguous,
    ProfileInvalid,
    ConfigParseFailed,
    JournalCorrupt,
};

struct Error {
    ErrorCode code{ErrorCode::None};
    std::string message;
    unsigned long os_code{0};  // GetLastError() when code == OsError, else 0

    [[nodiscard]] static Error make(ErrorCode code, std::string message,
                                    unsigned long os_code = 0) {
        return Error{code, std::move(message), os_code};
    }
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) {
    using enum ErrorCode;
    switch (code) {
    case None: return "none";
    case InvalidArgument: return "invalid-argument";
    case NotSupported: return "not-supported";
    case OsError: return "os-error";
    case NotElevated: return "not-elevated";
    case ProcessSpawnFailed: return "process-spawn-failed";
    case ProcessNotFound: return "process-not-found";
    case RegistryReadFailed: return "registry-read-failed";
    case InvalidPe: return "invalid-pe";
    case SectionNotFound: return "section-not-found";
    case ModuleNotFound: return "module-not-found";
    case ReadProcessMemoryFailed: return "read-process-memory-failed";
    case SignatureNotFound: return "signature-not-found";
    case PatchFailed: return "patch-failed";
    case RemoteAllocFailed: return "remote-alloc-failed";
    case RemoteWriteFailed: return "remote-write-failed";
    case SessionFailed: return "session-failed";
    case ProfileNotFound: return "profile-not-found";
    case AutoProfileAmbiguous: return "auto-profile-ambiguous";
    case ProfileInvalid: return "profile-invalid";
    case ConfigParseFailed: return "config-parse-failed";
    case JournalCorrupt: return "journal-corrupt";
    }
    return "unknown";
}

}  // namespace hoyoflux
