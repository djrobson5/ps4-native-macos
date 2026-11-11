// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>
#include "gpu/shadps4/video_core/image_sync.h"

using VideoCore::ImageSyncLedger;

int main() {
    ImageSyncLedger ledger;
    constexpr std::uint64_t A = 0x1000000, B = 0x2000000;

    // Nothing is known at first: the image is written back.
    assert(ledger.NeedsSync(A, 1, 10));

    // Once written back for a GPU write, the same image and write need nothing (the encoder's
    // later strips); a newer write, or another image at the address, does.
    ledger.NoteSynced(A, 1, 10);
    assert(!ledger.NeedsSync(A, 1, 10));
    assert(ledger.NeedsSync(A, 1, 11));
    assert(ledger.NeedsSync(A, 2, 10));
    assert(ledger.NeedsSync(B, 1, 10));

    // An image whose last GPU write is unknown (tick 0) is always written back.
    ledger.NoteSynced(B, 5, 0);
    assert(ledger.NeedsSync(B, 5, 0));

    // A new write at the same address replaces the old record.
    ledger.NoteSynced(A, 1, 11);
    assert(!ledger.NeedsSync(A, 1, 11));
    assert(ledger.NeedsSync(A, 1, 10));

    // Only the newest addresses are remembered (eight): the oldest is forgotten, and the image is
    // then written back once more, which costs time but never a wrong picture.
    for (std::uint64_t i = 0; i < 8; ++i) {
        ledger.NoteSynced(0x10000000 + i * 0x1000, 7, 3);
    }
    assert(ledger.NeedsSync(A, 1, 11));
    assert(!ledger.NeedsSync(0x10000000 + 7 * 0x1000, 7, 3));
}
