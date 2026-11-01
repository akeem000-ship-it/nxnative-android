// NVNformat → Vulkan format and component swizzle, for the Vulkan backend (nvn_vk.c). The Metal backend's table (dm/gfx/tex/nvn_fmt_mtl.c) made for Vulkan: same entries, same kinds.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>

typedef struct {
    VkFormat vk, alt;          // the format, and the one to use when the device cannot use it for this texture (0 = none)
    uint8_t kind;              // 1 exact; 2 substitute (precision, bit order or read mode differ, see the table); 0 no counterpart
    uint8_t expand;            // 3-component storage widened to 4 components on upload: the source bytes per component (1, 2, 4); 0 = none
    uint8_t bc;                // a BC format (1–7 = BCn, 6 = BC6H): decoded on the CPU when the device cannot sample BC (nvn_vk.c bc_decode)
    VkComponentSwizzle swz[4]; // r g b a read from these components of the Vulkan texel (IDENTITY = the same)
} nvn_vkfmt;

// The entry for an NVN format; kind 0 (vk 0) for an unknown or unmatched one
nvn_vkfmt nvn_fmt_to_vk(int nvn_format);
// The aspect of a depth / stencil format (0 for colour formats)
VkImageAspectFlags nvn_vk_aspect(VkFormat f);
