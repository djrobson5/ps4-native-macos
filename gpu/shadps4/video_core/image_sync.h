// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the renderer reports when the guest CPU is about to read an image's memory (a game
// that reads a screenshot texture itself): the images over the range, and whether the GPU's pixels
// were written back into the guest's memory first (Rasterizer::SyncImageForCpuRead).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace VideoCore {

/// One texture-cache image over the queried range, as plain data for the report.
struct ImageSyncImage {
    std::uint64_t address = 0;
    std::uint64_t guest_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;    ///< AmdGpu pixel format
    std::uint32_t bits = 0;      ///< bits a pixel
    std::uint32_t tile_mode = 0; ///< AmdGpu::TileMode
    std::uint32_t flags = 0;     ///< ImageFlagBits
    bool tiled = false;
    bool gpu_modified = false;
    bool render_target = false;
    bool depth_target = false;
    bool storage = false;
    bool texture = false;
    std::uint64_t write_tick = 0; ///< Image::gpu_write_tick: the last binding that let the GPU write it
    std::uint64_t uid = 0;
};

/// Remembers, per image address, the GPU write the guest's memory was last brought up to, so a
/// reader that asks once per strip (an encoder) has the image written back once. Used on the GPU
/// command thread only. An image whose last GPU write is unknown (tick 0: a copy or a clear sets
/// no tick) is always written back.
class ImageSyncLedger {
public:
    [[nodiscard]] bool NeedsSync(std::uint64_t address, std::uint64_t uid,
                                 std::uint64_t write_tick) const {
        if (write_tick == 0) {
            return true;
        }
        for (const Entry& entry : entries) {
            if (entry.address == address && entry.uid == uid && entry.write_tick == write_tick) {
                return false;
            }
        }
        return true;
    }

    /// The memory at `address` holds the pixels of GPU write `write_tick` of image `uid`.
    void NoteSynced(std::uint64_t address, std::uint64_t uid, std::uint64_t write_tick) {
        for (Entry& entry : entries) {
            if (entry.address == address) {
                entry = {address, uid, write_tick};
                return;
            }
        }
        entries[next] = {address, uid, write_tick};
        next = (next + 1) % entries.size();
    }

private:
    struct Entry {
        std::uint64_t address = 0, uid = 0, write_tick = 0;
    };
    std::array<Entry, 8> entries{};
    std::size_t next = 0;
};

struct ImageSyncReport {
    enum class Result {
        NotMapped,     ///< the range is not GPU-mapped memory, or the renderer is not running
        NoImage,       ///< no image over the range
        NotGpuModified, ///< images over it, none starting there that the GPU wrote
        NotCurrent,    ///< the image is CPU- or GPU-dirty: the guest's memory is newer than it
        AlreadySynced, ///< the memory holds the image's newest GPU write
        NotTiledBack,  ///< the image could not be written back (no buffer over its memory)
        TiledBack,     ///< the GPU's pixels were written into the guest's memory
    };
    Result result = Result::NotMapped;
    std::uint64_t address = 0; ///< the range asked about
    std::uint64_t size = 0;
    std::vector<ImageSyncImage> images; ///< every image over the range
    int chosen = -1;                    ///< index into `images`: the one written back, or -1
    std::uint64_t bytes = 0;            ///< tiled back
    double milliseconds = 0.0;          ///< tiling, GPU wait and download
};

} // namespace VideoCore
