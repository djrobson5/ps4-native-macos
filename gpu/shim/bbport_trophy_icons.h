// SPDX-License-Identifier: GPL-2.0-or-later
// The trophy notice's icon, for the titles whose style says so (bbport_trophy_style.h; 13 Sentinels
// only: the runtime carries no game artwork, the packaged app brings the pictures). The icons are
// Contents/Resources/achievements/<trophy id>.png in the app, put there by
// scripts/package-13s-app.sh. This file finds the folder, decodes a file to RGBA (SDL's PNG loader,
// so no new dependency), and keeps a small cache loaded off the present thread. No ImGui and no
// GPU: bbport_overlay.cpp puts the pixels in ImGui's font atlas, and tests/test_trophy_icons.cpp
// checks everything here. A missing, unreadable or non-PNG file is a failure the caller answers by
// drawing no icon, logged once per id.
#pragma once

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_version.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace BbTrophyIcons {

/// A decoded picture: straight (not premultiplied) RGBA, 8 bits per channel, row by row.
struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};

constexpr int MaxEdge = 128;      ///< larger pictures are shrunk to this (the notice shows them at ~60 px, at most 2x)
constexpr int MaxId = 127;        ///< the trophy ids the log accepts (docs/TROPHIES.md)
constexpr size_t MaxFile = 4u << 20; ///< a bigger file is not an icon
constexpr size_t MaxReady = 8;    ///< decoded pictures kept waiting for their notice

/// The folder of the icons. `env_override` (BB_TROPHY_ICON_DIR, may be null or empty) wins and is
/// used as given; otherwise `base_path` (SDL_GetBasePath(): Contents/Resources in an app, or the
/// executable's folder, Contents/MacOS) is tried as <base>/achievements and
/// <base>/../Resources/achievements. Empty when none is a directory (run outside the app).
inline std::filesystem::path ResolveDir(const char* env_override, const char* base_path) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (env_override && *env_override) {
        fs::path dir(env_override);
        return fs::is_directory(dir, error) ? dir : fs::path();
    }
    if (!base_path || !*base_path) return {};
    const fs::path base(base_path);
    for (const fs::path& candidate : {base / "achievements", (base / ".." / "Resources" / "achievements").lexically_normal()}) {
        if (fs::is_directory(candidate, error)) return candidate;
    }
    return {};
}

namespace detail {

/// Area-average shrink (straight alpha: colours weighted by alpha) so the notice's icon is not
/// aliased by the GPU's bilinear minification.
inline Image Shrink(const Image& src, int max_edge) {
    if (std::max(src.w, src.h) <= max_edge) return src;
    const float scale = float(max_edge) / float(std::max(src.w, src.h));
    Image dst;
    dst.w = std::max(1, int(float(src.w) * scale + 0.5f));
    dst.h = std::max(1, int(float(src.h) * scale + 0.5f));
    dst.rgba.resize(size_t(dst.w) * size_t(dst.h) * 4);
    const float step_x = float(src.w) / float(dst.w), step_y = float(src.h) / float(dst.h);
    for (int y = 0; y < dst.h; ++y) {
        const float y0 = float(y) * step_y, y1 = y0 + step_y;
        for (int x = 0; x < dst.w; ++x) {
            const float x0 = float(x) * step_x, x1 = x0 + step_x;
            float r = 0, g = 0, b = 0, a = 0, weight = 0;
            for (int sy = int(y0); sy < src.h && float(sy) < y1; ++sy) {
                const float wy = std::min(y1, float(sy + 1)) - std::max(y0, float(sy));
                for (int sx = int(x0); sx < src.w && float(sx) < x1; ++sx) {
                    const float w = wy * (std::min(x1, float(sx + 1)) - std::max(x0, float(sx)));
                    const unsigned char* p = &src.rgba[(size_t(sy) * size_t(src.w) + size_t(sx)) * 4];
                    const float alpha = float(p[3]) * w;
                    r += float(p[0]) * alpha, g += float(p[1]) * alpha, b += float(p[2]) * alpha;
                    a += alpha, weight += w;
                }
            }
            unsigned char* out = &dst.rgba[(size_t(y) * size_t(dst.w) + size_t(x)) * 4];
            if (a > 0.0f && weight > 0.0f) {
                out[0] = (unsigned char)std::clamp(r / a + 0.5f, 0.0f, 255.0f);
                out[1] = (unsigned char)std::clamp(g / a + 0.5f, 0.0f, 255.0f);
                out[2] = (unsigned char)std::clamp(b / a + 0.5f, 0.0f, 255.0f);
                out[3] = (unsigned char)std::clamp(a / weight + 0.5f, 0.0f, 255.0f);
            } else {
                out[0] = out[1] = out[2] = out[3] = 0;
            }
        }
    }
    return dst;
}

} // namespace detail

/// PNG bytes to an Image of at most MaxEdge, or nothing (`why` says what was wrong).
inline std::optional<Image> Decode(const unsigned char* data, size_t size, std::string* why = nullptr) {
    auto fail = [&](const char* text) -> std::optional<Image> {
        if (why) *why = text;
        return std::nullopt;
    };
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!data || size < sizeof(signature) || std::memcmp(data, signature, sizeof(signature)) != 0) return fail("not a PNG file");
#if SDL_VERSION_ATLEAST(3, 4, 0)
    SDL_IOStream* io = SDL_IOFromConstMem(data, size);
    if (!io) return fail("cannot read the file");
    SDL_Surface* loaded = SDL_LoadPNG_IO(io, true); // closes io
    if (!loaded) return fail("not a valid PNG file (truncated or corrupt)");
    SDL_Surface* rgba = loaded->format == SDL_PIXELFORMAT_RGBA32 ? loaded : SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    if (rgba != loaded) SDL_DestroySurface(loaded);
    if (!rgba) return fail("cannot convert the picture");
    std::optional<Image> result;
    if (rgba->w < 1 || rgba->h < 1 || rgba->w > 4096 || rgba->h > 4096 || !rgba->pixels) {
        if (why) *why = "unusable picture size";
    } else {
        Image full;
        full.w = rgba->w, full.h = rgba->h;
        full.rgba.resize(size_t(full.w) * size_t(full.h) * 4);
        for (int y = 0; y < full.h; ++y) {
            std::memcpy(&full.rgba[size_t(y) * size_t(full.w) * 4], static_cast<const unsigned char*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch),
                        size_t(full.w) * 4);
        }
        result = detail::Shrink(full, MaxEdge);
    }
    SDL_DestroySurface(rgba);
    return result;
#else
    return fail("this SDL has no PNG loader");
#endif
}

/// `<dir>/<id>.png` decoded, or nothing (missing, unreadable, too big, not a PNG, truncated).
inline std::optional<Image> Load(const std::filesystem::path& dir, int id, std::string* why = nullptr) {
    auto fail = [&](const char* text) -> std::optional<Image> {
        if (why) *why = text;
        return std::nullopt;
    };
    if (dir.empty() || id < 0 || id > MaxId) return fail("no icon folder or a bad id");
    std::ifstream file(dir / (std::to_string(id) + ".png"), std::ios::binary);
    if (!file) return fail("no such file");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return fail("cannot read the file");
    if (bytes.size() > MaxFile) return fail("file too big");
    return Decode(bytes.data(), bytes.size(), why);
}

/// Icons by trophy id, decoded on a worker thread per request. Any thread may call anything.
/// Keeps what is queued or loading, a few decoded pictures waiting to be shown (Take hands one
/// over and forgets it; the oldest is dropped past MaxReady and reloaded if asked for again) and a
/// mark for each id that failed, so its one log line is not repeated.
class Cache {
public:
    enum class State { Unknown, Loading, Ready, Failed };

    /// An empty `dir` (no icon folder) makes every request fail at once, without a log line.
    explicit Cache(std::filesystem::path dir, bool log = true) : shared(std::make_shared<Shared>()) {
        shared->dir = std::move(dir);
        shared->log = log;
    }

    /// Starts loading `id` unless it is known already (loading, ready or failed).
    void Request(int id) {
        std::scoped_lock lock{shared->mutex};
        if (shared->entries.count(id)) return;
        Entry& entry = shared->entries[id];
        entry.order = ++shared->counter;
        if (shared->dir.empty() || id < 0 || id > MaxId) {
            entry.state = State::Failed;
            return;
        }
        entry.state = State::Loading;
        std::thread([shared = shared, id] {
            std::string why;
            std::optional<Image> image = Load(shared->dir, id, &why);
            std::scoped_lock lock{shared->mutex};
            auto found = shared->entries.find(id);
            if (found == shared->entries.end() || found->second.state != State::Loading) return;
            if (image) {
                found->second.state = State::Ready;
                found->second.image = std::move(*image);
                shared->DropOldestReady();
            } else {
                found->second.state = State::Failed;
                if (shared->log) std::fprintf(stderr, "trophy icon %d: %s (%s), no icon\n", id, why.c_str(), shared->dir.string().c_str());
            }
        }).detach();
    }

    State Get(int id) const {
        std::scoped_lock lock{shared->mutex};
        auto found = shared->entries.find(id);
        return found == shared->entries.end() ? State::Unknown : found->second.state;
    }

    /// The decoded picture, once, when ready; the entry is forgotten (a later Request reloads).
    std::optional<Image> Take(int id) {
        std::scoped_lock lock{shared->mutex};
        auto found = shared->entries.find(id);
        if (found == shared->entries.end() || found->second.state != State::Ready) return std::nullopt;
        std::optional<Image> image = std::move(found->second.image);
        shared->entries.erase(found);
        return image;
    }

    /// For tests: entries held, by state.
    size_t Count(State state) const {
        std::scoped_lock lock{shared->mutex};
        size_t n = 0;
        for (const auto& entry : shared->entries) n += entry.second.state == state;
        return n;
    }

private:
    struct Entry {
        State state = State::Unknown;
        Image image;
        uint64_t order = 0;
    };
    struct Shared {
        mutable std::mutex mutex;
        std::filesystem::path dir;
        bool log = true;
        std::map<int, Entry> entries;
        uint64_t counter = 0;

        void DropOldestReady() {
            for (;;) {
                size_t ready = 0;
                auto oldest = entries.end();
                for (auto it = entries.begin(); it != entries.end(); ++it) {
                    if (it->second.state != State::Ready) continue;
                    ++ready;
                    if (oldest == entries.end() || it->second.order < oldest->second.order) oldest = it;
                }
                if (ready <= MaxReady) return;
                entries.erase(oldest);
            }
        }
    };
    std::shared_ptr<Shared> shared;
};

} // namespace BbTrophyIcons
