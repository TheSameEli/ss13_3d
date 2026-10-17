// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
// Small CPU check for the camera/near clipping/picking, without a BYOND process.
#include "volume.cpp"
static MSG postedMovement;
static unsigned postedCount = 0;
static BOOL WINAPI capturePostedMovement(HWND window, UINT message, WPARAM key, LPARAM flags) {
    ++postedCount;
    postedMovement = {};
    postedMovement.hwnd = window;
    postedMovement.message = message;
    postedMovement.wParam = key;
    postedMovement.lParam = flags;
    return TRUE;
}
#define PostMessageW capturePostedMovement
#define diagnostic inputDiagnostic
#include "first_person.cpp"
#undef diagnostic
#undef PostMessageW
#include <cassert>
#include <string>
static HCURSOR lastCursor = reinterpret_cast<HCURSOR>(1);
static HCURSOR WINAPI captureCursor(HCURSOR cursor) {
    lastCursor = cursor;
    return nullptr;
}
int main() {
    auto savedCursorFunction = nativeSetCursor;
    nativeSetCursor = captureCursor;
    enabled = true;
    locked = true;
    firstPersonSetCursor(reinterpret_cast<HCURSOR>(7));
    assert(lastCursor == nullptr);
    locked = false;
    firstPersonSetCursor(reinterpret_cast<HCURSOR>(7));
    assert(lastCursor == reinterpret_cast<HCURSOR>(7));
    enabled = false;
    nativeSetCursor = savedCursorFunction;
    assert(nativeMouseFlags(MK_LBUTTON, MK_CONTROL, false) == 9);
    assert(nativeMouseFlags(MK_LBUTTON, MK_SHIFT, false) == 17);
    assert(nativeMouseFlags(MK_RBUTTON, MK_CONTROL | MK_SHIFT, true) == 58);
    // A non-drawable native actor parent owns the rotation. Garment/body
    // children contain no matrix and must inherit it before compositing.
    unsigned char actorTree[432] = {};
    auto put = [&](unsigned offset, unsigned value) { memcpy(actorTree + offset, &value, 4); };
    put(0, 65535);
    put(8, 0x200);
    put(12, 1);
    put(36, unsigned(-4));
    put(44, ~0u);
    float one = 1, negative = -1;
    memcpy(actorTree + 0x90, &one, 4);
    memcpy(actorTree + 0x98, &negative, 4);
    uint16_t firstChild = 1, lastChild = 2;
    memcpy(actorTree + 0xae, &firstChild, 2);
    memcpy(actorTree + 0xb0, &lastChild, 2);
    put(216, 1);
    put(216 + 12, 1);
    put(216 + 36, unsigned(-4));
    put(216 + 44, ~0u);
    std::vector<Sprite> actorSprites;
    std::vector<unsigned char> actorSeen(2);
    walk(actorTree, 1, 2, 0, 0, 0, ~0u, actorSprites, actorSeen, 0);
    assert(actorSprites.size() == 1 && actorSprites[0].lying && actorSprites[0].lyingTurn < -1.5f);
    Camera camera = {640, 480};
    camera.view = {true, 0, 0};
    camera.eyeX = 0;
    camera.eyeY = 0;
    camera.eyeZ = 16;
    activeCamera = &camera;
    auto middle = camera.project(0, -32, 16, 0, 0, ~0u);
    assert(std::abs(middle.x - 319.5f) < .001f && std::abs(middle.y - 239.5f) < .001f);
    camera.view.yaw = 1.570796327f;
    middle = camera.project(32, 0, 16, 0, 0, ~0u);
    assert(std::abs(middle.x - 319.5f) < .001f);
    camera.view.yaw = 0;
    Texture art;
    art.w = art.h = 32;
    art.alpha.assign(1024, 255);
    Batch batch = {&art, {}};
    auto p = [&](float x, float y, float z, float u, float v) { return camera.project(x, y, z, u, v, ~0u); };
    quad(batch, p(-16, -32, 32, 0, 0), p(16, -32, 32, 1, 0), p(16, -32, 0, 1, 1), p(-16, -32, 0, 0, 1));
    unsigned char raw[216] = {};
    *reinterpret_cast<uint64_t *>(raw + 0xc) = 0x12300000001ull;
    Sprite sprite = {raw, -16, -32, -4, 0, ~0u, -1};
    AimHit hit;
    float distance = 1e30f;
    pickTriangles(batch, 0, sprite, camera, hit, distance);
    assert(hit.cid == 0x12300000001ull && std::abs(distance - 32) < .001f);
    art.alpha.assign(1024, 0);
    hit = {};
    distance = 1e30f;
    pickTriangles(batch, 0, sprite, camera, hit, distance);
    assert(!hit.cid);
    batch.vertices.clear();
    quad(batch, p(-16, 4, 32, 0, 0), p(16, -16, 32, 1, 0), p(16, -16, 0, 1, 1), p(-16, 4, 0, 0, 1));
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices)
        assert(v.cz >= 1.249f && std::isfinite(v.x) && std::isfinite(v.y));
    batch.vertices.clear();
    quad(batch, p(-16, 4, 32, 0, 0), p(16, 4, 32, 1, 0), p(16, 4, 0, 1, 1), p(-16, 4, 0, 0, 1));
    assert(batch.vertices.empty());
    enabled = true;
    locked = true;
    lookWindow = reinterpret_cast<HWND>(1);
    for (int turn = 0; turn < 4; ++turn) {
        yaw = turn * 1.570796327f;
        const unsigned ring[] = {'W', 'D', 'S', 'A'};
        for (int index = 0; index < 4; ++index) {
            unsigned key = ring[index];
            MSG down = {};
            down.message = WM_KEYDOWN;
            down.wParam = key;
            HandleFirstPersonMessage(down);
            assert(down.wParam == key && mappedKeys[key] == 1);
            const unsigned directions[] = {1, 2, 4, 8};
            assert(postedMovement.wParam == directions[(index + turn) % 4] &&
                   postedMovement.message == movementStepMessage && down.message == WM_NULL);
            MSG queued = postedMovement;
            HandleFirstPersonMessage(queued);
            assert(queued.message == WM_NULL && !movementStepPending);
            unsigned beforeRelease = postedCount;
            MSG repeat = {};
            repeat.message = WM_KEYDOWN;
            repeat.wParam = key;
            HandleFirstPersonMessage(repeat);
            assert(mappedKeys[key] == 1);
            MSG up = {};
            up.message = WM_KEYUP;
            up.wParam = key;
            HandleFirstPersonMessage(up);
            assert(up.wParam == key && !mappedKeys[key] && !worldMovement);
            assert(postedCount == beforeRelease && up.message == WM_NULL);
            up.message = WM_KEYUP;
            up.wParam = key;
            HandleFirstPersonMessage(up);
            assert(up.message == WM_NULL && !mappedKeys[key]);
        }
    }
    const unsigned compass[] = {1, 3, 2, 6, 4, 12, 8, 9};
    for (unsigned i = 0; i < 8; ++i) {
        assert(cameraMovement(i * .7853981634f, 1, 0) == compass[i]);
        assert(cameraMovement(i * .7853981634f, 0, 1) == compass[(i + 2) % 8]);
        assert(cameraMovement(i * .7853981634f, -1, 0) == compass[(i + 4) % 8]);
        assert(cameraMovement(i * .7853981634f, 0, -1) == compass[(i + 6) % 8]);
    }
    assert(cameraMovement(0, 1, 1) == 3 && cameraMovement(0, 0, 0) == 0);
    yaw = 0;
    mappedKeys['W'] = 1;
    updateWorldMovement();
    for (int i = 0; i < 10; ++i) {
        followMovementYawFromPhysical(1);
        assert(worldMovement == 1);
    }
    // A delayed/native arrow must never become a physical movement key.
    MSG arrow = {};
    arrow.message = WM_KEYDOWN;
    arrow.wParam = VK_DOWN;
    HandleFirstPersonMessage(arrow);
    assert(!mappedKeys[VK_DOWN]);
    followMovementYawFromPhysical(0);
    assert(worldMovement == 0);
    unsigned idlePosts = postedCount;
    yaw = 2.f;
    followMovementYawFromPhysical(0);
    assert(worldMovement == 0 && postedCount == idlePosts);
    for (unsigned direction : compass)
        assert(movementCommand(direction));
    assert(!movementCommand(0));
    assert(nativeMovementCommand("KeyDown W 0 0 100 100"));
    assert(nativeMovementCommand("KeyDown \"W\" 0 0 100 100"));
    assert(nativeMovementCommand("KeyDown \"North\" 0 0 100 100"));
    assert(!nativeMovementCommand("KeyUp \"W\" 0 0 100 100"));
    assert(nativeMovementCommand("KeyDown North 0 0 100 100"));
    assert(nativeMovementCommand(".north"));
    assert(!nativeMovementCommand("KeyUp W 0 0 100 100"));
    assert(!nativeMovementCommand("KeyDown X 0 0 100 100"));
    assert(!nativeMovementCommand("say hi"));
    locked = false;
    enabled = false;
    art.alpha.assign(1024, 255);
    sprite.appearance = &art;
    sprite.color = ~0u;
    for (int kind : {21, 22, 23}) {
        sprite.kind = static_cast<unsigned char>(kind);
        batch.vertices.clear();
        ::sprite(batch, sprite, camera);
        float nearZ = 1e30f, farZ = 0;
        for (auto &v : batch.vertices) {
            nearZ = std::min(nearZ, v.cz);
            farZ = std::max(farZ, v.cz);
        }
        assert(farZ - nearZ >= 15.f);
        for (auto &v : batch.vertices) {
            float worldY = camera.eyeY - v.cz;
            assert(worldY >= sprite.y - .01f && worldY <= sprite.y + 32.01f);
        }
    }
    sprite.kind = 25;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    printf("Computer vertices=%zu\n", batch.vertices.size());
    assert(batch.vertices.size() >= 24 && batch.vertices.size() <= 60);
    for (size_t i = 0; i + 6 < batch.vertices.size(); ++i)
        assert(batch.vertices[i].u == batch.vertices[0].u && batch.vertices[i].v == batch.vertices[0].v);
    sprite.kind = 8;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty() && batch.vertices.size() < 100);
    // A side-facing middle section has only a six-pixel-wide opaque strip.
    // Its UVs must sample that fabric, never the transparent canvas margins.
    Texture narrow;
    narrow.w = narrow.h = 32;
    narrow.alpha.assign(1024, 0);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 7; x < 13; ++x)
            narrow.alpha[y * 32 + x] = 255;
    seatFabric(narrow, 3);
    assert(narrow.seatUV[0] > 7.f / 32 && narrow.seatUV[2] < 13.f / 32);
    assert(narrow.seatUV[1] < 1.f / 32 && narrow.seatUV[3] > 31.f / 32);
    sprite.kind = 7;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    for (auto &v : batch.vertices)
        assert((v.color >> 24) < 100);
    for (int kind : {27, 28, 29}) {
        sprite.kind = static_cast<unsigned char>(kind);
        batch.vertices.clear();
        ::sprite(batch, sprite, camera);
        float lowest = 1e30f, highest = -1e30f;
        for (auto &v : batch.vertices) {
            lowest = std::min(lowest, v.cy);
            highest = std::max(highest, v.cy);
        }
        assert(!batch.vertices.empty());
        assert(std::abs(highest - lowest - (kind == 29 ? 18.f : kind == 28 ? 12.f : 8.f)) < .01f);
    }
    sprite.y = -96;
    sprite.x = -16;
    sprite.kind = 31;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    float gateLow = 1e30f, gateHigh = -1e30f;
    for (auto &v : batch.vertices) {
        gateLow = std::min(gateLow, v.cy);
        gateHigh = std::max(gateHigh, v.cy);
    }
    assert(std::abs(gateHigh - gateLow - 48) < .01f);
    sprite.kind = 40;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    float capsuleFront = 1e30f, capsuleBack = -1e30f;
    for (auto &v : batch.vertices) {
        capsuleFront = std::min(capsuleFront, v.cz);
        capsuleBack = std::max(capsuleBack, v.cz);
    }
    assert(std::abs(capsuleBack - capsuleFront - 24) < .01f);
    sprite.kind = 2;
    sprite.panelEdge = 3;
    sprite.appearance = &narrow;
    batch.texture = &narrow;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices)
        assert(v.u > 7.f / 32 && v.u < 13.f / 32);
    sprite.panelEdge = 0;
    sprite.appearance = &art;
    batch.texture = &art;
    sprite.kind = 14;
    sprite.x = -16;
    sprite.y = -92;
    sprite.doorX = -16;
    sprite.doorY = -64;
    sprite.doorW = sprite.doorH = 32;
    footprints = {{-16, -96, 5, 0, 0, 32, 32}};
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    float panelTop = -1e30f;
    for (auto &v : batch.vertices)
        panelTop = std::max(panelTop, v.cy + camera.eyeZ);
    assert(std::abs(panelTop - 27) < .01f);
    sprite.x = 12;
    sprite.y = -96;
    sprite.doorX = -16;
    sprite.doorY = -96;
    footprints = {{16, -96, 5, 0, 0, 32, 32}};
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices)
        assert(std::abs(v.cx - 15.75f) < .01f);
    footprints.clear();
    sprite.x = -16;
    sprite.doorW = sprite.doorH = 0;
    *reinterpret_cast<uint32_t *>(raw + 0xc) = 1;
    sprite.kind = 0;
    sprite.lying = true;
    sprite.lyingTurn = -1.570796327f;
    sprite.y = -96;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty());
    float proneBottom = 1e30f, proneTop = -1e30f;
    float proneNear = 1e30f, proneFar = -1e30f;
    for (auto &v : batch.vertices) {
        proneBottom = std::min(proneBottom, v.cy);
        proneTop = std::max(proneTop, v.cy);
        proneNear = std::min(proneNear, v.cz);
        proneFar = std::max(proneFar, v.cz);
    }
    assert(std::abs(proneBottom - (2 - camera.eyeZ)) < .01f);
    assert(std::abs(proneTop - proneBottom - 32 * .406736643f) < .01f);
    assert(std::abs(proneFar - proneNear - 32 * .913545458f) < .01f);
    // Outdoor sky must survive clipping when it is thousands of pixels away.
    Camera distant = camera;
    distant.view.pitch = 1.570796327f;
    distant.clipDepth = 16384;
    activeCamera = &distant;
    batch.vertices.clear();
    quad(batch, distant.project(-4096, -4096, 4120, 0, 0, ~0u), distant.project(4096, -4096, 4120, 1, 0, ~0u),
         distant.project(4096, 4096, 4120, 1, 1, ~0u), distant.project(-4096, 4096, 4120, 0, 1, ~0u));
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices)
        assert(std::isfinite(v.x) && std::isfinite(v.y) && v.cz > 4000);
    activeCamera = &camera;
    sprite.lying = false;
    sprite.kind = 0;
    sprite.plane = -6;
    sprite.lava = true;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(batch.vertices.size() == 6); // One continuous surface; no walls between liquid cells.
    bool recessed = false;
    for (auto &v : batch.vertices)
        if (std::abs(v.cy + camera.eyeZ + 3) < .01f)
            recessed = true;
    assert(recessed);
    camera.view.pitch = .65f;
    sprite.plane = -5;
    sprite.lava = false;
    sprite.rock = true;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    bool tallRock = false;
    for (auto &v : batch.vertices)
        if (std::abs(v.cy * std::cos(camera.view.pitch) + v.cz * std::sin(camera.view.pitch) + camera.eyeZ -
                     128) < .01f)
            tallRock = true;
    assert(tallRock);
    Texture rockArt;
    rockArt.w = rockArt.h = 32;
    rockArt.alpha.assign(1024, 0);
    for (unsigned y = 3; y < 30; ++y)
        for (unsigned x = 3; x < 30; ++x)
            rockArt.alpha[y * 32 + x] = 255;
    sprite.appearance = &rockArt;
    batch.texture = &rockArt;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices) {
        assert(v.u > 3.f / 32 && v.u < 30.f / 32);
        assert(v.v > 3.f / 32 && v.v < 30.f / 32);
    }
    // A tall wall lamp must extend upward from its mount, never through the floor.
    Texture lamp;
    lamp.w = lamp.h = 32;
    lamp.alpha.assign(1024, 0);
    for (unsigned y = 2; y < 30; ++y)
        for (unsigned x = 14; x < 18; ++x)
            lamp.alpha[y * 32 + x] = 255;
    camera.view.pitch = 0;
    sprite.rock = false;
    sprite.kind = 16;
    sprite.plane = -4;
    sprite.x = -16;
    sprite.y = -92;
    sprite.doorX = -16;
    sprite.doorY = -64;
    sprite.doorW = sprite.doorH = 32;
    footprints = {{-16, -96, 5, 0, 0, 32, 32}};
    sprite.appearance = &lamp;
    batch.texture = &lamp;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices)
        assert(v.cy + camera.eyeZ >= 1.99f);
    sprite.doorW = sprite.doorH = 0;
    footprints.clear();
    Texture openArt;
    openArt.w = openArt.h = 32;
    openArt.alpha.assign(1024, 0);
    for (unsigned y = 29; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x)
            openArt.alpha[y * 32 + x] = 255;
    for (unsigned y = 25; y < 32; ++y)
        for (unsigned x = 0; x < 2; ++x)
            openArt.alpha[y * 32 + x] = 255;
    camera.view.pitch = 0;
    sprite.rock = false;
    sprite.kind = 1;
    sprite.plane = -4;
    sprite.windowDoor = true;
    sprite.doorOpen = true;
    sprite.panelEdge = 1;
    sprite.appearance = &openArt;
    batch.texture = &openArt;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    unsigned raised = 0;
    for (auto &v : batch.vertices)
        if (v.cy + camera.eyeZ > 1)
            ++raised;
    assert(raised > 0 && raised <= 3); // Only the jamb rises; the open guide stays down.
    sprite.windowDoor = false;
    sprite.doorOpen = false;
    sprite.panelEdge = 0;
    sprite.kind = 17;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(!batch.vertices.empty());
    for (auto &v : batch.vertices)
        assert(std::abs(v.cy + camera.eyeZ - 1.f) < .01f);
    // Looking squarely through a glass cell must draw its front once,
    // rather than stacking its hidden back/top/internal faces over the view.
    sprite.kind = 6;
    sprite.plane = -5;
    sprite.x = -16;
    sprite.y = -96;
    sprite.appearance = &art;
    batch.texture = &art;
    batch.vertices.clear();
    footprints.clear();
    ::sprite(batch, sprite, camera);
    assert(batch.vertices.size() == 6);
    for (auto &v : batch.vertices)
        assert((v.color >> 24) < 50);
    // Ordinary chairs stay a single upright sprite, without extruded sides.
    sprite.kind = 51;
    sprite.plane = -4;
    sprite.x = -16;
    sprite.y = -96;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(batch.vertices.size() == 6);
    // A charger rests on the received table surface, even as a flat sprite.
    sprite.kind = 53;
    sprite.x = -16;
    sprite.y = -96;
    sprite.appearance = &art;
    batch.texture = &art;
    footprints = {{-16, -96, 3, 0, 0, 32, 32}};
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(batch.vertices.size() == 6);
    for (auto &v : batch.vertices)
        assert(v.cy + camera.eyeZ >= 12.24f);
    footprints.clear();
    sprite.kind = 1;
    sprite.doubleDoor = true;
    sprite.axis = 2;
    sprite.doorX = 0;
    sprite.doorY = -96;
    sprite.doorW = 64;
    sprite.doorH = 32;
    batch.vertices.clear();
    ::sprite(batch, sprite, camera);
    assert(batch.vertices.size() == 6);
    for (auto &v : batch.vertices) {
        assert(std::abs(v.cx - 16) < .01f);
        assert(v.v >= .5f);
        assert(v.cy + camera.eyeZ >= -.01f && v.cy + camera.eyeZ <= 48.01f);
    }
    sprite.doubleDoor = false;
    assert(std::size(lavaFrames) > 0 && std::size(rockFrames) > 0 && std::size(naturalFrames) > 0);
    ID3DBlob *compiled = nullptr, *errors = nullptr;
    HRESULT shaderResult =
        D3DCompile(lightingShaderSource, strlen(lightingShaderSource), nullptr, nullptr, nullptr, "main",
                   "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled, &errors);
    if (errors) {
        printf("Shader: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
        errors->Release();
    }
    assert(SUCCEEDED(shaderResult));
    if (compiled)
        compiled->Release();
    puts("Distant sky, recessed lava, cave height and D3D9 lighting shader: OK");
    puts("Movement press/repeat/release in all four camera directions: OK");
    puts("First-person projection, near clipping and alpha-aware aiming: OK");
}
