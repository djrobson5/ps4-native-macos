// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct AVFrame;

namespace Libraries::Videodec {

void CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src);

// Writes src as NV12 with a 64-byte pitch and a 16-aligned height; false if it does not fit max_size.
// Copied from saulob's 13 Sentinels PC port (github.com/saulob/13-sentinels-aegis-rim-pc/.../video_utils.cpp, GPL-2.0-or-later): used by
// Videodec2; the function above (the AvPlayer's) is unchanged.
bool CopyNV12DataChecked(u8* dst, u64 max_size, const AVFrame& src);

}
