// SPDX-License-Identifier: GPL-2.0-or-later
// The trophy notice's icons (bbport_trophy_icons.h) without a GPU: the folder lookup (app layout,
// env override, missing), decode-or-fallback on a good PNG, a truncated file, a file that is not a
// PNG and a missing one, and the load cache. The PNGs are written here (stored deflate blocks), no
// real icon is needed.
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "bbport_trophy_icons.h"

namespace fs = std::filesystem;
using BbTrophyIcons::Cache;
using BbTrophyIcons::Image;

namespace {

uint32_t Crc32(const unsigned char* data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Put32(std::vector<unsigned char>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((unsigned char)(v >> shift));
}

void Chunk(std::vector<unsigned char>& out, const char* type, const std::vector<unsigned char>& body) {
    Put32(out, uint32_t(body.size()));
    std::vector<unsigned char> tagged(type, type + 4);
    tagged.insert(tagged.end(), body.begin(), body.end());
    out.insert(out.end(), tagged.begin(), tagged.end());
    Put32(out, Crc32(tagged.data(), tagged.size()));
}

/// A valid RGBA PNG (8 bits, unfiltered scanlines, stored deflate blocks) of the pixels `rgba`.
std::vector<unsigned char> MakePng(int w, int h, const std::vector<unsigned char>& rgba) {
    std::vector<unsigned char> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba.begin() + size_t(y) * size_t(w) * 4, rgba.begin() + size_t(y + 1) * size_t(w) * 4);
    }
    std::vector<unsigned char> z = {0x78, 0x01};
    for (size_t at = 0; at < raw.size() || raw.empty();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((unsigned char)(n & 0xff)), z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 0xff)), z.push_back((unsigned char)((~n >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    Put32(z, (b << 16) | a);

    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<unsigned char> ihdr;
    Put32(ihdr, uint32_t(w)), Put32(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8 bit, RGBA, deflate, filter 0, no interlace
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    return png;
}

/// A picture with a different colour in every pixel: (x*5, y*7, x+y, 255 - x).
std::vector<unsigned char> Pattern(int w, int h) {
    std::vector<unsigned char> px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px.insert(px.end(), {(unsigned char)(x * 5), (unsigned char)(y * 7), (unsigned char)(x + y), (unsigned char)(255 - x)});
    return px;
}

void Write(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    assert(file.good());
}

template <class Done> bool Wait(Done done) {
    for (int i = 0; i < 500; ++i) { // up to 5 s
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void TestResolveDir(const fs::path& root) {
    const fs::path contents = root / "App.app" / "Contents";
    const fs::path icons = contents / "Resources" / "achievements";
    fs::create_directories(contents / "MacOS");
    // No icon folder yet: nothing, whatever the base.
    assert(BbTrophyIcons::ResolveDir(nullptr, (contents / "MacOS").c_str()).empty());
    assert(BbTrophyIcons::ResolveDir(nullptr, nullptr).empty());
    assert(BbTrophyIcons::ResolveDir("", "").empty());
    fs::create_directories(icons);
    // The executable's folder (Contents/MacOS), with and without a trailing slash.
    for (const std::string base : {(contents / "MacOS").string(), (contents / "MacOS").string() + "/"}) {
        const fs::path found = BbTrophyIcons::ResolveDir(nullptr, base.c_str());
        assert(!found.empty() && fs::equivalent(found, icons));
    }
    // SDL's base path in a bundle (Contents/Resources).
    const fs::path from_resources = BbTrophyIcons::ResolveDir(nullptr, (contents / "Resources").c_str());
    assert(!from_resources.empty() && fs::equivalent(from_resources, icons));
    // A folder beside the executable that is not an app: nothing.
    assert(BbTrophyIcons::ResolveDir(nullptr, root.c_str()).empty());
    // The override wins, even over a good app layout, and is exclusive: a missing one gives nothing.
    const fs::path other = root / "other";
    fs::create_directories(other);
    assert(BbTrophyIcons::ResolveDir(other.c_str(), (contents / "MacOS").c_str()) == other);
    assert(BbTrophyIcons::ResolveDir((root / "missing").c_str(), (contents / "MacOS").c_str()).empty());
    assert(BbTrophyIcons::ResolveDir("", (contents / "MacOS").c_str()) == BbTrophyIcons::ResolveDir(nullptr, (contents / "MacOS").c_str()));
    Write(root / "plain-file", {1, 2, 3}); // a file is not a folder
    assert(BbTrophyIcons::ResolveDir((root / "plain-file").c_str(), nullptr).empty());
}

void TestDecode(const fs::path& dir) {
    fs::create_directories(dir);
    std::string why;

    // A good PNG: exact pixels, no shrink under MaxEdge.
    const std::vector<unsigned char> pixels = Pattern(8, 6);
    const std::vector<unsigned char> good = MakePng(8, 6, pixels);
    Write(dir / "1.png", good);
    auto image = BbTrophyIcons::Load(dir, 1, &why);
    assert(image && image->w == 8 && image->h == 6 && image->rgba == pixels);

    // A big one is shrunk to MaxEdge, keeping the aspect; a flat colour stays that colour.
    std::vector<unsigned char> flat;
    for (int i = 0; i < 300 * 150; ++i) flat.insert(flat.end(), {10, 20, 30, 255});
    Write(dir / "2.png", MakePng(300, 150, flat));
    image = BbTrophyIcons::Load(dir, 2, &why);
    assert(image && image->w == BbTrophyIcons::MaxEdge && image->h == BbTrophyIcons::MaxEdge / 2);
    assert(image->rgba.size() == size_t(image->w) * size_t(image->h) * 4);
    for (size_t i = 0; i < image->rgba.size(); i += 4)
        assert(image->rgba[i] == 10 && image->rgba[i + 1] == 20 && image->rgba[i + 2] == 30 && image->rgba[i + 3] == 255);

    // Transparent pixels do not bleed their colour into the shrunk edge.
    std::vector<unsigned char> half;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            if (x < 128) half.insert(half.end(), {200, 0, 0, 255});
            else half.insert(half.end(), {0, 255, 0, 0});
        }
    Write(dir / "3.png", MakePng(256, 256, half));
    image = BbTrophyIcons::Load(dir, 3, &why);
    assert(image && image->w == 128 && image->h == 128);
    assert(image->rgba[(10 * 128 + 10) * 4] == 200 && image->rgba[(10 * 128 + 10) * 4 + 3] == 255);
    assert(image->rgba[(10 * 128 + 120) * 4 + 3] == 0);

    // A truncated file (header and part of the data), a file that is not a PNG, an empty one.
    Write(dir / "4.png", std::vector<unsigned char>(good.begin(), good.begin() + good.size() / 2));
    why.clear();
    assert(!BbTrophyIcons::Load(dir, 4, &why) && !why.empty());
    Write(dir / "5.png", std::vector<unsigned char>{'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g', ' ', 'a', 't', ' ', 'a', 'l', 'l'});
    why.clear();
    assert(!BbTrophyIcons::Load(dir, 5, &why) && why == "not a PNG file");
    Write(dir / "6.png", {});
    assert(!BbTrophyIcons::Load(dir, 6, &why));
    // Only the signature, then garbage.
    std::vector<unsigned char> signature_only(good.begin(), good.begin() + 8);
    signature_only.insert(signature_only.end(), 40, 0x55);
    Write(dir / "7.png", signature_only);
    assert(!BbTrophyIcons::Load(dir, 7, &why));
    // Missing, a bad id, no folder.
    why.clear();
    assert(!BbTrophyIcons::Load(dir, 9, &why) && why == "no such file");
    assert(!BbTrophyIcons::Load(dir, -1, &why));
    assert(!BbTrophyIcons::Load(dir, BbTrophyIcons::MaxId + 1, &why));
    assert(!BbTrophyIcons::Load(fs::path(), 1, &why));
    assert(!BbTrophyIcons::Decode(nullptr, 0, &why));
}

void TestCache(const fs::path& dir) {
    using State = Cache::State;
    // 12 good icons (ids 20..31), a truncated one (40), none for 41.
    const std::vector<unsigned char> good = MakePng(8, 8, Pattern(8, 8));
    for (int id = 20; id < 32; ++id) Write(dir / (std::to_string(id) + ".png"), good);
    Write(dir / "40.png", std::vector<unsigned char>(good.begin(), good.begin() + 30));

    Cache cache(dir, /*log=*/false);
    assert(cache.Get(20) == State::Unknown && !cache.Take(20));
    cache.Request(20);
    assert(Wait([&] { return cache.Get(20) == State::Ready; }));
    cache.Request(20); // known: no second load
    auto image = cache.Take(20);
    assert(image && image->w == 8 && image->h == 8);
    assert(cache.Get(20) == State::Unknown && !cache.Take(20)); // handed over once, then forgotten

    // Failures are remembered (one log line per id) and never become images.
    cache.Request(40);
    cache.Request(41);
    assert(Wait([&] { return cache.Get(40) == State::Failed && cache.Get(41) == State::Failed; }));
    assert(!cache.Take(40) && !cache.Take(41));
    assert(cache.Get(41) == State::Failed);

    // Many requests keep only a few decoded pictures.
    for (int id = 21; id < 32; ++id) cache.Request(id);
    assert(Wait([&] { return cache.Count(State::Loading) == 0; }));
    assert(cache.Count(State::Ready) <= BbTrophyIcons::MaxReady);
    assert(cache.Count(State::Ready) > 0);
    // An id the cache dropped reloads when asked for again.
    for (int id = 21; id < 32; ++id) {
        if (cache.Get(id) == State::Unknown) {
            cache.Request(id);
            assert(Wait([&] { return cache.Get(id) == State::Ready; }));
            break;
        }
    }

    // No folder: every request fails at once.
    Cache none{fs::path(), false};
    none.Request(1);
    assert(none.Get(1) == State::Failed && !none.Take(1));
    none.Request(500);
    assert(none.Get(500) == State::Failed);
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / ("bb-trophy-icons-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    TestResolveDir(root);
    TestDecode(root / "decode");
    TestCache(root / "decode");
    fs::remove_all(root);
    std::puts("PASS: trophy icons: folder lookup, decode or fall back (good, big, truncated, not a PNG, missing), cache");
    return 0;
}
