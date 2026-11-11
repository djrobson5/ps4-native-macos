// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: 13 Sentinels: Aegis Rim writes its save thumbnail from a screenshot texture that the GPU
// draws and the game then reads on the CPU. The guest's memory under that texture never holds the
// GPU's pixels (the renderer keeps the image on the host), so the thumbnail came out black. A hook
// at the entry of the game's JPEG encoder wrapper has the renderer write the texture's pixels back
// into the guest's memory, in the guest's layout, before the encoder reads it
// (Rasterizer::SyncImageForCpuRead).
#pragma once

#include <cstdint>

namespace BbSnapshotSync {
/// Installs the hook (13 Sentinels' profile only; BB_13S_SNAP_SYNC=0 leaves it out), from
/// bbgpu_patch_image.
void PatchImage(unsigned char* image, std::uint64_t size);
} // namespace BbSnapshotSync
