// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "first_person.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <cstdio>

namespace {
std::atomic<bool> enabled = false, locked = false;
std::atomic<float> yaw = 0, pitch = 0;
ULONGLONG lastToggle = 0;
HWND lookWindow = nullptr, lastMouseWindow = nullptr;
unsigned cursorHideCalls = 0;
using SetCursorFunction = HCURSOR(WINAPI *)(HCURSOR);
SetCursorFunction nativeSetCursor = ::SetCursor;
HCURSOR WINAPI firstPersonSetCursor(HCURSOR cursor) {
    return nativeSetCursor(enabled && locked ? nullptr : cursor);
}
void installCursorFilter() {
    for (HMODULE module :
         {GetModuleHandleW(nullptr), GetModuleHandleW(L"byondwin.dll"), GetModuleHandleW(L"byondcore.dll")}) {
        if (!module)
            continue;
        auto base = reinterpret_cast<unsigned char *>(module);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva)
            continue;
        auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + rva);
        for (; imports->Name; ++imports) {
            if (!imports->OriginalFirstThunk)
                continue;
            auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk),
                 addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                    continue;
                auto entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<char *>(entry->Name), "SetCursor") ||
                    addresses->u1.Function != reinterpret_cast<DWORD>(nativeSetCursor))
                    continue;
                DWORD old;
                if (VirtualProtect(&addresses->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    InterlockedCompareExchangePointer(
                        reinterpret_cast<void *volatile *>(&addresses->u1.Function),
                        reinterpret_cast<void *>(&firstPersonSetCursor),
                        reinterpret_cast<void *>(nativeSetCursor));
                    VirtualProtect(&addresses->u1.Function, sizeof(void *), old, &old);
                }
            }
        }
    }
}
UINT lastMouseMessage = 0;
POINT dragPoint = {};
unsigned dragButtons = 0;
unsigned mappedKeys[256] = {};
unsigned worldMovement = 0;
bool movementStepPending = false;
unsigned physicalMovement = 0;
unsigned movementSequence = 0;
ULONGLONG lastMovementRepeat = 0;
thread_local unsigned nativePhysicalKey = 0, nativeMappedKey = 0;
std::atomic<void *> nativeClient = nullptr;
using NativeCommand = unsigned char(__thiscall *)(void *, const char *, unsigned char, unsigned char,
                                                  unsigned char);
NativeCommand nativeCommandParser = nullptr;
void traceCommand(const char *command, int result);
void movementTrace(const char *event, const char *command = "", unsigned direction = 0, int x = 0, int y = 0,
                   uint64_t cid = 0, int level = 0) {
    static HANDLE file = INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE) {
        wchar_t path[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"BS220_NATIVE_LOG", path, MAX_PATH))
            return;
        wcscat_s(path, L".movement.jsonl");
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, 0,
                           nullptr);
    }
    if (file == INVALID_HANDLE_VALUE)
        return;
    char escaped[320] = {};
    unsigned at = 0;
    for (unsigned i = 0; command[i] && at < sizeof(escaped) - 3; ++i) {
        char c = command[i];
        if (c == '"' || c == '\\')
            escaped[at++] = '\\';
        escaped[at++] = c >= 32 ? c : ' ';
    }
    char line[800];
    sprintf_s(line,
              "{\"ms\":%llu,\"pid\":%lu,\"thread\":%lu,\"seq\":%u,\"event\":\"%s\",\"physical\":%u,\"world\":"
              "%u,\"direction\":%u,\"yaw\":%.5f,\"locked\":%d,\"pending\":%d,\"command\":\"%s\",\"cid\":\"%"
              "016llx\",\"x\":%d,\"y\":%d,\"level\":%d}\r\n",
              GetTickCount64(), GetCurrentProcessId(), GetCurrentThreadId(), movementSequence, event,
              physicalMovement, worldMovement, direction, yaw.load(), int(locked.load()),
              int(movementStepPending), escaped, cid, x, y, level);
    DWORD written;
    WriteFile(file, line, DWORD(strlen(line)), &written, nullptr);
}
bool nativeMovementCommand(const char *command) {
    if (!command)
        return false;
    while (*command == ' ' || *command == '"')
        ++command;
    if (!_strnicmp(command, "KeyDown ", 8)) {
        command += 8;
        while (*command == ' ' || *command == '"')
            ++command;
        char key[32] = {};
        unsigned n = 0;
        while (*command && *command != ' ' && *command != '"' && n < 31)
            key[n++] = *command++;
        for (auto name : {"W", "A", "S", "D", "North", "South", "East", "West", "Northeast", "Northwest",
                          "Southeast", "Southwest"})
            if (!_stricmp(key, name))
                return true;
    } else
        for (auto name :
             {".north", ".south", ".east", ".west", ".northeast", ".northwest", ".southeast", ".southwest"}) {
            size_t size = strlen(name);
            if (!_strnicmp(command, name, size) &&
                (!command[size] || command[size] == ' ' || command[size] == '"'))
                return true;
        }
    return false;
}
unsigned char __fastcall filterNativeCommand(void *client, void *, const char *command, unsigned char a,
                                             unsigned char b, unsigned char c) {
    if (enabled && locked && command) {
        bool blocked = nativeMovementCommand(command);
        if (blocked || strstr(command, "KeyUp"))
            movementTrace(blocked ? "native_blocked" : "native_release", command);
        if (blocked) {
            // Clear a direction that may already have reached the server
            // before capture. Preserve the native macro's arguments/context.
            const char *start = command;
            while (*start == ' ' || *start == '"')
                ++start;
            if (!_strnicmp(start, "KeyDown ", 8)) {
                char up[512];
                sprintf_s(up, "KeyUp %s", start + 8);
                nativeCommandParser(client, up, a, b, c);
            }
            return 1;
        }
    }
    return nativeCommandParser(client, command, a, b, c);
}
constexpr LPARAM movementInjection = LPARAM(1u << 25);
constexpr UINT movementStepMessage = WM_APP + 0x513;
constexpr UINT hoverMessage = WM_APP + 0x514;
void traceCommand(const char *command, int result) {
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"BS220_NATIVE_LOG", path, MAX_PATH))
        return;
    wcscat_s(path, L".input.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    char line[256];
    sprintf_s(line, "command result=%d %s\r\n", result, command);
    DWORD written;
    WriteFile(file, line, DWORD(strlen(line)), &written, nullptr);
    CloseHandle(file);
}
void movementMessage(unsigned key, bool down) {
    if (!lookWindow)
        return;
    unsigned worldKey = key == 'W' || key == VK_UP      ? VK_UP
                        : key == 'D' || key == VK_RIGHT ? VK_RIGHT
                        : key == 'S' || key == VK_DOWN  ? VK_DOWN
                        : key == 'A' || key == VK_LEFT  ? VK_LEFT
                                                        : 'X';
    unsigned scan = MapVirtualKeyW(worldKey, MAPVK_VK_TO_VSC);
    LPARAM flags = 1 | LPARAM(scan << 16) | movementInjection;
    if (worldKey != unsigned('X'))
        flags |= LPARAM(1u << 24);
    if (!down)
        flags |= LPARAM(3u << 30);
    // Run through the normal GUI dispatch, never re-enter BYOND's command
    // parser from a message hook. The incoming physical WASD is consumed;
    // only the camera-relative world arrow reaches the native macro table.
    int result = PostMessageW(lookWindow, down ? WM_KEYDOWN : WM_KEYUP, worldKey, flags);
    char command[80];
    sprintf_s(command, "world-key %s vk=%u", down ? "down" : "up", worldKey);
    traceCommand(command, result);
}
unsigned cameraMovement(float angle, int forward, int right) {
    if (!forward && !right)
        return 0;
    float x = forward * std::sin(angle) + right * std::cos(angle);
    float y = -forward * std::cos(angle) + right * std::sin(angle);
    int sector = (int(std::lround(std::atan2(x, -y) / .7853981634f)) + 8) % 8;
    const unsigned compass[] = {1, 3, 2, 6, 4, 12, 8, 9};
    return compass[sector];
}
void queueMovementStep(unsigned direction) {
    if (lookWindow && direction && !movementStepPending) {
        ++movementSequence;
        movementStepPending = PostMessageW(lookWindow, movementStepMessage, direction, 0) != 0;
        movementTrace("queued", "", direction);
    }
}
const char *movementCommand(unsigned direction) {
    switch (direction) {
    case 1:
        return ".north";
    case 2:
        return ".east";
    case 4:
        return ".south";
    case 8:
        return ".west";
    case 3:
        return ".northeast";
    case 6:
        return ".southeast";
    case 12:
        return ".southwest";
    case 9:
        return ".northwest";
    default:
        return nullptr;
    }
}
void updateWorldMovement() {
    int forward = int(mappedKeys['W']) - int(mappedKeys['S']);
    int right = int(mappedKeys['D']) - int(mappedKeys['A']);
    unsigned next = cameraMovement(yaw.load(), forward, right);
    bool pressed = !worldMovement && next;
    worldMovement = next;
    if (pressed)
        queueMovementStep(next);
}
void followMovementYawFromPhysical(unsigned physicalMask) {
    if (physicalMovement != physicalMask) {
        physicalMovement = physicalMask;
        movementTrace("physical_changed");
    }
    const unsigned letters[] = {'W', 'D', 'S', 'A'};
    for (unsigned i = 0; i < 4; ++i)
        mappedKeys[letters[i]] = (physicalMask >> i) & 1;
    updateWorldMovement();
    auto now = GetTickCount64();
    if (worldMovement && now - lastMovementRepeat >= 100)
        queueMovementStep(worldMovement);
}
void followMovementYaw() {
    unsigned physicalMask = 0;
    const unsigned letters[] = {'W', 'D', 'S', 'A'};
    for (unsigned i = 0; i < 4; ++i)
        if (GetAsyncKeyState(letters[i]) & 0x8000)
            physicalMask |= 1u << i;
    followMovementYawFromPhysical(physicalMask);
}
SRWLOCK aimLock = SRWLOCK_INIT;
AimHit aim;
ULONGLONG aimTime = 0;
using NativeHit = int(__thiscall *)(void *, POINT, void *, void *, void *);
using NativeHitList = int(__thiscall *)(void *, POINT, void *, void *, int, bool);
NativeHit originalHit = nullptr;
NativeHitList originalHitList = nullptr;
void *hitWindow = nullptr;
void *hitList = nullptr;
void *hitIcons = nullptr;
alignas(4) unsigned char sourceParams[0x44] = {};
bool sourceValid = false, dragStarted = false, suppressReleaseHit = false;
using ParamsCopy = void *(__thiscall *)(void *, const void *);
using ParamsDestroy = void(__thiscall *)(void *);
using DragCommand = int(__thiscall *)(void *, const void *, const void *);
void clearDrag() {
    if (sourceValid) {
        auto destroy = reinterpret_cast<ParamsDestroy>(
            GetProcAddress(GetModuleHandleW(L"byondcore.dll"), "??1MouseParams@@QAE@XZ"));
        destroy(sourceParams);
    }
    sourceValid = false;
    dragStarted = false;
}
void populateAim(void *window, void *list, void *icons, void *params, const AimHit &hit) {
    struct DisplayPoint {
        float x, y;
    };
    using FloatHit = int(__thiscall *)(void *, DisplayPoint, void *, void *, void *);
    auto native = reinterpret_cast<FloatHit>(reinterpret_cast<unsigned char *>(GetProcAddress(
        GetModuleHandleW(L"byondwin.dll"),
        "?HitCid@GraphicsWnd@@QAEHUDisplayPoint@@PAUMapIconList@@PAVIconList@@AAUMouseParams@@@Z")));
    native(window, {hit.x, hit.y}, list, icons, params);
    memcpy(params, &hit.cid, 8);
    memcpy(static_cast<unsigned char *>(params) + 0x20, hit.icon + 0x6c, 4);
    auto iw = *reinterpret_cast<const uint16_t *>(hit.icon + 0x70),
         ih = *reinterpret_cast<const uint16_t *>(hit.icon + 0x72);
    *reinterpret_cast<int16_t *>(static_cast<unsigned char *>(params) + 0x24) = int16_t(std::clamp(
        int(hit.x) - int(*reinterpret_cast<const int16_t *>(hit.icon + 0x4c)) + 1, 1, int(iw ? iw : 32)));
    *reinterpret_cast<int16_t *>(static_cast<unsigned char *>(params) + 0x26) =
        int16_t(std::clamp(int(ih) - int(hit.y) + int(*reinterpret_cast<const int16_t *>(hit.icon + 0x4e)), 1,
                           int(ih ? ih : 32)));
}
void sendDrag(bool drop) {
    if (!sourceValid || !hitWindow || !hitList || !hitIcons || !nativeClient.load())
        return;
    AimHit target;
    if (!ReadAim(hitWindow, target))
        return;
    auto module = GetModuleHandleW(L"byondcore.dll");
    auto copy = reinterpret_cast<ParamsCopy>(GetProcAddress(module, "??0MouseParams@@QAE@ABU0@@Z"));
    auto destroy = reinterpret_cast<ParamsDestroy>(GetProcAddress(module, "??1MouseParams@@QAE@XZ"));
    alignas(4) unsigned char targetParams[0x44];
    copy(targetParams, sourceParams);
    populateAim(hitWindow, hitList, hitIcons, targetParams, target);
    auto command = reinterpret_cast<DragCommand>(
        GetProcAddress(module, drop ? "?GenMouseDropCommand@DungClient@@QAEHABUMouseParams@@0@Z"
                                    : "?GenMouseDragCommand@DungClient@@QAEHABUMouseParams@@0@Z"));
    int result = command(nativeClient.load(), sourceParams, targetParams);
    char detail[160];
    sprintf_s(detail, "%s source=%016llx target=%016llx", drop ? "drop" : "drag",
              *reinterpret_cast<uint64_t *>(sourceParams), target.cid);
    traceCommand(detail, result);
    destroy(targetParams);
    dragStarted = true;
}
void sendMouse(const char *symbol, const void *params) {
    using Command = int(__thiscall *)(void *, const void *);
    auto command = reinterpret_cast<Command>(GetProcAddress(GetModuleHandleW(L"byondcore.dll"), symbol));
    if (command && nativeClient.load()) {
        int result = command(nativeClient.load(), params);
        char detail[220];
        sprintf_s(
            detail, "mouse flags=%04x %s",
            unsigned(*reinterpret_cast<const uint16_t *>(static_cast<const unsigned char *>(params) + 0x14)),
            symbol);
        traceCommand(detail, result);
    }
}
unsigned nativeMouseFlags(unsigned buttons, WPARAM state, bool alt) {
    // MouseParams uses BYOND flags, not the Windows MK_SHIFT/MK_MBUTTON values.
    return buttons | ((state & MK_CONTROL) ? 8u : 0u) | ((state & MK_SHIFT) ? 16u : 0u) | (alt ? 32u : 0u);
}
bool beginMouse(unsigned buttons) {
    AimHit hit;
    if (!hitWindow || !ReadAim(hitWindow, hit) || !hit.cid)
        return false;
    auto module = GetModuleHandleW(L"byondcore.dll");
    using Construct = void *(__thiscall *)(void *);
    auto construct = reinterpret_cast<Construct>(GetProcAddress(module, "??0MouseParams@@QAE@XZ"));
    construct(sourceParams);
    sourceValid = true;
    populateAim(hitWindow, hitList, hitIcons, sourceParams, hit);
    *reinterpret_cast<unsigned *>(sourceParams + 0x14) = buttons;
    using SetString = void *(__thiscall *)(void *, const char *);
    auto setString = reinterpret_cast<SetString>(GetProcAddress(module, "??4DMString@@QAEAAU0@PBD@Z"));
    setString(sourceParams + 0x1c, "mapwindow.map");
    sendMouse("?GenMouseDownCommand@DungClient@@QAEHABUMouseParams@@@Z", sourceParams);
    return true;
}
bool hudHit(void *list, const void *params) {
    if (!list)
        return false;
    auto bytes = static_cast<const unsigned char *>(list);
    auto icons = *reinterpret_cast<const unsigned char *const *>(bytes + 4);
    unsigned count = *reinterpret_cast<const unsigned short *>(bytes + 0xa);
    uint64_t cid = *reinterpret_cast<const uint64_t *>(params);
    if (!icons || !cid)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        auto icon = icons + i * 216;
        if (*reinterpret_cast<const uint64_t *>(icon + 0xc) != cid)
            continue;
        int plane = *reinterpret_cast<const short *>(icon + 0x24);
        if (plane == 40 || plane == 41)
            return true;
    }
    return false;
}
void diagnostic(const char *event, void *window, const AimHit &hit) {
    static HANDLE file = INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE) {
        wchar_t path[MAX_PATH];
        GetEnvironmentVariableW(L"BS220_NATIVE_LOG", path, MAX_PATH);
        wcscat_s(path, L".input.log");
        file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           0, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE)
        return;
    char line[240];
    sprintf_s(line, "%s window=%p aimWindow=%p cid=%016llx native=%.1f,%.1f locked=%d message=%x\r\n", event,
              window, hit.window, hit.cid, hit.x, hit.y, int(locked.load()), lastMouseMessage);
    DWORD written;
    WriteFile(file, line, DWORD(strlen(line)), &written, nullptr);
}
POINT center() {
    RECT r = {};
    GetClientRect(lookWindow, &r);
    return {(r.right - r.left) / 2, (r.bottom - r.top) / 2};
}
void warp() {
    if (!lookWindow)
        return;
    auto p = center();
    ClientToScreen(lookWindow, &p);
    SetCursorPos(p.x, p.y);
}
void release() {
    if (locked)
        movementTrace("release_mouse");
    if (locked.exchange(false)) {
        while (cursorHideCalls) {
            ShowCursor(TRUE);
            --cursorHideCalls;
        }
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
    }
    dragButtons = 0;
    clearDrag();
    memset(mappedKeys, 0, sizeof(mappedKeys));
    updateWorldMovement();
    movementStepPending = false;
    lookWindow = nullptr;
}
bool acquire(void *window) {
    AimHit hit;
    if (!ReadAim(window, hit) || !lastMouseWindow || lastMouseMessage != WM_LBUTTONDOWN)
        return false;
    lookWindow = lastMouseWindow;
    SetFocus(lookWindow);
    locked = true;
    movementTrace("capture");
    cursorHideCalls = 0;
    do {
        ++cursorHideCalls;
    } while (ShowCursor(FALSE) >= 0 && cursorHideCalls < 32);
    SetCursor(nullptr);
    warp();
    return true;
}
} // namespace
FirstPersonView GetFirstPersonView() {
    return {enabled.load(), yaw.load(), pitch.load()};
}
void ToggleFirstPerson() {
    auto now = GetTickCount64();
    if (now - lastToggle < 200)
        return;
    lastToggle = now;
    release();
    enabled = !enabled.load();
}
void SuspendFirstPersonInput() {
    release();
    enabled = false;
}
void PublishAim(const AimHit &hit) {
    AcquireSRWLockExclusive(&aimLock);
    aim = hit;
    aimTime = GetTickCount64();
    ReleaseSRWLockExclusive(&aimLock);
}
bool ReadAim(void *window, AimHit &hit) {
    AcquireSRWLockShared(&aimLock);
    hit = aim;
    bool valid = hit.window == window && GetTickCount64() - aimTime < 250;
    ReleaseSRWLockShared(&aimLock);
    return valid;
}
void UpdateFirstPersonInput() {
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    static bool held = false;
    bool pressed = foreground == GetCurrentProcessId() &&
                   (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (pressed && !held && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000))
        ToggleFirstPerson();
    held = pressed;
    if (!locked)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId() || !IsWindow(lookWindow) || IsIconic(GetAncestor(lookWindow, GA_ROOT))) {
        release();
        return;
    }
    // Modal BYOND windows and browser interfaces must remain usable. Mouse
    // look is resumed explicitly by clicking the map after closing them.
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    DWORD thread = GetWindowThreadProcessId(lookWindow, nullptr);
    if (GetForegroundWindow() != GetAncestor(lookWindow, GA_ROOT)) {
        release();
        return;
    }
    if (GetGUIThreadInfo(thread, &info) && info.hwndFocus) {
        wchar_t name[128] = {};
        GetClassNameW(info.hwndFocus, name, 128);
        if (wcsstr(name, L"Internet Explorer") || wcsstr(name, L"Chrome") || wcsstr(name, L"WebView")) {
            release();
            return;
        }
    }
    SetCursor(nullptr);
    followMovementYaw();
    static uint64_t lastHover = ~0ull;
    static ULONGLONG hoverTick = 0;
    AimHit hit;
    if (ReadAim(hitWindow, hit) && hit.cid != lastHover && GetTickCount64() - hoverTick >= 100) {
        if (PostMessageW(lookWindow, hoverMessage, 0, 0)) {
            lastHover = hit.cid;
            hoverTick = GetTickCount64();
        }
    }
}
void HandleFirstPersonMessage(MSG &m) {
    if (m.message == hoverMessage) {
        AimHit hit;
        if (enabled && locked && hitWindow && hitList && hitIcons && nativeClient.load() &&
            ReadAim(hitWindow, hit)) {
            auto module = GetModuleHandleW(L"byondcore.dll");
            auto construct = reinterpret_cast<void *(__thiscall *)(void *)>(
                GetProcAddress(module, "??0MouseParams@@QAE@XZ"));
            auto destroy = reinterpret_cast<ParamsDestroy>(GetProcAddress(module, "??1MouseParams@@QAE@XZ"));
            auto hover = reinterpret_cast<int(__thiscall *)(void *, const void *)>(
                GetProcAddress(module, "?GenMouseOverCommand@DungClient@@QAEHABUMouseParams@@@Z"));
            if (construct && destroy && hover) {
                alignas(4) unsigned char params[0x44] = {};
                construct(params);
                if (hit.cid)
                    populateAim(hitWindow, hitList, hitIcons, params, hit);
                auto setString = reinterpret_cast<void *(__thiscall *)(void *, const char *)>(
                    GetProcAddress(module, "??4DMString@@QAEAAU0@PBD@Z"));
                setString(params + 0x1c, "mapwindow.map");
                hover(nativeClient.load(), params);
                destroy(params);
            }
        }
        m.message = WM_NULL;
        return;
    }
    if (m.message == movementStepMessage) {
        // Execute one built-in move from the GUI queue. No key state, focus
        // navigation, parser-mode mutation or server KeyDown is involved.
        movementStepPending = false;
        const char *command = movementCommand(unsigned(m.wParam));
        if (enabled && locked && m.hwnd == lookWindow && command && nativeClient.load()) {
            auto parser = reinterpret_cast<NativeCommand>(
                GetProcAddress(GetModuleHandleW(L"byondcore.dll"), "?ParseCommand@DungClient@@QAEEPBDEEE@Z"));
            if (parser) {
                movementTrace("dispatch", command, unsigned(m.wParam));
                int result = parser(nativeClient.load(), command, 0, 0, 0);
                char detail[120];
                sprintf_s(detail, "%s result=%d", command, result);
                movementTrace("dispatch_result", detail, unsigned(m.wParam));
            }
        }
        lastMovementRepeat = GetTickCount64();
        m.message = WM_NULL;
        return;
    }
    if (m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST) {
        lastMouseWindow = m.hwnd;
        lastMouseMessage = m.message;
    }
    if ((m.message == WM_KEYDOWN || m.message == WM_KEYUP) && m.wParam == VK_F8 &&
        !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        if (m.message == WM_KEYDOWN && !(m.lParam & (1L << 30)))
            ToggleFirstPerson();
        m.message = WM_NULL;
        return;
    }
    if (enabled && m.message == WM_KEYUP && m.wParam == VK_TAB) {
        m.message = WM_NULL;
        return;
    }
    if (!enabled || !locked)
        return;
    if (m.message == WM_CHAR && ((m.wParam | 32) == 'w' || (m.wParam | 32) == 'a' || (m.wParam | 32) == 's' ||
                                 (m.wParam | 32) == 'd')) {
        m.message = WM_NULL;
        return;
    }
    if (m.message == WM_KEYDOWN && m.wParam == VK_TAB) {
        release();
        m.message = WM_NULL;
        return;
    }
    if (m.message == WM_MOUSEWHEEL) {
        // Use the station's native X binding (swap_hands), rather than
        // scrolling the crosshair target or simulating hand state locally.
        int steps = std::abs(int(short(HIWORD(m.wParam)))) / WHEEL_DELTA;
        for (int i = 0; i < steps; ++i) {
            movementMessage('X', true);
            movementMessage('X', false);
        }
        m.message = WM_NULL;
        return;
    }
    if ((m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN) && m.hwnd != lookWindow &&
        !IsChild(lookWindow, m.hwnd)) {
        release();
        return;
    }
    if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
        release();
        m.message = WM_NULL;
        return;
    }
    if (m.hwnd == lookWindow && (m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN)) {
        if (!dragButtons) {
            dragPoint = {};
            clearDrag();
            suppressReleaseHit = false;
        }
        dragButtons |= m.message == WM_LBUTTONDOWN ? MK_LBUTTON : MK_RBUTTON;
        beginMouse(nativeMouseFlags(dragButtons, m.wParam, (GetKeyState(VK_MENU) & 0x8000) != 0));
        // Do not enter DreamSeeker's native drag loop: our pointer is locked
        // and the source/target are determined by the crosshair instead.
        m.message = WM_NULL;
        return;
    }
    if (dragButtons && m.hwnd == lookWindow && (m.message == WM_LBUTTONUP || m.message == WM_RBUTTONUP)) {
        if (m.message == WM_LBUTTONUP && dragStarted) {
            sendDrag(true);
            suppressReleaseHit = true;
        }
        if (sourceValid) {
            if (!dragStarted)
                sendMouse("?GenClickCommand@DungClient@@QAEHABUMouseParams@@@Z", sourceParams);
            sendMouse("?GenMouseUpCommand@DungClient@@QAEHABUMouseParams@@@Z", sourceParams);
        }
        dragButtons &= ~unsigned(m.message == WM_LBUTTONUP ? MK_LBUTTON : MK_RBUTTON);
        if (!dragButtons)
            clearDrag();
        m.message = WM_NULL;
        return;
    }
    if (m.message == WM_MOUSEMOVE && m.hwnd == lookWindow) {
        // Warping queues additional WM_MOUSEMOVEs. Read the current cursor,
        // rather than rotating again from obsolete positions in that queue.
        auto p = center();
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(lookWindow, &cursor);
        int dx = cursor.x - p.x, dy = cursor.y - p.y;
        yaw = std::remainder(yaw.load() + dx * .003f, 6.283185307f);
        pitch = std::clamp(pitch.load() - dy * .003f, -1.50f, 1.45f);
        if (dx || dy)
            warp();
        // BYOND needs a displaced pointer to start and maintain its own drag.
        // Keep that pointer separate from the cursor used for mouse look.
        if (dragButtons) {
            dragPoint.x += dx;
            dragPoint.y += dy;
        }
        if ((dragButtons & MK_LBUTTON) && !dragStarted &&
            (std::abs(dragPoint.x) > GetSystemMetrics(SM_CXDRAG) ||
             std::abs(dragPoint.y) > GetSystemMetrics(SM_CYDRAG)))
            sendDrag(false);
        m.message = WM_NULL;
        return;
    }
    if (m.message == WM_KEYDOWN || m.message == WM_KEYUP) {
        unsigned key = unsigned(m.wParam);
        if (key >= 256)
            return;
        nativePhysicalKey = key;
        nativeMappedKey = key;
        if (m.lParam & movementInjection)
            return;
        const unsigned letters[] = {'W', 'D', 'S', 'A'};
        for (int i = 0; i < 4; ++i)
            if (key == letters[i]) {
                char detail[120];
                sprintf_s(detail, "vk=%u message=%u scan=%u repeat=%u", key, m.message,
                          unsigned((m.lParam >> 16) & 255), unsigned((m.lParam >> 30) & 1));
                movementTrace("key_event", detail);
                mappedKeys[key] = m.message == WM_KEYDOWN ? 1 : 0;
                updateWorldMovement();
                // Send explicit world directions, with balanced press/release.
                // Neither Windows layout nor native macro key substitution takes
                // part in first-person movement now.
                m.message = WM_NULL;
                return;
            }
    }
}
static LRESULT CALLBACK inputHook(int code, WPARAM removal, LPARAM payload) {
    if (code >= 0 && removal == PM_REMOVE)
        HandleFirstPersonMessage(*reinterpret_cast<MSG *>(payload));
    return CallNextHookEx(nullptr, code, removal, payload);
}
void installNativeCommandFilter() {
    auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
        base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imports->Name; ++imports) {
        if (_stricmp(reinterpret_cast<char *>(base + imports->Name), "byondcore.dll") ||
            !imports->OriginalFirstThunk)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk),
             addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char *>(entry->Name), "?ParseCommand@DungClient@@QAEEPBDEEE@Z"))
                continue;
            auto original =
                GetProcAddress(GetModuleHandleW(L"byondcore.dll"), reinterpret_cast<char *>(entry->Name));
            if (addresses->u1.Function != reinterpret_cast<DWORD>(original))
                return;
            DWORD old;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return;
            nativeCommandParser = reinterpret_cast<NativeCommand>(original);
            InterlockedExchangePointer(reinterpret_cast<void **>(&addresses->u1.Function),
                                       reinterpret_cast<void *>(&filterNativeCommand));
            VirtualProtect(&addresses->u1.Function, sizeof(void *), old, &old);
            traceCommand("native movement command filter installed", 1);
            return;
        }
    }
}
void EnsureFirstPersonInputHook() {
    static bool installed = (installCursorFilter(), installNativeCommandFilter(), true);
    (void)installed;
    static thread_local HHOOK hook =
        SetWindowsHookExW(WH_GETMESSAGE, inputHook, nullptr, GetCurrentThreadId());
    (void)hook;
}
void SetNativeHitFunctions(void *hit, void *hitListFunction) {
    if (hit)
        originalHit = reinterpret_cast<NativeHit>(hit);
    if (hitListFunction)
        originalHitList = reinterpret_cast<NativeHitList>(hitListFunction);
}
void SetFirstPersonClient(void *client) {
    nativeClient = client;
}
void TraceFirstPersonPosition(uint64_t cid, int x, int y, int level, float screenX, float screenY) {
    static uint64_t previousCid = 0;
    static int previousX = 0, previousY = 0, previousLevel = 0;
    if (!enabled || !cid)
        return;
    if (cid != previousCid || x != previousX || y != previousY || level != previousLevel) {
        char detail[160];
        sprintf_s(detail, "delta=%d,%d screen=%.2f,%.2f previous=%016llx", x - previousX, y - previousY,
                  screenX, screenY, previousCid);
        movementTrace("position", detail, 0, x, y, cid, level);
        previousCid = cid;
        previousX = x;
        previousY = y;
        previousLevel = level;
    }
}
void TraceFirstPersonSceneShift(uint64_t cid, float dx, float dy, int level) {
    static float accumulatedX = 0, accumulatedY = 0;
    static ULONGLONG previous = 0;
    accumulatedX += dx;
    accumulatedY += dy;
    auto now = GetTickCount64();
    if (now - previous >= 100 && (std::abs(accumulatedX) > .1f || std::abs(accumulatedY) > .1f)) {
        char detail[160];
        sprintf_s(detail, "player_pixel_delta=%.3f,%.3f", accumulatedX, accumulatedY);
        movementTrace("scene_shift", detail, 0, 0, 0, cid, level);
        accumulatedX = accumulatedY = 0;
        previous = now;
    }
}
int __fastcall FirstPersonHit(void *window, void *, POINT point, void *list, void *icons, void *params) {
    EnsureFirstPersonInputHook();
    hitWindow = window;
    hitList = list;
    hitIcons = icons;
    if (!lastMouseWindow) {
        lastMouseWindow = WindowFromPoint([] {
            POINT p;
            GetCursorPos(&p);
            return p;
        }());
        lastMouseMessage = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? WM_LBUTTONDOWN : 0;
    }
    if (enabled && !locked) {
        int result = originalHit(window, point, list, icons, params);
        if (result && hudHit(list, params))
            return result;
        if (acquire(window)) {
            AimHit hit;
            ReadAim(window, hit);
            diagnostic("capture", window, hit);
            return 0;
        }
        return result;
    }
    AimHit hit;
    if (!enabled || !locked || !ReadAim(window, hit))
        return originalHit(window, point, list, icons, params);
    if (suppressReleaseHit && lastMouseMessage == WM_LBUTTONUP)
        return 0;
    if (!hit.cid)
        return 0;
    if (lastMouseMessage == WM_LBUTTONDOWN || lastMouseMessage == WM_RBUTTONDOWN)
        diagnostic("click", window, hit);
    // Use the native display-coordinate path to populate all MouseParams;
    // override only the selected appearance when native flat picking overlaps it.
    populateAim(window, list, icons, params, hit);
    hitWindow = window;
    hitList = list;
    hitIcons = icons;
    if (lastMouseMessage == WM_LBUTTONDOWN && !sourceValid) {
        auto copy = reinterpret_cast<ParamsCopy>(
            GetProcAddress(GetModuleHandleW(L"byondcore.dll"), "??0MouseParams@@QAE@ABU0@@Z"));
        copy(sourceParams, params);
        sourceValid = true;
        using SetString = void *(__thiscall *)(void *, const char *);
        auto setString = reinterpret_cast<SetString>(
            GetProcAddress(GetModuleHandleW(L"byondcore.dll"), "??4DMString@@QAEAAU0@PBD@Z"));
        setString(sourceParams + 0x1c, "mapwindow.map");
    }
    return 1;
}
int __fastcall FirstPersonHitList(void *window, void *, POINT point, void *list, void *cids, int count,
                                  bool all) {
    if (enabled && locked && suppressReleaseHit && lastMouseMessage == WM_LBUTTONUP)
        return 0;
    AimHit hit;
    if (enabled && locked && ReadAim(window, hit)) {
        if (!hit.cid || count <= 0)
            return 0;
        memcpy(cids, &hit.cid, 8);
        return 1;
    }
    return originalHitList(window, point, list, cids, count, all);
}
