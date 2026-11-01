#!/bin/bash
# Build the Vulkan backend's tests (t_vk.c) for this machine and run them twice (BC sampled by the device, then decoded on the CPU), with the validation layer when it is installed.
# Needs a C compiler, the Vulkan loader (-lvulkan) and a Vulkan 1.1 device; on a desktop without a GPU, Mesa's lavapipe (apt install mesa-vulkan-drivers vulkan-validationlayers).
# Exit code = failures
set -e
here=$(cd "$(dirname "$0")" && pwd); dm=$(cd "$here/../.." && pwd)
cc=${CC:-cc}
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
"$cc" -O1 -g -std=gnu11 -Wall -Wno-unused-function -D_GNU_SOURCE -I"$dm/third_party/vulkan-headers/include" -I"$dm/gfx/nvn/include" -I"$dm/gfx/nvn" \
    "$here/t_vk.c" "$here/../gfx/nvn_vk.c" "$here/../gfx/nvn_fmt_vk.c" "$dm/gfx/nvn/nvn_tex.c" "$dm/gfx/tex/nvn_deswizzle.c" -lvulkan -lpthread -o "$out/t_vk"
set +e
val=; [ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ] && val=1
fails=0
echo "== BC sampled"; DM_VK_VALIDATE=$val "$out/t_vk"; fails=$((fails + $?))
echo "== BC decoded on the CPU"; DM_VK_VALIDATE=$val "$out/t_vk" nobc; fails=$((fails + $?))
exit $fails
