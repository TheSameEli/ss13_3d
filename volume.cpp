// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <deque>
#include <array>
#include "volume.h"
#include "byond_abi.h"
#include "lighting_shader.h"
#include "first_person.h"
#include "geometry.generated.h"
#include "metal.generated.h"
#include "space.generated.h"
#include "lava.generated.h"

namespace {
constexpr unsigned stride = ByondAbi::mapIconStride;
constexpr int planeRange = 107; // layers.dm: 57 - (-50)
static int planeOffset(int plane) {
    return int(std::floor((57.f - plane) / planeRange));
}
static int truePlane(int plane) {
    return plane + planeOffset(plane) * planeRange;
}
static int currentLevel = 0;
template <class T> T field(const void *p, unsigned offset) {
    return *reinterpret_cast<const T *>(static_cast<const unsigned char *>(p) + offset);
}
struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
    float lu = 0, lv = 0;
    float cx = 0, cy = 0, cz = 0;
    bool cameraVertex = false;
};
constexpr DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2;
struct Texture {
    IDirect3DTexture9 *gpu = nullptr;
    unsigned w = 0, h = 0, checked = ~0u;
    uint64_t hash = 0;
    unsigned char kind = 0, axis = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float textX = 0, textY = 0;
    std::vector<unsigned char> alpha, rgba, nativeArt;
    uint64_t seatHash = ~0ull;
    unsigned seatAxis = 0;
    float seatUV[4] = {0, 0, 1, 1};
};
// The E/W sofa art is a narrow longitudinal strip, not a front elevation.
// Cache an opaque fabric rectangle, preferring its longitudinal span; this
// excludes transparent margins and the armrest overlays on end sections.
static void seatFabric(Texture &t, unsigned axis) {
    if (t.seatHash == t.hash && t.seatAxis == axis)
        return;
    t.seatHash = t.hash;
    t.seatAxis = axis;
    t.seatUV[0] = t.seatUV[1] = 0;
    t.seatUV[2] = t.seatUV[3] = 1;
    if (t.alpha.size() != size_t(t.w) * t.h || !t.w || !t.h)
        return;
    double best = 0;
    std::vector<bool> solid(t.w);
    for (unsigned top = 0; top < t.h; ++top) {
        std::fill(solid.begin(), solid.end(), true);
        for (unsigned bottom = top; bottom < t.h; ++bottom) {
            for (unsigned x = 0; x < t.w; ++x)
                solid[x] = solid[x] && t.alpha[bottom * t.w + x] >= 240;
            unsigned start = 0;
            for (unsigned x = 0; x <= t.w; ++x) {
                if (x < t.w && solid[x])
                    continue;
                unsigned width = x - start, height = bottom - top + 1, length = axis >= 3 ? height : width;
                double score = double(width) * height * length * length;
                if (score > best) {
                    best = score;
                    t.seatUV[0] = (start + .5f) / t.w;
                    t.seatUV[1] = (top + .5f) / t.h;
                    t.seatUV[2] = (x - .5f) / t.w;
                    t.seatUV[3] = (bottom + .5f) / t.h;
                }
                start = x + 1;
            }
        }
    }
}
struct Sprite {
    const unsigned char *data;
    float x, y;
    int plane;
    unsigned order;
    DWORD color;
    int parent;
    unsigned char kind = 0, axis = 0;
    float feet = 0;
    Texture *appearance = nullptr;
    int level = 0;
    float doorX = 0, doorY = 0, doorW = 0, doorH = 0;
    Texture *spaceMask = nullptr;
    unsigned char seatJoins = 0;
    bool lying = false;
    float lyingTurn = 0;
    unsigned char panelEdge = 0;
    bool quarterTurn = false;
    bool emissive = false, windowDoor = false, lava = false, rock = false, doorOpen = false,
         doubleDoor = false;
    float doorOffset = 0, visualOffsetX = 0, visualOffsetY = 0;
};
struct PixelPoint {
    int16_t x, y;
};
using ScreenEdge = PixelPoint *(__thiscall *)(void *, PixelPoint *, const void *);
static ScreenEdge screenEdge;
static void *graphicsWindow;
struct Batch {
    Texture *texture;
    std::vector<Vertex> vertices;
    bool surface = false, unlit = false;
    int level = 0;
    Texture *spaceMask = nullptr;
    bool emissive = false, lava = false;
};
static IDirect3DDevice9 *cachedDevice;
static IDirect3DPixelShader9 *firstPersonLightingShader = nullptr;
struct DirectLight {
    IDirect3DTexture9 *gpu = nullptr;
    float uv[4] = {1, 1, 0, 0};
};
static DirectLight directLight(void *driver, const unsigned char *buffer, unsigned roots, unsigned width,
                               unsigned height, int level) {
    using Lookup = void *(__thiscall *)(void *, const char *);
    auto lookup = reinterpret_cast<Lookup>(
        reinterpret_cast<unsigned char *>(GetModuleHandleW(L"byondwin.dll")) + ByondAbi::lookupTexture);
    for (unsigned i = 0; i < roots; ++i) {
        auto icon = buffer + i * stride;
        if (field<int16_t>(icon, 0x24) != 22 - planeRange * level ||
            !(field<unsigned char>(icon, 0x28) & 0x80))
            continue;
        auto name = field<const char *>(icon, 0x38);
        if (!name)
            continue;
        auto plate = lookup(driver, name);
        if (!plate)
            continue;
        auto gpu = field<IDirect3DTexture9 *>(plate, 0x2c);
        if (!gpu)
            continue;
        D3DSURFACE_DESC desc;
        if (FAILED(gpu->GetLevelDesc(0, &desc)))
            continue;
        gpu->AddRef();
        return {gpu,
                {float(width) / desc.Width, float(height) / desc.Height,
                 -float(field<int>(plate, 4)) / desc.Width, -float(field<int>(plate, 8)) / desc.Height}};
    }
    return {};
}

static bool prepareFirstPersonLighting(IDirect3DDevice9 *device) {
    if (firstPersonLightingShader)
        return true;
    const char *source = lightingShaderSource;
    ID3DBlob *shader = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT result = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", "ps_2_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (SUCCEEDED(result))
        result = device->CreatePixelShader(static_cast<const DWORD *>(shader->GetBufferPointer()),
                                           &firstPersonLightingShader);
    if (shader)
        shader->Release();
    if (errors)
        errors->Release();
    return SUCCEEDED(result);
}
static std::unordered_map<uint64_t, Texture> textures;
struct Footprint {
    float x, y;
    unsigned char kind, axis;
    int level = 0;
    float w = 32, h = 32;
};
static std::vector<Footprint> footprints;
static std::vector<const Sprite *> seating;
static std::unordered_map<uint64_t, unsigned char> doorAxes;
struct Light {
    const Sprite *sprite;
    Texture *texture;
};
static std::vector<Light> lights;
static unsigned lightWidth, lightHeight;
static std::vector<std::array<float, 3>> lightMap;
static IDirect3DTexture9 *lightingTexture = nullptr;
static std::unordered_map<int, IDirect3DTexture9 *> levelLighting;
static std::unordered_map<int, IDirect3DTexture9 *> levelSurfaceLighting;
static bool uploadLighting(IDirect3DDevice9 *device) {
    D3DSURFACE_DESC desc = {};
    if (lightingTexture)
        lightingTexture->GetLevelDesc(0, &desc);
    if (desc.Width != lightWidth || desc.Height != lightHeight) {
        if (lightingTexture) {
            lightingTexture->Release();
            lightingTexture = nullptr;
        }
        if (FAILED(device->CreateTexture(lightWidth, lightHeight, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                         &lightingTexture, nullptr)))
            return false;
    }
    D3DLOCKED_RECT lock = {};
    if (FAILED(lightingTexture->LockRect(0, &lock, nullptr, 0)))
        return false;
    for (unsigned y = 0; y < lightHeight; ++y) {
        auto row = reinterpret_cast<DWORD *>(static_cast<unsigned char *>(lock.pBits) + y * lock.Pitch);
        for (unsigned x = 0; x < lightWidth; ++x) {
            auto rgb = lightMap[size_t(y) * lightWidth + x];
            row[x] = 0xff000000 | (DWORD(std::clamp(rgb[0], 0.f, 1.f) * 255) << 16) |
                     (DWORD(std::clamp(rgb[1], 0.f, 1.f) * 255) << 8) |
                     DWORD(std::clamp(rgb[2], 0.f, 1.f) * 255);
        }
    }
    lightingTexture->UnlockRect(0);
    return true;
}
static void buildLightMap(unsigned width, unsigned height, int level) {
    lightWidth = width;
    lightHeight = height;
    lightMap.assign(size_t(width) * height, {1, 1, 1});
    for (auto &light : lights) {
        auto &s = *light.sprite;
        auto &t = *light.texture;
        if (s.level != level)
            continue;
        // Airlock floor reflections are baked for the native flat view.
        // They must not become floating horizontal strips on rotated doors.
        if (s.plane == 11 && t.kind == 1)
            continue;
        unsigned firstX = unsigned(std::clamp(-int(s.x), 0, int(t.w))),
                 firstY = unsigned(std::clamp(-int(s.y), 0, int(t.h)));
        unsigned endX = unsigned(std::clamp(int(width) - int(s.x), 0, int(t.w))),
                 endY = unsigned(std::clamp(int(height) - int(s.y), 0, int(t.h)));
        for (unsigned y = firstY; y < endY; ++y)
            for (unsigned x = firstX; x < endX; ++x) {
                int px = int(s.x) + x, py = int(s.y) + y;
                if (px < 0 || py < 0 || px >= int(width) || py >= int(height))
                    continue;
                auto &rgb = lightMap[size_t(py) * width + px];
                auto source = t.rgba.data() + (y * t.w + x) * 4;
                float input[4] = {source[0] / 255.f, source[1] / 255.f, source[2] / 255.f, source[3] / 255.f};
                float value[3] = {input[0], input[1], input[2]};
                auto matrix = field<const float *>(s.data, 0xa4);
                if (matrix)
                    for (int c = 0; c < 3; ++c) {
                        value[c] = matrix[16 + c];
                        for (int j = 0; j < 4; ++j)
                            value[c] += input[j] * matrix[j * 4 + c];
                    }
                for (int c = 0; c < 3; ++c) {
                    float v = std::clamp(value[c] * ((s.color >> (c * 8)) & 255) / 255.f, 0.f, 1.f);
                    if (s.plane == 10)
                        rgb[c] = v;
                    else
                        rgb[c] = std::min(1.f, rgb[c] + v * input[3]);
                }
            }
    }
}
static bool nativeLightMap(void *driver, IDirect3DDevice9 *device, const unsigned char *buffer,
                           unsigned roots, unsigned width, unsigned height, int level) {
    using Lookup = void *(__thiscall *)(void *, const char *);
    auto lookup = reinterpret_cast<Lookup>(
        reinterpret_cast<unsigned char *>(GetModuleHandleW(L"byondwin.dll")) + ByondAbi::lookupTexture);
    const char *name = nullptr;
    for (unsigned i = 0; i < roots; ++i) {
        auto icon = buffer + i * stride;
        if (field<int16_t>(icon, 0x24) == 22 - planeRange * level &&
            (field<unsigned char>(icon, 0x28) & 0x80)) {
            name = field<const char *>(icon, 0x38);
            break;
        }
    }
    if (!name)
        return false;
    auto plate = lookup(driver, name);
    if (!plate)
        return false;
    auto gpu = field<IDirect3DTexture9 *>(plate, 0x2c);
    if (!gpu)
        return false;
    D3DSURFACE_DESC desc;
    if (FAILED(gpu->GetLevelDesc(0, &desc)))
        return false;
    IDirect3DSurface9 *source = nullptr, *copy = nullptr;
    if (FAILED(gpu->GetSurfaceLevel(0, &source)))
        return false;
    HRESULT result = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                         D3DPOOL_SYSTEMMEM, &copy, nullptr);
    if (SUCCEEDED(result))
        result = device->GetRenderTargetData(source, copy);
    D3DLOCKED_RECT lock = {};
    if (SUCCEEDED(result))
        result = copy->LockRect(&lock, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(result)) {
        lightWidth = width;
        lightHeight = height;
        lightMap.assign(size_t(width) * height, {1, 1, 1});
        int left = field<int>(plate, 4), top = field<int>(plate, 8);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                int sx = int(x) - left, sy = int(y) - top;
                if (sx < 0 || sy < 0 || sx >= int(desc.Width) || sy >= int(desc.Height))
                    continue;
                auto pixel = static_cast<const unsigned char *>(lock.pBits) + sy * lock.Pitch + sx * 4;
                float alpha = pixel[3] / 255.f;
                auto &rgb = lightMap[size_t(y) * width + x];
                // Render targets already contain premultiplied RGB. Applying alpha
                // again darkens partial light / emissive pixels a second time.
                for (int c = 0; c < 3; ++c)
                    rgb[c] = std::min(1.f, 1.f - alpha + pixel[2 - c] / 255.f);
            }
        copy->UnlockRect();
    }
    if (copy)
        copy->Release();
    source->Release();
    return SUCCEEDED(result);
}
static bool savedFrame;
static unsigned frames;
static HANDLE diagnostic = INVALID_HANDLE_VALUE;
static DWORD tinted(DWORD rgba, float shade) {
    unsigned r = unsigned((rgba & 255) * shade), g = unsigned(((rgba >> 8) & 255) * shade),
             b = unsigned(((rgba >> 16) & 255) * shade);
    return (rgba & 0xff000000) | (r << 16) | (g << 8) | b;
}
static DWORD multiply(DWORD a, DWORD b) {
    DWORD result = 0;
    for (unsigned s = 0; s < 32; s += 8)
        result |= (((a >> s) & 255) * ((b >> s) & 255) / 255) << s;
    return result;
}
static const GeometryHint *benchGeometry(const unsigned char *rgba, unsigned width, unsigned height) {
    if (width > 255 || height > 255)
        return nullptr;
    uint64_t hash = 14695981039346656037ull;
    hash = (hash ^ width) * 1099511628211ull;
    hash = (hash ^ height) * 1099511628211ull;
    for (unsigned i = 0; i < width * height; ++i)
        hash = (hash ^ rgba[i * 4 + 3]) * 1099511628211ull;
    auto hint = std::lower_bound(std::begin(benchShapeHints), std::end(benchShapeHints), hash,
                                 [](const GeometryHint &a, uint64_t b) { return a.hash < b; });
    return hint != std::end(benchShapeHints) && hint->hash == hash ? hint : nullptr;
}
static Texture *texture(IDirect3DDevice9 *device, void *icons, NativePixels pixels, unsigned image) {
    if (image >= 65535)
        return nullptr;
    auto pix = static_cast<const uint32_t *>(pixels(icons, image));
    if (!pix || !pix[0] || !pix[1] || !pix[2] || pix[1] > 1024 || pix[2] > 1024 || pix[3] != pix[1] * pix[2])
        return nullptr;
    auto &t = textures[(uint64_t(reinterpret_cast<uintptr_t>(icons)) << 32) | image];
    if (t.checked == frames)
        return t.gpu ? &t : nullptr;
    t.checked = frames;
    auto rgba = reinterpret_cast<const unsigned char *>(pix[0]);
    // PixBits usually repeats unchanged artwork. CRT compares whole words /
    // vectors; bytewise 64-bit FNV on x86 is much slower, especially for light
    // sheets. Keep the original fingerprint only for changed bitmap content.
    if (t.gpu && t.w == pix[1] && t.h == pix[2] && t.rgba.size() == size_t(pix[3]) * 4 &&
        memcmp(t.nativeArt.empty() ? t.rgba.data() : t.nativeArt.data(), rgba, t.rgba.size()) == 0)
        return &t;
    uint64_t hash = 14695981039346656037ull;
    for (unsigned j = 0; j < pix[3] * 4; ++j) {
        unsigned char byte = ((j & 3) != 3 && !rgba[(j & ~3u) + 3]) ? 0 : rgba[j];
        hash = (hash ^ byte) * 1099511628211ull;
    }
    if (t.gpu && t.hash == hash && t.w == pix[1] && t.h == pix[2])
        return &t;
    if (t.gpu)
        t.gpu->Release();
    t.gpu = nullptr;
    t.nativeArt.clear();
    t.w = pix[1];
    t.h = pix[2];
    t.hash = hash;
    t.kind = 0;
    t.axis = 0;
    auto hint = std::lower_bound(std::begin(geometryHints), std::end(geometryHints), hash,
                                 [](const GeometryHint &a, uint64_t b) { return a.hash < b; });
    if (hint != std::end(geometryHints) && hint->hash == hash) {
        t.kind = hint->kind;
        t.axis = hint->axis;
    } else if (auto bench = benchGeometry(rgba, t.w, t.h)) {
        t.kind = bench->kind;
        t.axis = bench->axis;
    }
    if (t.kind == 25 || t.kind == 45 || t.kind == 14 || t.kind == 36 || t.kind == 52) {
        auto front = std::lower_bound(std::begin(computerFrontHints), std::end(computerFrontHints), hash,
                                      [](const ComputerFrontHint &a, uint64_t b) { return a.hash < b; });
        if (front != std::end(computerFrontHints) && front->hash == hash) {
            t.nativeArt.assign(rgba, rgba + pix[3] * 4);
            rgba = computerFrontPixels + front->offset;
        } else
            t.nativeArt.clear();
    }
    if (FAILED(device->CreateTexture(t.w, t.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.gpu, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lock = {};
    if (FAILED(t.gpu->LockRect(0, &lock, nullptr, 0))) {
        t.gpu->Release();
        t.gpu = nullptr;
        return nullptr;
    }
    t.alpha.resize(t.w * t.h);
    t.rgba.assign(rgba, rgba + t.w * t.h * 4);
    for (unsigned y = 0; y < t.h; ++y)
        for (unsigned x = 0; x < t.w; ++x) {
            unsigned j = (y * t.w + x) * 4;
            auto out = reinterpret_cast<unsigned char *>(lock.pBits) + y * lock.Pitch + x * 4;
            out[0] = rgba[j + 2];
            out[1] = rgba[j + 1];
            out[2] = rgba[j];
            out[3] = t.kind == 37 && rgba[j + 3] > 8 ? 255 : rgba[j + 3];
            t.alpha[y * t.w + x] = rgba[j + 3];
        }
    t.gpu->UnlockRect(0);
    return &t;
}
static std::unordered_map<uint64_t, Texture> appearances;
static uint64_t mixHash(uint64_t hash, uint64_t value) {
    return (hash ^ value) * 1099511628211ull;
}
static void assembleAppearances(IDirect3DDevice9 *device, void *icons, NativePixels pixels,
                                std::vector<Sprite> &sprites) {
    // GetMapIcons may flatten an atom's overlays into siblings. Its native Cid,
    // rather than the scene-tree parent, identifies the complete appearance.
    std::unordered_map<uint64_t, std::vector<unsigned>> groups;
    for (unsigned i = 0; i < sprites.size(); ++i) {
        auto &s = sprites[i];
        if (s.plane != -4 && s.plane != -3 && s.plane != -2 && s.plane != 4 && s.plane != 5)
            continue;
        uint64_t cid = field<uint64_t>(s.data, 0xc);
        if (!cid)
            continue;
        // Unrotated status/speech icons can share the body's Cid and plane.
        // They must not enter the rotated body bitmap: an icon below the
        // rotated canvas would become its lowest pixel and lift the body.
        groups[cid ^ (uint64_t(uint16_t(s.plane - s.level * planeRange)) << 48) ^ (s.lying ? 1ull << 47 : 0)]
            .push_back(i);
    }
    for (auto &group : groups) {
        auto &entries = group.second;
        auto &base = sprites[entries.front()];
        uint64_t hash = 14695981039346656037ull;
        float left = base.x, top = base.y, right = base.x, bottom = base.y;
        unsigned valid = 0;
        for (auto index : entries) {
            auto &s = sprites[index];
            auto t = texture(device, icons, pixels, field<unsigned>(s.data, 0));
            if (!t)
                continue;
            ++valid;
            left = std::min(left, s.x);
            top = std::min(top, s.y);
            right = std::max(right, s.x + t->w);
            bottom = std::max(bottom, s.y + t->h);
            hash = mixHash(hash, t->hash);
            hash = mixHash(hash, s.color);
            hash = mixHash(hash, uint32_t(int(s.x - base.x)) | (uint64_t(uint32_t(int(s.y - base.y))) << 32));
            auto matrix = field<const float *>(s.data, 0xa4);
            if (matrix)
                for (unsigned j = 0; j < 20; ++j) {
                    uint32_t bits;
                    memcpy(&bits, matrix + j, 4);
                    hash = mixHash(hash, bits);
                }
        }
        if (!valid || right <= left || bottom <= top || right - left > 512 || bottom - top > 512)
            continue;
        auto &composite = appearances[group.first];
        unsigned width = unsigned(right - left), height = unsigned(bottom - top);
        if (!composite.gpu || composite.hash != hash || composite.w != width || composite.h != height) {
            composite.rgba.assign(size_t(width) * height * 4, 0);
            composite.alpha.resize(size_t(width) * height);
            for (auto index : entries) {
                auto &s = sprites[index];
                auto t = texture(device, icons, pixels, field<unsigned>(s.data, 0));
                if (!t)
                    continue;
                auto matrix = field<const float *>(s.data, 0xa4);
                for (unsigned y = 0; y < t->h; ++y)
                    for (unsigned x = 0; x < t->w; ++x) {
                        auto src = t->rgba.data() + (y * t->w + x) * 4;
                        float input[4] = {src[0] / 255.f, src[1] / 255.f, src[2] / 255.f, src[3] / 255.f},
                              color[4];
                        for (int c = 0; c < 4; ++c) {
                            color[c] = input[c];
                            if (matrix) {
                                color[c] = matrix[16 + c];
                                for (int j = 0; j < 4; ++j)
                                    color[c] += input[j] * matrix[j * 4 + c];
                            }
                            color[c] = std::clamp(color[c], 0.f, 1.f) * ((s.color >> (c * 8)) & 255) / 255.f;
                        }
                        unsigned px = unsigned(s.x - left) + x, py = unsigned(s.y - top) + y;
                        auto dst = composite.rgba.data() + (py * width + px) * 4;
                        float alpha = color[3], oldAlpha = dst[3] / 255.f,
                              newAlpha = alpha + oldAlpha * (1 - alpha);
                        if (newAlpha > 0)
                            for (int c = 0; c < 3; ++c)
                                dst[c] = static_cast<unsigned char>(
                                    std::clamp((color[c] * alpha + dst[c] / 255.f * oldAlpha * (1 - alpha)) /
                                                   newAlpha,
                                               0.f, 1.f) *
                                        255.f +
                                    .5f);
                        dst[3] = static_cast<unsigned char>(newAlpha * 255.f + .5f);
                    }
            }
            if (composite.gpu && (composite.w != width || composite.h != height)) {
                composite.gpu->Release();
                composite.gpu = nullptr;
            }
            composite.w = width;
            composite.h = height;
            composite.hash = hash;
            if (!composite.gpu)
                device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &composite.gpu,
                                      nullptr);
            bool curtain =
                std::any_of(entries.begin(), entries.end(), [&](size_t i) { return sprites[i].kind == 37; });
            D3DLOCKED_RECT lock = {};
            if (composite.gpu && SUCCEEDED(composite.gpu->LockRect(0, &lock, nullptr, 0))) {
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = 0; x < width; ++x) {
                        auto src = composite.rgba.data() + (y * width + x) * 4;
                        auto dst = static_cast<unsigned char *>(lock.pBits) + y * lock.Pitch + x * 4;
                        dst[0] = src[2];
                        dst[1] = src[1];
                        dst[2] = src[0];
                        dst[3] = curtain && src[3] > 8 ? 255 : src[3];
                        composite.alpha[y * width + x] = src[3];
                    }
                composite.gpu->UnlockRect(0);
            }
        }
        if (!composite.gpu)
            continue;
        // A native flattened group may start with a mask/underlay. Keep the
        // geometry of its actual door, window or furniture artwork.
        unsigned char kind = 0, axis = 0;
        auto rank = [](unsigned char k) {
            return k == 6 || k == 2 ? 100
                   : k == 1         ? 90
                   : k == 45        ? 85
                   : k == 15 || k == 18 || k == 21 || k == 22 || k == 23 || k == 25 || k == 27 || k == 28 ||
                           k == 29 || k == 31 || k == 37 || k == 40 || k == 41 || k == 42 || k == 43 ||
                           k == 44 || k == 46
                       ? 80
                   : k == 53                      ? 80
                   : k == 8 || k == 24 || k == 51 ? 75
                   : k == 11 || k == 3            ? 70
                                                  : 10;
        };
        for (auto index : entries)
            if (sprites[index].kind) {
                if (!kind || rank(sprites[index].kind) > rank(kind)) {
                    kind = sprites[index].kind;
                    axis = sprites[index].axis;
                } else if (sprites[index].kind == kind && !axis)
                    axis = sprites[index].axis;
            }
        if (kind) {
            base.kind = kind;
            base.axis = axis;
        }
        for (auto index : entries)
            if (sprites[index].panelEdge) {
                base.panelEdge = sprites[index].panelEdge;
                break;
            }
        for (auto index : entries) {
            base.emissive |= sprites[index].emissive;
            base.windowDoor |= sprites[index].windowDoor;
            base.doorOpen |= sprites[index].doorOpen;
            base.doubleDoor |= sprites[index].doubleDoor;
            base.doorOffset = std::max(base.doorOffset, sprites[index].doorOffset);
        }
        base.x = left;
        base.y = top;
        base.color = 0xffffffff;
        base.appearance = &composite;
        for (auto index : entries)
            if (sprites[index].lying) {
                base.lying = true;
                base.lyingTurn = sprites[index].lyingTurn;
                break;
            }
        for (unsigned j = 1; j < entries.size(); ++j)
            sprites[entries[j]].color = 0;
    }
}
static bool nativeText(void *driver, const Sprite &s, Texture &t) {
    // Maptext has its own HTML raster cache; +CC is only the ordinary icon.
    // This pinned ABI is the same cache lookup used by native DrawMapText.
    using GetText = int(__thiscall *)(void *, const char *, unsigned, unsigned, const void *, void *);
    static auto getText = reinterpret_cast<GetText>(
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"byondwin.dll")) + ByondAbi::getMapText);
    auto html = field<const char *>(s.data, 0x18);
    unsigned width = field<uint16_t>(s.data, 0x20), height = field<uint16_t>(s.data, 0x22);
    if (!html || !width || !height)
        return false;
    unsigned char draw[20] = {};
    if (getText(driver, html, width, height, s.data + 0xc, draw) != 1)
        return false;
    auto gpu = field<IDirect3DTexture9 *>(draw, 0);
    D3DSURFACE_DESC desc;
    if (!gpu || FAILED(gpu->GetLevelDesc(0, &desc)))
        return false;
    t.gpu = gpu;
    gpu->AddRef();
    t.w = desc.Width;
    t.h = desc.Height;
    t.textX = float(field<int16_t>(s.data, 0x1c) + field<int16_t>(draw, 4));
    t.textY = float(field<int16_t>(s.data, 0x1e) + field<int16_t>(draw, 6));
    return true;
}
static void walk(const unsigned char *buffer, unsigned count, unsigned capacity, unsigned index, float x,
                 float y, DWORD color, std::vector<Sprite> &out, std::vector<unsigned char> &seen,
                 unsigned depth, int parent = -1, bool lying = false, float lyingTurn = 0,
                 bool quarterTurn = false) {
    if (index >= capacity || depth > 32 || seen[index])
        return;
    seen[index] = 1;
    auto icon = buffer + index * stride;
    x += field<int16_t>(icon, 0x4c);
    y += field<int16_t>(icon, 0x4e);
    if (field<unsigned>(icon, 8) & 0x30) {
        PixelPoint edge = {};
        if (screenEdge && graphicsWindow)
            screenEdge(graphicsWindow, &edge, icon);
        x += edge.x;
        y += edge.y;
    }
    color = multiply(color, field<DWORD>(icon, 0x2c));
    // Some actors have a transform-only parent with no drawable bitmap.
    // Its pose still applies to every body/garment child.
    if (field<unsigned>(icon, 8) & 0x200) {
        float a = field<float>(icon, 0x8c), b = field<float>(icon, 0x90), d = field<float>(icon, 0x98),
              e = field<float>(icon, 0x9c);
        quarterTurn =
            std::isfinite(b) && std::isfinite(d) && std::abs(b) > std::abs(a) && std::abs(d) > std::abs(e);
        if (field<uint32_t>(icon, 0xc) == 1) {
            lying = quarterTurn;
            lyingTurn = std::atan2(d, a);
        }
    }
    int rawPlane = field<int16_t>(icon, 0x24), level = planeOffset(rawPlane), plane = truePlane(rawPlane);
    // HUD planes use native screen coordinates; world planes have tile coordinates.
    if (plane == -21 || plane == -20 || plane == -10 || (plane >= -6 && plane <= -2) ||
        (plane >= 4 && plane <= 6) || plane == 10 || plane == 11 || plane == 25 || plane == 34 ||
        plane == 35 || plane == 40 || plane == 41)
        if (!(field<unsigned char>(icon, 0x28) & 0x80) &&
            (field<unsigned>(icon, 0) < 65535 || field<void *>(icon, 0x18))) {
            out.push_back({icon, x, y, plane, unsigned(out.size()), color, parent});
            out.back().level = level;
            out.back().lying = lying;
            out.back().lyingTurn = lyingTurn;
            out.back().quarterTurn = quarterTurn;
            parent = int(out.size()) - 1;
        }
    unsigned first = field<uint16_t>(icon, 0xae), end = field<uint16_t>(icon, 0xb0);
    if (first > end || end > capacity)
        return;
    for (unsigned i = first; i < end; ++i)
        walk(buffer, count, capacity, i, x, y, color, out, seen, depth + 1, parent, lying, lyingTurn,
             quarterTurn);
}
struct Camera {
    float width, height;
    FirstPersonView view = GetFirstPersonView();
    float eyeX = 0, eyeY = 0, eyeZ = 24, clipDepth = 640.f;
    float focal() const {
        return width * .5f / std::tan(.829031394f);
    } // 95 degree horizontal FOV.
    void projectEye(Vertex &vertex) const {
        float inverse = 1.f / std::max(.001f, vertex.cz);
        vertex.x = width * .5f + vertex.cx * focal() * inverse - .5f;
        vertex.y = height * .5f - vertex.cy * focal() * inverse - .5f;
        vertex.z = 640.f / 638.75f * (1.f - 1.25f * inverse);
        vertex.rhw = inverse;
    }
    Vertex project(float x, float y, float z, float u, float v, DWORD color) const {
        if (view.enabled) {
            float dx = x - eyeX, dy = y - eyeY, dz = z - eyeZ;
            float sy = std::sin(view.yaw), cy = std::cos(view.yaw), sp = std::sin(view.pitch),
                  cp = std::cos(view.pitch);
            float forward = dx * sy - dy * cy;
            Vertex vertex = {0, 0, 0, 1, color, u, v};
            vertex.cx = dx * cy + dy * sy;
            vertex.cy = dz * cp - forward * sp;
            vertex.cz = forward * cp + dz * sp;
            vertex.cameraVertex = true;
            projectEye(vertex);
            return vertex;
        }
        float dy = y - height * .5f;
        // North is farther from the camera, screen Y is downward.
        // Post-projection depth must be affine in reciprocal distance; linear
        // distance lets pixel-sized side faces cut through the front artwork.
        float distance = 800.f - dy * .60f - z * .80f;
        float scale = 800.f / std::max(80.f, distance);
        return {width * .5f + (x - width * .5f) * scale - .5f,
                height * .52f + (dy * .80f - z * .60f) * scale - .5f,
                std::clamp(1.05f - 84.f / std::max(80.f, distance), 0.f, .99f),
                1.f / std::max(80.f, distance),
                color,
                u,
                v};
    }
};
static const Camera *activeCamera = nullptr;
static void pickTriangles(const Batch &batch, size_t first, const Sprite &s, const Camera &camera,
                          AimHit &hit, float &nearestDepth) {
    if (!camera.view.enabled || s.plane >= 34 || s.spaceMask)
        return;
    auto &t = *batch.texture;
    float px = camera.width * .5f - .5f, py = camera.height * .5f - .5f;
    for (size_t i = first; i + 2 < batch.vertices.size(); i += 3) {
        auto &a = batch.vertices[i];
        auto &b = batch.vertices[i + 1];
        auto &c = batch.vertices[i + 2];
        float determinant = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(determinant) < .00001f)
            continue;
        float wa = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / determinant;
        float wb = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / determinant, wc = 1 - wa - wb;
        if (wa < 0 || wb < 0 || wc < 0)
            continue;
        float inverse = wa * a.rhw + wb * b.rhw + wc * c.rhw;
        if (inverse <= 0)
            continue;
        float distance = 1 / inverse;
        if (distance >= nearestDepth)
            continue;
        float u = (wa * a.u * a.rhw + wb * b.u * b.rhw + wc * c.u * c.rhw) / inverse;
        float v = (wa * a.v * a.rhw + wb * b.v * b.rhw + wc * c.v * c.rhw) / inverse;
        u = (u - t.u0) / (t.u1 - t.u0);
        v = (v - t.v0) / (t.v1 - t.v0);
        unsigned x = unsigned(std::clamp(int(u * t.w), 0, int(t.w) - 1)),
                 y = unsigned(std::clamp(int(v * t.h), 0, int(t.h) - 1));
        if (!t.alpha.empty() && t.alpha[size_t(y) * t.w + x] <= 8)
            continue;
        nearestDepth = distance;
        hit.cid = field<uint64_t>(s.data, 0xc);
        hit.x = s.x - s.visualOffsetX + x + .5f;
        hit.y = s.y - s.visualOffsetY + y + .5f;
        memcpy(hit.icon, s.data, 216);
        if (field<unsigned>(s.data, 8) & 2)
            hit.cid = 0; // Native mouse_opacity=0 still occludes if its artwork is solid.
    }
}
static void quad(Batch &batch, const Vertex &a, const Vertex &b, const Vertex &c, const Vertex &d) {
    auto map = [&](Vertex vertex) {
        auto &t = *batch.texture;
        vertex.u = t.u0 + vertex.u * (t.u1 - t.u0);
        vertex.v = t.v0 + vertex.v * (t.v1 - t.v0);
        return vertex;
    };
    if (a.cameraVertex && activeCamera) {
        // Clip in camera space before dividing by depth. Geometry crossing the
        // player's eye must not explode into full-screen triangles.
        float tx = activeCamera->width * .5f / activeCamera->focal(),
              ty = activeCamera->height * .5f / activeCamera->focal();
        auto distance = [&](const Vertex &v, int plane) {
            switch (plane) {
            case 0:
                return v.cz - 1.25f;
            case 1:
                return activeCamera->clipDepth - v.cz;
            case 2:
                return v.cz * tx + v.cx;
            case 3:
                return v.cz * tx - v.cx;
            case 4:
                return v.cz * ty + v.cy;
            default:
                return v.cz * ty - v.cy;
            }
        };
        unsigned insideOr = 0, outsideAnd = 63;
        for (auto vertex : {a, b, c, d}) {
            unsigned code = 0;
            for (int plane = 0; plane < 6; ++plane)
                if (distance(vertex, plane) < 0)
                    code |= 1u << plane;
            insideOr |= code;
            outsideAnd &= code;
        }
        if (outsideAnd)
            return;
        if (!insideOr) {
            batch.vertices.insert(batch.vertices.end(), {map(a), map(b), map(c), map(a), map(c), map(d)});
            return;
        }
        std::vector<Vertex> polygon = {a, b, c, d}, next;
        auto mix = [](const Vertex &x, const Vertex &y, float t) {
            Vertex v = x;
            auto lerp = [&](float a0, float a1) { return a0 + (a1 - a0) * t; };
            v.cx = lerp(x.cx, y.cx);
            v.cy = lerp(x.cy, y.cy);
            v.cz = lerp(x.cz, y.cz);
            v.u = lerp(x.u, y.u);
            v.v = lerp(x.v, y.v);
            v.lu = lerp(x.lu, y.lu);
            v.lv = lerp(x.lv, y.lv);
            v.color = 0;
            for (unsigned shift = 0; shift < 32; shift += 8)
                v.color |= DWORD(lerp(float((x.color >> shift) & 255), float((y.color >> shift) & 255)))
                           << shift;
            return v;
        };
        for (int plane = 0; plane < 6 && !polygon.empty(); ++plane) {
            next.clear();
            Vertex previous = polygon.back();
            float pd = distance(previous, plane);
            for (auto v : polygon) {
                float vd = distance(v, plane);
                if ((pd >= 0) != (vd >= 0))
                    next.push_back(mix(previous, v, pd / (pd - vd)));
                if (vd >= 0)
                    next.push_back(v);
                previous = v;
                pd = vd;
            }
            polygon.swap(next);
        }
        for (auto &v : polygon)
            activeCamera->projectEye(v);
        for (size_t i = 1; i + 1 < polygon.size(); ++i)
            batch.vertices.insert(batch.vertices.end(),
                                  {map(polygon[0]), map(polygon[i]), map(polygon[i + 1])});
        return;
    }
    batch.vertices.insert(batch.vertices.end(), {map(a), map(b), map(c), map(a), map(c), map(d)});
}
static void sprite(Batch &batch, const Sprite &s, const Camera &camera) {
    auto &t = *batch.texture;
    float w = float(t.w), h = float(t.h);
    if (s.rock && s.plane == -5)
        seatFabric(t,
                   0); // Opaque rock core: transparent smoothing margins must not become holes at the base.
    if (s.plane == -20) {
        // Parallax is a screen background, not a tile-sized world surface.
        // Projecting its large repeating sheets crosses the camera near plane.
        DWORD color = tinted(s.kind == 37 ? s.color | 0xff000000 : s.color, 1.f);
        quad(batch, {s.x - .5f, s.y - .5f, .999f, 1, color, 0, 0},
             {s.x + w - .5f, s.y - .5f, .999f, 1, color, 1, 0},
             {s.x + w - .5f, s.y + h - .5f, .999f, 1, color, 1, 1},
             {s.x - .5f, s.y + h - .5f, .999f, 1, color, 0, 1});
        return;
    }
    if (s.plane == 40 || s.plane == 41) {
        // Screen sprites remain sharp and outside the 3D camera.
        float top = s.y + t.textY;
        float textLeft = s.x + t.textX;
        DWORD color = tinted(s.kind == 37 ? s.color | 0xff000000 : s.color, 1.f);
        quad(batch, {textLeft - .5f, top - .5f, 0, 1, color, 0, 0},
             {textLeft + w - .5f, top - .5f, 0, 1, color, 1, 0},
             {textLeft + w - .5f, top + h - .5f, 0, 1, color, 1, 1},
             {textLeft - .5f, top + h - .5f, 0, 1, color, 0, 1});
        return;
    }
    auto p = [&](float x, float y, float z, float u, float v, float shade = 1.f) {
        DWORD color = tinted(s.kind == 37 ? s.color | 0xff000000 : s.color, shade);
        if (s.kind == 6 || s.kind == 2 || s.kind == 7)
            color = (color & 0x00ffffff) | (DWORD((color >> 24) * (s.kind == 7   ? .38f
                                                                   : s.kind == 6 ? .18f
                                                                                 : .32f))
                                            << 24);
        float tu = u, tv = v;
        if (s.rock && s.plane == -5) {
            tu = t.seatUV[0] + u * (t.seatUV[2] - t.seatUV[0]);
            tv = t.seatUV[1] + v * (t.seatUV[3] - t.seatUV[1]);
        }
        auto vertex = camera.project(x, y, z + (currentLevel - s.level) * (camera.view.enabled ? 48.f : 32.f),
                                     tu, tv, color);
        // Carry original appearance coordinates through perspective. Sampling
        // only quad corners erased lights inside tiles and made dark wedges.
        vertex.lu = (s.x + u * w) / lightWidth;
        vertex.lv = (s.y + v * h) / lightHeight;
        if (camera.view.enabled) {
            vertex.lu = x / lightWidth;
            vertex.lv = y / lightHeight;
        }
        if (s.plane == -5 || s.kind == 6 || s.kind == 7) {
            // Side faces sample the light at their physical position. Reusing
            // the front artwork UV repeated emissive highlights on every side.
            vertex.lu = x / lightWidth;
            vertex.lv = y / lightHeight;
            if (camera.view.enabled) {
                float dx = camera.eyeX - x, dy = camera.eyeY - y, length = std::sqrt(dx * dx + dy * dy);
                if (length > 1) {
                    vertex.lu = (x + dx / length * 8) / lightWidth;
                    vertex.lv = (y + dy / length * 8) / lightHeight;
                }
            }
        }
        if (s.spaceMask) {
            vertex.lu = s.spaceMask->u0 + vertex.lu * (s.spaceMask->u1 - s.spaceMask->u0);
            vertex.lv = s.spaceMask->v0 + vertex.lv * (s.spaceMask->v1 - s.spaceMask->v0);
        }
        return vertex;
    };
    if (s.plane == 34 || s.plane == 35) {
        // Project the character's tile anchor, keeping native pixel_z and
        // maptext offsets upright, with the original animation and alpha.
        auto anchor =
            camera.project(s.x + 16, s.y + 32, (currentLevel - s.level) * (camera.view.enabled ? 48.f : 32.f),
                           0, 0, tinted(s.color, 1.f));
        if (camera.view.enabled && anchor.cz < 1.25f)
            return;
        float scale = anchor.rhw * (camera.view.enabled ? camera.focal() : 800.f) * .5f;
        float left = anchor.x + (t.textX - 16) * scale, top = anchor.y + (t.textY - 32) * scale;
        DWORD color = tinted(s.kind == 37 ? s.color | 0xff000000 : s.color, 1.f);
        quad(batch, {left, top, 0, 1, color, 0, 0}, {left + w * scale, top, 0, 1, color, 1, 0},
             {left + w * scale, top + h * scale, 0, 1, color, 1, 1},
             {left, top + h * scale, 0, 1, color, 0, 1});
        return;
    }
    if (s.plane == -6 || s.plane == -21 || s.plane == -20 || s.plane == -10 || s.kind == 17) {
        float height = s.lava && camera.view.enabled ? -3.f : s.kind == 17 ? 1.f : 0.f;
        quad(batch, p(s.x, s.y, height, 0, 0), p(s.x + w, s.y, height, 1, 0),
             p(s.x + w, s.y + h, height, 1, 1), p(s.x, s.y + h, height, 0, 1));
        return;
    }
    unsigned char kind = s.kind, axis = s.axis;
    if (kind == 44) {
        quad(batch, p(s.x, s.y, .25f, 0, 0), p(s.x + w, s.y, .25f, 1, 0), p(s.x + w, s.y + h, .25f, 1, 1),
             p(s.x, s.y + h, .25f, 0, 1));
        return;
    }
    if (kind == 41 || kind == 43) {
        unsigned bottom = 0;
        for (unsigned y = 0; y < t.h; ++y)
            for (unsigned x = 0; x < t.w; ++x)
                if (t.alpha[y * t.w + x] > 8)
                    bottom = std::max(bottom, y + 1);
        float scale = kind == 41 ? 48.f / h : 1.5f, cx = s.x + w * .5f, cy = s.y + h * .5f;
        float hi = kind == 41 ? 48.f : bottom * scale, lo = hi - h * scale, half = w * .5f * scale;
        quad(batch, p(cx - half, cy, hi, 0, 0), p(cx + half, cy, hi, 1, 0), p(cx + half, cy, lo, 1, 1),
             p(cx - half, cy, lo, 0, 1));
        return;
    }
    if (kind == 37) {
        float y = s.y + h * .5f, height = camera.view.enabled ? 48.f : 32.f;
        unsigned top = t.h, bottom = 0;
        for (unsigned yy = 0; yy < t.h; ++yy)
            for (unsigned xx = 0; xx < t.w; ++xx)
                if (t.alpha[yy * t.w + xx] > 8) {
                    top = std::min(top, yy);
                    bottom = std::max(bottom, yy + 1);
                }
        if (bottom <= top)
            return;
        float v0 = (top + .5f) / h, v1 = (bottom - .5f) / h;
        if (s.quarterTurn) {
            float x = s.x + w * .5f;
            quad(batch, p(x, s.y, height, 0, v0), p(x, s.y + h, height, 1, v0), p(x, s.y + h, 0, 1, v1),
                 p(x, s.y, 0, 0, v1));
        } else
            quad(batch, p(s.x, y, height, 0, v0), p(s.x + w, y, height, 1, v0), p(s.x + w, y, 0, 1, v1),
                 p(s.x, y, 0, 0, v1));
        return;
    }
    if (s.lying && field<uint32_t>(s.data, 0xc) == 1) {
        float height = 2.f;
        for (auto &support : footprints)
            if (support.level == s.level && support.kind >= 27 && support.kind <= 29 &&
                s.x + w * .5f >= support.x && s.x + w * .5f < support.x + support.w &&
                s.y + h * .5f >= support.y && s.y + h * .5f < support.y + support.h)
                height = std::max(height, support.kind == 29 ? 20.f : support.kind == 28 ? 14.f : 10.f);
        float cx = s.x + w * .5f, cy = s.y + h * .5f, co = std::cos(s.lyingTurn), si = std::sin(s.lyingTurn);
        // Lay the sideways body close to its support, tilting the far edge
        // 24 degrees toward the viewer. Center only the opaque silhouette,
        // so transparent canvas offsets cannot lift or displace the body.
        float lowest = 1e30f, highest = -1e30f;
        if (t.alpha.size() == size_t(t.w) * t.h)
            for (unsigned y = 0; y < t.h; ++y)
                for (unsigned x = 0; x < t.w; ++x)
                    if (t.alpha[y * t.w + x] > 8)
                        for (int corner = 0; corner < 4; ++corner) {
                            float dx = x + float(corner & 1) - w * .5f, dy = y + float(corner >> 1) - h * .5f,
                                  vertical = -si * dx - co * dy;
                            lowest = std::min(lowest, vertical);
                            highest = std::max(highest, vertical);
                        }
        if (lowest == 1e30f) {
            lowest = -w * .5f;
            highest = w * .5f;
        }
        float rx = std::cos(camera.view.yaw), ry = std::sin(camera.view.yaw);
        constexpr float tiltSin = .406736643f, tiltCos = .913545458f;
        auto prone = [&](float u, float v) {
            float dx = (u - .5f) * w, dy = (v - .5f) * h, lateral = co * dx - si * dy,
                  vertical = -si * dx - co * dy;
            float depth = (vertical - (lowest + highest) * .5f) * tiltCos;
            return p(cx + rx * lateral + ry * depth, cy + ry * lateral - rx * depth,
                     height + (vertical - lowest) * tiltSin, u, v);
        };
        quad(batch, prone(0, 0), prone(1, 0), prone(1, 1), prone(0, 1));
        return;
    }
    if (kind == 27 || kind == 28 || kind == 29) {
        // Beds and pods are horizontal furniture. The native directional
        // bitmap supplies the footprint, including multi-tile sleepers.
        float height = kind == 29 ? 18.f : kind == 28 ? 12.f : 8.f;
        float materialU = .5f, materialV = .5f;
        int best = 1000;
        for (unsigned y = 0; y < t.h; ++y)
            for (unsigned x = 0; x < t.w; ++x) {
                if (t.rgba.size() != size_t(t.w) * t.h * 4)
                    continue;
                auto rgba = &t.rgba[(y * t.w + x) * 4];
                if (rgba[3] < 240)
                    continue;
                int high = std::max({int(rgba[0]), int(rgba[1]), int(rgba[2])});
                int low = std::min({int(rgba[0]), int(rgba[1]), int(rgba[2])});
                int score = (high - low) * 5 + std::abs(high - 80);
                if (score < best) {
                    best = score;
                    materialU = (x + .5f) / w;
                    materialV = (y + .5f) / h;
                }
            }
        auto casing = [&](float x, float y, float z) { return p(x, y, z, materialU, materialV, .75f); };
        auto span = [&](int row) {
            int left = int(t.w), right = 0;
            if (row >= 0 && row < int(t.h) && t.alpha.size() == size_t(t.w) * t.h)
                for (unsigned x = 0; x < t.w; ++x)
                    if (t.alpha[size_t(row) * t.w + x] > 100) {
                        left = std::min(left, int(x));
                        right = int(x) + 1;
                    }
            return std::pair<int, int>{left, right};
        };
        for (int row = 0; row < int(t.h);) {
            auto edge = span(row);
            if (edge.second <= edge.first) {
                ++row;
                continue;
            }
            int end = row + 1;
            while (end < int(t.h) && span(end) == edge)
                ++end;
            float left = s.x + edge.first, right = s.x + edge.second, frontY = s.y + row, backY = s.y + end;
            quad(batch, casing(left, frontY, height), casing(left, backY, height), casing(left, backY, 0),
                 casing(left, frontY, 0));
            quad(batch, casing(right, backY, height), casing(right, frontY, height), casing(right, frontY, 0),
                 casing(right, backY, 0));
            // Close only the newly exposed boundary at a profile change.
            for (int side = 0; side < 2; ++side) {
                auto adjacent = span(side ? end : row - 1);
                float y = side ? backY : frontY;
                auto close = [&](int l, int r) {
                    if (r <= l)
                        return;
                    quad(batch, casing(s.x + l, y, height), casing(s.x + r, y, height), casing(s.x + r, y, 0),
                         casing(s.x + l, y, 0));
                };
                if (adjacent.second <= adjacent.first)
                    close(edge.first, edge.second);
                else {
                    close(edge.first, std::min(edge.second, adjacent.first));
                    close(std::max(edge.first, adjacent.second), edge.second);
                }
            }
            row = end;
        }
        quad(batch, p(s.x, s.y, height, 0, 0), p(s.x + w, s.y, height, 1, 0),
             p(s.x + w, s.y + h, height, 1, 1), p(s.x, s.y + h, height, 0, 1));
        return;
    }
    float elevation = 0;
    bool loose = kind == 0 && field<uint32_t>(s.data, 0xc) == 2;
    if (kind == 15 || kind == 53 || loose)
        for (auto &support : footprints)
            if (support.level == s.level && (support.kind == 3 || support.kind == 11) &&
                s.x + w * .5f >= support.x && s.x + w * .5f < support.x + support.w &&
                s.y + h * .5f >= support.y && s.y + h * .5f < support.y + support.h) {
                elevation = 12.25f;
                break;
            }
    if (kind == 12 || kind == 14 || kind == 16 || kind == 42) {
        // Mount the visible artwork on the nearest real wall face. The empty
        // margins in a 32px camera icon are not a 32px floating object.
        int l = int(t.w), r = 0, top = int(t.h), bottom = 0;
        for (unsigned y = 0; y < t.h; ++y)
            for (unsigned x = 0; x < t.w; ++x)
                if (t.alpha[y * t.w + x] > 8) {
                    l = std::min(l, int(x));
                    r = std::max(r, int(x) + 1);
                    top = std::min(top, int(y));
                    bottom = std::max(bottom, int(y) + 1);
                }
        float cx = s.x + (l + r) * .5f, cy = s.y + (top + bottom) * .5f, best = 48.f * 48.f, mx = 0, my = 0;
        int face = -1;
        if (!t.nativeArt.empty()) {
            unsigned sourceLeft = t.w, sourceRight = 0, sourceTop = t.h, sourceBottom = 0;
            for (unsigned y = 0; y < t.h; ++y)
                for (unsigned x = 0; x < t.w; ++x)
                    if (t.nativeArt[(y * t.w + x) * 4 + 3] > 8) {
                        sourceLeft = std::min(sourceLeft, x);
                        sourceRight = std::max(sourceRight, x + 1);
                        sourceTop = std::min(sourceTop, y);
                        sourceBottom = std::max(sourceBottom, y + 1);
                    }
            cx = s.x + (sourceLeft + sourceRight) * .5f;
            cy = s.y + (sourceTop + sourceBottom) * .5f;
        }
        float offsetX = s.doorW ? s.x - s.doorX : 0, offsetY = s.doorW ? s.y - s.doorY : 0;
        int preferred = -1;
        if (std::max(std::abs(offsetX), std::abs(offsetY)) >= 16) {
            if (std::abs(offsetX) > std::abs(offsetY))
                preferred = offsetX > 0 ? 2 : 3;
            else
                preferred = offsetY > 0 ? 0 : 1;
        }
        for (auto &wall : footprints)
            if (wall.kind == 5 && wall.level == s.level)
                for (int side = 0; side < 4; ++side) {
                    if (preferred >= 0 && side != preferred)
                        continue;
                    float x = std::clamp(cx, wall.x, wall.x + wall.w),
                          y = std::clamp(cy, wall.y, wall.y + wall.h);
                    if (side == 0)
                        y = wall.y - .25f;
                    if (side == 1)
                        y = wall.y + wall.h + .25f;
                    if (side == 2)
                        x = wall.x - .25f;
                    if (side == 3)
                        x = wall.x + wall.w + .25f;
                    float distance = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                    if (distance < best) {
                        best = distance;
                        mx = x;
                        my = y;
                        face = side;
                    }
                }
        if (face >= 0 && r > l && bottom > top) {
            float hi = camera.view.enabled
                           ? (kind == 12 ? 47.f : (kind == 16 ? 6.f : (kind == 42 ? 38.f : 27.f)))
                           : 27.f;
            if (s.doorW && kind == 14) {
                float residual = preferred >= 2 ? -offsetY : (std::abs(offsetY) - 28.f);
                if (preferred < 0)
                    residual = -offsetY;
                hi = std::clamp(hi + residual, 20.f, 40.f);
            }
            if (kind == 16 && camera.view.enabled)
                hi = std::max(hi, float(bottom - top) + 2.f);
            float half = (r - l) * .5f, lo = hi - (bottom - top);
            if (kind == 14 && !camera.view.enabled) {
                // Readable front artwork, attached to the chosen wall. Keep
                // normal depth testing so the opposite wall still hides it.
                half = (r - l) * .5f;
                lo = hi - (bottom - top);
                quad(batch, p(mx - half, my + .5f, hi, l / w, top / h),
                     p(mx + half, my + .5f, hi, r / w, top / h),
                     p(mx + half, my + .5f, lo, r / w, bottom / h),
                     p(mx - half, my + .5f, lo, l / w, bottom / h));
            } else if (face < 2)
                quad(batch, p(mx - half, my, hi, l / w, top / h), p(mx + half, my, hi, r / w, top / h),
                     p(mx + half, my, lo, r / w, bottom / h), p(mx - half, my, lo, l / w, bottom / h));
            else
                quad(batch, p(mx, my - half, hi, l / w, top / h), p(mx, my + half, hi, r / w, top / h),
                     p(mx, my + half, lo, r / w, bottom / h), p(mx, my - half, lo, l / w, bottom / h));
            return;
        }
    }
    bool grillePanel = kind == 7 && (axis == 3 || axis == 4);
    if (grillePanel) {
        kind = 2;
        axis -= 2;
    }
    if (kind == 1 || kind == 2) {
        if (s.doubleDoor && s.doorW) {
            auto point = [&](float u, float v) {
                float x = s.doorX + u * 64, y = s.doorY + 16;
                if (axis == 2) {
                    x = s.doorX + 16;
                    y = s.doorY + 32 - u * 64;
                }
                return p(x, y, (camera.view.enabled ? 48.f : 32.f) * (1 - v), u, .5f + v * .5f);
            };
            quad(batch, point(0, 0), point(1, 0), point(1, 1), point(0, 1));
            return;
        }
        if ((kind == 2 || s.windowDoor) && s.panelEdge) {
            int left = int(t.w), right = 0, top = int(t.h), bottom = 0;
            for (unsigned y = 0; y < t.h; ++y)
                for (unsigned x = 0; x < t.w; ++x)
                    if (t.alpha[y * t.w + x] > 8) {
                        left = std::min(left, int(x));
                        right = std::max(right, int(x) + 1);
                        top = std::min(top, int(y));
                        bottom = std::max(bottom, int(y) + 1);
                    }
            if (right > left && bottom > top) {
                if (s.windowDoor && s.doorOpen) {
                    // Open artwork is a floor guide plus a short upright jamb.
                    // Its one-pixel guide must not expand into a full door leaf.
                    bool horizontal = s.panelEdge <= 2;
                    unsigned length = horizontal ? t.w : t.h, cross = horizontal ? t.h : t.w;
                    auto thickness = [&](unsigned along) {
                        unsigned count = 0;
                        for (unsigned c = 0; c < cross; ++c) {
                            unsigned x = horizontal ? along : c, y = horizontal ? c : along;
                            if (t.alpha[y * t.w + x] > 8)
                                ++count;
                        }
                        return count;
                    };
                    unsigned guide = cross;
                    for (unsigned a = 0; a < length; ++a) {
                        unsigned n = thickness(a);
                        if (n)
                            guide = std::min(guide, n);
                    }
                    for (unsigned start = 0; start < length;) {
                        unsigned count = thickness(start), end = start + 1;
                        bool jamb = count > guide;
                        while (end < length && (thickness(end) > guide) == jamb &&
                               (thickness(end) > 0) == (count > 0))
                            ++end;
                        if (count) {
                            float u0 = float(start) / length, u1 = float(end) / length;
                            auto point = [&](float u, float v) {
                                float tu = horizontal ? u : (left + .5f + v * (right - left - 1)) / w,
                                      tv = horizontal ? (top + .5f + v * (bottom - top - 1)) / h : u;
                                float x = s.x + u * w, y = s.y + (s.panelEdge == 1 ? h : 0);
                                if (!horizontal) {
                                    x = s.x + (s.panelEdge == 3 ? w : 0);
                                    y = s.y + u * h;
                                }
                                if (!jamb) {
                                    if (horizontal)
                                        y -= v * 2;
                                    else
                                        x -= v * 2;
                                }
                                return p(x, y, jamb ? (camera.view.enabled ? 48.f : 26.f) * (1 - v) : .5f, tu,
                                         tv);
                            };
                            quad(batch, point(u0, 0), point(u1, 0), point(u1, 1), point(u0, 1));
                        }
                        start = end;
                    }
                    return;
                }
                if (s.windowDoor) {
                    if (s.panelEdge <= 2) {
                        left = 0;
                        right = int(w);
                    } else {
                        top = 0;
                        bottom = int(h);
                    }
                }
                auto pane = [&](float u, float v) {
                    float tu = s.panelEdge <= 2 ? u : v, tv = s.panelEdge <= 2 ? v : u;
                    tu = (left + .5f + tu * (right - left - 1)) / w;
                    tv = (top + .5f + tv * (bottom - top - 1)) / h;
                    float px = s.x + u * w, py = s.y + (s.panelEdge == 1 ? h : 0);
                    if (s.panelEdge >= 3) {
                        px = s.x + (s.panelEdge == 3 ? w : 0);
                        py = s.y + u * h;
                    }
                    return p(px, py, (camera.view.enabled ? 48.f : 26.f) * (1 - v), tu, tv);
                };
                quad(batch, pane(0, 0), pane(1, 0), pane(1, 1), pane(0, 1));
                return;
            }
        }
        if (!axis) {
            float alongX = 0, alongY = 0;
            for (auto &f : footprints)
                if (f.kind == 5 || f.kind == 2 || f.kind == 1) {
                    float dx = std::abs(f.x - s.x), dy = std::abs(f.y - s.y);
                    if (dy < 1 && dx > 1 && dx <= 64)
                        alongX += 32 / dx;
                    if (dx < 1 && dy > 1 && dy <= 64)
                        alongY += 32 / dy;
                }
            axis = alongY > alongX ? 2 : 1;
        }
        float z = grillePanel ? 23.5f : (kind == 1 ? (camera.view.enabled ? 48.f : 32.f) : 26.f),
              thick = kind == 1 ? 3.f : 1.f;
        if (kind == 1 && s.doorW > 0 && s.doorH > 0) {
            auto face = [&](float u, float v, float inset = 0, float shade = 1.f) {
                float px = s.x + u * w, py = s.y + v * h;
                if (s.doorOpen && !s.windowDoor) {
                    auto narrow = [](float a) {
                        return a < 8 ? a * .25f : a > 24 ? 32 - (32 - a) * .25f : 2 + (a - 8) * 1.75f;
                    };
                    px = s.doorX + narrow(px - s.doorX);
                    py = s.doorY + narrow(py - s.doorY);
                }
                float height = (camera.view.enabled ? 48.f : 32.f) * (1 - (py - s.doorY) / s.doorH);
                if (axis == 2)
                    return p(s.doorX + s.doorH * .5f + s.doorOffset - inset, s.doorY + (px - s.doorX), height,
                             u, v, shade);
                return p(px, s.doorY + s.doorH * .5f + s.doorOffset - inset, height, u, v, shade);
            };
            if (s.doorOpen && !s.windowDoor) {
                for (unsigned y = 0; y < t.h; ++y)
                    for (unsigned x = 0; x < t.w; ++x)
                        if (t.alpha[y * t.w + x] > 8) {
                            float u = float(x) / t.w, v = float(y) / t.h, du = 1.f / t.w, dv = 1.f / t.h;
                            quad(batch, face(u, v), face(u + du, v), face(u + du, v + dv), face(u, v + dv));
                        }
                return;
            }
            quad(batch, face(0, 0), face(1, 0), face(1, 1), face(0, 1));
            quad(batch, face(0, 0, thick, .8f), face(1, 0, thick, .8f), face(1, 0, 0, .8f),
                 face(0, 0, 0, .8f));
            return;
        }
        // The face spans X or Y according to the frame / neighbouring supports.
        if (axis == 2) {
            float x = s.x + w * .5f;
            quad(batch, p(x, s.y, z, 0, 0), p(x, s.y + h, z, 1, 0), p(x, s.y + h, 0, 1, 1),
                 p(x, s.y, 0, 0, 1));
            quad(batch, p(x - thick, s.y, z, 0, 0, .8f), p(x - thick, s.y + h, z, 1, 0, .8f),
                 p(x, s.y + h, z, 1, 0, .8f), p(x, s.y, z, 0, 0, .8f));
        } else {
            float y = s.y + h * .5f;
            quad(batch, p(s.x, y, z, 0, 0), p(s.x + w, y, z, 1, 0), p(s.x + w, y, 0, 1, 1),
                 p(s.x, y, 0, 0, 1));
            quad(batch, p(s.x, y - thick, z, 0, 0, .8f), p(s.x + w, y - thick, z, 1, 0, .8f),
                 p(s.x + w, y, z, 1, 0, .8f), p(s.x, y, z, 0, 0, .8f));
        }
        return;
    }
    if (kind == 6 && camera.view.enabled) {
        // Transparent cells have only their externally visible faces. Back
        // faces and internal joins must not darken every pane repeatedly.
        float height = 48.f;
        auto joined = [&](float dx, float dy) {
            for (auto &f : footprints)
                if (f.kind == 6 && f.level == s.level && std::abs(f.x - s.x - dx) < 1 &&
                    std::abs(f.y - s.y - dy) < 1)
                    return true;
            return false;
        };
        if (camera.eyeY >= s.y + h && !joined(0, h))
            quad(batch, p(s.x, s.y + h, height, 0, 0), p(s.x + w, s.y + h, height, 1, 0),
                 p(s.x + w, s.y + h, 0, 1, 1), p(s.x, s.y + h, 0, 0, 1));
        if (camera.eyeY <= s.y && !joined(0, -h))
            quad(batch, p(s.x, s.y, height, 0, 0), p(s.x + w, s.y, height, 1, 0), p(s.x + w, s.y, 0, 1, 1),
                 p(s.x, s.y, 0, 0, 1));
        if (camera.eyeX >= s.x + w && !joined(w, 0))
            quad(batch, p(s.x + w, s.y, height, 0, 0), p(s.x + w, s.y + h, height, 0, 1),
                 p(s.x + w, s.y + h, 0, 1, 1), p(s.x + w, s.y, 0, 1, 0));
        if (camera.eyeX <= s.x && !joined(-w, 0))
            quad(batch, p(s.x, s.y, height, 0, 0), p(s.x, s.y + h, height, 0, 1), p(s.x, s.y + h, 0, 1, 1),
                 p(s.x, s.y, 0, 1, 0));
        if (camera.eyeZ > height)
            quad(batch, p(s.x, s.y, height, 0, 0), p(s.x + w, s.y, height, 1, 0),
                 p(s.x + w, s.y + h, height, 1, 1), p(s.x, s.y + h, height, 0, 1));
        return;
    }
    if (s.plane == -5 || kind == 6 || kind == 7) {
        // Each wall has the exact tile footprint; neighbouring top faces touch.
        float z = kind == 7 ? (s.axis == 5 ? (camera.view.enabled ? 44.f : 24.f) : 25.5f)
                            : (kind == 6 ? (camera.view.enabled ? 48.f : 26.f)
                                         : (camera.view.enabled ? 48.f : 32.f));
        if (s.rock && camera.view.enabled)
            z = 128.f;
        quad(batch, p(s.x, s.y, z, 0, 0), p(s.x + w, s.y, z, 1, 0), p(s.x + w, s.y + h, z, 1, 1),
             p(s.x, s.y + h, z, 0, 1));
        quad(batch, p(s.x, s.y + h, z, 0, 0, .64f), p(s.x + w, s.y + h, z, 1, 0, .64f),
             p(s.x + w, s.y + h, 0, 1, 1, .64f), p(s.x, s.y + h, 0, 0, 1, .64f));
        if (s.plane == -5) {
            // Side wall art runs along native Y; keep that along the wall,
            // while native X becomes its height rather than stretching a seam.
            quad(batch, p(s.x + w, s.y, z, 0, 0, .78f), p(s.x + w, s.y + h, z, 0, 1, .78f),
                 p(s.x + w, s.y + h, 0, 1, 1, .78f), p(s.x + w, s.y, 0, 1, 0, .78f));
            quad(batch, p(s.x, s.y, z, 0, 0, .78f), p(s.x, s.y + h, z, 0, 1, .78f),
                 p(s.x, s.y + h, 0, 1, 1, .78f), p(s.x, s.y, 0, 1, 0, .78f));
        } else {
            quad(batch, p(s.x + w, s.y, z, 0, 0, .78f), p(s.x + w, s.y + h, z, 1, 0, .78f),
                 p(s.x + w, s.y + h, 0, 1, 1, .78f), p(s.x + w, s.y, 0, 0, 1, .78f));
            quad(batch, p(s.x, s.y, z, 0, 0, .78f), p(s.x, s.y + h, z, 1, 0, .78f),
                 p(s.x, s.y + h, 0, 1, 1, .78f), p(s.x, s.y, 0, 0, 1, .78f));
        }
        quad(batch, p(s.x, s.y, z, 0, 0, .64f), p(s.x + w, s.y, z, 1, 0, .64f),
             p(s.x + w, s.y, 0, 1, 1, .64f), p(s.x, s.y, 0, 0, 1, .64f));
        return;
    }
    // A sofa section has a horizontal cushion and an upright back. Native
    // tile coordinates anchor adjoining sections; the sprite's canvas offsets
    // must not separate a bench into independent pixel-shaped columns.
    if (kind == 8 || kind == 24) {
        seatFabric(t, axis);
        float x = s.doorW ? s.doorX : s.x, y = s.doorW ? s.doorY : s.y;
        float width = s.doorW ? s.doorW : w, depth = s.doorH ? s.doorH : h;
        auto point = [&](float u, float v, float z, float tu, float tv, float shade = 1.f) {
            float px = x + u * width, py = y + v * depth;
            if (axis == 2) {
                px = x + (1 - u) * width;
                py = y + (1 - v) * depth;
            }
            if (axis == 3) {
                px = x + v * width;
                py = y + (1 - u) * depth;
            }
            if (axis == 4) {
                px = x + (1 - v) * width;
                py = y + u * depth;
            }
            // Rotate texture coordinates as well as the physical footprint.
            if (axis >= 3) {
                float length = tu;
                tu = axis == 3 ? tv : 1 - tv;
                tv = axis == 3 ? 1 - length : length;
            }
            tu = t.seatUV[0] + tu * (t.seatUV[2] - t.seatUV[0]);
            tv = t.seatUV[1] + tv * (t.seatUV[3] - t.seatUV[1]);
            return p(px, py, z, tu, tv, shade);
        };
        quad(batch, point(0, .12f, 9, 0, .45f), point(1, .12f, 9, 1, .45f), point(1, .94f, 9, 1, .78f),
             point(0, .94f, 9, 0, .78f));
        quad(batch, point(0, .12f, 22, 0, 0), point(1, .12f, 22, 1, 0), point(1, .12f, 9, 1, .45f),
             point(0, .12f, 9, 0, .45f));
        quad(batch, point(1, 0, 22, 1, 0, .75f), point(0, 0, 22, 0, 0, .75f), point(0, 0, 0, 0, .78f, .75f),
             point(1, 0, 0, 1, .78f, .75f));
        quad(batch, point(0, 0, 22, 0, 0), point(1, 0, 22, 1, 0), point(1, .12f, 22, 1, .12f),
             point(0, .12f, 22, 0, .12f));
        quad(batch, point(0, .94f, 9, 0, .78f, .8f), point(1, .94f, 9, 1, .78f, .8f),
             point(1, .94f, 0, 1, 1, .8f), point(0, .94f, 0, 0, 1, .8f));
        for (int side = 0; side < 2; ++side) {
            if (!(s.seatJoins & (1u << side))) {
                float u = float(side);
                quad(batch, point(u, .12f, 9, u, .45f, .8f), point(u, .94f, 9, u, .78f, .8f),
                     point(u, .94f, 0, u, 1, .8f), point(u, .12f, 0, u, 1, .8f));
            }
        }
        return;
    }
    if (kind == 10 && axis >= 1 && axis <= 4) {
        int l = int(t.w), r = 0, top = int(t.h), bottom = 0;
        for (unsigned y = 0; y < t.h; ++y)
            for (unsigned x = 0; x < t.w; ++x)
                if (t.alpha[y * t.w + x] > 100) {
                    l = std::min(l, int(x));
                    r = std::max(r, int(x) + 1);
                    top = std::min(top, int(y));
                    bottom = std::max(bottom, int(y) + 1);
                }
        if (r > l && bottom > top) {
            if (axis <= 2) {
                float y = s.y + (axis == 1 ? h : 0);
                quad(batch, p(s.x + l, y, 16, l / w, top / h), p(s.x + r, y, 16, r / w, top / h),
                     p(s.x + r, y, 0, r / w, bottom / h), p(s.x + l, y, 0, l / w, bottom / h));
            } else {
                float x = s.x + (axis == 3 ? w : 0);
                quad(batch, p(x, s.y + top, 16, l / w, top / h), p(x, s.y + bottom, 16, l / w, bottom / h),
                     p(x, s.y + bottom, 13, r / w, bottom / h), p(x, s.y + top, 13, r / w, top / h));
                // Side-view DMI gives only a narrow bar; two thin supports use
                // its own pixels rather than stretching it into a solid wall.
                for (float y : {s.y + float(top) + 1, s.y + float(bottom) - 1})
                    quad(batch, p(x, y, 13, l / w, top / h), p(x, y + 1, 13, r / w, top / h),
                         p(x, y + 1, 0, r / w, bottom / h), p(x, y, 0, l / w, bottom / h));
            }
            return;
        }
    }
    if (kind == 3 || kind == 4 || kind == 10 || kind == 11 || kind == 13 || kind == 38) {
        // SS13 object art already contains its apparent front/top. Raising the
        // whole canvas upright rotates that baked artwork by ninety degrees.
        // Preserve the tile's XY axes and add a shallow alpha-shaped relief.
        bool table = kind == 3 || kind == 11;
        float height = kind == 10 ? 16.f : (kind == 13 ? 12.f : (table ? 12.f : 1.f));
        auto opaque = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < int(t.w) && y < int(t.h) &&
                   t.alpha[size_t(y) * t.w + x] > (kind == 11 ? 240 : 100);
        };
        for (int y = 0; y < int(t.h); ++y)
            for (int x = 0; x < int(t.w); ++x)
                if (opaque(x, y)) {
                    float x0 = s.x + x, x1 = x0 + 1, y0 = s.y + y, y1 = y0 + 1, u = (x + .5f) / w,
                          v = (y + .5f) / h;
                    if (!opaque(x, y + 1))
                        quad(batch, p(x0, y1, height, u, v, .75f), p(x1, y1, height, u, v, .75f),
                             p(x1, y1, 0, u, v, .75f), p(x0, y1, 0, u, v, .75f));
                    if (!opaque(x + 1, y))
                        quad(batch, p(x1, y0, height, u, v, .85f), p(x1, y1, height, u, v, .85f),
                             p(x1, y1, 0, u, v, .85f), p(x1, y0, 0, u, v, .85f));
                    if (!opaque(x - 1, y))
                        quad(batch, p(x0, y0, height, u, v, .85f), p(x0, y1, height, u, v, .85f),
                             p(x0, y1, 0, u, v, .85f), p(x0, y0, 0, u, v, .85f));
                    if (!opaque(x, y - 1))
                        quad(batch, p(x0, y0, height, u, v, .75f), p(x1, y0, height, u, v, .75f),
                             p(x1, y0, 0, u, v, .75f), p(x0, y0, 0, u, v, .75f));
                }
        unsigned lower = 0, upper = 0;
        if (table)
            for (unsigned y = 0; y < t.h; ++y)
                for (unsigned x = 0; x < t.w; ++x)
                    if (t.alpha[y * t.w + x] > 100) {
                        if (y >= t.h * 3 / 4)
                            ++lower;
                        else
                            ++upper;
                    }
        bool hasLegs = table && lower * 6 < upper;
        float topV = hasLegs ? .75f : 1.f;
        quad(batch, p(s.x, s.y, height, 0, 0), p(s.x + w, s.y, height, 1, 0),
             p(s.x + w, s.y + h, height, 1, topV), p(s.x, s.y + h, height, 0, topV));
        if (hasLegs)
            quad(batch, p(s.x, s.y + h, height, 0, .75f), p(s.x + w, s.y + h, height, 1, .75f),
                 p(s.x + w, s.y + h, 0, 1, 1), p(s.x, s.y + h, 0, 0, 1));
        return;
    }
    // Furniture and machinery stand upright; only surface and floor fixtures lie in XY.
    // Extrusion follows the alpha silhouette instead of making the sprite a cube.
    float thickness = kind == 31   ? 3.f
                      : kind == 9  ? 18.f
                      : kind == 21 ? 22.f
                      : kind == 22 ? 24.f
                      : kind == 23 ? 20.f
                                   : (camera.view.enabled && kind == 18 ? 16.f : 5.f);

    float feet = s.y + (camera.view.enabled && (kind == 8 || kind == 9) ? h * .5f : h);
    float heightScale = camera.view.enabled && kind == 31                 ? 48.f / h
                        : camera.view.enabled && (kind == 8 || kind == 9) ? 24.f / h
                                                                          : 1.f;
    float topZ = h * heightScale;
    if (camera.view.enabled && (kind == 0 || kind == 51 || kind == 53 || kind == 30 || kind == 33)) {
        // DOOM-style sprites: rotate around their vertical axis, without
        // changing their pixels or synthesising a model for a character/item.
        float cx = s.x + w * .5f, cy = s.y + h * .5f;
        float rx = std::cos(camera.view.yaw), ry = std::sin(camera.view.yaw);
        float scale = loose ? .55f : 1.f, half = w * .5f * scale;
        unsigned bottom = t.h;
        if (kind == 0 || kind == 53) {
            bottom = 0;
            for (unsigned y = 0; y < t.h; ++y)
                for (unsigned x = 0; x < t.w; ++x)
                    if (t.alpha[y * t.w + x] > 8)
                        bottom = std::max(bottom, y + 1);
        }
        float base = elevation - (h - bottom) * scale;
        bool flip = false;
        if (kind == 33 && axis == 2)
            for (auto &f : footprints)
                if (f.kind == 38 && f.level == s.level && std::abs(f.x - s.x) < 33 &&
                    std::abs(f.y - s.y) < 33) {
                    flip = true;
                    break;
                }
        quad(batch, p(cx - rx * half, cy - ry * half, base + h * scale, 0, flip ? 1.f : 0.f),
             p(cx + rx * half, cy + ry * half, base + h * scale, 1, flip ? 1.f : 0.f),
             p(cx + rx * half, cy + ry * half, base, 1, flip ? 0.f : 1.f),
             p(cx - rx * half, cy - ry * half, base, 0, flip ? 0.f : 1.f));
        return;
    }
    if (kind == 15 || kind == 18 || kind == 21 || kind == 22 || kind == 23 || kind == 25 || kind == 40 ||
        kind == 45 || kind == 46) {
        // Transparent canvas margins are not physical height. Rest the last
        // visible row on the actual support, including tabletop appliances.
        unsigned bottom = 0;
        for (unsigned y = 0; y < t.h; ++y)
            for (unsigned x = 0; x < t.w; ++x)
                if (t.alpha[y * t.w + x] > 8)
                    bottom = std::max(bottom, y + 1);
        elevation -= h - bottom;
        feet = s.y + h * .5f;
        if (kind == 21 || kind == 22 || kind == 23)
            feet = (s.doorW ? s.doorY : s.y) + 16.f + thickness * .5f;
    }
    if (kind == 25 || kind == 40 || kind == 45 || kind == 46) {
        // A computer's side is casing, not a repetition of every button,
        // monitor pixel and transparent hole on the front artwork.
        float materialU = .1f, materialV = .1f;
        int best = 1000;
        if (t.rgba.size() == size_t(t.w) * t.h * 4)
            for (unsigned y = 0; y < t.h; ++y)
                for (unsigned x = 0; x < t.w; ++x) {
                    auto color = &t.rgba[(y * t.w + x) * 4];
                    if (color[3] < 240)
                        continue;
                    int low = std::min({int(color[0]), int(color[1]), int(color[2])}),
                        high = std::max({int(color[0]), int(color[1]), int(color[2])});
                    int score = (high - low) * 5 + std::abs(high - 64);
                    if (score < best) {
                        best = score;
                        materialU = (x + .5f) / w;
                        materialV = (y + .5f) / h;
                    }
                }
        auto casePoint = [&](float x, float y, float z, float u, float v, float shade = 1.f) {
            if (kind == 25) {
                float dx = x - (s.x + w * .5f), dy = y - (s.y + h * .5f);
                if (axis == 2) {
                    x = s.x + w * .5f - dx;
                    y = s.y + h * .5f - dy;
                }
                if (axis == 3) {
                    x = s.x + w * .5f + dy;
                    y = s.y + h * .5f - dx;
                }
                if (axis == 4) {
                    x = s.x + w * .5f - dy;
                    y = s.y + h * .5f + dx;
                }
            }
            return p(x, y, z, u, v, shade);
        };
        auto body = [&](float x, float y, float z, float shade = .8f) {
            return casePoint(x, y, z, materialU, materialV, shade);
        };
        const float depth = kind == 40 ? 24.f : 14.f;
        for (int row = 0; row < int(t.h);) {
            auto bounds = [&](int y) {
                int l = int(t.w), r = 0;
                if (y >= 0 && y < int(t.h))
                    for (int x = 0; x < int(t.w); ++x)
                        if (t.alpha[size_t(y) * t.w + x] > 100) {
                            l = std::min(l, x);
                            r = x + 1;
                        }
                return std::pair<int, int>{l, r};
            };
            auto span = bounds(row);
            if (span.second <= span.first) {
                ++row;
                continue;
            }
            int end = row + 1;
            while (end < int(t.h) && bounds(end) == span)
                ++end;
            float left = s.x + span.first, right = s.x + span.second, top = h - row + elevation,
                  bottom = h - end + elevation;
            quad(batch, body(left, feet, top), body(left, feet - depth, top),
                 body(left, feet - depth, bottom), body(left, feet, bottom));
            quad(batch, body(right, feet - depth, top), body(right, feet, top), body(right, feet, bottom),
                 body(right, feet - depth, bottom));
            quad(batch, body(left, feet - depth, top), body(right, feet - depth, top),
                 body(right, feet - depth, bottom), body(left, feet - depth, bottom));
            quad(batch, body(left, feet - depth, top), body(right, feet - depth, top), body(right, feet, top),
                 body(left, feet, top));
            row = end;
        }
        quad(batch, casePoint(s.x, feet, h + elevation, 0, 0), casePoint(s.x + w, feet, h + elevation, 1, 0),
             casePoint(s.x + w, feet, elevation, 1, 1), casePoint(s.x, feet, elevation, 0, 1));
        // Arcade cabinets retain their decorative art on both exposed broad
        // faces; a dark casing sample must not hide the native game texture.
        if (kind == 45)
            quad(batch, p(s.x + w, feet - depth - .1f, h + elevation, 0, 0),
                 p(s.x, feet - depth - .1f, h + elevation, 1, 0), p(s.x, feet - depth - .1f, elevation, 1, 1),
                 p(s.x + w, feet - depth - .1f, elevation, 0, 1));
        return;
    }
    // Standing character geometry uses the original camera projection.
    auto face = [&](float x, float y, float z, float u, float v, float shade = 1.f) {
        return p(x, y, z, u, v, shade);
    };
    auto opaque = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < int(t.w) && y < int(t.h) && t.alpha[size_t(y) * t.w + x] > 100;
    };
    for (int y = 0; y < int(t.h); ++y)
        for (int x = 0; x < int(t.w); ++x)
            if (opaque(x, y)) {
                float x0 = s.x + x, x1 = x0 + 1, z0 = (h - y - 1) * heightScale + elevation,
                      z1 = z0 + heightScale;
                float u = (x + .5f) / w, v = (y + .5f) / h;
                if (!opaque(x + 1, y))
                    quad(batch, face(x1, feet, z1, u, v, .65f), face(x1, feet - thickness, z1, u, v, .65f),
                         face(x1, feet - thickness, z0, u, v, .65f), face(x1, feet, z0, u, v, .65f));
                if (!opaque(x, y - 1))
                    quad(batch, face(x0, feet - thickness, z1, u, v, .86f),
                         face(x1, feet - thickness, z1, u, v, .86f), face(x1, feet, z1, u, v, .86f),
                         face(x0, feet, z1, u, v, .86f));
                if (camera.view.enabled && !opaque(x - 1, y))
                    quad(batch, face(x0, feet - thickness, z1, u, v, .65f), face(x0, feet, z1, u, v, .65f),
                         face(x0, feet, z0, u, v, .65f), face(x0, feet - thickness, z0, u, v, .65f));
                if (camera.view.enabled && !opaque(x, y + 1))
                    quad(batch, face(x0, feet, z0, u, v, .65f), face(x1, feet, z0, u, v, .65f),
                         face(x1, feet - thickness, z0, u, v, .65f),
                         face(x0, feet - thickness, z0, u, v, .65f));
            }
    // Artwork faces the camera; only its thickness extends into the floor plane.
    quad(batch, face(s.x, feet, topZ + elevation, 0, 0), face(s.x + w, feet, topZ + elevation, 1, 0),
         face(s.x + w, feet, elevation, 1, 1), face(s.x, feet, elevation, 0, 1));
    if (camera.view.enabled)
        quad(batch, face(s.x + w, feet - thickness, topZ + elevation, 1, 0, .75f),
             face(s.x, feet - thickness, topZ + elevation, 0, 0, .75f),
             face(s.x, feet - thickness, elevation, 0, 1, .75f),
             face(s.x + w, feet - thickness, elevation, 1, 1, .75f));
}
static void capture(IDirect3DDevice9 *device, IDirect3DSurface9 *target, const D3DSURFACE_DESC &desc) {
    if (savedFrame)
        return;
    IDirect3DSurface9 *copy = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                   &copy, nullptr)))
        return;
    if (SUCCEEDED(device->GetRenderTargetData(target, copy))) {
        D3DLOCKED_RECT lock = {};
        if (SUCCEEDED(copy->LockRect(&lock, nullptr, D3DLOCK_READONLY))) {
            wchar_t path[MAX_PATH];
            GetEnvironmentVariableW(L"BS220_NATIVE_FRAME", path, MAX_PATH);
            HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                BITMAPFILEHEADER fh = {};
                fh.bfType = 0x4d42;
                fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
                fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4;
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof(ih);
                ih.biWidth = desc.Width;
                ih.biHeight = -(LONG)desc.Height;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                DWORD written;
                WriteFile(file, &fh, sizeof(fh), &written, nullptr);
                WriteFile(file, &ih, sizeof(ih), &written, nullptr);
                for (unsigned y = 0; y < desc.Height; ++y)
                    WriteFile(file, static_cast<unsigned char *>(lock.pBits) + y * lock.Pitch, desc.Width * 4,
                              &written, nullptr);
                CloseHandle(file);
                savedFrame = true;
            }
            copy->UnlockRect();
        }
    }
    copy->Release();
}
} // namespace

bool DrawNativeVolume(void *driver, void *list, void *icons, NativePixels pixels) {
    LARGE_INTEGER renderStarted;
    QueryPerformanceCounter(&renderStarted);
    if (!list || !icons || !pixels)
        return false;
    // Verified by D3DXCreateTexture(device, ...) in the exact pinned build.
    auto device = field<IDirect3DDevice9 *>(driver, 0x20);
    if (!device)
        return false;
    unsigned roots = field<uint16_t>(list, 8), capacity = field<uint16_t>(list, 10);
    auto buffer = field<const unsigned char *>(list, 4);
    if (!buffer || roots > capacity || !roots)
        return false;
    std::vector<Sprite> sprites;
    std::vector<unsigned char> seen(capacity);
    graphicsWindow = field<void *>(driver, 4);
    if (!screenEdge)
        screenEdge = reinterpret_cast<ScreenEdge>(reinterpret_cast<unsigned char *>(
            GetProcAddress(GetModuleHandleW(L"byondwin.dll"), ByondAbi::screenEdgeSymbol)));
    for (unsigned i = 0; i < roots; ++i)
        walk(buffer, roots, capacity, i, 0, 0, 0xffffffff, sprites, seen, 0);
    unsigned floors = 0;
    for (auto &s : sprites)
        if (s.plane == -6 || s.plane == -21 || s.plane == -20)
            ++floors;
    if (!floors)
        return false; // Keep the native lobby/preview and empty scenes.
    if (device != cachedDevice) {
        if (firstPersonLightingShader) {
            firstPersonLightingShader->Release();
            firstPersonLightingShader = nullptr;
        }
        doorAxes.clear();
        for (auto &light : levelLighting)
            if (light.second)
                light.second->Release();
        levelLighting.clear();
        lightingTexture = nullptr;
        for (auto &light : levelSurfaceLighting)
            if (light.second)
                light.second->Release();
        levelSurfaceLighting.clear();
        for (auto &entry : textures)
            if (entry.second.gpu)
                entry.second.gpu->Release();
        textures.clear();
        for (auto &entry : appearances)
            if (entry.second.gpu)
                entry.second.gpu->Release();
        appearances.clear();
        cachedDevice = device;
    }
    IDirect3DSurface9 *target = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target)))
        return false;
    D3DSURFACE_DESC desc;
    if (FAILED(target->GetDesc(&desc))) {
        target->Release();
        return false;
    }
    D3DSURFACE_DESC logical = desc;
    auto mapSurface = field<IDirect3DSurface9 *>(driver, 0x230);
    if (mapSurface)
        mapSurface->GetDesc(&logical);
    float nearest = 1e30f;
    for (auto &s : sprites)
        if (s.plane == -6 || ((field<uint32_t>(s.data, 0xc) == 1) && s.plane == -4)) {
            float dx = s.x + 16 - logical.Width * .5f, dy = s.y + 16 - logical.Height * .5f;
            float distance = dx * dx + dy * dy;
            if (distance < nearest) {
                nearest = distance;
                currentLevel = s.level;
            }
        }
    nearest = 32.f * 32.f;
    for (auto &s : sprites)
        if (field<uint32_t>(s.data, 0xc) == 1 && (s.plane == -4 || s.plane == -2 || s.plane == 5)) {
            float dx = s.x + 16 - logical.Width * .5f, dy = s.y + 16 - logical.Height * .5f,
                  distance = dx * dx + dy * dy;
            if (distance < nearest) {
                nearest = distance;
                currentLevel = s.level;
            }
        }
    IDirect3DStateBlock9 *state = nullptr;
    IDirect3DSurface9 *oldTarget = nullptr;
    IDirect3DSurface9 *oldDepth = nullptr;
    IDirect3DSurface9 *depth = nullptr;
    IDirect3DSurface9 *work = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state))) {
        target->Release();
        return false;
    }
    state->Capture();
    device->GetRenderTarget(0, &oldTarget);
    device->GetDepthStencilSurface(&oldDepth);
    HRESULT depthResult = device->CreateDepthStencilSurface(desc.Width, desc.Height, D3DFMT_D24S8,
                                                            D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr);
    HRESULT workResult = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE,
                                                    0, FALSE, &work, nullptr);
    if (FAILED(depthResult) || FAILED(workResult)) {
        if (depth)
            depth->Release();
        if (work)
            work->Release();
        if (oldTarget)
            oldTarget->Release();
        if (oldDepth)
            oldDepth->Release();
        state->Release();
        target->Release();
        return false;
    }
    device->SetDepthStencilSurface(nullptr);
    device->SetRenderTarget(0, work);
    device->SetDepthStencilSurface(depth);
    D3DVIEWPORT9 viewport = {0, 0, desc.Width, desc.Height, 0.f, 1.f};
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(fvf);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_DEPTHBIAS, 0);
    device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAREF, 8);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    Camera camera = {float(logical.Width), float(logical.Height)};
    camera.eyeX = camera.width * .5f;
    camera.eyeY = camera.height * .5f;
    uint64_t selfCid = 0;
    float selfDistance = 32.f * 32.f;
    float centerX = camera.eyeX, centerY = camera.eyeY;
    for (auto &s : sprites)
        if (field<uint32_t>(s.data, 0xc) == 1 && s.plane == -4 && s.level == currentLevel) {
            float dx = s.x + 16 - centerX, dy = s.y + 16 - centerY;
            if (dx * dx + dy * dy < selfDistance) {
                selfDistance = dx * dx + dy * dy;
                selfCid = field<uint64_t>(s.data, 0xc);
                camera.eyeX = s.x + 16;
                camera.eyeY = s.y + 16;
            }
        }
    if (camera.view.enabled && selfCid)
        for (auto &s : sprites)
            if (field<uint64_t>(s.data, 0xc) == selfCid && s.plane == -4) {
                TraceFirstPersonPosition(selfCid, field<int16_t>(s.data, 0x50), field<int16_t>(s.data, 0x52),
                                         s.level, camera.eyeX, camera.eyeY);
                break;
            }
    // Actor tile fields are viewport-relative. Measure actual movement from
    // the same stationary floor atom shifting under the camera instead.
    static uint64_t anchorCid = 0;
    static int anchorLevel = 0;
    static float anchorX = 0, anchorY = 0;
    const Sprite *anchor = nullptr;
    if (camera.view.enabled) {
        for (auto &s : sprites)
            if (s.plane == -6 && s.level == currentLevel && field<uint64_t>(s.data, 0xc) == anchorCid) {
                anchor = &s;
                break;
            }
        if (anchor && anchorLevel == currentLevel) {
            TraceFirstPersonSceneShift(anchorCid, anchorX - anchor->x, anchorY - anchor->y, currentLevel);
        } else {
            for (auto &s : sprites)
                if (s.plane == -6 && s.level == currentLevel && field<uint64_t>(s.data, 0xc)) {
                    anchor = &s;
                    break;
                }
        }
        if (anchor) {
            anchorCid = field<uint64_t>(anchor->data, 0xc);
            anchorLevel = currentLevel;
            anchorX = anchor->x;
            anchorY = anchor->y;
        }
    } else
        anchorCid = 0;
    activeCamera = &camera;
    AimHit aim;
    aim.window = graphicsWindow;
    float aimDepth = 1e30f;
    // Read hand selection and contents from the real HUD, so dropping,
    // swapping or losing a hand cannot leave a locally invented item behind.
    const Sprite *activeSlot = nullptr;
    Texture *heldArt = nullptr;
    DWORD heldColor = 0xffffffff;
    unsigned handSide = 0;
    if (camera.view.enabled) {
        for (auto &s : sprites)
            if (s.plane >= 40) {
                auto art = texture(device, icons, pixels, field<unsigned>(s.data, 0));
                if (art && art->kind == 20) {
                    handSide = art->axis;
                    if (s.parent >= 0)
                        activeSlot = &sprites[s.parent];
                    else
                        activeSlot = &s;
                    break;
                }
            }
        float nearestItem = 1e30f;
        auto slotArt =
            activeSlot ? texture(device, icons, pixels, field<unsigned>(activeSlot->data, 0)) : nullptr;
        if (activeSlot && slotArt)
            for (auto &s : sprites)
                if (s.plane >= 40 && (s.color >> 24) != 0 &&
                    field<uint64_t>(s.data, 0xc) != field<uint64_t>(activeSlot->data, 0xc)) {
                    auto art = texture(device, icons, pixels, field<unsigned>(s.data, 0));
                    if (!art || art->kind == 19 || art->kind == 20)
                        continue;
                    bool inside = false;
                    int parent = s.parent;
                    for (unsigned ancestor = 0; parent >= 0 && ancestor < 32; ++ancestor) {
                        if (&sprites[parent] == activeSlot) {
                            inside = true;
                            break;
                        }
                        parent = sprites[parent].parent;
                    }
                    // Large guns have negative pixel offsets and 64px canvases;
                    // their origin is not the hand-slot origin. Compare centers.
                    float dx = s.x + art->w * .5f - (activeSlot->x + slotArt->w * .5f);
                    float dy = s.y + art->h * .5f - (activeSlot->y + slotArt->h * .5f);
                    float distance = inside ? -1.f : dx * dx + dy * dy;
                    if ((inside || (std::abs(dx) < 12 && std::abs(dy) < 12)) && distance < nearestItem) {
                        heldArt = art;
                        heldColor = s.color;
                        nearestItem = distance;
                    }
                }
    }
    unsigned vertices = 0;
    bool success = true;
    lights.clear();
    footprints.clear();
    for (auto &s : sprites)
        if (s.plane < 40) {
            if (camera.view.enabled && (s.plane == 10 || s.plane == 11))
                continue;
            auto t = texture(device, icons, pixels, field<unsigned>(s.data, 0));
            if (s.plane == 10 || s.plane == 11) {
                if (t)
                    lights.push_back({&s, t});
                continue;
            }
            if (t) {
                s.kind = t->kind;
                s.axis = t->axis;
                s.feet = s.y + t->h;
                s.lava = (s.plane == -6 || s.plane == -10) &&
                         std::binary_search(std::begin(lavaFrames), std::end(lavaFrames), t->hash);
                s.rock = s.plane == -5 &&
                         std::binary_search(std::begin(rockFrames), std::end(rockFrames), t->hash);
                if (s.lava) {
                    s.emissive = true;
                    footprints.push_back({s.x, s.y, 47, 0, s.level});
                }
                if (s.kind == 48)
                    s.kind = 5;
            }
            if (s.kind == 35 || s.kind == 36) {
                s.emissive = true;
                s.kind = s.kind == 35 ? 4 : 16;
            }
            if (s.kind == 52) {
                s.doubleDoor = true;
                s.kind = 1;
            }
            if (t && s.plane == -4 && !s.kind &&
                std::binary_search(std::begin(ceilingFrames), std::end(ceilingFrames), t->hash))
                s.kind = 17;
            if (s.kind == 50) {
                s.doorOpen = true;
                s.kind = 34;
            }
            if (s.kind == 34) {
                s.kind = 1;
                s.doorOffset = 4.f;
            }
            if (s.kind == 39 || s.kind == 49) {
                s.doorOpen = s.kind == 49;
                s.windowDoor = true;
                s.panelEdge = s.axis;
                if (!s.panelEdge && t) {
                    unsigned left = t->w, right = 0, top = t->h, bottom = 0;
                    for (unsigned y = 0; y < t->h; ++y)
                        for (unsigned x = 0; x < t->w; ++x)
                            if (t->alpha[y * t->w + x] > 8) {
                                left = std::min(left, x);
                                right = std::max(right, x + 1);
                                top = std::min(top, y);
                                bottom = std::max(bottom, y + 1);
                            }
                    s.panelEdge = right - left >= bottom - top ? (top + bottom >= t->h ? 1 : 2)
                                                               : (left + right >= t->w ? 3 : 4);
                }
                s.kind = 1;
                s.axis = s.panelEdge >= 3 ? 2 : 1;
            }
            if (s.kind == 32) {
                s.panelEdge = s.axis;
                s.kind = 2;
                s.axis = s.panelEdge >= 3 ? 2 : 1;
            }
            if (s.kind == 1 && s.quarterTurn)
                s.axis = s.axis == 2 ? 1 : 2;
            if (s.kind == 30)
                s.lying = false; // Speech/status artwork stays upright.
            bool sameOwner =
                s.parent >= 0 && field<uint64_t>(s.data, 0xc) == field<uint64_t>(sprites[s.parent].data, 0xc);
            if (sameOwner && sprites[s.parent].plane == s.plane)
                s.feet = sprites[s.parent].feet;
            if (!s.kind && sameOwner) {
                s.kind = sprites[s.parent].kind;
                s.axis = sprites[s.parent].axis;
            }
            if (t && (s.kind || s.plane == -5))
                footprints.push_back(
                    {s.x, s.y, s.kind ? s.kind : static_cast<unsigned char>(5), s.axis, s.level});
        }
    bool naturalEnvironment = false;
    float environmentDistance = 1e30f;
    for (auto &ground : sprites)
        if (ground.plane == -6 && ground.level == currentLevel) {
            float dx = ground.x + 16 - camera.eyeX, dy = ground.y + 16 - camera.eyeY,
                  distance = dx * dx + dy * dy;
            if (distance < environmentDistance) {
                auto art = texture(device, icons, pixels, field<unsigned>(ground.data, 0));
                if (art) {
                    environmentDistance = distance;
                    naturalEnvironment =
                        ground.lava ||
                        std::binary_search(std::begin(naturalFrames), std::end(naturalFrames), art->hash);
                }
            }
        }
    std::vector<int> levels;
    for (auto &s : sprites)
        if (s.plane < 34 && std::find(levels.begin(), levels.end(), s.level) == levels.end())
            levels.push_back(s.level);
    sprites.reserve(sprites.size() + levels.size());
    std::unordered_map<int, DirectLight> directLights;
    bool directLighting = camera.view.enabled && prepareFirstPersonLighting(device);
    lightWidth = logical.Width;
    lightHeight = logical.Height;
    for (int level : levels) {
        if (directLighting) {
            auto light = directLight(driver, buffer, roots, logical.Width, logical.Height, level);
            if (light.gpu) {
                directLights[level] = light;
                continue;
            }
        }
        if (!nativeLightMap(driver, device, buffer, roots, logical.Width, logical.Height, level))
            buildLightMap(logical.Width, logical.Height, level);
        lightingTexture = levelLighting[level];
        uploadLighting(device);
        levelLighting[level] = lightingTexture;
        auto nativeColors = lightMap;
        buildLightMap(logical.Width, logical.Height, level);
        auto physicalColors = lightMap;
        lightMap = std::move(nativeColors);
        // Preserve the station's lighting everywhere else. Only remove baked
        // object emissive masks from structural surfaces beside walls/doors.
        for (auto &s : sprites)
            if (s.level == level && (s.plane == -5 || (s.kind == 1 && s.axis == 2))) {
                auto t = texture(device, icons, pixels, field<unsigned>(s.data, 0));
                if (!t || t->w > 64 || t->h > 64)
                    continue;
                int left = std::max(0, int(s.x) - 4), right = std::min(int(lightWidth), int(s.x + t->w) + 4);
                int top = std::max(0, int(s.y) - 4), bottom = std::min(int(lightHeight), int(s.y + t->h) + 4);
                for (int y = top; y < bottom; ++y)
                    for (int x = left; x < right; ++x)
                        lightMap[size_t(y) * lightWidth + x] = physicalColors[size_t(y) * lightWidth + x];
            }
        lightingTexture = levelSurfaceLighting[level];
        uploadLighting(device);
        levelSurfaceLighting[level] = lightingTexture;
    }
    assembleAppearances(device, icons, pixels, sprites);
    std::unordered_map<int, std::pair<float, float>> tileOrigins;
    for (auto &s : sprites)
        if (s.plane == -6 && tileOrigins.find(s.level) == tileOrigins.end())
            tileOrigins[s.level] = {s.x - field<int16_t>(s.data, 0x50) * 32.f,
                                    s.y + field<int16_t>(s.data, 0x52) * 32.f};
    for (auto &s : sprites)
        if ((s.kind == 1 || s.kind == 8 || s.kind == 24 || s.kind == 12 || s.kind == 14 || s.kind == 16 ||
             s.kind == 42 || s.kind == 21 || s.kind == 22 || s.kind == 23) &&
            (s.color >> 24) != 0) {
            auto origin = tileOrigins.find(s.level);
            if (origin == tileOrigins.end())
                continue;
            s.doorX = origin->second.first + field<int16_t>(s.data, 0x50) * 32.f;
            s.doorY = origin->second.second - field<int16_t>(s.data, 0x52) * 32.f;
            s.doorW = std::max(32.f, float(field<uint16_t>(s.data, 4)));
            s.doorH = 32.f;
        }
    if (camera.view.enabled) {
        for (auto &cabinet : sprites)
            if ((cabinet.kind == 21 || cabinet.kind == 22 || cabinet.kind == 23) && cabinet.doorW) {
                auto art = cabinet.appearance
                               ? cabinet.appearance
                               : texture(device, icons, pixels, field<unsigned>(cabinet.data, 0));
                if (!art)
                    continue;
                float x = cabinet.doorX + (32.f - art->w) * .5f, y = cabinet.doorY + (32.f - art->h) * .5f;
                cabinet.visualOffsetX = x - cabinet.x;
                cabinet.visualOffsetY = y - cabinet.y;
                cabinet.x = x;
                cabinet.y = y;
            }
        struct Pile {
            int level, tx, ty;
            std::vector<Sprite *> items;
        };
        std::vector<Pile> piles;
        for (auto &item : sprites)
            if (item.plane == -4 && item.kind == 0 && (item.color >> 24) &&
                field<uint32_t>(item.data, 0xc) == 2) {
                auto origin = tileOrigins.find(item.level);
                if (origin == tileOrigins.end())
                    continue;
                int tx = field<int16_t>(item.data, 0x50), ty = field<int16_t>(item.data, 0x52);
                auto pile = std::find_if(piles.begin(), piles.end(), [&](const Pile &p) {
                    return p.level == item.level && p.tx == tx && p.ty == ty;
                });
                if (pile == piles.end()) {
                    piles.push_back({item.level, tx, ty, {}});
                    pile = piles.end() - 1;
                }
                pile->items.push_back(&item);
            }
        for (auto &pile : piles) {
            auto origin = tileOrigins.find(pile.level);
            float tileX = origin->second.first + pile.tx * 32.f,
                  tileY = origin->second.second - pile.ty * 32.f;
            bool storage = false;
            for (auto &cabinet : sprites)
                if (cabinet.level == pile.level && cabinet.kind == 21 &&
                    std::abs(cabinet.doorX - tileX) < 1 && std::abs(cabinet.doorY - tileY) < 1) {
                    storage = true;
                    break;
                }
            if (pile.items.size() < 2 && !storage)
                continue;
            std::sort(pile.items.begin(), pile.items.end(), [](const Sprite *a, const Sprite *b) {
                return field<uint64_t>(a->data, 0xc) < field<uint64_t>(b->data, 0xc);
            });
            unsigned columns = unsigned(std::ceil(std::sqrt(double(pile.items.size())))),
                     rows = unsigned((pile.items.size() + columns - 1) / columns);
            for (unsigned i = 0; i < pile.items.size(); ++i) {
                auto &item = *pile.items[i];
                auto art = item.appearance ? item.appearance
                                           : texture(device, icons, pixels, field<unsigned>(item.data, 0));
                if (!art)
                    continue;
                float cx = tileX + (columns > 1 ? 10.f + 12.f * (i % columns) / (columns - 1) : 16.f),
                      cy = tileY + (storage ? (rows > 1 ? 28.f + 3.f * (i / columns) / (rows - 1) : 29.f)
                                            : (rows > 1 ? 10.f + 12.f * (i / columns) / (rows - 1) : 16.f));
                float x = cx - art->w * .5f, y = cy - art->h * .5f;
                item.visualOffsetX = x - item.x;
                item.visualOffsetY = y - item.y;
                item.x = x;
                item.y = y;
            }
        }
    }
    footprints.clear();
    seating.clear();
    for (auto &s : sprites)
        if ((s.color >> 24) != 0) {
            if (s.lava)
                footprints.push_back({s.x, s.y, 47, 0, s.level});
            if (s.kind || s.plane == -5) {
                auto t =
                    s.appearance ? s.appearance : texture(device, icons, pixels, field<unsigned>(s.data, 0));
                footprints.push_back({s.doorW ? s.doorX : s.x, s.doorW ? s.doorY : s.y,
                                      s.kind ? s.kind : static_cast<unsigned char>(5), s.axis, s.level,
                                      t ? float(t->w) : 32.f, t ? float(t->h) : 32.f});
            }
            if ((s.kind == 8 || s.kind == 24) && s.appearance)
                seating.push_back(&s);
        }
    // Connectivity belongs to physical tiles, not sprite canvas offsets or
    // a shared Cid (each furniture section is a separate native atom).
    for (auto &seat : sprites)
        if ((seat.kind == 8 || seat.kind == 24) && (seat.color >> 24) != 0) {
            float x = seat.doorW ? seat.doorX : seat.x, y = seat.doorW ? seat.doorY : seat.y;
            float width = seat.doorW ? seat.doorW : 32, seatDepth = seat.doorH ? seat.doorH : 32;
            for (int side = 0; side < 2; ++side) {
                // Local u reverses for north/east-facing art.
                float sign = (side ? 1.f : -1.f) * ((seat.axis == 2 || seat.axis == 3) ? -1.f : 1.f);
                float nx = x + (seat.axis <= 2 ? sign * width : 0),
                      ny = y + (seat.axis >= 3 ? sign * seatDepth : 0);
                for (auto &other : sprites)
                    if (&other != &seat && other.kind == seat.kind && other.level == seat.level &&
                        other.axis == seat.axis && (other.color >> 24) != 0 &&
                        std::abs((other.doorW ? other.doorX : other.x) - nx) < 1 &&
                        std::abs((other.doorW ? other.doorY : other.y) - ny) < 1) {
                        seat.seatJoins |= 1u << side;
                        break;
                    }
            }
        }
    if (camera.view.enabled) {
        static Texture lavaSurface;
        static unsigned lavaFrame = ~0u;
        unsigned frame = unsigned(GetTickCount64() / 2000) % unsigned(std::size(lavaSurfacePixels) / 1024);
        if (!lavaSurface.gpu || lavaSurface.checked != unsigned(reinterpret_cast<uintptr_t>(device))) {
            if (lavaSurface.gpu)
                lavaSurface.gpu->Release();
            lavaSurface = {};
            lavaSurface.w = lavaSurface.h = 32;
            lavaSurface.alpha.assign(1024, 255);
            device->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &lavaSurface.gpu, nullptr);
            lavaSurface.checked = unsigned(reinterpret_cast<uintptr_t>(device));
            lavaFrame = ~0u;
        }
        if (lavaSurface.gpu && lavaFrame != frame) {
            D3DLOCKED_RECT lock;
            if (SUCCEEDED(lavaSurface.gpu->LockRect(0, &lock, nullptr, 0))) {
                for (unsigned y = 0; y < 32; ++y)
                    memcpy(static_cast<unsigned char *>(lock.pBits) + y * lock.Pitch,
                           lavaSurfacePixels + frame * 1024 + y * 32, 128);
                lavaSurface.gpu->UnlockRect(0);
                lavaFrame = frame;
            }
        }
        for (auto &floor : sprites)
            if (floor.lava && floor.plane == -6) {
                if (lavaSurface.gpu)
                    floor.appearance = &lavaSurface;
                for (auto &overlay : sprites)
                    if (&overlay != &floor && !overlay.lava && overlay.plane == -6 &&
                        overlay.level == floor.level &&
                        field<uint64_t>(overlay.data, 0xc) == field<uint64_t>(floor.data, 0xc))
                        overlay.color = 0;
            }
    }
    // Resolve axes after all layers of an atom have been combined. Preserve
    // explicit DMI direction and infer directionless firedoors from the whole
    // wall / door line, including a directional airlock on the same tile.
    for (auto &s : sprites)
        if ((s.kind == 1 || s.kind == 2) && (s.color >> 24) != 0) {
            auto cid = field<uint64_t>(s.data, 0xc);
            if (!s.axis) {
                float alongX = 0, alongY = 0;
                for (auto &f : footprints)
                    if (f.level == s.level && (f.kind == 5 || f.kind == 6 || f.kind == 2 || f.kind == 1)) {
                        float dx = std::abs(f.x - (s.doorW ? s.doorX : s.x)),
                              dy = std::abs(f.y - (s.doorW ? s.doorY : s.y));
                        if (dx < 1 && dy < 1 && f.axis && f.kind == 1) {
                            if (f.axis == 1)
                                alongX += 8;
                            else
                                alongY += 8;
                            continue;
                        }
                        float weight = f.kind == 1 ? .4f : 1.f;
                        if (dy < 1 && dx >= 31 && dx <= camera.width)
                            alongX += weight * 32 / dx;
                        if (dx < 1 && dy >= 31 && dy <= camera.height)
                            alongY += weight * 32 / dy;
                    }
                auto cached = doorAxes.find(cid);
                s.axis = std::abs(alongX - alongY) > .05f ? (alongY > alongX ? 2 : 1)
                                                          : (cached != doorAxes.end() ? cached->second : 1);
            }
            if (cid)
                doorAxes[cid] = s.axis;
        }
    // Overlay lighting above the game plane must use the exact same door
    // anchor and axis as the body, while keeping its own source lighting UVs.
    std::unordered_map<uint64_t, const Sprite *> doorBodies;
    for (auto &s : sprites)
        if (s.kind == 1 && s.plane == -4 && (s.color >> 24) != 0)
            doorBodies[field<uint64_t>(s.data, 0xc)] = &s;
    for (auto &s : sprites)
        if (s.kind == 1 && s.plane != -4) {
            auto body = doorBodies.find(field<uint64_t>(s.data, 0xc));
            if (body == doorBodies.end())
                continue;
            auto &base = *body->second;
            s.axis = base.axis;
            s.doorX = base.doorX;
            s.doorY = base.doorY;
            s.doorW = base.doorW;
            s.doorH = base.doorH;
        }
    // Grilles share the glass footprint instead of becoming a front billboard.
    // Directional panes need the same axis; full-tile glass encloses a cage.
    for (auto &s : sprites)
        if (s.kind == 7) {
            for (auto &glass : sprites)
                if (glass.level == s.level && (glass.kind == 2 || glass.kind == 6) &&
                    std::abs(s.x - glass.x) < 1 && std::abs(s.y - glass.y) < 1) {
                    s.axis = glass.kind == 2 ? glass.axis + 2 : 5;
                    break;
                }
        }
    // Reuse BYOND's space silhouette. Black unseen rooms must stay black;
    // the parallax plane alone is an unmasked, screen-sized repeating image.
    std::deque<Texture> spaceTextures;
    using Lookup = void *(__thiscall *)(void *, const char *);
    auto lookup = reinterpret_cast<Lookup>(
        reinterpret_cast<unsigned char *>(GetModuleHandleW(L"byondwin.dll")) + ByondAbi::lookupTexture);
    auto planeTexture = [&](int plane, int level, Texture &t) -> const unsigned char * {
        for (unsigned i = 0; i < roots; ++i) {
            auto icon = buffer + i * stride;
            if (field<int16_t>(icon, 0x24) != plane - level * planeRange ||
                !(field<unsigned char>(icon, 0x28) & 0x80))
                continue;
            auto name = field<const char *>(icon, 0x38);
            if (!name)
                return nullptr;
            auto plate = lookup(driver, name);
            if (!plate)
                return nullptr;
            auto gpu = field<IDirect3DTexture9 *>(plate, 0x2c);
            if (!gpu)
                return nullptr;
            D3DSURFACE_DESC size;
            if (FAILED(gpu->GetLevelDesc(0, &size)))
                return nullptr;
            t.gpu = gpu;
            gpu->AddRef();
            t.w = logical.Width;
            t.h = logical.Height;
            t.u0 = -float(field<int>(plate, 4)) / size.Width;
            t.v0 = -float(field<int>(plate, 8)) / size.Height;
            t.u1 = t.u0 + float(t.w) / size.Width;
            t.v1 = t.v0 + float(t.h) / size.Height;
            return icon;
        }
        return nullptr;
    };
    for (auto &s : sprites)
        if (s.plane == -20 || s.plane == -21 || s.plane == -10)
            s.color = 0;
    for (int level : levels) {
        spaceTextures.emplace_back();
        auto &art = spaceTextures.back();
        auto icon = planeTexture(-20, level, art);
        if (!icon)
            continue;
        spaceTextures.emplace_back();
        auto &mask = spaceTextures.back();
        if (!planeTexture(-21, level, mask))
            continue;
        Sprite space = {icon, 0, 0, -21, 0, 0xffffffff, -1};
        space.appearance = &art;
        space.level = level;
        space.spaceMask = &mask;
        sprites.push_back(space);
    }
    // Preserve native order at equal depth; only merge adjacent compatible sprites.
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                  camera.view.enabled ? 0xff000000 : 0xff080b10, 1.f, 0);
    bool ownScene = SUCCEEDED(device->BeginScene());
    if (camera.view.enabled) {
        static Texture sky;
        if (!sky.gpu || sky.checked != unsigned(reinterpret_cast<uintptr_t>(device))) {
            if (sky.gpu)
                sky.gpu->Release();
            sky = {};
            sky.w = sky.h = 480;
            if (SUCCEEDED(device->CreateTexture(480, 480, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &sky.gpu,
                                                nullptr))) {
                D3DLOCKED_RECT lock;
                if (SUCCEEDED(sky.gpu->LockRect(0, &lock, nullptr, 0))) {
                    for (unsigned y = 0; y < 480; ++y)
                        memcpy(static_cast<unsigned char *>(lock.pBits) + y * lock.Pitch,
                               spacePixels + y * 480, 480 * 4);
                    sky.gpu->UnlockRect(0);
                }
            }
            sky.checked = unsigned(reinterpret_cast<uintptr_t>(device));
        }
        if (sky.gpu) {
            Camera skyCamera = camera;
            skyCamera.clipDepth = 16384.f;
            activeCamera = &skyCamera;
            Batch skyBatch = {&sky, {}};
            float extent = 4096.f;
            auto point = [&](float x, float y, float z, float u, float v) {
                auto p = camera.project(camera.eyeX + x, camera.eyeY + y, camera.eyeZ + z, u, v, 0xffffffff);
                return p;
            };
            for (float z : {-extent, extent})
                quad(skyBatch, point(-extent, -extent, z, 0, 0), point(extent, -extent, z, 1, 0),
                     point(extent, extent, z, 1, 1), point(-extent, extent, z, 0, 1));
            for (int side = 0; side < 4; ++side) {
                float x0 = side == 3 ? extent : -extent, y0 = side == 1 ? extent : -extent,
                      x1 = side < 2 ? extent : x0, y1 = side >= 2 ? extent : y0;
                quad(skyBatch, point(x0, y0, extent, 0, 0), point(x1, y1, extent, 1, 0),
                     point(x1, y1, -extent, 1, 1), point(x0, y0, -extent, 0, 1));
            }
            for (auto &v : skyBatch.vertices) {
                v.x = (v.x + .5f) * desc.Width / logical.Width - .5f;
                v.y = (v.y + .5f) * desc.Height / logical.Height - .5f;
                v.z = .99999f;
            }
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetTexture(0, sky.gpu);
            if (!skyBatch.vertices.empty())
                device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, unsigned(skyBatch.vertices.size() / 3),
                                        skyBatch.vertices.data(), sizeof(Vertex));
            activeCamera = &camera;
        }
    }
    std::deque<Texture> textTextures;
    for (int pass = 0; pass < 4; ++pass) {
        if (pass == 3 && camera.view.enabled && heldArt && handSide) {
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetPixelShader(nullptr);
            device->SetFVF(fvf);
            device->SetTexture(0, heldArt->gpu);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            float side = handSide == 1 ? -6.f : 6.f, half = 7.f * heldArt->w / heldArt->h;
            auto item = [&](float x, float y, float z, float u, float v) {
                Vertex vertex = {};
                vertex.cx = x;
                vertex.cy = y;
                vertex.cz = z;
                vertex.u = u;
                vertex.v = v;
                vertex.color = heldColor;
                vertex.cameraVertex = true;
                camera.projectEye(vertex);
                return vertex;
            };
            Batch hand = {heldArt, {}};
            // Long items point inward/upward from the selected hand. Source
            // artwork stays a sprite, with perspective rather than a HUD copy.
            float inward = handSide == 1 ? 1.f : -1.f;
            auto held = [&](float u, float v) {
                float along = (u - .5f) * half * 2, across = (v - .5f) * 10;
                return item(side + inward * (along * .72f + across * .69f),
                            -14 + along * .69f - across * .72f, 15 + along * .35f, u, v);
            };
            if (handSide == 1)
                quad(hand, held(0, 0), held(1, 0), held(1, 1), held(0, 1));
            else
                quad(hand, held(1, 0), held(0, 0), held(0, 1), held(1, 1));
            float sx = float(desc.Width) / logical.Width, sy = float(desc.Height) / logical.Height;
            for (auto &vertex : hand.vertices) {
                vertex.x = (vertex.x + .5f) * sx - .5f;
                vertex.y = (vertex.y + .5f) * sy - .5f;
            }
            if (!hand.vertices.empty())
                device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, unsigned(hand.vertices.size() / 3),
                                        hand.vertices.data(), sizeof(Vertex));
        }
        bool hud = pass == 3;
        std::vector<Batch> batches;
        std::vector<const Sprite *> ordered;
        for (auto &s : sprites)
            if ((s.plane >= 40) == hud && (s.color >> 24) != 0) {
                if (camera.view.enabled && s.spaceMask)
                    continue; // The distant sky replaces the flat native space mask.
                if (s.plane == -20 && s.level != currentLevel)
                    continue;
                if (s.plane == 10 || s.plane == 11)
                    continue;
                bool text = s.plane == 34 || s.plane == 35;
                if (text != (pass == 2) && !hud)
                    continue;
                bool transparent = s.plane == -10 || s.kind == 2 || s.kind == 6 || s.kind == 7 ||
                                   s.kind == 11 || s.windowDoor || (s.kind != 37 && (s.color >> 24) < 255);
                if (hud || text || transparent == (pass == 1))
                    ordered.push_back(&s);
            }
        if (pass == 1)
            std::stable_sort(ordered.begin(), ordered.end(), [&](const Sprite *a, const Sprite *b) {
                if (!camera.view.enabled)
                    return a->y < b->y;
                auto distance = [&](const Sprite *s) {
                    float x = s->x + 16 - camera.eyeX, y = s->y + 16 - camera.eyeY;
                    return x * x + y * y;
                };
                return distance(a) > distance(b);
            });
        if (pass == 0)
            std::stable_sort(ordered.begin(), ordered.end(), [](const Sprite *a, const Sprite *b) {
                if (a->level != b->level)
                    return a->level > b->level;
                bool spaceA = a->plane == -20 || a->plane == -21, spaceB = b->plane == -20 || b->plane == -21;
                return spaceA && !spaceB;
            });
        for (auto entry : ordered) {
            auto &s = *entry;
            unsigned image = field<unsigned>(s.data, 0);
            Texture *t = nullptr;
            if (s.plane == 34 || s.plane == 35 || (s.plane >= 40 && field<const char *>(s.data, 0x18))) {
                textTextures.emplace_back();
                if (nativeText(driver, s, textTextures.back()))
                    t = &textTextures.back();
            } else
                t = s.appearance ? s.appearance : texture(device, icons, pixels, image);
            if (!t)
                continue;
            // Keep the floor smooth to avoid shimmer; sharpen furniture and
            // wall fixtures without changing the final-resolution rendering.
            bool surface = s.plane == -6 || s.plane == -5 || s.kind == 6 || s.kind == 7;
            bool unlit = s.plane == -20 || s.plane == -21 || s.plane == 25 || s.plane >= 34;
            if (batches.empty() || batches.back().texture != t || batches.back().surface != surface ||
                batches.back().unlit != unlit || batches.back().level != s.level ||
                batches.back().spaceMask != s.spaceMask || batches.back().emissive != s.emissive ||
                batches.back().lava != s.lava)
                batches.push_back({t, {}, surface, unlit, s.level, s.spaceMask, s.emissive, s.lava});
            auto &batch = batches.back();
            size_t firstVertex = batch.vertices.size();
            if (camera.view.enabled && selfCid && field<uint64_t>(s.data, 0xc) == selfCid && s.plane < 34) {
                float rx = std::cos(camera.view.yaw), ry = std::sin(camera.view.yaw), fx = ry, fy = -rx;
                auto leg = [&](float x, float z, float u, float v) {
                    auto p = camera.project(camera.eyeX + rx * x + fx * 4, camera.eyeY + ry * x + fy * 4, z,
                                            u, v, s.color);
                    p.lu = camera.eyeX / lightWidth;
                    p.lv = camera.eyeY / lightHeight;
                    return p;
                };
                quad(batch, leg(-8, 17, .25f, .55f), leg(8, 17, .75f, .55f), leg(8, 0, .75f, 1),
                     leg(-8, 0, .25f, 1));
            } else
                sprite(batch, s, camera);
            pickTriangles(batch, firstVertex, s, camera, aim, aimDepth);
        }
        if (camera.view.enabled && pass == 0) {
            // Original metal wall panel, repeated above actual station floors.
            static Texture ceiling;
            if (!ceiling.gpu || ceiling.checked != unsigned(reinterpret_cast<uintptr_t>(device))) {
                if (ceiling.gpu)
                    ceiling.gpu->Release();
                ceiling = {};
                ceiling.w = ceiling.h = 32;
                if (SUCCEEDED(device->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                    &ceiling.gpu, nullptr))) {
                    D3DLOCKED_RECT locked;
                    if (SUCCEEDED(ceiling.gpu->LockRect(0, &locked, nullptr, 0))) {
                        for (unsigned y = 0; y < 32; ++y)
                            memcpy(static_cast<unsigned char *>(locked.pBits) + y * locked.Pitch,
                                   metalPanelPixels + y * 32, 128);
                        ceiling.gpu->UnlockRect(0);
                    }
                }
                ceiling.checked = unsigned(reinterpret_cast<uintptr_t>(device));
            }
            if (ceiling.gpu) {
                std::vector<Batch> caveBatches;
                batches.push_back({&ceiling, {}, true, false, currentLevel, nullptr});
                auto &batch = batches.back();
                std::vector<std::pair<float, float>> roofCells;
                for (auto &s : sprites)
                    if (s.plane == -6 && s.level >= currentLevel) {
                        if (s.level != currentLevel) {
                            bool upperFloor = false;
                            for (auto &upper : sprites)
                                if (upper.plane == -6 && upper.level == currentLevel &&
                                    std::abs(upper.x - s.x) < 1 && std::abs(upper.y - s.y) < 1) {
                                    upperFloor = true;
                                    break;
                                }
                            if (upperFloor)
                                continue;
                        }
                        if (std::find(roofCells.begin(), roofCells.end(), std::make_pair(s.x, s.y)) !=
                            roofCells.end())
                            continue;
                        auto art = texture(device, icons, pixels, field<unsigned>(s.data, 0));
                        if (!art)
                            continue;
                        if (s.lava)
                            continue;
                        bool natural =
                            std::binary_search(std::begin(naturalFrames), std::end(naturalFrames), art->hash);
                        Texture *roof = &ceiling;
                        float roofHeight = 48.f;
                        if (natural) {
                            // Only bridge natural walls on opposite sides of a cave;
                            // a lone cliff must not turn outdoor terrain into a room.
                            const Sprite *closest = nullptr;
                            float distance = 1e30f;
                            bool west = false, east = false, north = false, south = false;
                            for (auto &wall : sprites)
                                if (wall.rock && wall.level == s.level) {
                                    float dx = wall.x - s.x, dy = wall.y - s.y;
                                    if (std::abs(dy) < 33 && std::abs(dx) < 192) {
                                        west |= dx < 0;
                                        east |= dx > 0;
                                    }
                                    if (std::abs(dx) < 33 && std::abs(dy) < 192) {
                                        north |= dy < 0;
                                        south |= dy > 0;
                                    }
                                    float d = dx * dx + dy * dy;
                                    if (d < distance) {
                                        distance = d;
                                        closest = &wall;
                                    }
                                }
                            if (!closest || !((west && east) || (north && south)))
                                continue;
                            roof = texture(device, icons, pixels, field<unsigned>(closest->data, 0));
                            if (!roof)
                                continue;
                            roofHeight = 128.f;
                        } else if (std::binary_search(std::begin(outdoorFrames), std::end(outdoorFrames),
                                                      art->hash))
                            continue;
                        roofCells.push_back({s.x, s.y});
                        Batch cave = {roof, {}, true, false, currentLevel, nullptr};
                        auto &roofBatch = natural ? cave : batch;
                        auto p = [&](float x, float y, float u, float v) {
                            auto vertex = camera.project(x, y, roofHeight, u, v, 0xffffffff);
                            vertex.lu = x / lightWidth;
                            vertex.lv = y / lightHeight;
                            return vertex;
                        };
                        size_t start = roofBatch.vertices.size();
                        quad(roofBatch, p(s.x, s.y, 0, 0), p(s.x + art->w, s.y, 1, 0),
                             p(s.x + art->w, s.y + art->h, 1, 1), p(s.x, s.y + art->h, 0, 1));
                        AimHit blocked;
                        float ceilingDepth = aimDepth;
                        pickTriangles(roofBatch, start, s, camera, blocked, ceilingDepth);
                        if (natural)
                            caveBatches.push_back(std::move(cave));
                        if (ceilingDepth < aimDepth) {
                            aimDepth = ceilingDepth;
                            aim.cid = 0;
                        }
                    }
                for (auto &cave : caveBatches)
                    batches.push_back(std::move(cave));
            }
        }
        if (camera.view.enabled && pass == 0) {
            static Texture fogPlate;
            if (!fogPlate.gpu || fogPlate.checked != unsigned(reinterpret_cast<uintptr_t>(device))) {
                if (fogPlate.gpu)
                    fogPlate.gpu->Release();
                fogPlate = {};
                fogPlate.w = fogPlate.h = 1;
                if (SUCCEEDED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                    &fogPlate.gpu, nullptr))) {
                    D3DLOCKED_RECT lock;
                    if (SUCCEEDED(fogPlate.gpu->LockRect(0, &lock, nullptr, 0))) {
                        *static_cast<DWORD *>(lock.pBits) = 0xffffffff;
                        fogPlate.gpu->UnlockRect(0);
                    }
                }
                fogPlate.checked = unsigned(reinterpret_cast<uintptr_t>(device));
            }
            if (fogPlate.gpu) {
                batches.push_back({&fogPlate, {}, true, true, currentLevel, nullptr});
                auto &fog = batches.back();
                auto point = [&](float x, float y, float z) {
                    return camera.project(x, y, z, 0, 0, 0xff000000);
                };
                float width = float(logical.Width), height = float(logical.Height);
                for (int side = 0; side < 4; ++side) {
                    float x0 = side == 3 ? width : 0, y0 = side == 1 ? height : 0;
                    float x1 = side < 2 ? width : x0, y1 = side >= 2 ? height : y0;
                    quad(fog, point(x0, y0, 96), point(x1, y1, 96), point(x1, y1, -96), point(x0, y0, -96));
                }
            }
        }
        device->SetRenderState(D3DRS_ZENABLE, pass >= 2 ? FALSE : TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, pass == 0 ? TRUE : FALSE);
        for (auto &batch : batches) {
            if (batch.vertices.empty())
                continue;
            // The translucent space silhouette is a background, never an
            // occluder. Its fractional edge alpha must not compete with glass.
            device->SetRenderState(D3DRS_ZWRITEENABLE, pass == 0 && !batch.spaceMask ? TRUE : FALSE);
            float sx = float(desc.Width) / logical.Width, sy = float(desc.Height) / logical.Height;
            for (auto &vertex : batch.vertices) {
                vertex.x = (vertex.x + .5f) * sx - .5f;
                vertex.y = (vertex.y + .5f) * sy - .5f;
            }
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetTexture(0, batch.texture->gpu);
            auto &illumination = batch.surface ? levelSurfaceLighting : levelLighting;
            auto light = illumination.find(batch.level);
            bool hasLighting = light != illumination.end() && light->second;
            device->SetTexture(1, hasLighting ? light->second : nullptr);
            device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
            device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            device->SetTextureStageState(1, D3DTSS_COLOROP,
                                         hasLighting && !batch.unlit ? D3DTOP_MODULATE : D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
            device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
            if (batch.spaceMask) {
                device->SetTexture(1, batch.spaceMask->gpu);
                device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                device->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_TEXTURE);
            }
            device->SetPixelShader(nullptr);
            auto direct = directLights.find(batch.level);
            if (direct != directLights.end() && !batch.unlit && !batch.spaceMask) {
                device->SetTexture(1, direct->second.gpu);
                device->SetPixelShader(firstPersonLightingShader);
                device->SetPixelShaderConstantF(0, direct->second.uv, 1);
                float fogBounds[] = {float(logical.Width), float(logical.Height), 1.f / 32.f, 0};
                device->SetPixelShaderConstantF(1, fogBounds, 1);
                bool lava = batch.lava;
                float emission[] = {batch.emissive ? 1.f : 0.f, lava ? 1.f : 0.f, 0, 0};
                device->SetPixelShaderConstantF(2, emission, 1);
                float glow[] = {0, 0, 0, 0};
                float nearestLava = 1e30f;
                for (auto &pool : sprites)
                    if (pool.lava && pool.level == batch.level) {
                        float dx = pool.x + 16 - batch.vertices.front().lu * lightWidth,
                              dy = pool.y + 16 - batch.vertices.front().lv * lightHeight,
                              d = dx * dx + dy * dy;
                        if (d < nearestLava) {
                            nearestLava = d;
                            glow[0] = (pool.x + 16) / lightWidth;
                            glow[1] = (pool.y + 16) / lightHeight;
                            glow[2] = 1.f;
                        }
                    }
                device->SetPixelShaderConstantF(3, glow, 1);
                float radius = std::min(logical.Width, logical.Height) * .5f;
                float viewFog[] = {camera.eyeX / lightWidth, camera.eyeY / lightHeight,
                                   std::max(32.f, radius - 32.f), 1.f / 48.f};
                device->SetPixelShaderConstantF(4, viewFog, 1);
                float ambient[] = {naturalEnvironment ? .70f : 0.f, 0, 0, 0};
                device->SetPixelShaderConstantF(5, ambient, 1);
            }
            device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            if (FAILED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, unsigned(batch.vertices.size() / 3),
                                               batch.vertices.data(), sizeof(Vertex))))
                success = false;
            vertices += unsigned(batch.vertices.size());
        }
    }
    if (camera.view.enabled) {
        // The gap between two feet is transparent in the original sprite.
        // Treat the small region occupied by our feet as our body, so looking
        // down through that gap does not silently select the floor instead.
        if (selfCid && camera.view.pitch < -.65f) {
            float ownDepth = 4.f / std::cos(camera.view.pitch);
            float height = camera.eyeZ + ownDepth * std::sin(camera.view.pitch);
            bool body = height >= 0 && height <= 17;
            float groundDepth = camera.eyeZ / -std::sin(camera.view.pitch),
                  forward = groundDepth * std::cos(camera.view.pitch);
            if (!body && forward >= 0 && forward < 4) {
                ownDepth = groundDepth - .2f;
                body = true;
            }
            if (body && ownDepth <= aimDepth + .2f) {
                for (auto &s : sprites)
                    if (s.plane == -4 && field<uint64_t>(s.data, 0xc) == selfCid) {
                        aim.cid = selfCid;
                        aim.x = s.x + 12;
                        aim.y = s.y + 28;
                        memcpy(aim.icon, s.data, 216);
                        break;
                    }
            }
        }
        PublishAim(aim);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        float x = desc.Width * .5f - .5f, y = desc.Height * .5f - .5f;
        DWORD color = aim.cid ? 0xff9de6ad : 0xffeeeeee;
        struct CrossVertex {
            float x, y, z, rhw;
            DWORD color;
        };
        std::vector<CrossVertex> cross;
        auto bar = [&](float left, float top, float right, float bottom) {
            CrossVertex a = {left, top, 0, 1, color}, b = {right, top, 0, 1, color},
                        c = {right, bottom, 0, 1, color}, d = {left, bottom, 0, 1, color};
            cross.insert(cross.end(), {a, b, c, a, c, d});
        };
        bar(x - 12, y - 1, x - 4, y + 1);
        bar(x + 4, y - 1, x + 12, y + 1);
        bar(x - 1, y - 12, x + 1, y - 4);
        bar(x - 1, y + 4, x + 1, y + 12);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 8, cross.data(), sizeof(CrossVertex));
        // Passive north-up minimap: only native appearances already received
        // for this level, never unseen server tiles or another floor.
        float size = desc.Width * .22f, mapScale = size / logical.Width;
        float mapX = desc.Width - size - 12, mapY = 12, mapW = logical.Width * mapScale,
              mapH = logical.Height * mapScale;
        cross.clear();
        color = 0xe00b1015;
        bar(mapX - 3, mapY - 3, mapX + mapW + 3, mapY + mapH + 3);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, cross.data(), sizeof(CrossVertex));
        device->SetFVF(fvf);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        auto mapLight = directLights.find(currentLevel);
        if (mapLight != directLights.end()) {
            device->SetPixelShader(firstPersonLightingShader);
            device->SetTexture(1, mapLight->second.gpu);
            device->SetPixelShaderConstantF(0, mapLight->second.uv, 1);
            float bounds[] = {float(logical.Width), float(logical.Height), 1.f / 32.f, 0},
                  off[] = {0, 0, 0, 0};
            device->SetPixelShaderConstantF(1, bounds, 1);
            device->SetPixelShaderConstantF(2, off, 1);
            device->SetPixelShaderConstantF(3, off, 1);
            device->SetPixelShaderConstantF(4, off, 1);
            device->SetPixelShaderConstantF(5, off, 1);
        } else {
            auto mapIllumination = levelLighting.find(currentLevel);
            if (mapIllumination != levelLighting.end() && mapIllumination->second) {
                device->SetTexture(1, mapIllumination->second);
                device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
                device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
                device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
                device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
            }
        }
        for (auto &s : sprites)
            if (s.level == currentLevel && s.plane <= 6 && s.plane >= -6 && (s.color >> 24) && !s.spaceMask) {
                auto t =
                    s.appearance ? s.appearance : texture(device, icons, pixels, field<unsigned>(s.data, 0));
                if (!t || !t->gpu)
                    continue;
                float left = std::max(0.f, s.x), top = std::max(0.f, s.y),
                      right = std::min(float(logical.Width), s.x + t->w),
                      bottom = std::min(float(logical.Height), s.y + t->h);
                if (left >= right || top >= bottom)
                    continue;
                auto p = [&](float px, float py) {
                    return Vertex{mapX + px * mapScale,
                                  mapY + py * mapScale,
                                  0,
                                  1,
                                  s.color,
                                  t->u0 + (px - s.x) / t->w * (t->u1 - t->u0),
                                  t->v0 + (py - s.y) / t->h * (t->v1 - t->v0),
                                  px / lightWidth,
                                  py / lightHeight};
                };
                auto a = p(left, top), b = p(right, top), c = p(right, bottom), d = p(left, bottom);
                Vertex tile[] = {a, b, c, a, c, d};
                device->SetTexture(0, t->gpu);
                device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, tile, sizeof(Vertex));
            }
        device->SetPixelShader(nullptr);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        float playerX = mapX + camera.eyeX * mapScale, playerY = mapY + camera.eyeY * mapScale,
              rx = std::cos(camera.view.yaw), ry = std::sin(camera.view.yaw), fx = ry, fy = -rx;
        CrossVertex pointer[] = {{playerX + fx * 8, playerY + fy * 8, 0, 1, 0xff7dff98},
                                 {playerX - fx * 4 + rx * 4, playerY - fy * 4 + ry * 4, 0, 1, 0xff7dff98},
                                 {playerX - fx * 4 - rx * 4, playerY - fy * 4 - ry * 4, 0, 1, 0xff7dff98}};
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, pointer, sizeof(CrossVertex));
    }
    activeCamera = nullptr;
    for (auto &t : textTextures)
        if (t.gpu)
            t.gpu->Release();
    for (auto &t : spaceTextures)
        if (t.gpu)
            t.gpu->Release();
    for (auto &light : directLights)
        light.second.gpu->Release();
    if (ownScene)
        device->EndScene();
    device->SetDepthStencilSurface(nullptr);
    if (oldTarget)
        device->SetRenderTarget(0, oldTarget);
    device->SetDepthStencilSurface(oldDepth);
    state->Apply();
    if (success && vertices) {
        success = SUCCEEDED(device->StretchRect(work, nullptr, target, nullptr, D3DTEXF_POINT));
        if (success)
            capture(device, target, desc);
    }
    work->Release();
    depth->Release();
    if (oldDepth)
        oldDepth->Release();
    if (oldTarget)
        oldTarget->Release();
    state->Release();
    target->Release();
    LARGE_INTEGER renderEnded, frequency;
    QueryPerformanceCounter(&renderEnded);
    QueryPerformanceFrequency(&frequency);
    static double elapsedMs = 0;
    static unsigned samples = 0;
    static ULONGLONG intervalStart = GetTickCount64();
    elapsedMs += 1000.0 * (renderEnded.QuadPart - renderStarted.QuadPart) / frequency.QuadPart;
    ++samples;
    if (++frames == 1 || frames % 120 == 0) {
        if (diagnostic == INVALID_HANDLE_VALUE) {
            wchar_t path[MAX_PATH];
            GetEnvironmentVariableW(L"BS220_VOLUME_LOG", path, MAX_PATH);
            diagnostic = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_ALWAYS, 0, nullptr);
        }
        unsigned kinds[15] = {}, verticalDoors = 0;
        for (auto &s : sprites)
            if (s.kind < 15) {
                ++kinds[s.kind];
                if (s.kind == 1 && s.axis == 2 && (s.color >> 24) != 0)
                    ++verticalDoors;
            }
        auto now = GetTickCount64();
        char line[400];
        sprintf_s(line,
                  "volume frame=%u sprites=%u floors=%u vertices=%u size=%ux%u captured=%d success=%d "
                  "doors=%u verticalDoors=%u glass=%u surfaces=%u seats=%u renderMs=%.2f presentFps=%.1f\r\n",
                  frames, unsigned(sprites.size()), floors, vertices, desc.Width, desc.Height,
                  int(savedFrame), int(success), kinds[1], verticalDoors, kinds[2] + kinds[6], kinds[3],
                  unsigned(seating.size()), elapsedMs / samples,
                  now > intervalStart ? 1000.0 * samples / (now - intervalStart) : 0.0);
        DWORD written;
        if (diagnostic != INVALID_HANDLE_VALUE)
            WriteFile(diagnostic, line, DWORD(strlen(line)), &written, nullptr);
        elapsedMs = 0;
        samples = 0;
        intervalStart = now;
    }
    return success && vertices > 0;
}
