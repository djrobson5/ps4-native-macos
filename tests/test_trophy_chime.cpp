// SPDX-License-Identifier: GPL-2.0-or-later
// The trophy chimes (bbport_trophy_chime.h): lengths, the normalized peak, and the match against
// the WAVs the maintainer approved by ear (skipped, with a message, unless BB_CHIME_WAV_DIR names
// a folder that holds them; they are not in git). Also the
// per-title style table (bbport_trophy_style.h): corner and chime.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "bbport_trophy_chime.h"
#include "bbport_trophy_style.h"

namespace {

float PeakOf(const std::vector<float>& v) {
    float peak = 0.0f;
    for (float x : v) peak = std::fmax(peak, std::fabs(x));
    return peak;
}

/// 16-bit mono PCM WAV to floats in [-1, 1); false when missing or not that format.
bool ReadWav(const std::string& path, std::vector<float>& out, int& rate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<unsigned char> b((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    auto u32 = [&](size_t i) { return uint32_t(b[i]) | uint32_t(b[i + 1]) << 8 | uint32_t(b[i + 2]) << 16 | uint32_t(b[i + 3]) << 24; };
    auto u16 = [&](size_t i) { return uint16_t(b[i] | b[i + 1] << 8); };
    int channels = 0, bits = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t size = u32(at + 4);
        const size_t body = at + 8;
        if (!std::memcmp(b.data() + at, "fmt ", 4) && body + 16 <= b.size()) {
            channels = u16(body + 2), rate = int(u32(body + 4)), bits = u16(body + 14);
        } else if (!std::memcmp(b.data() + at, "data", 4)) {
            if (channels != 1 || bits != 16) return false;
            const size_t n = std::min<size_t>(size, b.size() - body) / 2;
            out.resize(n);
            for (size_t i = 0; i < n; ++i) out[i] = float(int16_t(u16(body + i * 2))) / 32768.0f;
            return true;
        }
        at = body + size + (size & 1);
    }
    return false;
}

void CheckAgainstWav(const char* name, BbTrophyChime::Kind kind, size_t expected_length) {
    const std::vector<float> synth = BbTrophyChime::Synthesize(kind);
    assert(synth.size() == expected_length);
    assert(std::fabs(PeakOf(synth) - float(BbTrophyChime::Peak)) < 1e-6f);
    assert(BbTrophyChime::Synthesize(kind) == synth); // deterministic

    const char* dir = std::getenv("BB_CHIME_WAV_DIR");
    std::vector<float> wav;
    int rate = 0;
    if (!dir || !*dir) {
        std::printf("SKIP: %s not compared (BB_CHIME_WAV_DIR not set); lengths and peak checked only\n", name);
        return;
    }
    const std::string base = dir;
    if (!ReadWav(base + "/" + name, wav, rate)) {
        std::printf("SKIP: %s not readable in %s; lengths and peak checked only\n", name, base.c_str());
        return;
    }
    assert(rate == BbTrophyChime::Rate);
    assert(wav.size() == synth.size());
    double max_diff = 0.0, sum_sq = 0.0;
    for (size_t i = 0; i < synth.size(); ++i) {
        const double d = std::fabs(double(synth[i]) - double(wav[i]));
        max_diff = std::fmax(max_diff, d);
        sum_sq += d * d;
    }
    std::printf("%s: %zu samples, synth peak %.5f, WAV peak %.5f, max |diff| %.6f, rms diff %.7f\n", name,
                synth.size(), PeakOf(synth), PeakOf(wav), max_diff, std::sqrt(sum_sq / double(synth.size())));
    assert(max_diff < 0.002); // 16-bit rounding is 0.00003
}

} // namespace

int main() {
    using BbTrophyStyle::Corner;
    // Per title: Bloodborne top right, silent, text only (no game artwork in the runtime);
    // 13 Sentinels bottom right with the chimes and the trophy icons.
    constexpr auto bloodborne = BbTrophyStyle::For("CUSA03173");
    constexpr auto sentinels = BbTrophyStyle::For("CUSA19610");
    static_assert(bloodborne.corner == Corner::TopRight && !bloodborne.chime && !bloodborne.icons);
    static_assert(sentinels.corner == Corner::BottomRight && sentinels.chime && sentinels.icons);
    constexpr auto unknown = BbTrophyStyle::For("CUSA00000");
    static_assert(unknown.corner == Corner::TopRight && !unknown.chime && !unknown.icons);

    CheckAgainstWav("B-shimmer-ding.wav", BbTrophyChime::Kind::Trophy, size_t(int(BbTrophyChime::Rate * 1.4)));
    CheckAgainstWav("P2-fanfare.wav", BbTrophyChime::Kind::Platinum, size_t(int(BbTrophyChime::Rate * 3.6)));

    std::puts("PASS: trophy chimes: lengths, peak 0.7, match with the approved WAVs, per-title style (corner, chime, icons)");
    return 0;
}
