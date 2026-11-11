// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_utils.h"

#include "common/alignment.h"
#include "common/assert.h"

#include <libavutil/frame.h>

#include <cstring>

namespace Libraries::Videodec {

void CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src) {
    const auto dst_pitch = Common::AlignUp<u32>(src.width, 64);
    const auto dst_height = Common::AlignUp<u32>(src.height, 16);

    ASSERT(((dst_pitch * dst_height * 3) / 2) <= max_size);

    const auto luma_dst = dst;
    const auto chroma_dst = dst + dst_pitch * dst_height;

    if (src.width != dst_pitch) {
        for (u32 y = 0; y < src.height; ++y) {
            std::memcpy(luma_dst + y * dst_pitch, src.data[0] + y * src.linesize[0], src.width);
        }
        for (u32 y = 0; y < src.height / 2; ++y) {
            std::memcpy(chroma_dst + y * dst_pitch, src.data[1] + y * src.linesize[1], src.width);
        }
    } else {
        std::memcpy(luma_dst, src.data[0], src.width * src.height);
        std::memcpy(chroma_dst, src.data[1], (src.width * src.height) / 2);
    }

    if (src.height != dst_height) {
        // Extend the data vertically to the crop space
        const auto ly = src.height - 1;
        for (u32 y = src.height; y < dst_height; ++y) {
            std::memcpy(luma_dst + y * dst_pitch, src.data[0] + ly * src.linesize[0], src.width);
        }
        const auto cy = (src.height / 2) - 1;
        for (u32 y = src.height / 2; y < dst_height / 2; ++y) {
            std::memcpy(chroma_dst + y * dst_pitch, src.data[1] + cy * src.linesize[1], src.width);
        }
    }
}


bool CopyNV12DataChecked(u8* dst, u64 max_size, const AVFrame& src) {
    const u32 width = static_cast<u32>(src.width);
    const u32 height = static_cast<u32>(src.height);
    const u32 dst_pitch = Common::AlignUp<u32>(width, 64);
    const u32 dst_height = Common::AlignUp<u32>(height, 16);
    if (!width || !height || (u64{dst_pitch} * dst_height * 3) / 2 > max_size) {
        return false;
    }

    u8* const luma_dst = dst;
    u8* const chroma_dst = dst + u64{dst_pitch} * dst_height;
    const u32 chroma_width = (width + 1) & ~1u;
    const u32 chroma_rows = (height + 1) / 2;

    for (u32 y = 0; y < height; ++y) {
        std::memcpy(luma_dst + u64{y} * dst_pitch, src.data[0] + u64{y} * src.linesize[0], width);
    }
    for (u32 y = 0; y < chroma_rows; ++y) {
        std::memcpy(chroma_dst + u64{y} * dst_pitch, src.data[1] + u64{y} * src.linesize[1],
                    chroma_width);
    }

    // The rows below the picture repeat its last row.
    for (u32 y = height; y < dst_height; ++y) {
        std::memcpy(luma_dst + u64{y} * dst_pitch, luma_dst + u64{height - 1} * dst_pitch, width);
    }
    for (u32 y = chroma_rows; y < dst_height / 2; ++y) {
        std::memcpy(chroma_dst + u64{y} * dst_pitch, chroma_dst + u64{chroma_rows - 1} * dst_pitch,
                    chroma_width);
    }
    return true;
}

} // namespace Libraries::Videodec
