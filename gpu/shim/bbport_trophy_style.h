// SPDX-License-Identifier: GPL-2.0-or-later
// Where a game's trophy notice sits, whether it chimes and shows the trophy's icon, per title (bbport_overlay.cpp's
// kTrophyTitles takes its row from here). No ImGui or SDL: tests/test_trophy_chime.cpp checks it.
#pragma once

#include <string_view>

namespace BbTrophyStyle {

enum class Corner { TopRight, BottomRight };

struct Style {
    Corner corner = Corner::TopRight;
    bool chime = false; ///< a sound as each notice appears (BB_TROPHY_SOUND=0 turns it off)
    bool icons = false; ///< the trophy's own picture from the app's achievements folder (bbport_trophy_icons.h); no game artwork otherwise
};

struct Row {
    std::string_view title; ///< the game's TITLE_ID
    Style style;
};

inline constexpr Row kRows[] = {
    {"CUSA03173", {Corner::TopRight, false, false}},  // Bloodborne: top right, silent, text only
    {"CUSA19610", {Corner::BottomRight, true, true}}, // 13 Sentinels: bottom right, chimes, icons
};

/// The style of a title; a title without a row gets the default (top right, silent, no icons).
constexpr Style For(std::string_view title) {
    for (const Row& row : kRows)
        if (row.title == title) return row.style;
    return {};
}

} // namespace BbTrophyStyle
