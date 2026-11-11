// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_snapshot_sync.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bbport_guest_code_hooks.h"
#include "common/types.h"
#include "core/memory.h"
#include "game_profile.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace BbSnapshotSync {
namespace {
using Result = VideoCore::ImageSyncReport::Result;

/// The game's JPEG encoder wrapper for the save thumbnail, as an image offset (the main
/// executable's image base is 0). It encodes the screenshot texture on the CPU, a strip a call: r9
/// is the texture's guest address, and the first stack arguments are its width and height.
constexpr std::uint64_t EncoderEntry = 0x7ffa0;
constexpr std::uint8_t EncoderPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00};

/// The 8_8_8_8 texture's size in the guest's tiled layout (8x8 micro tiles), for the range the
/// image lookup covers when the renderer knows no image there.
std::uint64_t TiledSize(std::uint32_t width, std::uint32_t height) {
    constexpr std::uint32_t MaxSide = 16384;
    if (width == 0 || height == 0 || width > MaxSide || height > MaxSide) {
        return 4096;
    }
    return std::uint64_t((width + 7) & ~7u) * ((height + 7) & ~7u) * 4;
}

std::string FlagNames(std::uint32_t flags) {
    std::string out;
    const auto add = [&](std::uint32_t bit, const char* name) {
        if (flags & bit) {
            out += out.empty() ? name : std::string("|") + name;
        }
    };
    add(1 << 0, "MaybeCpuDirty");
    add(1 << 1, "CpuDirty");
    add(1 << 2, "GpuDirty");
    add(1 << 3, "GpuModified");
    add(1 << 6, "Registered");
    return out.empty() ? "none" : out;
}

/// How the GPU wrote the image: the usages the texture cache saw the image bound with (kept for
/// the image's life: a render target or depth target, a storage image); else a copy or a clear.
std::string HowWritten(const VideoCore::ImageSyncImage& image) {
    std::string out;
    const auto add = [&](bool on, const char* name) {
        if (on) {
            out += out.empty() ? name : std::string("+") + name;
        }
    };
    add(image.render_target, "render target");
    add(image.depth_target, "depth target");
    add(image.storage, "storage");
    if (out.empty()) {
        out = image.gpu_modified ? "other (copy or clear)" : "not written";
    }
    return out;
}

std::string DescribeImage(const VideoCore::ImageSyncImage& image) {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "image %#llx %ux%u fmt %u tile %u%s flags %#x (%s, %s%s), size %llu, write tick %llu",
                  static_cast<unsigned long long>(image.address), image.width, image.height,
                  image.format, image.tile_mode, image.tiled ? "" : " (linear)", image.flags,
                  FlagNames(image.flags).c_str(), HowWritten(image).c_str(),
                  image.texture ? ", also sampled" : "", static_cast<unsigned long long>(image.guest_size),
                  static_cast<unsigned long long>(image.write_tick));
    return text;
}

std::atomic<std::uint32_t> lines_printed{0};
std::atomic<std::uint32_t> already_synced{0};
std::atomic<std::uint64_t> last_other_ms{0};
std::atomic<int> last_other_result{-1};

std::uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The hook: the encoder wrapper is entered (once a strip). Called on the game's thread, in the
/// trap handler of the guest CPU, so it may block: the renderer runs the write-back on its GPU
/// command thread and this thread waits for it.
void __attribute__((sysv_abi)) OnEncoderEntry(std::uint64_t, const Core::GuestRegisterSnapshot* registers) {
    const std::uint64_t pixels = registers->r9;
    std::uint32_t width = 0, height = 0;
    std::memcpy(&width, reinterpret_cast<const void*>(registers->rsp + 8), sizeof(width));
    std::memcpy(&height, reinterpret_cast<const void*>(registers->rsp + 0x10), sizeof(height));
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (!rasterizer || pixels == 0) {
        return;
    }
    const VideoCore::ImageSyncReport report = rasterizer->SyncImageForCpuRead(pixels, TiledSize(width, height));

    if (report.result == Result::AlreadySynced) {
        already_synced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // One line per encode start (a write-back); the other outcomes repeat every strip, so one
    // line a second, or when the outcome changes.
    // An image the GPU wrote at an unknown tick (a copy or a clear) is written back every call:
    // that one is rate limited too.
    const bool announce = report.result == Result::TiledBack && report.images[report.chosen].write_tick != 0;
    const std::uint64_t now = NowMs();
    if (!announce) {
        const int result = static_cast<int>(report.result);
        const bool changed = last_other_result.exchange(result) != result;
        std::uint64_t last = last_other_ms.load(std::memory_order_relaxed);
        if (!changed && now - last < 1000) {
            return;
        }
        last_other_ms.store(now, std::memory_order_relaxed);
    }
    if (lines_printed.fetch_add(1, std::memory_order_relaxed) >= 300) {
        return;
    }
    std::string line = "Snap sync: r9=";
    char head[96];
    std::snprintf(head, sizeof(head), "%#llx %ux%u; ", static_cast<unsigned long long>(pixels), width, height);
    line += head;
    const auto describe_all = [&] {
        std::string all;
        for (const auto& image : report.images) {
            all += "[" + DescribeImage(image) + "] ";
        }
        return all;
    };
    char tail[128];
    switch (report.result) {
    case Result::TiledBack:
        line += DescribeImage(report.images[report.chosen]);
        std::snprintf(tail, sizeof(tail), "; tiled back %llu bytes in %.2f ms",
                      static_cast<unsigned long long>(report.bytes), report.milliseconds);
        line += tail;
        break;
    case Result::NotTiledBack:
        line += DescribeImage(report.images[report.chosen]);
        line += "; not tiled back: no buffer over its memory, or a pixel width the tiling pass lacks";
        break;
    case Result::NotCurrent:
        line += DescribeImage(report.images[report.chosen]);
        line += "; not tiled back: the guest's memory is newer than the image (CPU- or GPU-dirty)";
        break;
    case Result::NotGpuModified:
        line += "no GPU-modified image starting at r9; images over the range: " + describe_all();
        break;
    case Result::NoImage:
        line += "no image at r9 (none registered over the range)";
        break;
    case Result::NotMapped:
        line += "r9 is not GPU-mapped memory";
        break;
    case Result::AlreadySynced:
        break;
    }
    if (const std::uint32_t skipped = already_synced.exchange(0, std::memory_order_relaxed)) {
        std::snprintf(tail, sizeof(tail), "; (%u calls found it already written back)", skipped);
        line += tail;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t) {
    if (!Game::IsThirteenSentinels()) {
        return;
    }
    const char* env = std::getenv("BB_13S_SNAP_SYNC");
    if (env && env[0] == '0') {
        std::printf("Snap sync: off (BB_13S_SNAP_SYNC=0)\n");
        return;
    }
    const bool installed = Core::InstallGuestCodeHook(image + EncoderEntry, EncoderPrologue, 0, OnEncoderEntry);
    std::printf("Snap sync: thumbnail encoder hook %s\n",
                installed ? "installed" : "not installed (other code at that offset)");
}

} // namespace BbSnapshotSync
