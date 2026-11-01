#!/bin/bash
# Build the Vulkan backend's tests (t_vk.c) for this machine and run them twice (BC sampled by the device, then decoded on the CPU), with the validation layer when it is installed.
# Needs a C compiler, the Vulkan loader (-lvulkan), glslangValidator and a Vulkan 1.1 device; on a desktop without a GPU, Mesa's lavapipe
# (apt install mesa-vulkan-drivers vulkan-validationlayers glslang-tools).
# Exit code = failures
set -e
here=$(cd "$(dirname "$0")" && pwd); dm=$(cd "$here/../.." && pwd)
cc=${CC:-cc}
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
"$cc" -O1 -g -std=gnu11 -Wall -Wno-unused-function -D_GNU_SOURCE -I"$dm/third_party/vulkan-headers/include" -I"$dm/gfx/nvn/include" -I"$dm/gfx/nvn" \
    "$here/t_vk.c" "$here/../gfx/nvn_vk.c" "$here/../gfx/nvn_fmt_vk.c" "$dm/gfx/nvn/nvn_tex.c" "$dm/gfx/tex/nvn_deswizzle.c" -lvulkan -lpthread -o "$out/t_vk"
# the draw tests' shaders, as dm/android/tools/nvn_vk_shaders.py lays them out (keys 1–5) with a shader map
sh="$out/shaders"; mkdir "$sh"
for k in "0000000000000001 vs.vert vertex" "0000000000000002 ps_vcol.frag pixel" "0000000000000003 ps_ubo.frag pixel" "0000000000000004 ps_tex.frag pixel" "0000000000000005 cs.comp compute"; do
    set -- $k
    glslangValidator -V --target-env vulkan1.1 -o "$sh/$1.spv" "$here/shaders/$2" >/dev/null || { echo "glslangValidator failed on $2" >&2; exit 2; }
    cp "$here/shaders/${2%.*}.res" "$sh/$1.res"
    printf 'test\t0\t%s\t%s\t%s\n' "$3" "$1" "$2" >> "$sh/shader_map.tsv"
done
python3 "$here/../tools/spv_mobile.py" "$sh/0000000000000001.spv" "$sh/0000000000000001.gl.spv" --gl-depth >/dev/null
export DM_VK_SHADERS="$sh" DM_NVN_SHMAP="$sh/shader_map.tsv"
set +e
val=; [ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ] && val=1
fails=0
echo "== BC sampled"; DM_VK_VALIDATE=$val "$out/t_vk"; fails=$((fails + $?))
echo "== BC decoded on the CPU"; DM_VK_VALIDATE=$val "$out/t_vk" nobc; fails=$((fails + $?))
exit $fails
