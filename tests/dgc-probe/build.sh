#!/bin/bash
# Build the dgc-probe rigs without make (the RP6 host has no make; the
# Makefile stays as the documented reference for the same steps).
#
#   ./build.sh probe12            # build one probe (shaders + spv headers too)
#   ./build.sh                    # all probes
#   ICD=/path/freedreno_icd.json ./build.sh --run probe12   # build + run
#
# Everything compiles in podman (no compiler on the device). Image defaults
# match the Makefile header: one -wsi-glslang-vulkan image has gcc,
# glslangValidator, vulkan-headers and libvulkan.so.1.
set -uo pipefail
cd "$(dirname "$0")"

IMG=${IMG:-localhost/mesa-build-fedora44-wsi-glslang-vulkan}
VKLIB=${VKLIB:-/usr/lib64/libvulkan.so.1}
# ICD по умолчанию — комбинированный драйвер (игровой стек + серия DGC):
# единственная сборка, на которой имеет смысл проверять DGC. Голую сборку без
# стека 0001-0014 удалили: на ней vkd3d не поднимает SM 6.6, и прогон ничего
# не доказывает.
ICD=${ICD:-/var/home/armada/opencode/build/out-star/freedreno_icd.json}
POD="podman run --rm -v $PWD:/src:Z"

# probe -> "vert frag comp" shaders it includes (via *_spv.h)
shaders_for() {
   case "$1" in
      probe0d|probe12|probe13)   echo "vs_full:vert fs_bands:frag" ;;
      probe17)                   echo "vs_full:vert fs_green:frag" ;;
      probe18)                   echo "vs_full:vert fs_green:frag fs_red:frag" ;;
      probe19)                   echo "vs_full:vert fs_green:frag vs_pc:vert" ;;
      probe20)                   echo "vs_full:vert fs_green:frag" ;;
      probe21)                   echo "vs_full:vert fs_green:frag vs_pc:vert" ;;
      probe22)                   echo "vs_full:vert fs_green:frag" ;;
      probe23)                   echo "vs_full:vert fs_green:frag vs_pc:vert" ;;
      probe14|probe15)             echo "cs_dgc:comp" ;;
      probe16)                     echo "cs_dgc_pc:comp" ;;
      probe_dbg)                 echo "vs_full:vert fs_bands:frag fs_green:frag" ;;
      probe7d)                   echo "cp:comp" ;;
      probe9)                    echo "vs_ssb:vert fs_bands:frag" ;;
      probe10)                   echo "vs_ssb:vert fs_ssb:frag" ;;
      probe11)                   echo "vs_full:vert fs_green:frag" ;;
      *)                         echo "" ;;
   esac
}

build_shader() {  # $1=name $2=ext
   local n=$1 e=$2
   [ -s "shaders/$n.spv" ] && [ "shaders/$n.$e" -nt "shaders/$n.spv" ] ||
      $POD "$IMG" glslangValidator -V "/src/shaders/$n.$e" -o "/src/shaders/$n.spv" || return 1
   [ -s "${n}_spv.h" ] && [ "shaders/$n.spv" -nt "${n}_spv.h" ] ||
      python3 spv2h.py "shaders/$n.spv" "${n}_spv.h" "$(echo "$n" | tr '[:lower:]' '[:upper:]')_SPV" || return 1
}

build_probe() {
   local p=$1
   for s in $(shaders_for "$p"); do
      build_shader "${s%%:*}" "${s##*:}" || return 1
   done
   echo "== build $p"
   $POD "$IMG" gcc -O2 -Wall -o "/src/$p" "/src/$p.c" "$VKLIB" || return 1
}

if [ "${1:-}" = "--run" ]; then
   shift
   p=${1:?probe name required}
   build_probe "$p" || exit 1
   echo "== run $p (ICD=$ICD)"
   VK_DRIVER_FILES=$ICD ./"$p"
   echo "== RC=$?"
   exit 0
fi

targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
   targets=(probe0d probe7d probe9 probe10 probe11 probe12 probe13 probe14 probe15 probe16 probe17 probe18 probe19 probe_dbg)
fi
rc=0
for p in "${targets[@]}"; do
   build_probe "$p" || rc=1
done
ls -l probe* 2>/dev/null | grep -v '\.c$'
exit $rc
