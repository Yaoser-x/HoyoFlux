#pragma once

// Durable, local file operations used by portable configuration, migration,
// and diagnostics.  These helpers deliberately keep every temporary file in
// the destination directory so publishing remains an on-volume operation.

#include "domain/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace hoyoflux::win32 {

// Reads a regular file as opaque bytes.  File-not-found, access denial and a
// read failure remain distinguishable through the returned Win32 error code.
Result<std::string> read_file_bytes(const std::filesystem::path& path);

// Writes `bytes` through a unique sibling temporary file, flushes it, replaces
// the destination, and reads the destination back to verify the publish.
// No unsupported ReplaceFile flags are used.
Result<void> write_file_atomic(const std::filesystem::path& path,
                               std::string_view bytes);

// Deletes a source only after obtaining a handle that prevents concurrent
// replacement/writes, rereading it through that handle, and comparing the
// exact expected bytes.  This closes the check-then-delete race in migration.
Result<void> remove_file_if_unchanged(const std::filesystem::path& path,
                                      std::string_view expected_bytes);

// SHA-256 digest for archival names and migration records.  Output is lower
// case hexadecimal and is produced by the Windows BCrypt implementation.
Result<std::string> sha256_hex(std::string_view bytes);

}  // namespace hoyoflux::win32
