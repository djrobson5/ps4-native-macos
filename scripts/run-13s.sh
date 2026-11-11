#!/usr/bin/env bash
# scripts/run-13s.sh [bb-probe args...]: runs 13 Sentinels: Aegis Rim (CUSA19610 1.00) natively on
# Apple Silicon. run.sh is Bloodborne's launcher (its game check, 1.09 patches, frame-rate patches
# and online setup); this sets only what the native Metal path needs.
#   S13_GAME_DIR  the game folder (eboot.bin, sce_module/, sce_sys/); required
#   S13_DATA_DIR  writable folder for saves, caches, settings and logs (default: ./run-13s)
#   S13_TIMEOUT   seconds before bb-probe stops itself (default 0: none); a backstop kill follows
#                 30 s later
# Extra arguments go to bb-probe, e.g. --headless (no GPU, no window).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
game=${S13_GAME_DIR:?set S13_GAME_DIR to the game folder}
data=${S13_DATA_DIR:-$PWD/run-13s}
mkdir -p "$data/user" "$data/logs"
data=$(cd "$data" && pwd)
[[ -x out/bb-probe ]] || { echo 'No out/bb-probe: run bash build.sh first.' >&2; exit 1; }
[[ $(uname -m) == arm64 ]] || { echo 'The native path needs Apple Silicon.' >&2; exit 1; }

# S13_RENDERER=metal (default, run.sh's arm64 default) or moltenvk (the Vulkan renderer on MoltenVK,
# as run.sh's BB_METAL_FRAME=0).
export BB_ARCH=arm64 BB_GUEST_IN_PLACE=0
case ${S13_RENDERER:-metal} in
metal)
    export BB_METAL_FRAME=1 BB_METAL_NATIVE_BOOT=1 BB_GFX_BACKEND=metal
    # Metal has no quad list or triangle fan topology: draw both as indexed triangle lists (both
    # are off by default, so Bloodborne is unchanged). Without them the Metal frame skips those
    # draws.
    export BB_QUADS_AS_TRIANGLES=${BB_QUADS_AS_TRIANGLES:-1}
    export BB_FANS_AS_TRIANGLES=${BB_FANS_AS_TRIANGLES:-1}
    ;;
moltenvk)
    export BB_METAL_FRAME=0 BB_METAL_NATIVE_BOOT=0 BB_GFX_BACKEND=vulkan
    export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$PWD/deps/macos-arm64/share/vulkan/icd.d/MoltenVK_icd.json}
    export BB_VULKAN_LOADER=${BB_VULKAN_LOADER:-$PWD/deps/macos-arm64/lib/libvulkan.1.dylib}
    export MVK_CONFIG_LOG_LEVEL=${MVK_CONFIG_LOG_LEVEL:-1}
    ;;
*) echo "S13_RENDERER: metal or moltenvk" >&2; exit 1 ;;
esac
export BB_DATA_DIR=$data BB_USER_DIR=$data/user BB_GPU_USER_DIR=$data/user BB_CONFIG=$data/bbport.ini
# run.sh's GPU defaults on Apple GPUs; 13 Sentinels gets no frame-rate patch, so a 60 Hz vblank.
export BB_PREUPLOAD=${BB_PREUPLOAD:-1} BB_PC_MODEL=${BB_PC_MODEL:-0} BB_UFFD=0
export BB_COPY_GPU_BUFFERS=${BB_COPY_GPU_BUFFERS:-1}
export BB_GPU_WRITE_TWINS=${BB_GPU_WRITE_TWINS:-1} BB_GPU_WRITE_TWINS_MAX=${BB_GPU_WRITE_TWINS_MAX:-65536}
export BB_VBLANK_HZ=${BB_VBLANK_HZ:-60}

timeout=${S13_TIMEOUT:-0}
log=$data/logs/$(date +%Y%m%d-%H%M%S).log
echo "Log: $log"
args=(- --app0 "$game" --user "$data/user" --timeout "$timeout" "$@")
if [[ $timeout != 0 ]]; then
    perl -e 'alarm shift; exec @ARGV' $((timeout + 30)) out/bb-probe "${args[@]}" 2>&1 | tee "$log"
else
    out/bb-probe "${args[@]}" 2>&1 | tee "$log"
fi
exit "${PIPESTATUS[0]}"
