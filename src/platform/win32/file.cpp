#include "platform/win32/file.hpp"

#include "platform/win32/unique_handle.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace hoyoflux::win32 {
namespace {

constexpr std::wstring_view kTemporarySuffix = L".hoyoflux-writing-";

Error os_error(std::string_view message, DWORD code = GetLastError()) {
    return Error::make(ErrorCode::OsError, std::string(message), code);
}

Result<std::string> read_handle_bytes(HANDLE handle,
                                      const std::filesystem::path& path) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        return std::unexpected(os_error("无法获取文件大小: " + path.string()));
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "文件过大，无法安全读取: " + path.string()));
    }
    if (!SetFilePointerEx(handle, {}, nullptr, FILE_BEGIN)) {
        return std::unexpected(os_error("无法定位文件开头: " + path.string()));
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr)) {
            return std::unexpected(os_error("读取文件失败: " + path.string()));
        }
        if (read == 0) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "读取文件时意外结束: " + path.string()));
        }
        offset += read;
    }
    return bytes;
}

Result<void> write_all(HANDLE handle, std::string_view bytes,
                       const std::filesystem::path& path) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)) {
            return std::unexpected(os_error("写入文件失败: " + path.string()));
        }
        if (written == 0) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "写入文件时没有写入数据: " + path.string()));
        }
        offset += written;
    }
    return {};
}

Result<std::filesystem::path> create_unique_sibling(
    const std::filesystem::path& destination, UniqueHandle* output_handle) {
    const auto parent = destination.parent_path().empty()
        ? std::filesystem::path{L"."} : destination.parent_path();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto candidate = parent /
            (destination.filename().wstring() + std::wstring(kTemporarySuffix) +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        HANDLE raw = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
                                 nullptr);
        if (raw != INVALID_HANDLE_VALUE) {
            output_handle->reset(raw);
            return candidate;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return std::unexpected(os_error(
                "无法创建同目录临时文件: " + candidate.string()));
        }
    }
    return std::unexpected(Error::make(
        ErrorCode::OsError, "无法分配唯一临时文件: " + destination.string()));
}

void remove_best_effort(const std::filesystem::path& path) {
    DeleteFileW(path.c_str());
}

}  // namespace

Result<std::string> read_file_bytes(const std::filesystem::path& path) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return std::unexpected(os_error("无法读取文件: " + path.string()));
    }
    return read_handle_bytes(file.get(), path);
}

Result<void> write_file_atomic(const std::filesystem::path& path,
                               std::string_view bytes) {
    UniqueHandle temporary_handle;
    auto temporary = create_unique_sibling(path, &temporary_handle);
    if (!temporary) return std::unexpected(temporary.error());

    auto written = write_all(temporary_handle.get(), bytes, *temporary);
    if (!written) {
        temporary_handle.reset();
        remove_best_effort(*temporary);
        return written;
    }
    if (!FlushFileBuffers(temporary_handle.get())) {
        const auto error = os_error("无法刷盘临时文件: " + temporary->string());
        temporary_handle.reset();
        remove_best_effort(*temporary);
        return std::unexpected(error);
    }
    temporary_handle.reset();

    const DWORD attributes = GetFileAttributesW(path.c_str());
    BOOL published = FALSE;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        // REPLACEFILE_WRITE_THROUGH is explicitly unsupported by Windows.
        published = ReplaceFileW(path.c_str(), temporary->c_str(), nullptr, 0,
                                 nullptr, nullptr);
    } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
        published = MoveFileExW(temporary->c_str(), path.c_str(),
                                MOVEFILE_WRITE_THROUGH);
    }
    if (!published) {
        const DWORD error = GetLastError();
        // A failed publish may still have completed after the API error on an
        // unusual file system.  Check the final bytes before touching recovery
        // material.
        auto final_bytes = read_file_bytes(path);
        if (final_bytes && *final_bytes == bytes) return {};
        remove_best_effort(*temporary);
        return std::unexpected(Error::make(
            ErrorCode::OsError, "无法发布文件: " + path.string(), error));
    }

    auto final_bytes = read_file_bytes(path);
    if (!final_bytes) return std::unexpected(final_bytes.error());
    if (*final_bytes != bytes) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "发布后重新读取的内容不一致: " + path.string()));
    }
    return {};
}

Result<void> remove_file_if_unchanged(const std::filesystem::path& path,
                                      std::string_view expected_bytes) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return std::unexpected(os_error("无法锁定待删除的源文件: " + path.string()));
    }
    auto current = read_handle_bytes(file.get(), path);
    if (!current) return std::unexpected(current.error());
    if (*current != expected_bytes) {
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "迁移期间源文件已变化，已保留两个副本: " + path.string()));
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition,
                                    sizeof(disposition))) {
        return std::unexpected(os_error("无法删除已验证的源文件: " + path.string()));
    }
    return {};
}

Result<std::string> sha256_hex(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "无法初始化 SHA-256", static_cast<unsigned long>(status)));
    }
    DWORD object_length = 0;
    DWORD returned = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&object_length),
                               sizeof(object_length), &returned, 0);
    if (status < 0 || object_length == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::unexpected(Error::make(
            ErrorCode::OsError, "无法查询 SHA-256 状态大小", static_cast<unsigned long>(status)));
    }
    std::vector<UCHAR> object(object_length);
    std::array<UCHAR, 32> digest{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(algorithm, &hash, object.data(), object_length,
                              nullptr, 0, 0);
    if (status >= 0 && !bytes.empty()) {
        status = BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(),
                                  static_cast<ULONG>(digest.size()), 0);
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "无法计算 SHA-256", static_cast<unsigned long>(status)));
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const UCHAR value : digest) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0F]);
    }
    return result;
}

}  // namespace hoyoflux::win32
