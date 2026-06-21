/**
 * @file shader_layout.cppm
 * @brief Device-free SPIR-V decoration scanner for Vulkan shader ABI checks.
 */
module;
#include "common.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

export module bioseq.gpu.vulkan.shader_layout;

export namespace bioseq::gpu::vulkan {

/// Decoded SPIR-V layout decorations used to validate storage-buffer ABIs.
struct SpirvLayoutScan {
    std::vector<std::uint32_t> array_strides;
    std::vector<std::array<std::uint32_t, 4>> structs4;
    std::vector<std::uint32_t> bindings;

    [[nodiscard]] bool has_array_stride(std::uint32_t stride) const noexcept {
        for (const std::uint32_t candidate : array_strides) {
            if (candidate == stride) return true;
        }
        return false;
    }

    [[nodiscard]] bool has_struct_offsets(
        const std::array<std::uint32_t, 4>& offsets) const noexcept {
        for (const auto& candidate : structs4) {
            if (candidate == offsets) return true;
        }
        return false;
    }

    [[nodiscard]] bool has_binding(std::uint32_t binding) const noexcept {
        for (const std::uint32_t candidate : bindings) {
            if (candidate == binding) return true;
        }
        return false;
    }
};

/// Scan a compiled SPIR-V module for ArrayStride, Binding, and 4-member struct offsets.
[[nodiscard]] inline SpirvLayoutScan scan_spirv_layout(
    std::span<const std::uint32_t> spirv)
{
    SpirvLayoutScan scan;
    constexpr std::uint32_t kMagic = 0x07230203u;
    constexpr std::uint16_t kOpDecorate = 71;
    constexpr std::uint16_t kOpMemberDecorate = 72;
    constexpr std::uint32_t kDecorationArrayStride = 6;
    constexpr std::uint32_t kDecorationOffset = 35;
    constexpr std::uint32_t kDecorationBinding = 33;

    if (spirv.size() < 5 || spirv[0] != kMagic) {
        return scan;
    }

    struct StructOffsets {
        std::uint32_t struct_id = 0;
        std::array<std::uint32_t, 4> offsets{};
        std::array<bool, 4> seen{};
        bool overflow = false;
    };
    std::vector<StructOffsets> struct_offsets;

    std::size_t word_index = 5;
    while (word_index < spirv.size()) {
        const std::uint32_t word = spirv[word_index];
        const auto op = static_cast<std::uint16_t>(word & 0xffffu);
        const auto word_count = static_cast<std::uint16_t>(word >> 16);
        if (word_count == 0 || word_index + word_count > spirv.size()) {
            break;
        }

        if (op == kOpDecorate && word_count >= 3) {
            const std::uint32_t decoration = spirv[word_index + 2];
            if (decoration == kDecorationArrayStride && word_count >= 4) {
                scan.array_strides.push_back(spirv[word_index + 3]);
            } else if (decoration == kDecorationBinding && word_count >= 4) {
                scan.bindings.push_back(spirv[word_index + 3]);
            }
        } else if (op == kOpMemberDecorate && word_count >= 4) {
            const std::uint32_t struct_id = spirv[word_index + 1];
            const std::uint32_t member = spirv[word_index + 2];
            const std::uint32_t decoration = spirv[word_index + 3];
            if (decoration == kDecorationOffset && word_count >= 5) {
                StructOffsets* entry = nullptr;
                for (auto& candidate : struct_offsets) {
                    if (candidate.struct_id == struct_id) {
                        entry = &candidate;
                        break;
                    }
                }
                if (entry == nullptr) {
                    struct_offsets.push_back(StructOffsets{struct_id, {}, {}, false});
                    entry = &struct_offsets.back();
                }
                if (member < 4) {
                    entry->offsets[member] = spirv[word_index + 4];
                    entry->seen[member] = true;
                } else {
                    entry->overflow = true;
                }
            }
        }
        word_index += word_count;
    }

    for (const auto& entry : struct_offsets) {
        if (entry.overflow) continue;
        if (entry.seen[0] && entry.seen[1] && entry.seen[2] && entry.seen[3]) {
            scan.structs4.push_back(entry.offsets);
        }
    }
    return scan;
}

} // namespace bioseq::gpu::vulkan
