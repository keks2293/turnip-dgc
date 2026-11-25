#!/bin/bash
# VK_EXT_device_generated_commands (turnip). Builds the dgc-27 branch:
# the DGC commits on top of upstream Mesa 27.0-devel.
# src tree: /var/home/armada/opencode/mesa27-dgc, a git worktree of
# mesa27-src, so the patched ship tree in mesa27-src is never touched.
#
# Build rules (cache in /tmp) are the same as ../turnip-patches/scripts/:
#   /src     - the Mesa tree (SD card, read-only in practice)
#   /out     - tmpfs build dir: /tmp/opencode/mesa27-dgc-build
#   /ccache  - tmpfs ccache:    /tmp/opencode/ccache (cap 1G)
#   /hostout - artifacts to the SD card: build/out-dgc27 (~5 MB)
#   -j2, never together with the game.
#
# Fresh setup + build:
#   mkdir -p /tmp/opencode/mesa27-dgc-build /var/home/armada/opencode/build/out-dgc27
#   podman run --rm \
#     -v /var/home/armada/opencode/mesa27-dgc:/src:Z \
#     -v /var/home/armada/opencode/dgc:/repo:Z \
#     -v /tmp/opencode/mesa27-dgc-build:/out:Z \
#     -v /tmp/opencode/ccache:/ccache:Z \
#     -v /var/home/armada/opencode/build/out-dgc27:/hostout:Z \
#     localhost/mesa-build-fedora44-wsi-glslang bash /repo/scripts/build-dgc27.sh
#
# Incremental (only sources changed): FRESH=0, the build dir is reused.
set -euxo pipefail

JOBS="${BUILD_JOBS:-2}"
HOSTOUT="${HOSTOUT:-/hostout}"
FRESH="${FRESH:-1}"

cd /src

# The bracket keeps this check from matching its own command line.
if pgrep -x wineserver >/dev/null 2>&1 ||
   pgrep -x wine >/dev/null 2>&1 ||
   pgrep -f '[S]tarfield' >/dev/null 2>&1; then
    echo "WARNING: Wine/the game is running - build the driver with the game closed." >&2
    exit 1
fi

export CCACHE_DIR=/ccache
export CCACHE_MAXSIZE=1G
ccache -z || true

if [ "$FRESH" = "1" ]; then
    rm -rf /out/build
    meson setup /out/build /src \
        --buildtype=release \
        --prefix=/usr \
        -Dopengl=false \
        -Dplatforms=x11,wayland \
        -Dgallium-drivers= \
        -Dvulkan-drivers=freedreno \
        -Dfreedreno-kmds=msm \
        -Dvulkan-layers= \
        -Dshared-glapi=disabled \
        -Dllvm=disabled \
        -Dvalgrind=disabled \
        -Dlibunwind=disabled \
        -Dbuild-tests=false
fi

ninja -C /out/build -j"$JOBS" src/freedreno/vulkan/libvulkan_freedreno.so

SO=/out/build/src/freedreno/vulkan/libvulkan_freedreno.so

# KMD check: an msm build imports drmGetVersion (as in build-turnip-mesa27.sh).
if UNDEF="$(nm -D --undefined-only "$SO" 2>/dev/null)"; then
    if [[ "$UNDEF" != *drmGetVersion* ]]; then
        echo "WARNING: no drmGetVersion import - this looks like a kgsl build, not msm" >&2
        exit 1
    fi
fi

# DGC must really be compiled in. Note the vk* entry points are NOT exported
# symbols - an ICD is entered through vk_icdGetInstanceProcAddr /
# GetDeviceProcAddr and the common mesa layer dispatches those to the tu_*
# functions, and only when the extension bit is set. So check for the driver's
# own symbols (local, hidden visibility) instead of vk* ones.
if SYMS="$(nm "$SO" 2>/dev/null)"; then
    for sym in tu_dgc_execute tu_CreateIndirectCommandsLayoutEXT \
               tu_dgc_execute_generated_commands; do
        grep -q "$sym" <<<"$SYMS" ||
           echo "WARNING: $sym is missing - DGC code did not get compiled in" >&2
    done
fi

mkdir -p "$HOSTOUT"
cp -v "$SO" "$HOSTOUT/libvulkan_freedreno.so"
# The container only sees /hostout, while the manifest must name the path the
# host loader reads, so the host passes that path in.
cat > "$HOSTOUT/freedreno_icd.json" <<EOF
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": "${HOSTOUT_PATH:-$HOSTOUT}/libvulkan_freedreno.so",
        "api_version": "1.4.354",
        "library_arch": "64"
    }
}
EOF

sha256sum "$HOSTOUT/libvulkan_freedreno.so" | tee "$HOSTOUT/SHA256SUMS"
ls -l "$HOSTOUT/"
