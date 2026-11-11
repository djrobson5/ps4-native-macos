// SPDX-License-Identifier: GPL-2.0-or-later
// The trophy chimes, synthesized at runtime (no audio file is shipped): a bell-like ding for a
// regular trophy ("B") and a short fanfare for the platinum ("P2"). Original recipes written for
// this project. No ImGui or SDL: tests/test_trophy_chime.cpp compares the output with the WAVs
// the maintainer approved by ear.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace BbTrophyChime {

constexpr int Rate = 44100; ///< Hz, mono
constexpr double Peak = 0.7; ///< of full scale, after normalizing
constexpr double Pi = 3.14159265358979323846;

enum class Kind { Trophy, Platinum };

namespace detail {

struct Partial {
    double ratio, amp;
};

inline constexpr Partial kBell[] = {{1, 1.0}, {2.76, 0.3}, {5.4, 0.12}, {8.9, 0.05}};
inline constexpr Partial kSoft[] = {{1, 1.0}, {2, 0.2}, {3, 0.05}};

/// MIDI note number to Hz (A4 = 69 = 440 Hz).
inline double Freq(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

/// One decaying note mixed into `buf` at `start` seconds: a 4 ms attack, an exponential decay, and
/// partials that decay faster the higher they are; `detune` adds a second voice at a slightly
/// higher pitch (0.6 as loud).
template <std::size_t N>
void Note(std::vector<double>& buf, double start, double freq, double dur, double amp,
          const Partial (&partials)[N], double decay, double detune = 0.0) {
    const int s = int(start * Rate);
    const int count = int(dur * Rate);
    for (int i = 0; i < count; ++i) {
        if (std::size_t(s + i) >= buf.size()) break;
        const double t = double(i) / Rate;
        const double env = std::min(1.0, t / 0.004) * std::exp(-t * decay);
        double v = 0.0;
        for (const Partial& p : partials)
            v += p.amp * std::sin(2.0 * Pi * freq * p.ratio * t) * std::exp(-t * decay * (p.ratio * 0.6));
        if (detune != 0.0) {
            double w = 0.0;
            for (const Partial& p : partials)
                w += p.amp * std::sin(2.0 * Pi * freq * (1.0 + detune) * p.ratio * t) *
                     std::exp(-t * decay * (p.ratio * 0.6));
            v += 0.6 * w;
        }
        buf[std::size_t(s + i)] += amp * env * v;
    }
}

} // namespace detail

/// The chime as mono float samples at `Rate`, normalized to a peak of `Peak`. Costs tens of
/// milliseconds of CPU: call it off the present thread.
inline std::vector<float> Synthesize(Kind kind) {
    using namespace detail;
    std::vector<double> buf;
    if (kind == Kind::Trophy) {
        // 1.4 s: a bell on E6, a second voice a hair sharp, and a soft E5 under it.
        buf.assign(std::size_t(int(Rate * 1.4)), 0.0);
        Note(buf, 0, Freq(88), 1.3, 1.0, kBell, 3.2);
        Note(buf, 0, Freq(88) * 1.004, 1.3, 0.6, kBell, 3.2);
        Note(buf, 0.02, Freq(76), 1.0, 0.35, kSoft, 5);
    } else {
        // 3.6 s: two B5 and an E6, a rising sparkle, then a long chord on E.
        buf.assign(std::size_t(int(Rate * 3.6)), 0.0);
        const struct { double t; int n; double d; } pickup[] = {{0, 83, 0.5}, {0.14, 83, 0.5}, {0.28, 88, 1.0}};
        for (const auto& p : pickup) {
            Note(buf, p.t, Freq(p.n), p.d + 0.6, 0.9, kBell, 3.5, 0.004);
            Note(buf, p.t, Freq(p.n - 12), p.d + 0.4, 0.35, kSoft, 4);
        }
        const int sparkle[] = {95, 100, 104};
        for (int i = 0; i < 3; ++i) Note(buf, 0.75 + i * 0.06, Freq(sparkle[i]), 1.2, 0.45, kBell, 3.5, 0.004);
        for (int n : {64, 76, 80, 83, 88}) {
            if (n < 83) Note(buf, 0.95, Freq(n), 2.6, 0.5, kSoft, 1.3, 0.003);
            else Note(buf, 0.95, Freq(n), 2.6, 0.5, kBell, 1.3, 0.003);
        }
    }
    double peak = 0.0;
    for (double v : buf) peak = std::max(peak, std::fabs(v));
    std::vector<float> out(buf.size());
    if (peak > 0.0)
        for (std::size_t i = 0; i < buf.size(); ++i) out[i] = float(buf[i] / peak * Peak);
    return out;
}

} // namespace BbTrophyChime
