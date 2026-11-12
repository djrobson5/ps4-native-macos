#!/usr/bin/env bash
# tests/app_launch_equality.sh: Bloodborne.app prepares its launch by running the bundle's own run.sh
# (src/app_bundle.c); this checks that what it ends on is what run.sh itself gives the probe when
# it is run from the repository, for several bbport.ini and wayfarer.cfg choices. And that
# 13 Sentinels.app sets none of the new variables unless wayfarer.cfg asks for them. And the shader
# compile step's switches: BB_SHADER_NOTE=1 by default on Metal only, and --compile-shaders. No window is
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
    # The app's one default that run.sh does not have: BB_SHADER_NOTE=1 on the Metal renderer (a
    # reference with BB_METAL_FRAME=0 is the MoltenVK one). It is given to run.sh here, so the
    # comparison still covers it.
    local note=(BB_SHADER_NOTE=1) a
    for a in "$@"; do [[ $a == BB_METAL_FRAME=0 ]] && note=(); done
    env -i HOME="$HOME" PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" "${note[@]}" \
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

# The shader compile step's switches. raw_app prints the app's whole dry run (stdout; its status is
# the app's); the arguments line is as dumped by run.sh and taken over, the directory the one the
# game would start in.
raw_app() { # data dir, then NAME=value (the environment) and other words (the app's arguments), in any order
    local data=$1 a vars=() args=()
    shift
    for a in "$@"; do if [[ $a =~ ^[A-Z_]+= ]]; then vars+=("$a"); else args+=("$a"); fi; done
    env -i HOME="$HOME" PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" BB_APP_DATA_DIR="$data" BB_APP_DRY_RUN=1 ${vars[@]+"${vars[@]}"} \
        "$bb_app/Contents/MacOS/Bloodborne" ${args[@]+"${args[@]}"} 2>/dev/null
}
check() { # NAME, then a condition's exit status
    local name=$1 ok=$2
    if [[ $ok == 0 ]]; then echo "PASS $name"; else echo "FAIL $name"; failures=$((failures + 1)); fi
    runs=$((runs + 1))
}
has() { grep -qxF -- "$2" <<<"$1"; } # has "$text" "line"
data=$work/data-note
mkdir -p "$data/moltenvk"
printf 'renderer=moltenvk\n' > "$data/moltenvk/wayfarer.cfg"

normal=$(raw_app "$data")
has "$normal" 'App: BB_SHADER_NOTE=1'; check 'Bloodborne on Metal: BB_SHADER_NOTE=1 by default' $?
! grep -q 'BB_SHADER_COMPILE' <<<"$normal"; check 'a normal launch sets no BB_SHADER_COMPILE*' $?
mv_run=$(raw_app "$data/moltenvk")
! grep -q 'BB_SHADER_NOTE' <<<"$mv_run"; check 'renderer=moltenvk: no BB_SHADER_NOTE' $?
has "$mv_run" 'App: BB_METAL_FRAME=0'; check 'renderer=moltenvk: BB_METAL_FRAME=0 (the choice still applies)' $?
has "$(raw_app "$data" BB_SHADER_NOTE=0)" 'App: BB_SHADER_NOTE=0'; check 'the environment'"'"'s BB_SHADER_NOTE wins' $?
! grep -q 'BB_SHADER_NOTE' <<<"$(raw_app "$data" BB_METAL_FRAME=0)"; check 'BB_METAL_FRAME=0 in the environment: no BB_SHADER_NOTE' $?

# --compile-shaders: the same arguments and directory as a normal launch (the app's own argument is
# not the game's), the three variables more.
forced=$(raw_app "$data" --compile-shaders)
for v in BB_SHADER_COMPILE=1 BB_SHADER_COMPILE_EXIT=1 BB_SHADER_COMPILE_FORCE=1 BB_SHADER_NOTE=1; do
    has "$forced" "App: $v"; check "--compile-shaders: $v" $?
done
[[ $(grep -E '^App: (arguments:|directory )' <<<"$forced") == "$(grep -E '^App: (arguments:|directory )' <<<"$normal")" ]]
check '--compile-shaders: the same arguments and directory as a normal launch' $?
grep -q '^App: directory /' <<<"$normal"; check 'the dry run names the directory (a guard for the line above)' $?
! grep -q -- 'compile-shaders' <<<"$forced"; check '--compile-shaders is not among the game'"'"'s arguments' $?
# Others' arguments and AppKit's are as before, in either order; the game gets the same ones.
forced_more=$(raw_app "$data" -psn_0_12345 --compile-shaders --headless)
normal_more=$(raw_app "$data" -psn_0_12345 --headless)
[[ $(grep -E '^App: (arguments:|directory )' <<<"$forced_more") == "$(grep -E '^App: (arguments:|directory )' <<<"$normal_more")" ]] &&
    grep -q -- '\[--headless\]' <<<"$forced_more"
check '--compile-shaders with -psn_ and --headless: the game gets --headless only' $?
# The rest of the environment is a normal launch's, plus the three.
diff <(grep -E '^App: [A-Z]' <<<"$normal" | grep -vE '^App: (arguments|directory|data|title)' ) \
     <(grep -E '^App: [A-Z]' <<<"$forced" | grep -vE '^App: (arguments|directory|data|title|BB_SHADER_COMPILE)') >/dev/null
check '--compile-shaders: no other variable differs from a normal launch' $?

# Refused: nothing is launched (no dry run output, no run.sh dump), and the codes are the documented ones.
rm -rf "$data/moltenvk/out"
status=0
out=$(raw_app "$data/moltenvk" --compile-shaders) || status=$?
[[ $status == 4 && -z $out && ! -e $data/moltenvk/out ]]; check "--compile-shaders with renderer=moltenvk: exit 4, nothing launched (exit $status)" $?
status=0
out=$(raw_app "$data" BB_METAL_FRAME=0 --compile-shaders) || status=$?
[[ $status == 4 && -z $out ]]; check "--compile-shaders with BB_METAL_FRAME=0 in the environment: exit 4 (exit $status)" $?
status=0
out=$(raw_app "$data" BB_VK_DRIVER=kosmickrisp --compile-shaders) || status=$?
[[ $status == 4 && -z $out ]]; check "--compile-shaders with BB_VK_DRIVER set: exit 4 (exit $status)" $?
msg=$(env -i HOME="$HOME" PATH="$PATH" BB_APP_DATA_DIR="$data/moltenvk" BB_APP_DRY_RUN=1 "$bb_app/Contents/MacOS/Bloodborne" --compile-shaders 2>&1 >/dev/null) || true
grep -q 'MoltenVK' <<<"$msg"; check '... and a message on stderr says why' $?
s13_data=$work/data-13s-compile
mkdir -p "$s13_data"
status=0
out=$(env -i HOME="$HOME" PATH="$PATH" S13_APP_DATA_DIR="$s13_data" BB_APP_DRY_RUN=1 "$s13_app/Contents/MacOS/13 Sentinels" --compile-shaders 2>/dev/null) || status=$?
[[ $status == 3 && -z $out ]]; check "--compile-shaders on 13 Sentinels: exit 3, nothing launched (exit $status)" $?
msg=$(env -i HOME="$HOME" PATH="$PATH" S13_APP_DATA_DIR="$s13_data" BB_APP_DRY_RUN=1 "$s13_app/Contents/MacOS/13 Sentinels" --compile-shaders 2>&1 >/dev/null) || true
grep -q 'not Bloodborne' <<<"$msg"; check '... and a message on stderr says why' $?

# 13 Sentinels: no new variable without its key; with them, the values below.
data=$work/data-13s
mkdir -p "$data"
s13() { env -i HOME="$HOME" PATH="$PATH" S13_APP_DATA_DIR="$data" BB_APP_DRY_RUN=1 "$@" "$s13_app/Contents/MacOS/13 Sentinels" 2>/dev/null | grep -E '^App: (BB_|MTL_|VK_|MVK_)'; }
new_vars='^App: (BB_SHADER_NOTE|BB_DISPLAY|BB_PRESENT_MODE|BB_FPS|BB_FPS_LIMIT|BB_FRAME_STATS|BB_FRAME_LOG|MTL_HUD_ENABLED)='
if s13 | grep -E "$new_vars"; then echo 'FAIL 13 Sentinels: a new variable is set without its wayfarer.cfg key'; failures=$((failures + 1))
else echo 'PASS 13 Sentinels: no new variable without wayfarer.cfg keys'; fi
printf 'display=2\nvsync=1\nfps_mode=60\nfps_limit=0\nmetal_hud=0\nframe_log=1\n' > "$data/wayfarer.cfg"
got=$(s13 | grep -E "$new_vars" | sed -E 's#[0-9]{8}-[0-9]{6}#<stamp>#; s#=.*/logs/#=<data>/logs/#' | LC_ALL=C sort | tr '\n' ' ')
want='App: BB_DISPLAY=2 App: BB_FRAME_LOG=<data>/logs/<stamp>.frames.csv App: BB_FRAME_STATS=1 App: BB_PRESENT_MODE=Fifo '
if [[ $got == "$want" ]]; then echo 'PASS 13 Sentinels: display, vsync and frame_log set; fps_mode ignored; fps_limit=0 and metal_hud=0 set nothing'
else echo "FAIL 13 Sentinels: wanted [$want] got [$got]"; failures=$((failures + 1)); fi

echo "app launch equality: $((runs + 3)) comparisons, $failures failed"
((failures == 0))
