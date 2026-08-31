#pragma once

// RAII wrapper for HANDLE. The project bans raw `HANDLE` + manual
// `CloseHandle` and `goto`-based cleanup; every owned kernel handle flows
// through this type instead.

#include <windows.h>

#include <utility>

namespace hoyoflux::win32 {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { close(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != handle) {
            close();
            handle_ = handle;
        }
    }

private:
    void close() noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE handle_{nullptr};
};

}  // namespace hoyoflux::win32
