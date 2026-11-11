#!/usr/bin/env bash
# scripts/package-13s-app.sh: builds "13 Sentinels.app" from the last build (bash build.sh) and puts
# it, with a copy of the game's folder, in one folder (default ~/Games/13 Sentinels Aegis Rim):
#
#   <dest>/13 Sentinels.app   the bundle's executable is bb-probe itself (src/app_bundle.c sets up
#                             what scripts/run-13s.sh would); data in
#                             ~/Library/Application Support/13 Sentinels
#   <dest>/CUSA19610/         the game (eboot.bin, sce_module/, sce_sys/, the CPKs), copied once
#
# Options:
#   --dest DIR    the folder (default: ~/Games/13 Sentinels Aegis Rim)
#   --game DIR    the game's folder to copy when <dest>/CUSA19610 is missing
#                 (default: ../dump/CUSA19610/uroot beside this repo). Before anything is built it
#                 is checked with scripts/game_check_13s.py (CUSA19610 1.00, the supported
#                 eboot.bin): a game the runtime would refuse stops the script with the reason.
#   --icon PNG    the app icon's picture, put on a dark square. Without it the picture is chosen
#                 by scripts/game_check_13s.py --pick-icon: the game folder's sce_sys/icon0.png (the
#                 one to be copied, or <dest>/CUSA19610's), else the newest save data's icon0.png
#                 under $S13_APP_DATA_DIR (default ~/Library/Application Support/13 Sentinels),
#                 else none: the app gets no icon (macOS's generic one). One line says which.
#                 Game pictures stay out of git.
#   --no-game     leave the game's folder alone (only the app)
#   --icon-cache DIR   where the trophy icons are kept between runs (default: build/trophy-icons,
#                 which git ignores)
#
# The trophy icons: every icon_url of gpu/shim/trophy_catalog_13s.json (Sony's public image CDN) is
# downloaded to Contents/Resources/achievements/<trophy number>.png, once (the cache is looked at
# first). Wayfarer shows them beside the trophies. A download that fails is a warning: that trophy
# has no picture and Wayfarer draws a grade badge instead. The pictures stay out of git. Beside them,
# achievements.txt is the trophy list with nothing earned, so Wayfarer lists the trophies before the
# game's first run writes its own ledger.
#
# The payload comes from `cmake --install` (CMakeLists.txt: the libraries in Frameworks/ with only
# @-relative search paths, MoltenVK and its ICD file); Bloodborne's launcher parts (bash, Python,
# run.sh, scripts, the recompiler kit) are left out. The app is signed ad hoc.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
repo=$PWD

dest=$HOME/Games/'13 Sentinels Aegis Rim'
game_src=$(cd .. && pwd)/dump/CUSA19610/uroot
icon=
icon_cache=
copy_game=1
while (($#)); do
    case $1 in
    --dest) dest=$2; shift 2 ;;
    --game) game_src=$2; shift 2 ;;
    --icon) icon=$2; shift 2 ;;
    --no-game) copy_game=0; shift ;;
    --icon-cache) icon_cache=$2; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# The game that is about to be copied must be the one the runtime boots (before the long work).
game_args=()
if ((copy_game)) && [[ ! -f $dest/CUSA19610/eboot.bin ]]; then
    game_args=(--game "$game_src")
    python3 -I scripts/game_check_13s.py "$game_src" >&2 || exit 1
fi

app_name='13 Sentinels'
bundle_id=org.bbruntime.13sentinels
build=build/macos-arm64
[[ -x out/bb-probe && -d $build ]] || { echo 'No build: run bash build.sh first.' >&2; exit 1; }
if pgrep -f "$app_name.app/Contents/MacOS/" >/dev/null; then
    echo "$app_name is running: quit it first." >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/13s-app.XXXXXX")
trap 'rm -rf "$work"' EXIT
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

# The icon: the picture on a dark square, every size iconutil wants. The game's is built before the
# game is copied, hence the game folder to be copied is given too.
icon_key=
if [[ -n $icon ]]; then
    echo "Icon: $icon (given)"
else
    icon=$(python3 -I scripts/game_check_13s.py --pick-icon ${game_args[@]+"${game_args[@]}"} --game "$dest/CUSA19610")
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
catalog=gpu/shim/trophy_catalog_13s.json
[[ -n $icon_cache ]] || icon_cache=$repo/build/trophy-icons
mkdir -p "$icon_cache" "$app/Contents/Resources/achievements"
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
        if curl -fsSL --max-time 30 --retry 2 -A 'Mozilla/5.0 (Macintosh)' -o "$cached.part" "$url" &&
           [[ $(head -c 4 "$cached.part" | od -An -tx1 | tr -d ' \n') == 89504e47 ]]; then
            mv "$cached.part" "$cached"
            icons_fetched=$((icons_fetched + 1))
        else
            rm -f "$cached.part"
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

# The trophy list, nothing earned, in the ledger's format (src/runtime_trophies.c): Wayfarer shows it
# from the moment the app is installed, until the game's own user/achievements.txt exists.
python3 -I -c '
import json, sys
trophies = json.load(open(sys.argv[1], encoding="utf-8"))["trophies"]
assert [t["id"] for t in trophies] == list(range(len(trophies)))
lines = ["; The trophy list as installed, nothing earned (scripts/package-13s-app.sh).", "[unlocked]", "[grades]"]
lines += ["%d = %s" % (t["id"], t["grade"]) for t in trophies]
lines.append("[list]")
for t in trophies:
    en = t["translations"]["en"]
    name, description = en["name"], en.get("description", "")
    for text in (name, description):
        assert "|" not in text and "\n" not in text, t["id"]
    lines.append("%d = %s | %s" % (t["id"], name, description))
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(f"Trophy list: {len(trophies)} trophies in the app")' "$catalog" "$app/Contents/Resources/achievements/achievements.txt"

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
    <key>BBSourceCommit</key><string>$commit</string>
</dict>
</plist>
EOF
plutil -lint "$app/Contents/Info.plist" >/dev/null

# Nothing may point outside the system and the bundle (the build's absolute paths would still work
# on this Mac and hide a library left out).
for binary in "$app/Contents/MacOS/$app_name" "$app"/Contents/Frameworks/*.dylib; do
    [[ -L $binary ]] && continue
    if otool -l "$binary" | grep -E '^ +(path|name) /' | grep -vE ' (/System/|/usr/lib/)'; then
        echo "A path outside the bundle in $binary" >&2
        exit 1
    fi
done

for library in "$app"/Contents/Frameworks/*.dylib; do
    [[ -L $library ]] || codesign --force --sign - --timestamp=none "$library" 2>/dev/null
done
codesign --force --sign - --timestamp=none "$app" 2>/dev/null
codesign --verify --deep --strict "$app"

mkdir -p "$dest"
if ((copy_game)) && [[ ! -f $dest/CUSA19610/eboot.bin ]]; then
    [[ -f $game_src/eboot.bin ]] || { echo "No game at $game_src (no eboot.bin)" >&2; exit 1; }
    echo "Game: copying $game_src to $dest/CUSA19610"
    rm -rf "$dest/CUSA19610.partial"
    if ! cp -Rc "$game_src" "$dest/CUSA19610.partial" 2>/dev/null; then
        rm -rf "$dest/CUSA19610.partial" # not an APFS volume: a plain copy
        cp -R "$game_src" "$dest/CUSA19610.partial"
    fi
    mv "$dest/CUSA19610.partial" "$dest/CUSA19610"
fi

# The new app replaces the old one in a single rename (the old one goes once that has worked).
rm -rf "$dest/.$app_name.app.new" "$dest/.$app_name.app.old"
mv "$app" "$dest/.$app_name.app.new"
[[ -d $dest/$app_name.app ]] && mv "$dest/$app_name.app" "$dest/.$app_name.app.old"
mv "$dest/.$app_name.app.new" "$dest/$app_name.app"
rm -rf "$dest/.$app_name.app.old"
echo "App: $dest/$app_name.app ($(du -sh "$dest/$app_name.app" | cut -f1), $commit)"
