// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#pragma once
#include <windows.h>

// Owned kernel handle. Borrowed HWND/HMODULE values must never use this wrapper.
class WinHandle {
  public:
    explicit WinHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~WinHandle() {
        if (*this)
            CloseHandle(value_);
    }
    WinHandle(const WinHandle &) = delete;
    WinHandle &operator=(const WinHandle &) = delete;
    HANDLE get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_;
};
