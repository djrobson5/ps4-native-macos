#!/usr/bin/env bash
# scripts/package-app.sh --title 13s|bloodborne: builds "13 Sentinels.app" or "Bloodborne.app" from
# the last build (bash build.sh) and puts it, with a copy of the game's folder, in one folder
# (default ~/Games/13 Sentinels Aegis Rim, or ~/Games/Bloodborne):
#
#   <dest>/<name>.app   the bundle's executable is bb-probe itself (src/app_bundle.c prepares the
#                       launch: for 13 Sentinels what scripts/run-13s.sh would set up, for
#                       Bloodborne what the bundle's own run.sh sets up); data in
#                       ~/Library/Application Support/<name>
#   <dest>/<CUSA...>/   the game (eboot.bin, sce_module/, sce_sys/, ...), copied once:
#                       CUSA19610 (13 Sentinels) or CUSA03173 (Bloodborne)
#
# Options:
#   --title T     13s or bloodborne (required)
#   --dest DIR    the folder (default: see above)
#   --game DIR    the game's folder to copy when <dest>/<CUSA...> is missing
#                 (default: ../dump/<CUSA...>/uroot beside this repo). Before anything is built it
#                 is checked with scripts/game_check_13s.py (CUSA19610 1.00, the supported
#                 eboot.bin) or scripts/game_check_bb.py: a game the runtime would refuse stops the
#                 script with the reason.
#   --icon PNG    the app icon's picture, put on a dark square. Without it the picture is chosen
#                 by the game check's --pick-icon: the game folder's sce_sys/icon0.png (the
#                 one to be copied, or <dest>/<CUSA...>'s), else the newest save data's icon0.png
#                 under $S13_APP_DATA_DIR or $BB_APP_DATA_DIR (default ~/Library/Application
#                 Support/<name>), else none: the app gets no icon (macOS's generic one). One line
#                 says which. Game pictures stay out of git.
#   --no-game     leave the game's folder alone (only the app)
#   --icon-cache DIR   where the trophy icons are kept between runs (default: build/trophy-icons,
#                 which git ignores)
#   --no-trophy-icons  download none (the checks that package an app need no network)
#
# The trophy icons: every icon_url of the title's catalog (gpu/shim/trophy_catalog_13s.json or
# trophy_catalog.json; Sony's public image CDN) is downloaded to
# Contents/Resources/achievements/<trophy number>.png, once (the cache is looked at first). Wayfarer
# shows them beside the trophies. A download that fails is a warning: that trophy has no picture and
# Wayfarer draws a grade badge instead. A catalog without icon_url entries is not an error. The
# pictures stay out of git. Beside them, achievements.txt is the trophy list with nothing earned, so
# Wayfarer lists the trophies before the game's first run writes its own ledger.
#
# The payload comes from `cmake --install` (CMakeLists.txt: the libraries in Frameworks/ with only
# @-relative search paths, MoltenVK and its ICD file). 13 Sentinels leaves Bloodborne's launcher
# parts (bash, Python, run.sh, scripts, patches, tools) out; Bloodborne keeps them, since its
# preparation is the real run.sh (the recompiler kit stays out: nothing at start needs it). The app
# is signed ad hoc, every Mach-O file in it first.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
repo=$PWD

title=
dest=
game_src=
icon=
icon_cache=
copy_game=1
fetch_icons=1
while (($#)); do
    case $1 in
    --title) title=${2:-}; shift 2 ;;
    --dest) dest=$2; shift 2 ;;
    --game) game_src=$2; shift 2 ;;
    --icon) icon=$2; shift 2 ;;
    --no-game) copy_game=0; shift ;;
    --icon-cache) icon_cache=$2; shift 2 ;;
    --no-trophy-icons) fetch_icons=0; shift ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# What differs between the two apps.
case $title in
13s)
    app_name='13 Sentinels'
    bundle_id=org.bbruntime.13sentinels
    title_id=CUSA19610
    default_dest=$HOME/Games/'13 Sentinels Aegis Rim'
    checker=scripts/game_check_13s.py
    catalog=gpu/shim/trophy_catalog_13s.json
    list_origin=scripts/package-13s-app.sh
    launcher_parts=0
    ;;
bloodborne)
    app_name=Bloodborne
    bundle_id=org.bbruntime.bloodborne
    title_id=CUSA03173
    default_dest=$HOME/Games/Bloodborne
    checker=scripts/game_check_bb.py
    catalog=gpu/shim/trophy_catalog.json
    list_origin=scripts/package-app.sh
    launcher_parts=1
    ;;
*) echo 'Usage: scripts/package-app.sh --title 13s|bloodborne [--dest DIR] [--game DIR] [--icon PNG] [--no-game] [--icon-cache DIR]' >&2; exit 1 ;;
esac
[[ -n $dest ]] || dest=$default_dest
[[ -n $game_src ]] || game_src=$(cd .. && pwd)/dump/$title_id/uroot
# A symlinked game folder is resolved first: cp copies a link named on its command line as a link,
# which would leave <dest>/<CUSA...> pointing at the original instead of being a copy.
if [[ -d $game_src ]]; then game_src=$(cd "$game_src" && pwd -P); fi
[[ -f $checker ]] || { echo "$checker is missing: the game cannot be checked." >&2; exit 1; }

# The game that is about to be copied must be the one the runtime boots (before the long work).
game_args=()
if ((copy_game)) && [[ ! -f $dest/$title_id/eboot.bin ]]; then
    game_args=(--game "$game_src")
    python3 -I "$checker" "$game_src" >&2 || exit 1
fi

build=build/macos-arm64
[[ -x out/bb-probe && -d $build ]] || { echo 'No build: run bash build.sh first.' >&2; exit 1; }
if pgrep -f "$app_name.app/Contents/MacOS/" >/dev/null; then
    echo "$app_name is running: quit it first." >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/$title-app.XXXXXX")
trap 'rm -rf "${work:?}"' EXIT
payload=$work/payload.app
echo "Payload: cmake --install $build"
cmake --install "$build" --prefix "$payload" >"$work/install.log" || { cat "$work/install.log" >&2; exit 1; }

app=$work/$app_name.app
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/game/share"
cp "$payload/Contents/MacOS/bb-probe" "$app/Contents/MacOS/$app_name"
cp -a "$payload/Contents/Frameworks" "$app/Contents/Frameworks"
cp -a "$payload/Contents/Resources/game/share/vulkan" "$app/Contents/Resources/game/share/vulkan"
# FSR 4 on Metal (an upscaler setting; off by default) looks in Resources/game/fsr4_metal.
[[ -d $payload/Contents/Resources/game/fsr4_metal ]] &&
    cp -a "$payload/Contents/Resources/game/fsr4_metal" "$app/Contents/Resources/game/fsr4_metal"
mkdir -p "$app/Contents/Resources/licenses"
cp "$payload/Contents/Resources/game/LICENSE" "$app/Contents/Resources/licenses/bloodborne-runtime.LICENSE"
[[ -f $payload/Contents/Resources/game/LICENSE.GPL-3.0 ]] &&
    cp "$payload/Contents/Resources/game/LICENSE.GPL-3.0" "$app/Contents/Resources/licenses/bbnet20.LICENSE.GPL-3.0"
if ((launcher_parts)); then
    # What run.sh needs beside itself (src/app_bundle.c: bash run.sh with BB_PREBUILT): its bash,
    # its Python, the GPU check (run.sh looks beside the probe for it), scripts/, patches/, tools/.
    cp "$payload/Contents/MacOS/bash" "$payload/Contents/MacOS/bb-gpu-capabilities" "$app/Contents/MacOS/"
    cp -a "$payload/Contents/Resources/python" "$app/Contents/Resources/python"
    for part in run.sh scripts patches tools README.md; do
        [[ -e $payload/Contents/Resources/game/$part ]] || { echo "The payload has no $part" >&2; exit 1; }
        cp -a "$payload/Contents/Resources/game/$part" "$app/Contents/Resources/game/$part"
    done
fi

# The icon: the picture on a dark square, every size iconutil wants. The game's is built before the
# game is copied, hence the game folder to be copied is given too.
icon_key=
if [[ -n $icon ]]; then
    echo "Icon: $icon (given)"
else
    icon=$(python3 -I "$checker" --pick-icon ${game_args[@]+"${game_args[@]}"} --game "$dest/$title_id")
    case $icon in
    '') echo 'Icon: none (the app gets the generic icon)' ;;
    */user/savedata/*) echo "Icon: save (the newest save data's icon0.png: $icon)" ;;
    *) echo "Icon: game ($icon)" ;;
    esac
fi
if [[ -n $icon ]]; then
    [[ -f $icon ]] || { echo "No icon file: $icon" >&2; exit 1; }
    square=$work/square.png
    sips -s format png -Z 860 "$icon" --out "$work/scaled.png" >/dev/null
    sips -p 1024 1024 --padColor 101418 "$work/scaled.png" --out "$square" >/dev/null 2>&1
    set=$work/AppIcon.iconset
    mkdir -p "$set"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$square" --out "$set/icon_${size}x${size}.png" >/dev/null
        sips -z $((size * 2)) $((size * 2)) "$square" --out "$set/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$app/Contents/Resources/AppIcon.icns"
    icon_key='    <key>CFBundleIconFile</key><string>AppIcon</string>'
fi

# The trophy icons (before the signing: the bundle's seal covers them).
[[ -n $icon_cache ]] || icon_cache=$repo/build/trophy-icons
mkdir -p "$icon_cache" "$app/Contents/Resources/achievements"
if ((fetch_icons)); then
    icons_total=0 icons_fetched=0 icons_cached=0 icons_failed=0
    while read -r number url; do
        icons_total=$((icons_total + 1))
        if [[ $url != https://image.api.playstation.com/* ]]; then
            echo "Warning: trophy $number: not Sony's image CDN, skipped: $url" >&2
            icons_failed=$((icons_failed + 1))
            continue
        fi
        key=$(printf %s "$url" | shasum -a 256 | cut -c1-16)
        cached=$icon_cache/$number-$key.png
        if [[ ! -s $cached ]]; then
            # A generic User-Agent; a partial download is never cached (a temporary name, then mv).
            # Sony's CDN serves some icons as JPEG under a .PNG name (Bloodborne's, 2026-11-11):
            # those are converted, so the app always holds PNGs.
            magic=
            if curl -fsSL --max-time 30 --retry 2 -A 'Mozilla/5.0 (Macintosh)' -o "$cached.part" "$url"; then
                magic=$(head -c 4 "$cached.part" | od -An -tx1 | tr -d ' \n')
            fi
            if [[ $magic == ffd8ff* ]] &&
               sips -s format png "$cached.part" --out "$cached.png.part" >/dev/null 2>&1; then
                mv "$cached.png.part" "$cached.part"
                magic=89504e47
            fi
            if [[ $magic == 89504e47 ]]; then
                mv "$cached.part" "$cached"
                icons_fetched=$((icons_fetched + 1))
            else
                rm -f "$cached.part" "$cached.png.part"
                echo "Warning: trophy $number: icon not downloaded ($url)" >&2
                icons_failed=$((icons_failed + 1))
                continue
            fi
        else
            icons_cached=$((icons_cached + 1))
        fi
        cp "$cached" "$app/Contents/Resources/achievements/$number.png"
    done < <(python3 -I -c '
import json, sys
for trophy in json.load(open(sys.argv[1], encoding="utf-8"))["trophies"]:
    if trophy.get("icon_url"):
        print(trophy["id"], trophy["icon_url"])' "$catalog")
    echo "Trophy icons: $((icons_fetched + icons_cached)) of $icons_total in the app ($icons_fetched downloaded, $icons_cached from the cache, $icons_failed failed)"
else
    echo 'Trophy icons: none (--no-trophy-icons)'
fi

# The trophy list, nothing earned, in the ledger's format (src/runtime_trophies.c): Wayfarer shows it
# from the moment the app is installed, until the game's own user/achievements.txt exists.
python3 -I -c '
import json, sys
trophies = json.load(open(sys.argv[1], encoding="utf-8"))["trophies"]
assert [t["id"] for t in trophies] == list(range(len(trophies)))
lines = ["; The trophy list as installed, nothing earned (%s)." % sys.argv[3], "[unlocked]", "[grades]"]
lines += ["%d = %s" % (t["id"], t["grade"]) for t in trophies]
lines.append("[list]")
for t in trophies:
    en = t["translations"]["en"]
    name, description = en["name"], en.get("description", "")
    for text in (name, description):
        assert "|" not in text and "\n" not in text, t["id"]
    lines.append("%d = %s | %s" % (t["id"], name, description))
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(f"Trophy list: {len(trophies)} trophies in the app")' "$catalog" "$app/Contents/Resources/achievements/achievements.txt" "$list_origin"

minos=$(otool -l "$payload/Contents/MacOS/bb-probe" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
commit=$(git -C "$repo" rev-parse --short HEAD)
dirty=$(git -C "$repo" status --porcelain --untracked-files=no | grep -v '^ [Mm] bbcpu$' | head -1 || true)
[[ -n $dirty ]] && commit=$commit+dirty
cat >"$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>$bundle_id</string>
    <key>CFBundleName</key><string>$app_name</string>
    <key>CFBundleDisplayName</key><string>$app_name</string>
    <key>CFBundleExecutable</key><string>$app_name</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>$(git -C "$repo" rev-list --count HEAD)</string>
$icon_key
    <key>NSHighResolutionCapable</key><true/>
    <key>LSMinimumSystemVersion</key><string>${minos:-15.0}</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>BBTitleID</key><string>$title_id</string>
    <key>BBSourceCommit</key><string>$commit</string>
</dict>
</plist>
EOF
plutil -lint "$app/Contents/Info.plist" >/dev/null

# Every Mach-O file of the bundle (the executables and libraries; Python's extension modules too).
# Candidates are the files that are executable or end in .so/.dylib, then the magic number decides.
macho_files=()
while IFS= read -r -d '' file; do
    [[ $(head -c 4 "$file" | od -An -tx1 | tr -d ' \n') == cffaedfe ]] && macho_files+=("$file")
done < <(find "$app/Contents" -type f \( -perm +111 -o -name '*.so' -o -name '*.dylib' \) -print0)

# Nothing may point outside the system and the bundle (the build's absolute paths would still work
# on this Mac and hide a library left out).
for binary in ${macho_files[@]+"${macho_files[@]}"}; do
    if otool -l "$binary" | grep -E '^ +(path|name) /' | grep -vE ' (/System/|/usr/lib/)'; then
        echo "A path outside the bundle in $binary" >&2
        exit 1
    fi
done

# Each file is signed before the bundle (the bundle's own signature seals them; the install's
# stripping of search paths left them unsigned or with a stale signature).
for binary in ${macho_files[@]+"${macho_files[@]}"}; do
    [[ $binary == "$app/Contents/MacOS/$app_name" ]] && continue
    codesign --force --sign - --timestamp=none "$binary" 2>/dev/null
done
codesign --force --sign - --timestamp=none "$app" 2>/dev/null
codesign --verify --deep --strict "$app"

mkdir -p "$dest"
if ((copy_game)) && [[ ! -f $dest/$title_id/eboot.bin ]]; then
    [[ -f $game_src/eboot.bin ]] || { echo "No game at $game_src (no eboot.bin)" >&2; exit 1; }
    echo "Game: copying $game_src to $dest/$title_id"
    rm -rf "${dest:?}/${title_id:?}.partial"
    if ! cp -Rc "$game_src" "$dest/$title_id.partial" 2>/dev/null; then
        rm -rf "${dest:?}/${title_id:?}.partial" # not an APFS volume: a plain copy
        cp -R "$game_src" "$dest/$title_id.partial"
    fi
    mv "$dest/$title_id.partial" "$dest/$title_id"
fi

# The new app replaces the old one in a single rename (the old one goes once that has worked).
rm -rf "${dest:?}/.${app_name:?}.app.new" "${dest:?}/.${app_name:?}.app.old"
mv "$app" "$dest/.$app_name.app.new"
[[ -d $dest/$app_name.app ]] && mv "$dest/$app_name.app" "$dest/.$app_name.app.old"
mv "$dest/.$app_name.app.new" "$dest/$app_name.app"
rm -rf "${dest:?}/.${app_name:?}.app.old"
echo "App: $dest/$app_name.app ($(du -sh "$dest/$app_name.app" | cut -f1), $commit)"
