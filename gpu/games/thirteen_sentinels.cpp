// SPDX-License-Identifier: GPL-2.0-or-later
// 13 Sentinels: Aegis Rim (PS4, 1.00): what the translator knows about its code. It runs on the
// generic paths; the profile only names the game, for the hooks that are written for its
// executable (shim/bbport_snapshot_sync.cpp, installed from bbgpu_patch_image).
#include "game_profile.h"

namespace Game {
namespace {
constexpr const char* Serials[] = {"CUSA19610"};
} // namespace

extern const Profile ThirteenSentinelsProfile = {
    .name = "13 Sentinels: Aegis Rim",
    .serials = Serials,
};

} // namespace Game
