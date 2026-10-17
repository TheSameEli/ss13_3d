// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#pragma once
#include <windows.h>
#include <cstdint>
#include "byond_abi.h"
struct FirstPersonView {
    bool enabled;
    float yaw, pitch;
};
struct AimHit {
    void *window = nullptr;
    uint64_t cid = 0;
    float x = 0, y = 0;
    unsigned char icon[ByondAbi::mapIconStride] = {};
};
FirstPersonView GetFirstPersonView();
void ToggleFirstPerson();
void SuspendFirstPersonInput();
void HandleFirstPersonMessage(MSG &message);
void UpdateFirstPersonInput();
void EnsureFirstPersonInputHook();
void PublishAim(const AimHit &hit);
bool ReadAim(void *window, AimHit &hit);
int __fastcall FirstPersonHit(void *window, void *, POINT point, void *list, void *icons, void *params);
int __fastcall FirstPersonHitList(void *window, void *, POINT point, void *list, void *cids, int count,
                                  bool all);
void SetNativeHitFunctions(void *hit, void *hitList);
void SetFirstPersonClient(void *client);
void TraceFirstPersonPosition(uint64_t cid, int x, int y, int level, float screenX, float screenY);
void TraceFirstPersonSceneShift(uint64_t cid, float dx, float dy, int level);
