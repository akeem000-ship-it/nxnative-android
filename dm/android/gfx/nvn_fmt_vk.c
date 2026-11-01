// NVNformat (nvn.h 0x01–0x9C) → Vulkan. Written entry by entry in NVN enum order, beside the Metal table (dm/gfx/tex/nvn_fmt_mtl.c), whose notes hold here too unless one says otherwise.
#include "nvn_fmt_vk.h"

#define I VK_COMPONENT_SWIZZLE_IDENTITY
#define ONE VK_COMPONENT_SWIZZLE_ONE
#define R_ VK_COMPONENT_SWIZZLE_R
#define G_ VK_COMPONENT_SWIZZLE_G
#define B_ VK_COMPONENT_SWIZZLE_B
#define A_ VK_COMPONENT_SWIZZLE_A
#define E(f) {VK_FORMAT_##f, 0, 1, 0, 0, {I, I, I, I}}            // exact
#define X(f) {VK_FORMAT_##f, 0, 1, 0, 0, {I, I, I, ONE}}          // exact, no alpha (a reads as 1)
#define S(f) {VK_FORMAT_##f, 0, 2, 0, 0, {I, I, I, I}}            // substitute
#define W(f, n) {VK_FORMAT_##f, 0, 2, n, 0, {I, I, I, ONE}}       // 3 components of n bytes each, widened to 4 on upload (a = 1)
#define BC(f, n, a) {VK_FORMAT_##f, 0, 1, 0, n, {I, I, I, a}}     // BCn (a: alpha, or ONE for the formats without)
#define N {0, 0, 0, 0, 0, {I, I, I, I}}

static const nvn_vkfmt T[0x9D] = {
    [0x01] = E(R8_UNORM), E(R8_SNORM), E(R8_UINT), E(R8_SINT),
    [0x05] = E(R16_SFLOAT), E(R16_UNORM), E(R16_SNORM), E(R16_UINT), E(R16_SINT),
    [0x0A] = E(R32_SFLOAT), E(R32_UINT), E(R32_SINT),
    [0x0D] = E(R8G8_UNORM), E(R8G8_SNORM), E(R8G8_UINT), E(R8G8_SINT),
    [0x11] = E(R16G16_SFLOAT), E(R16G16_UNORM), E(R16G16_SNORM), E(R16G16_UINT), E(R16G16_SINT),
    [0x16] = E(R32G32_SFLOAT), E(R32G32_UINT), E(R32G32_SINT),
    // RGB8 / RGB16 / RGB32: Vulkan names 3-component formats, but phones do not sample them; widened to the 4-component format on upload, as on Metal
    [0x19] = W(R8G8B8A8_UNORM, 1), W(R8G8B8A8_SNORM, 1), W(R8G8B8A8_UINT, 1), W(R8G8B8A8_SINT, 1),
    [0x1D] = W(R16G16B16A16_SFLOAT, 2), W(R16G16B16A16_UNORM, 2), W(R16G16B16A16_SNORM, 2), W(R16G16B16A16_UINT, 2), W(R16G16B16A16_SINT, 2),
    [0x22] = W(R32G32B32A32_SFLOAT, 4), W(R32G32B32A32_UINT, 4), W(R32G32B32A32_SINT, 4),
    [0x25] = E(R8G8B8A8_UNORM), E(R8G8B8A8_SNORM), E(R8G8B8A8_UINT), E(R8G8B8A8_SINT),
    [0x29] = E(R16G16B16A16_SFLOAT), E(R16G16B16A16_UNORM), E(R16G16B16A16_SNORM), E(R16G16B16A16_UINT), E(R16G16B16A16_SINT),
    [0x2E] = E(R32G32B32A32_SFLOAT), E(R32G32B32A32_UINT), E(R32G32B32A32_SINT),
    // STENCIL8: S8_UINT is optional; D24S8 / D32S8 are the fallbacks (the stencil aspect alone is used)
    [0x31] = {VK_FORMAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 0, 0, {I, I, I, I}}, E(D16_UNORM),
    // DEPTH24: X8_D24 where the device has it (Mali), else D32_SFLOAT as on Metal (Adreno); the same for DEPTH24_STENCIL8
    [0x33] = {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, 1, 0, 0, {I, I, I, I}}, E(D32_SFLOAT),
    [0x35] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 0, 0, {I, I, I, I}}, E(D32_SFLOAT_S8_UINT),
    [0x37] = X(R8G8B8A8_SRGB), E(R8G8B8A8_SRGB),
    // 16-bit packed formats, read with R in the low bits (Maxwell's A4B4G4R4 / B5G6R5 / A1B5G5R5 layouts); as on Metal the bit order is unverified on real data: substitutes.
    // Vulkan 1.1 has no R-low 4444 or 1555: R4G4B4A4 / R5G5B5A1 read with their components reversed
    [0x39] = {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 0, 2, 0, 0, {A_, B_, G_, R_}},
             {VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0, 2, 0, 0, {A_, B_, G_, ONE}},
             {VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0, 2, 0, 0, {A_, B_, G_, R_}},
             S(B5G6R5_UNORM_PACK16),
    [0x3D] = E(A2B10G10R10_UNORM_PACK32), E(A2B10G10R10_UINT_PACK32), E(B10G11R11_UFLOAT_PACK32), E(E5B9G9R9_UFLOAT_PACK32),
    [0x41] = BC(BC1_RGB_UNORM_BLOCK, 1, ONE), BC(BC1_RGBA_UNORM_BLOCK, 1, I), BC(BC2_UNORM_BLOCK, 2, I), BC(BC3_UNORM_BLOCK, 3, I),
             BC(BC1_RGB_SRGB_BLOCK, 1, ONE), BC(BC1_RGBA_SRGB_BLOCK, 1, I), BC(BC2_SRGB_BLOCK, 2, I), BC(BC3_SRGB_BLOCK, 3, I),
    [0x49] = BC(BC4_UNORM_BLOCK, 4, I), BC(BC4_SNORM_BLOCK, 4, I), BC(BC5_UNORM_BLOCK, 5, I), BC(BC5_SNORM_BLOCK, 5, I),
             BC(BC7_UNORM_BLOCK, 7, I), BC(BC7_SRGB_BLOCK, 7, I), BC(BC6H_SFLOAT_BLOCK, 6, I), BC(BC6H_UFLOAT_BLOCK, 6, I),
    // *_UI2F / *_I2F: integer storage read as float; as on Metal the integer format, converted by the shader
    [0x51] = S(R8_UINT), S(R8_SINT), S(R16_UINT), S(R16_SINT), S(R32_UINT), S(R32_SINT),
    [0x57] = S(R8G8_UINT), S(R8G8_SINT), S(R16G16_UINT), S(R16G16_SINT), S(R32G32_UINT), S(R32G32_SINT),
    [0x5D] = W(R8G8B8A8_UINT, 1), W(R8G8B8A8_SINT, 1), W(R16G16B16A16_UINT, 2), W(R16G16B16A16_SINT, 2), W(R32G32B32A32_UINT, 4), W(R32G32B32A32_SINT, 4),
    [0x63] = S(R8G8B8A8_UINT), S(R8G8B8A8_SINT), S(R16G16B16A16_UINT), S(R16G16B16A16_SINT), S(R32G32B32A32_UINT), S(R32G32B32A32_SINT),
    // RGB10A2 signed: Vulkan has them (optional; Metal has not)
    [0x69] = S(A2B10G10R10_SNORM_PACK32), S(A2B10G10R10_SINT_PACK32), S(A2B10G10R10_UINT_PACK32), S(A2B10G10R10_SINT_PACK32),
    [0x6D] = X(R8G8B8A8_UNORM), X(R8G8B8A8_SNORM), X(R8G8B8A8_UINT), X(R8G8B8A8_SINT),
    [0x71] = X(R16G16B16A16_SFLOAT), X(R16G16B16A16_UNORM), X(R16G16B16A16_SNORM), X(R16G16B16A16_UINT), X(R16G16B16A16_SINT),
    [0x76] = X(R32G32B32A32_SFLOAT), X(R32G32B32A32_UINT), X(R32G32B32A32_SINT),
    // 0x79–0x94 ASTC: nvn_fmt_to_vk
    [0x95] = {VK_FORMAT_R5G6B5_UNORM_PACK16, 0, 2, 0, 0, {I, I, I, I}},
             {VK_FORMAT_B5G5R5A1_UNORM_PACK16, 0, 2, 0, 0, {I, I, I, ONE}},
             S(B5G5R5A1_UNORM_PACK16), S(A1R5G5B5_UNORM_PACK16),
    [0x99] = X(B8G8R8A8_UNORM), E(B8G8R8A8_UNORM), X(B8G8R8A8_SRGB), E(B8G8R8A8_SRGB),
};

// NVN ASTC order: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12, the same as Vulkan's; each Vulkan block size has UNORM then SRGB
nvn_vkfmt nvn_fmt_to_vk(int f) {
    if (f >= 0x79 && f <= 0x94) {
        int k = (f - 0x79) % 14, srgb = f >= 0x87;
        return (nvn_vkfmt){(VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + srgb), 0, 1, 0, 0, {I, I, I, I}};
    }
    if (f > 0 && f < (int)(sizeof T / sizeof *T)) return T[f];
    return (nvn_vkfmt)N;
}
VkImageAspectFlags nvn_vk_aspect(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return 0;
    }
}
