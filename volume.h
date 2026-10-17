// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#pragma once
#include <cstdint>
using NativePixels = const void *(__thiscall *)(void *, uint32_t);
bool DrawNativeVolume(void *driver, void *list, void *icons, NativePixels pixels);
