// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "volume.h"
#include "byond_abi.h"
#include "first_person.h"

// ABI for this exact x86 build, inferred from its decorated export.
using GetMapIcons = char(__thiscall *)(void *, const char *, void **, unsigned char, unsigned char);
static GetMapIcons original;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static volatile LONG calls;
static DWORD lastSnapshot;
static wchar_t snapshotPath[MAX_PATH];
static wchar_t texturePath[MAX_PATH];
static bool textureSaved;
static void *currentIcons;
using GetPixBits = const void *(__thiscall *)(void *, uint32_t);
static GetPixBits getPixBits;
using SpriteDriver = void *(__thiscall *)(void *, void *);
static SpriteDriver originalDriver;
static volatile LONG driverCalls;
using RenderScene = int(__thiscall *)(void *, void *, uintptr_t);
static RenderScene originalRender;
using PresentScene = int(__thiscall *)(void *, unsigned char);
static PresentScene originalPresent;
static thread_local void *activeList;
static thread_local void *activeIcons;
struct OwnedScene {
    void *icons = nullptr;
    std::vector<unsigned char> buffer;
    unsigned char header[12] = {};
};
static std::unordered_map<void *, void *> driverIcons;
static std::unordered_map<void *, OwnedScene> retainedScenes;
static volatile LONG renderCalls;
static const char *symbol = "?GetMapIcons@DungClient@@QAEDPBDPAPAUMapIconList@@EE@Z";

static void log(const char *text) {
    DWORD written;
    if (logFile != INVALID_HANDLE_VALUE)
        WriteFile(logFile, text, (DWORD)lstrlenA(text), &written, nullptr);
}

static void snapshot(void *list) {
    // 516.1688: GetMapIcons clears count at +8 and allocates capacity at +A
    // with 0xD8 stride; SubHitCid indexes the same buffer at +4.
    auto bytes = static_cast<unsigned char *>(list);
    auto count = *reinterpret_cast<uint16_t *>(bytes + 8);
    auto capacity = *reinterpret_cast<uint16_t *>(bytes + 10);
    auto icons = *reinterpret_cast<unsigned char **>(bytes + 4);
    if (!count || count > capacity || !icons)
        return;
    uint32_t extent = count;
    for (uint32_t i = 0; i < extent; ++i) {
        auto icon = icons + i * ByondAbi::mapIconStride;
        auto first = *reinterpret_cast<uint16_t *>(icon + 0xae);
        auto end = *reinterpret_cast<uint16_t *>(icon + 0xb0);
        if (first > end || end > capacity)
            return;
        if (end > extent)
            extent = end;
    }
    DWORD now = GetTickCount();
    if (lastSnapshot && now - lastSnapshot < 10000)
        return;
    lastSnapshot = now;
    HANDLE file = CreateFileW(snapshotPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    uint32_t header[] = {0x53493242, 2, ByondAbi::mapIconStride, count, extent};
    WriteFile(file, header, sizeof(header), &written, nullptr);
    WriteFile(file, icons, extent * ByondAbi::mapIconStride, &written, nullptr);
    CloseHandle(file);
    char line[160];
    sprintf_s(line, "snapshot roots=%u entries=%u capacity=%u stride=216 bytes=%u\r\n", count, extent,
              capacity, extent * ByondAbi::mapIconStride);
    log(line);
}

static void captureTexture(void *list, uintptr_t options) {
    if (textureSaved || !currentIcons || options != reinterpret_cast<uintptr_t>(currentIcons) + 4)
        return;
    auto bytes = static_cast<unsigned char *>(list);
    auto icons = *reinterpret_cast<unsigned char **>(bytes + 4);
    uint32_t extent = *reinterpret_cast<uint16_t *>(bytes + 8);
    uint32_t capacity = *reinterpret_cast<uint16_t *>(bytes + 10);
    for (uint32_t i = 0; i < extent; ++i) {
        auto icon = icons + i * ByondAbi::mapIconStride;
        auto end = *reinterpret_cast<uint16_t *>(icon + 0xb0);
        if (end > capacity)
            return;
        if (end > extent)
            extent = end;
    }
    for (uint32_t i = 0; i < extent; ++i) {
        auto icon = icons + i * ByondAbi::mapIconStride;
        auto plane = *reinterpret_cast<int16_t *>(icon + 0x24);
        auto image = *reinterpret_cast<uint32_t *>(icon);
        if (plane < 0 || plane > 20 || image >= 0xffff)
            continue;
        auto pix = static_cast<const uint32_t *>(getPixBits(currentIcons, image));
        if (!pix)
            continue;
        // PixBits::AssignBits sets pointer +0, width +4, height +8, area +C.
        uint32_t w = pix[1], h = pix[2];
        if (!pix[0] || !w || !h || w > 512 || h > 512 || pix[3] != w * h)
            continue;
        auto rgba = reinterpret_cast<const unsigned char *>(pix[0]);
        // Pick visible artwork, skipping uniform lighting/render-target masks.
        unsigned low = 255, high = 0;
        for (uint32_t j = 0; j < w * h; ++j)
            if (rgba[j * 4 + 3]) {
                for (int channel = 0; channel < 3; ++channel) {
                    unsigned value = rgba[j * 4 + channel];
                    if (value < low)
                        low = value;
                    if (value > high)
                        high = value;
                }
            }
        if (high < low || high - low < 24)
            continue;
        auto bgra = static_cast<unsigned char *>(HeapAlloc(GetProcessHeap(), 0, w * h * 4));
        if (!bgra)
            return;
        for (uint32_t j = 0; j < w * h; ++j) {
            bgra[j * 4] = rgba[j * 4 + 2];
            bgra[j * 4 + 1] = rgba[j * 4 + 1];
            bgra[j * 4 + 2] = rgba[j * 4];
            bgra[j * 4 + 3] = rgba[j * 4 + 3];
        }
        HANDLE file = CreateFileW(texturePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh = {};
            fh.bfType = 0x4d42;
            fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
            fh.bfSize = fh.bfOffBits + w * h * 4;
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof(ih);
            ih.biWidth = w;
            ih.biHeight = -(LONG)h;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = w * h * 4;
            DWORD written;
            WriteFile(file, &fh, sizeof(fh), &written, nullptr);
            WriteFile(file, &ih, sizeof(ih), &written, nullptr);
            WriteFile(file, bgra, w * h * 4, &written, nullptr);
            CloseHandle(file);
            textureSaved = true;
            char line[160];
            sprintf_s(line, "native texture captured sprite=%u image=%u plane=%d size=%ux%u\r\n", i, image,
                      plane, w, h);
            log(line);
        }
        HeapFree(GetProcessHeap(), 0, bgra);
        return;
    }
}

static int __fastcall observeRender(void *driver, void *, void *list, uintptr_t options) {
    LONG n = InterlockedIncrement(&renderCalls);
    __try {
        if (list && getPixBits)
            captureTexture(list, options);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        textureSaved = true;
        log("texture capture failed; original render preserved\r\n");
    }
    if (n <= 3 || n % 300 == 0) {
        __try {
            char line[256];
            auto count = list ? *reinterpret_cast<uint16_t *>(static_cast<unsigned char *>(list) + 8) : 0;
            sprintf_s(line, "native render call=%ld thread=%lu driver=%p list=%p sprites=%u options=%08x\r\n",
                      n, GetCurrentThreadId(), driver, list, count, (unsigned)options);
            log(line);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log("render diagnostic unreadable\r\n");
        }
    }
    void *previousList = activeList;
    void *previousIcons = activeIcons;
    auto found = driverIcons.find(driver);
    void *icons = found == driverIcons.end() ? reinterpret_cast<void *>(options - 4) : found->second;
    activeList = list;
    activeIcons = options == reinterpret_cast<uintptr_t>(icons) + 4 ? icons : nullptr;
    if (list && activeIcons) {
        auto raw = static_cast<unsigned char *>(list);
        unsigned capacity = *reinterpret_cast<uint16_t *>(raw + 10);
        auto buffer = *reinterpret_cast<unsigned char **>(raw + 4);
        if (buffer && capacity) {
            auto &retained = retainedScenes[driver];
            retained.icons = activeIcons;
            retained.buffer.assign(buffer, buffer + capacity * ByondAbi::mapIconStride);
            memcpy(retained.header, raw, 12);
            *reinterpret_cast<unsigned char **>(retained.header + 4) = retained.buffer.data();
        }
    }
    int result = originalRender(driver, list, options);
    activeList = previousList;
    activeIcons = previousIcons;
    return result;
}

static int __fastcall observePresent(void *driver, void *, unsigned char flush) {
    EnsureFirstPersonInputHook();
    UpdateFirstPersonInput();
    const bool volumeEnabled = GetFirstPersonView().enabled;
    // Native slot 23 copies its composed surface into the backbuffer.
    // Flush first; doing it after our draw would overwrite the entire volume.
    using Flush = int(__thiscall *)(void *);
    if (flush) {
        auto vtable = *reinterpret_cast<uintptr_t **>(driver);
        if (reinterpret_cast<Flush>(vtable[23])(driver) != 1)
            return originalPresent(driver, 0);
    }
    __try {
        if (volumeEnabled && activeList && activeIcons)
            DrawNativeVolume(driver, activeList, activeIcons, getPixBits);
        else if (volumeEnabled) {
            auto found = retainedScenes.find(driver);
            if (found != retainedScenes.end())
                DrawNativeVolume(driver, found->second.header, found->second.icons, getPixBits);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log("volume renderer memory exception\r\n");
    }
    return originalPresent(driver, 0);
}

static void installRendererHooks(uintptr_t *vtable) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"byondwin.dll"));
    // Current hardware driver slot 21 starts with MapIconList processing
    // and returns with ret 8. Refuse a different driver implementation.
    if (!originalRender && vtable[21] == base + ByondAbi::renderScene) {
        originalRender = reinterpret_cast<RenderScene>(vtable[21]);
        DWORD old, ignored;
        if (VirtualProtect(vtable + 21, sizeof(void *), PAGE_READWRITE, &old)) {
            auto previous = InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(vtable + 21),
                                                              reinterpret_cast<void *>(&observeRender),
                                                              reinterpret_cast<void *>(originalRender));
            VirtualProtect(vtable + 21, sizeof(void *), old, &ignored);
            log(previous == reinterpret_cast<void *>(originalRender)
                    ? "native scene renderer slot 21 hook installed\r\n"
                    : "renderer slot race\r\n");
        } else
            log("renderer slot protection failed\r\n");
    }
    if (!originalPresent && vtable[24] == base + ByondAbi::presentScene) {
        originalPresent = reinterpret_cast<PresentScene>(vtable[24]);
        DWORD old, ignored;
        if (VirtualProtect(vtable + 24, sizeof(void *), PAGE_READWRITE, &old)) {
            auto previous = InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(vtable + 24),
                                                              reinterpret_cast<void *>(&observePresent),
                                                              reinterpret_cast<void *>(originalPresent));
            VirtualProtect(vtable + 24, sizeof(void *), old, &ignored);
            log(previous == reinterpret_cast<void *>(originalPresent)
                    ? "native Present slot 24 hook installed\r\n"
                    : "Present slot race\r\n");
        }
    }
}

static void *__fastcall observeDriver(void *icons, void *, void *window) {
    void *driver = originalDriver(icons, window);
    currentIcons = icons;
    if (driver)
        driverIcons[driver] = icons;
    if (InterlockedIncrement(&driverCalls) <= 3 && driver) {
        __try {
            auto vtable = *reinterpret_cast<uintptr_t **>(driver);
            auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"byondwin.dll"));
            char line[2048];
            int used = sprintf_s(line, "driver icons=%p window=%p driver=%p vtable=%p slots(rva)=", icons,
                                 window, driver, vtable);
            for (int i = 0; i < 40; ++i)
                used += sprintf_s(line + used, sizeof(line) - used, "%08x ", (unsigned)(vtable[i] - base));
            sprintf_s(line + used, sizeof(line) - used, "\r\n");
            log(line);
            installRendererHooks(vtable);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log("driver diagnostic unreadable\r\n");
        }
    }
    return driver;
}

static char __fastcall observe(void *self, void *, const char *control, void **result, unsigned char a,
                               unsigned char b) {
    SetFirstPersonClient(self);
    char status = original(self, control, result, a, b);
    LONG n = InterlockedIncrement(&calls);
    __try {
        if (result && *result)
            snapshot(*result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log("snapshot diagnostic unreadable\r\n");
    }
    if (n <= 3 || n % 300 == 0) {
        // Read only while the original caller still owns the returned scene.
        // SEH guards diagnostics, never the original call or its return value.
        __try {
            char line[2048];
            void *list = result ? *result : nullptr;
            int used =
                sprintf_s(line, "scene call=%ld thread=%lu self=%p control=%.80s status=%d list=%p words=", n,
                          GetCurrentThreadId(), self, control ? control : "<null>", (int)status, list);
            if (list) {
                auto words = static_cast<const uint32_t *>(list);
                for (int i = 0; i < 48; ++i)
                    used += sprintf_s(line + used, sizeof(line) - used, "%08x ", words[i]);
            }
            sprintf_s(line + used, sizeof(line) - used, "\r\n");
            log(line);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log("scene diagnostic unreadable; original call preserved\r\n");
        }
    }
    return status;
}

static DWORD WINAPI install(void *) {
    wchar_t path[MAX_PATH];
    GetEnvironmentVariableW(L"BS220_NATIVE_LOG", path, MAX_PATH);
    GetEnvironmentVariableW(L"BS220_NATIVE_SNAPSHOT", snapshotPath, MAX_PATH);
    GetEnvironmentVariableW(L"BS220_NATIVE_TEXTURE", texturePath, MAX_PATH);
    getPixBits = reinterpret_cast<GetPixBits>(
        GetProcAddress(GetModuleHandleW(L"byondwin.dll"), "?GetPixBits@IconList@@QAEPBUPixBits@@K@Z"));
    logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    log("BS220 native scene probe x86: starting\r\n");
    auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        log("wrong architecture\r\n");
        return 1;
    }
    auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    bool hooked = false;
    for (; imports->Name; ++imports) {
        auto dll = reinterpret_cast<char *>(base + imports->Name);
        if (_stricmp(dll, "byondcore.dll") && _stricmp(dll, "byondwin.dll"))
            continue;
        if (!imports->OriginalFirstThunk)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk);
        auto addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            auto name = reinterpret_cast<char *>(entry->Name);
            bool scene = !strcmp(name, symbol);
            bool driver =
                !strcmp(name, "?GetSpriteDriverOrDefault@IconList@@QAEPAUGraphicsDriver@@PAUGraphicsWnd@@@Z");
            bool hit = !strcmp(
                name, "?HitCid@GraphicsWnd@@QAEHVCPoint@@PAUMapIconList@@PAVIconList@@AAUMouseParams@@@Z");
            bool hitList =
                !strcmp(name, "?HitCidList@GraphicsWnd@@QAEHVCPoint@@PAUMapIconList@@PAUCid@@H_N@Z");
            if (!scene && !driver && !hit && !hitList)
                continue;
            auto expected = GetProcAddress(GetModuleHandleA(dll), name);
            if (reinterpret_cast<FARPROC>(addresses->u1.Function) != expected) {
                log("IAT already changed; refusing\r\n");
                return 2;
            }
            if (scene)
                original = reinterpret_cast<GetMapIcons>(expected);
            else if (driver)
                originalDriver = reinterpret_cast<SpriteDriver>(expected);
            else
                SetNativeHitFunctions(hit ? reinterpret_cast<void *>(expected) : nullptr,
                                      hitList ? reinterpret_cast<void *>(expected) : nullptr);
            DWORD old, ignored;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                log("IAT protect failed\r\n");
                return 3;
            }
            auto previous =
                InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(&addresses->u1.Function),
                                                  scene    ? reinterpret_cast<void *>(&observe)
                                                  : driver ? reinterpret_cast<void *>(&observeDriver)
                                                  : hit    ? reinterpret_cast<void *>(&FirstPersonHit)
                                                           : reinterpret_cast<void *>(&FirstPersonHitList),
                                                  reinterpret_cast<void *>(expected));
            VirtualProtect(&addresses->u1.Function, sizeof(void *), old, &ignored);
            log(previous == reinterpret_cast<void *>(expected)
                    ? (scene    ? "GetMapIcons IAT hook installed\r\n"
                       : driver ? "SpriteDriver IAT hook installed\r\n"
                       : hit    ? "First person HitCid hook installed\r\n"
                                : "First person HitCidList hook installed\r\n")
                    : "IAT race; refusing\r\n");
            if (scene && previous == reinterpret_cast<void *>(expected))
                hooked = true;
        }
    }
    installRendererHooks(reinterpret_cast<uintptr_t *>(
        reinterpret_cast<unsigned char *>(GetModuleHandleW(L"byondwin.dll")) + ByondAbi::rendererVtable));
    if (!hooked)
        log("GetMapIcons import not found\r\n");
    return hooked ? 0 : 4;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, install, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    // Keep the DLL loaded until process exit; no unsafe live unhook/unload.
    return TRUE;
}
