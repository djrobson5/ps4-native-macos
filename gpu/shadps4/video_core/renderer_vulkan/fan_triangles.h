// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport (FansAsTriangles): the triangle list a triangle fan (or polygon) is drawn as on a GPU
// without fans (Metal). Triangle k of a fan with vertices v0 v1 v2 ... is (v[k+1], v[k+2], v[0]),
// the rotation of (v0, v[k+1], v[k+2]) Vulkan's provoking vertex rule gives: the same winding,
// and the same first (provoking) vertex, which flat-shaded attributes come from. In last-vertex
// mode (`last`) the provoking vertex is v[k+2], the triangle's last: (v0, v[k+1], v[k+2]).
//
// With primitive restart a run of indices ends at an index equal to the type's maximum (0xFFFF or
// 0xFFFFFFFF); each run is its own fan, and a run of fewer than 3 indices draws nothing.

#include <cstdint>

namespace Vulkan::FanTriangles {

/// Indices of the triangle list for a non-indexed fan of `vertices` vertices: (n - 2) * 3, or 0
/// for n < 3.
constexpr uint64_t SequentialCount(uint32_t vertices) {
    return vertices < 3 ? 0 : uint64_t(vertices - 2) * 3;
}

/// Writes the triangle list for a non-indexed fan; `out` holds SequentialCount(vertices) entries.
template <typename Index>
void BuildSequential(uint32_t vertices, Index* out, bool last = false) {
    for (uint32_t k = 0; k + 2 < vertices; ++k, out += 3) {
        out[0] = Index(last ? 0 : k + 1);
        out[1] = Index(last ? k + 1 : k + 2);
        out[2] = Index(last ? k + 2 : 0);
    }
}

/// Visits each fan of an indexed draw: `run(first, length)` for every run of indices that are not
/// the restart index (the whole draw without restart). Runs of length 0 are not reported.
template <typename Index, typename Run>
void ForEachRun(const Index* in, uint32_t count, bool restart, Run&& run) {
    constexpr Index restart_index = ~Index(0);
    uint32_t first = 0;
    for (uint32_t i = 0; i <= count; ++i) {
        if (i == count || (restart && in[i] == restart_index)) {
            if (i > first) {
                run(first, i - first);
            }
            first = i + 1;
        }
    }
}

/// Indices of the triangle list for an indexed fan draw of `count` guest indices.
template <typename Index>
uint64_t IndexedCount(const Index* in, uint32_t count, bool restart) {
    uint64_t total = 0;
    ForEachRun(in, count, restart, [&](uint32_t, uint32_t length) {
        total += SequentialCount(length);
    });
    return total;
}

/// Writes the triangle list for an indexed fan draw; `out` holds IndexedCount(...) entries. The
/// restart indices are not copied: no index of the output needs restart.
template <typename Index>
void BuildIndexed(const Index* in, uint32_t count, bool restart, Index* out, bool last = false) {
    ForEachRun(in, count, restart, [&](uint32_t first, uint32_t length) {
        const Index* fan = in + first;
        for (uint32_t k = 0; k + 2 < length; ++k, out += 3) {
            out[0] = last ? fan[0] : fan[k + 1];
            out[1] = last ? fan[k + 1] : fan[k + 2];
            out[2] = last ? fan[k + 2] : fan[0];
        }
    });
}

} // namespace Vulkan::FanTriangles
