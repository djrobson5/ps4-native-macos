#!/usr/bin/env bash
# tests/app_launch_equality.sh: Bloodborne.app prepares its launch by running the bundle's own run.sh
# (src/app_bundle.c); this checks that what it ends on is what run.sh itself gives the probe when
# it is run from the repository, for several bbport.ini and wayfarer.cfg choices. And that
# 13 Sentinels.app sets none of the new variables unless wayfarer.cfg asks for them. No window is
# opened: the apps run with BB_APP_DRY_RUN=1, run.sh's last line runs a wrapper that records its
# arguments and environment.
#
#   BB_TEST_GAME=<a Bloodborne game folder (CUSA03173: eboot.bin, sce_module, ...)> tests/app_launch_equality.sh
#
# The game folder is only read (it is linked into the scratch folder, never written). Without it, or
# without a build (bash build.sh), the test is skipped with exit code 77. It packages both apps into
# a scratch folder (scripts/package-app.sh, no trophy icons, no game copy) and uses scratch data
# folders (BB_APP_DATA_DIR, S13_APP_DATA_DIR): the player's are never touched.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
repo=$PWD

skip() { echo "SKIP: $*"; exit 77; }
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || skip 'the app is built for Apple Silicon'
game=${BB_TEST_GAME:-}
[[ -n $game && -f $game/eboot.bin ]] || skip 'BB_TEST_GAME is not set to a Bloodborne game folder (with eboot.bin)'
[[ -x out/bb-probe && -x out/bb-gpu-capabilities && -d build/macos-arm64 ]] || skip 'no build: run bash build.sh first'
if pgrep -f 'Bloodborne.app/Contents/MacOS/|13 Sentinels.app/Contents/MacOS/' >/dev/null; then skip 'an app is running'; fi

work=$(mktemp -d "${TMPDIR:-/tmp}/app-equality.XXXXXX")
work=$(cd "$work" && pwd -P) # the app resolves its own path (/var is /private/var here): so does the reference
trap 'rm -rf "${work:?}"' EXIT
export S13_APP_DATA_DIR=$work/data-icon BB_APP_DATA_DIR=$work/data-icon # where the icon pick looks: nowhere
echo "Packaging into $work (this takes a minute)"
/bin/bash scripts/package-app.sh --title bloodborne --dest "$work/bb" --no-game --no-trophy-icons >"$work/package-bb.log" 2>&1 ||
    { cat "$work/package-bb.log" >&2; exit 1; }
/bin/bash scripts/package-13s-app.sh --dest "$work/13s" --no-game --icon-cache "$work/none" >"$work/package-13s.log" 2>&1 ||
    { cat "$work/package-13s.log" >&2; exit 1; }
ln -s "$game" "$work/bb/CUSA03173"
mkdir "$work/13s/CUSA19610"
: > "$work/13s/CUSA19610/eboot.bin" # a dry run only looks for it
bb_app=$work/bb/Bloodborne.app
s13_app="$work/13s/13 Sentinels.app"
unset S13_APP_DATA_DIR BB_APP_DATA_DIR

# The reference: run.sh from this checkout, its last line a wrapper that records what it was given.
bin=$work/ref-bin
mkdir "$bin"
ln -s "$repo/out/bb-gpu-capabilities" "$bin/bb-gpu-capabilities" # run.sh looks for it beside BB_PROBE
cat > "$bin/probe-wrapper" <<'WRAP'
#!/bin/bash
{ printf 'arguments:'; for a in "$@"; do printf ' [%s]' "$a"; done; printf '\n'; env; } > "$WRAPPER_OUT"
WRAP
chmod +x "$bin/probe-wrapper"

# What differs by construction, left out of both sides (the app's own variables and the pieces of
# the launch that name the app's layout): the comparison is of the rest.
ignored='^(BB_PREBUILT|BB_PROBE|BB_PROBE_PROGRAM|BB_LAUNCH_DUMP|BB_APP_BUNDLE|BB_APP_DRY_RUN|BB_APP_DATA_DIR|BB_APP_PREPARING|BB_APP_LOG|PYTHON|PYTHONDONTWRITEBYTECODE|BB_TEST_GAME|WRAPPER_OUT)='
echo "Left out of both sides: BB_PREBUILT, BB_PROBE, BB_PROBE_PROGRAM, BB_LAUNCH_DUMP, BB_APP_BUNDLE, BB_APP_DRY_RUN,"
echo "  BB_APP_DATA_DIR, BB_APP_PREPARING, BB_APP_LOG, PYTHON, PYTHONDONTWRITEBYTECODE (the app's own)."
echo "Normalized: the program's name (the app's first argument), the frame log's stamp, the MoltenVK files' paths."

# "arguments: ..." and the BB_/MTL_/VK_/MVK_ variables, sorted, normalized.
normalize() {
    sed -E -e 's#^App: ##' \
        -e 's#[0-9]{8}[-_][0-9]{6}#<stamp>#g' \
        -e 's#^(VK_DRIVER_FILES)=.*#\1=<icd>#' -e 's#^(BB_VULKAN_LOADER)=.*#\1=<loader>#' |
        grep -vE "$ignored" | LC_ALL=C sort
}
app_side() { # data dir, app, extra environment...
    local data=$1 app=$2
    shift 2
    env -i HOME="$HOME" PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" BB_APP_DATA_DIR="$data" BB_APP_DRY_RUN=1 "$@" \
        "$app/Contents/MacOS/$(basename "$app" .app)" 2>/dev/null |
        grep -E '^App: (arguments:|(BB_|MTL_|VK_|MVK_)[A-Za-z0-9_]*=)' |
        sed -E 's#^(App: arguments:) \[[^]]*\] #\1 #' | normalize
}
ref_side() { # data dir, extra environment...
    local data=$1
    shift
    env -i HOME="$HOME" PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" \
        BB_PREBUILT=1 BB_GAME_DIR="$work/bb/CUSA03173" BB_DATA_DIR="$data" BB_CONFIG="$data/bbport.ini" \
        BB_USER_DIR="$data/user" BB_MODS_ENABLED=0 PYTHON="$bb_app/Contents/Resources/python/bin/python3" \
        BB_PROBE="$bin/probe-wrapper" BB_PROBE_PROGRAM="$repo/out/bb-probe" WRAPPER_OUT="$work/ref.out" \
        "$@" "$bb_app/Contents/MacOS/bash" "$repo/run.sh" > "$work/ref-run.log" 2>&1 ||
        { cat "$work/ref-run.log" >&2; return 1; }
    { sed -n 1p "$work/ref.out"; sed 1d "$work/ref.out" | grep -E '^(BB_|MTL_|VK_|MVK_)[A-Za-z0-9_]*='; } | normalize
}

failures=0 runs=0
# compare NAME 'bbport.ini text' 'wayfarer.cfg text' [VAR=value for the reference]...
compare() {
    local name=$1 ini=$2 cfg=$3
    shift 3
    local data=$work/data-$((++runs))
    mkdir -p "$data"
    if [[ -n $ini ]]; then printf '%s\n' "$ini" > "$data/bbport.ini"; fi
    if [[ -n $cfg ]]; then printf '%s\n' "$cfg" > "$data/wayfarer.cfg"; fi
    app_side "$data" "$bb_app" > "$work/app.txt" || { echo "FAIL $name: the app's dry run failed"; failures=$((failures + 1)); return; }
    ref_side "$data" "$@" > "$work/ref.txt" || { echo "FAIL $name: run.sh failed"; failures=$((failures + 1)); return; }
    if diff -u "$work/ref.txt" "$work/app.txt" > "$work/diff.txt"; then
        echo "PASS $name ($(wc -l < "$work/app.txt" | tr -d ' ') lines)"
    else
        echo "FAIL $name (- run.sh, + the app):"
        cat "$work/diff.txt"
        failures=$((failures + 1))
    fi
}

ini_none=
ini_1440='output_res=2560x1440'
ini_metalfx=$'upscaler=metalfx\npreset=2'
ini_effects=$'effect_ssr=1\nskip_intro=1\nmodel_lod=-2'
cfg_perf=$'vsync=0\nfps_limit=60\nfps_mode=60'
perf_env=(BB_PRESENT_MODE=Immediate BB_FPS_LIMIT=60 BB_FPS=60)
for variant in none:"$ini_none" 1440p:"$ini_1440" metalfx:"$ini_metalfx" effects:"$ini_effects"; do
    label=${variant%%:*} ini=${variant#*:}
    compare "bbport.ini $label, no wayfarer.cfg" "$ini" ''
    compare "bbport.ini $label, vsync=0 fps_limit=60 fps_mode=60" "$ini" "$cfg_perf" "${perf_env[@]}"
done
compare 'wayfarer.cfg renderer=moltenvk fullscreen=1 display=2 metal_hud=1' "$ini_none" \
    $'renderer=moltenvk\nfullscreen=1\ndisplay=2\nmetal_hud=1\n# a comment\nunknown_key=1\nvsync=maybe' \
    BB_METAL_FRAME=0 BB_FULLSCREEN=1 BB_DISPLAY=2 MTL_HUD_ENABLED=1
compare 'wayfarer.cfg frame_log=1' "$ini_none" 'frame_log=1' BB_FRAME_STATS=1 BB_FRAME_LOG="$work/data-$((runs + 1))/logs/20000101-000000.frames.csv"
# The last one: the variables are in the app's environment, so the app side needs them too.
data=$work/data-env
mkdir -p "$data"
printf 'fps_mode=60\nvsync=0\n' > "$data/wayfarer.cfg"
app_side "$data" "$bb_app" BB_FPS=90 BB_PRESENT_MODE=Fifo > "$work/app.txt"
ref_side "$data" BB_FPS=90 BB_PRESENT_MODE=Fifo > "$work/ref.txt"
if diff -u "$work/ref.txt" "$work/app.txt" > "$work/diff.txt"; then echo "PASS the environment's values win (BB_FPS=90, Fifo)"
else echo 'FAIL the environment did not win:'; cat "$work/diff.txt"; failures=$((failures + 1)); fi

# The handover installs the dumped environment whole: a variable unsetenv can't remove (a name of
# 300 characters) must not keep the old environment, with what run.sh unset and the preparation's
# own variables.
rm -f "$data/wayfarer.cfg"
long="$(printf 'A%.0s' {1..300})=1"
if app_side "$data" "$bb_app" "$long" BB_RENDER_RES=1x1 BB_AUTO_RENDER_RES=1 |
        grep -E '^App: (BB_APP_PREPARING|BB_LAUNCH_DUMP|BB_RENDER_RES|BB_AUTO_RENDER_RES)='; then
    echo 'FAIL a long variable name kept the old environment'; failures=$((failures + 1))
else echo 'PASS a long variable name: the dumped environment replaces the old one whole'; fi
runs=$((runs + 1))

# 13 Sentinels: no new variable without its key; with them, the values below.
data=$work/data-13s
mkdir -p "$data"
s13() { env -i HOME="$HOME" PATH="$PATH" S13_APP_DATA_DIR="$data" BB_APP_DRY_RUN=1 "$@" "$s13_app/Contents/MacOS/13 Sentinels" 2>/dev/null | grep -E '^App: (BB_|MTL_|VK_|MVK_)'; }
new_vars='^App: (BB_DISPLAY|BB_PRESENT_MODE|BB_FPS|BB_FPS_LIMIT|BB_FRAME_STATS|BB_FRAME_LOG|MTL_HUD_ENABLED)='
if s13 | grep -E "$new_vars"; then echo 'FAIL 13 Sentinels: a new variable is set without its wayfarer.cfg key'; failures=$((failures + 1))
else echo 'PASS 13 Sentinels: no new variable without wayfarer.cfg keys'; fi
printf 'display=2\nvsync=1\nfps_mode=60\nfps_limit=0\nmetal_hud=0\nframe_log=1\n' > "$data/wayfarer.cfg"
got=$(s13 | grep -E "$new_vars" | sed -E 's#[0-9]{8}-[0-9]{6}#<stamp>#; s#=.*/logs/#=<data>/logs/#' | LC_ALL=C sort | tr '\n' ' ')
want='App: BB_DISPLAY=2 App: BB_FRAME_LOG=<data>/logs/<stamp>.frames.csv App: BB_FRAME_STATS=1 App: BB_PRESENT_MODE=Fifo '
if [[ $got == "$want" ]]; then echo 'PASS 13 Sentinels: display, vsync and frame_log set; fps_mode ignored; fps_limit=0 and metal_hud=0 set nothing'
else echo "FAIL 13 Sentinels: wanted [$want] got [$got]"; failures=$((failures + 1)); fi

echo "app launch equality: $((runs + 3)) comparisons, $failures failed"
((failures == 0))
