// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 TheSameEli
#pragma once
// BYOND 516.1688 x86. Verified from installed DLL disassembly.
namespace ByondAbi {
constexpr unsigned mapIconStride = 216;
constexpr const char *screenEdgeSymbol = "?ScreenEdge@GraphicsWnd@@QAE?AUPixelPoint@@PAUMapIcon@@@Z";
constexpr unsigned rendererVtable = 0x17fe88;
constexpr unsigned renderScene = 0x112b90;
constexpr unsigned presentScene = 0x10f240;
constexpr unsigned lookupTexture = 0x116e90;
constexpr unsigned getMapText = 0x110610;
} // namespace ByondAbi
