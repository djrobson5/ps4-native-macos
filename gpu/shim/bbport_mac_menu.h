// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbMacMenu {
/// macOS: removes the Command shortcuts of the menu bar SDL made (bbport_mac_menu.mm). Called
/// once the window exists, on the main thread.
void ClearCommandShortcuts();

/// macOS: the strip a camera notch takes off the window's screen, in points (its safe-area insets'
/// top plus bottom, and left plus right); 0 when there is none. A fullscreen window is kept below
/// the notch, so a MacBook's 3024x1964 panel gives the game 3024x1900 (bbport_mac_menu.mm).
/// `ns_window` is the window's NSWindow; without one, the main screen's.
void ScreenInsets(void* ns_window, double* horizontal, double* vertical);
} // namespace BbMacMenu
