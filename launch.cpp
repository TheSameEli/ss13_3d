// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include "win_handle.h"

static uintptr_t remoteKernel(DWORD pid) {
    HANDLE snap = INVALID_HANDLE_VALUE;
    // Windows can report ERROR_BAD_LENGTH while startup loads DLLs.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            break;
        Sleep(25);
    }
    WinHandle snapshot(snap);
    if (!snapshot)
        return 0;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    uintptr_t address = 0;
    if (Module32FirstW(snap, &entry))
        do {
            if (!_wcsicmp(entry.szModule, L"kernel32.dll")) {
                address = (uintptr_t)entry.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &entry));
    return address;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        fwprintf(stderr, L"Usage: launch.exe dreamseeker.exe probe.dll byond://host:port\n");
        return 1;
    }
    if (wcsncmp(argv[3], L"byond://", 8) || wcspbrk(argv[3], L"\" \t\r\n")) {
        fwprintf(stderr, L"Expected a byond://host:port URL\n");
        return 1;
    }
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" " + argv[3];
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(argv[1], &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError());
        return 2;
    }
    WinHandle processHandle(process.hProcess);
    WinHandle processThread(process.hThread);
    // Inject only into the client we just created, never an existing session.
    WaitForInputIdle(process.hProcess, 15000);
    size_t bytes = (wcslen(argv[2]) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T written;
    uintptr_t kernel = remoteKernel(process.dwProcessId);
    auto localKernel = GetModuleHandleW(L"kernel32.dll");
    auto load = GetProcAddress(localKernel, "LoadLibraryW");
    // Refuse forwarded exports: the module-relative offset must belong to kernel32.
    MEMORY_BASIC_INFORMATION info = {};
    VirtualQuery(reinterpret_cast<void *>(load), &info, sizeof(info));
    if (!remote || !kernel || info.AllocationBase != localKernel ||
        !WriteProcessMemory(process.hProcess, remote, argv[2], bytes, &written) || written != bytes) {
        DWORD error = GetLastError();
        if (remote)
            VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
        fwprintf(stderr, L"Injection setup failed: %lu; client left running\n", error);
        return 3;
    }
    auto address =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(kernel + (uintptr_t)load - (uintptr_t)localKernel);
    HANDLE thread = CreateRemoteThread(process.hProcess, nullptr, 0, address, remote, 0, nullptr);
    WinHandle loaderThread(thread);
    if (!loaderThread) {
        DWORD error = GetLastError();
        VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
        fwprintf(stderr, L"Injection failed: %lu; client left running\n", error);
        return 4;
    }
    DWORD wait = WaitForSingleObject(thread, 15000), module = 0;
    // The remote thread can still be reading the DLL path on timeout. Leave
    // that allocation alive until the client exits, but close our handles.
    if (wait != WAIT_OBJECT_0) {
        fwprintf(stderr, L"Injection wait failed/timed out; client left running\n");
        return 5;
    }
    BOOL readExit = GetExitCodeThread(thread, &module);
    VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
    if (!readExit || !module) {
        fwprintf(stderr, L"LoadLibrary failed; client left running\n");
        return 6;
    }
    wprintf(L"Native probe loaded in new DreamSeeker PID %lu\n", process.dwProcessId);
    return 0;
}
