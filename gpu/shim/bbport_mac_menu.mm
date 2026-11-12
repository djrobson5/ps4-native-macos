// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's menu bar without Command shortcuts (app issue #14).
//
// SDL gives the app a menu bar with Hide (⌘H), Quit (⌘Q), Close (⌘W), Minimize (⌘M) and Full
// Screen (⌃⌘F). A held key repeats, and once Command is down its repeats are Command shortcuts:
// holding W to walk and touching ⌘ closed the game. The items stay in the menu bar, without
// their shortcuts; the window's close button, the in-game Quit and the quit binding (Command+Q by
// default, only on a new press: src/runtime_pad.c quit_key, app issue #20) close the game.

#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cstdio>

#include "bbport_mac_menu.h"

static unsigned ClearShortcuts(NSMenu* menu) {
    unsigned cleared = 0;
    for (NSMenuItem* item in menu.itemArray) {
        if (item.keyEquivalent.length &&
            (item.keyEquivalentModifierMask & NSEventModifierFlagCommand)) {
            item.keyEquivalent = @"";
            ++cleared;
        }
        if (item.submenu) {
            cleared += ClearShortcuts(item.submenu);
        }
    }
    return cleared;
}

void BbMacMenu::ClearCommandShortcuts() {
    @autoreleasepool {
        if (NSMenu* menu = NSApp.mainMenu) {
            std::printf("Window: %u Command shortcuts taken off the menu bar\n",
                        ClearShortcuts(menu));
        }
    }
}

void BbMacMenu::ScreenInsets(void* ns_window, double* horizontal, double* vertical) {
    *horizontal = 0;
    *vertical = 0;
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)ns_window;
        NSScreen* screen = window.screen ? window.screen : NSScreen.mainScreen;
        if (!screen) return;
        const NSEdgeInsets insets = screen.safeAreaInsets;
        *horizontal = std::max(0.0, double(insets.left)) + std::max(0.0, double(insets.right));
        *vertical = std::max(0.0, double(insets.top)) + std::max(0.0, double(insets.bottom));
    }
}
#endif
