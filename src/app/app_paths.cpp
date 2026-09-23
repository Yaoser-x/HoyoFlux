#include "app/app_paths.hpp"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <string>

namespace hoyoflux::app {
namespace {

Result<std::filesystem::path> executable_path() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD size = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "无法获取 HoyoFlux.exe 所在路径",
                GetLastError()));
        }
        if (size < buffer.size()) {
            buffer.resize(size);
            return std::filesystem::path(std::move(buffer));
        }
        if (buffer.size() >= 32768) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "HoyoFlux.exe 所在路径过长"));
        }
        buffer.resize(buffer.size() * 2);
    }
}

Result<void> prove_writable(const std::filesystem::path& directory) {
    const auto probe = directory /
        (L".hoyoflux-write-test-" + std::to_wstring(GetCurrentProcessId()));
    {
        std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return std::unexpected(Error::make(
                ErrorCode::OsError,
                "软件目录不可写，请将 HoyoFlux 移到普通文件夹后重试: " +
                    directory.string()));
        }
        stream << "writable";
        stream.flush();
        if (!stream) {
            return std::unexpected(Error::make(
                ErrorCode::OsError,
                "无法写入软件目录，请检查文件夹权限: " + directory.string()));
        }
    }
    std::error_code ec;
    std::filesystem::remove(probe, ec);
    if (ec) {
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "无法清理软件目录中的写入测试文件: " + probe.string(),
            static_cast<unsigned long>(ec.value())));
    }
    return {};
}

}  // namespace

Result<AppPaths> resolve_app_paths() {
    auto executable = executable_path();
    if (!executable) {
        return std::unexpected(executable.error());
    }
    AppPaths paths;
    paths.executable = std::move(*executable);
    paths.root = paths.executable.parent_path();
    paths.config = paths.root / L"config.toml";
    paths.data = paths.root / L"data";
    paths.state = paths.data / L"state";
    paths.journal = paths.state / L"active-session.json";
    paths.migration_record = paths.state / L"portable-migration.toml";
    paths.backups = paths.data / L"backups";
    paths.diagnostics = paths.data / L"diagnostics.txt";
    return paths;
}

Result<void> prepare_portable_directories(const AppPaths& paths) {
    std::error_code ec;
    std::filesystem::create_directories(paths.state, ec);
    if (ec) {
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "无法创建本地数据目录: " + paths.state.string(),
            static_cast<unsigned long>(ec.value())));
    }
    std::filesystem::create_directories(paths.backups, ec);
    if (ec) {
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "无法创建本地备份目录: " + paths.backups.string(),
            static_cast<unsigned long>(ec.value())));
    }
    if (auto writable = prove_writable(paths.root); !writable) {
        return writable;
    }
    return prove_writable(paths.data);
}

Result<std::filesystem::path> legacy_appdata_root() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "无法定位旧版 AppData 配置目录",
            static_cast<unsigned long>(result)));
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path / L"HoyoFlux";
}

}  // namespace hoyoflux::app
