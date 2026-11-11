// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/fan_triangles.h"

using namespace Vulkan::FanTriangles;

template <typename Index>
static std::vector<Index> Indexed(const std::vector<Index>& in, bool restart) {
    std::vector<Index> out(IndexedCount(in.data(), uint32_t(in.size()), restart));
    BuildIndexed(in.data(), uint32_t(in.size()), restart, out.data());
    return out;
}

int main() {
    using V32 = std::vector<uint32_t>;
    using V16 = std::vector<uint16_t>;

    // Non-indexed: n vertices make n - 2 triangles, (k+1, k+2, 0); fewer than 3 make none.
    assert(SequentialCount(0) == 0 && SequentialCount(2) == 0);
    assert(SequentialCount(3) == 3 && SequentialCount(6) == 12);
    V32 seq(SequentialCount(5));
    BuildSequential(5, seq.data());
    assert((seq == V32{1, 2, 0, 2, 3, 0, 3, 4, 0}));

    // Last-vertex provoking mode: (0, k+1, k+2), the last vertex being the fan's newest.
    V32 last(SequentialCount(4));
    BuildSequential(4, last.data(), true);
    assert((last == V32{0, 1, 2, 0, 2, 3}));
    V16 last16(IndexedCount(V16{10, 20, 30, 40}.data(), 4, false));
    BuildIndexed(V16{10, 20, 30, 40}.data(), 4, false, last16.data(), true);
    assert((last16 == V16{10, 20, 30, 10, 30, 40}));

    // Indexed, 16-bit: the guest's indices through the same pattern.
    assert((Indexed<uint16_t>(V16{10, 20, 30, 40}, false) == V16{20, 30, 10, 30, 40, 10}));
    assert(Indexed<uint16_t>(V16{10, 20}, false).empty());
    assert(Indexed<uint16_t>(V16{}, false).empty());

    // Indexed, 32-bit, with values above 16 bits.
    assert((Indexed<uint32_t>(V32{70000, 80000, 90000}, false) == V32{80000, 90000, 70000}));

    // Without restart the maximum index is an ordinary index.
    assert((Indexed<uint16_t>(V16{1, 2, 0xFFFF}, false) == V16{2, 0xFFFF, 1}));

    // Restart splits the draw: each run is its own fan, a run under 3 draws nothing, and no
    // restart index reaches the output.
    assert((Indexed<uint16_t>(V16{1, 2, 3, 4, 0xFFFF, 5, 6, 7}, true) ==
            V16{2, 3, 1, 3, 4, 1, 6, 7, 5}));
    assert((Indexed<uint32_t>(V32{1, 2, 3, 0xFFFFFFFF, 4, 5, 0xFFFFFFFF, 6, 7, 8, 9}, true) ==
            V32{2, 3, 1, 7, 8, 6, 8, 9, 6}));
    // Leading, trailing and repeated restarts.
    assert((Indexed<uint16_t>(V16{0xFFFF, 1, 2, 3, 0xFFFF, 0xFFFF, 4, 5, 6, 0xFFFF}, true) ==
            V16{2, 3, 1, 5, 6, 4}));
    assert(Indexed<uint16_t>(V16{0xFFFF, 0xFFFF}, true).empty());
    // A 16-bit restart index is not the 32-bit one.
    assert((Indexed<uint32_t>(V32{1, 0xFFFF, 2}, true) == V32{0xFFFF, 2, 1}));
}
