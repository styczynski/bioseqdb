/**
 * @file shader_layout.cppm
 * @brief Device-free SPIR-V decoration scanner for Vulkan shader ABI checks.
 */
module;
#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

export module bioseq.gpu.vulkan.shader_layout;

export namespace bioseq::gpu::vulkan {

inline constexpr std::uint32_t k_spirv_layout_unknown = 0xffffffffu;

struct SpirvStructLayout {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> member_offsets;
};

/// Layout resolved through one descriptor variable, its pointer/block type,
/// and the binding's array element type. `member_offsets` are therefore
/// binding-specific instead of an unsafe global "some struct looked right"
/// match. Empty offsets mean a scalar/vector array element.
struct SpirvBindingLayout {
    std::uint32_t descriptor_set = 0;
    std::uint32_t binding = 0;
    std::uint32_t array_stride = 0;
    std::uint32_t element_struct_id = 0;
    std::vector<std::uint32_t> member_offsets;
};

/// Decoded SPIR-V layout decorations used to validate storage-buffer and push
/// constant ABIs. Legacy global views remain for established callers, while
/// new ABI checks use binding-specific resolution and arbitrary member counts.
struct SpirvLayoutScan {
    std::vector<std::uint32_t> array_strides;
    std::vector<std::array<std::uint32_t, 4>> structs4;
    std::vector<std::uint32_t> bindings;
    std::vector<SpirvStructLayout> structs;
    std::vector<SpirvBindingLayout> binding_layouts;
    std::vector<std::uint32_t> push_constant_offsets;

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

    [[nodiscard]] const SpirvBindingLayout* binding_layout(
        std::uint32_t descriptor_set,
        std::uint32_t binding) const noexcept
    {
        for (const auto& candidate : binding_layouts) {
            if (candidate.descriptor_set == descriptor_set
                && candidate.binding == binding) return &candidate;
        }
        return nullptr;
    }

    [[nodiscard]] bool has_binding_array_stride(
        std::uint32_t descriptor_set,
        std::uint32_t binding,
        std::uint32_t stride) const noexcept
    {
        const auto* layout = binding_layout(descriptor_set, binding);
        return layout != nullptr && layout->array_stride == stride;
    }

    [[nodiscard]] bool has_binding_struct_offsets(
        std::uint32_t descriptor_set,
        std::uint32_t binding,
        std::span<const std::uint32_t> offsets) const noexcept
    {
        const auto* layout = binding_layout(descriptor_set, binding);
        if (layout == nullptr || layout->member_offsets.size() != offsets.size()) {
            return false;
        }
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            if (layout->member_offsets[i] != offsets[i]) return false;
        }
        return true;
    }

    [[nodiscard]] bool has_push_constant_offsets(
        std::span<const std::uint32_t> offsets) const noexcept
    {
        if (push_constant_offsets.size() != offsets.size()) return false;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            if (push_constant_offsets[i] != offsets[i]) return false;
        }
        return true;
    }
};

/// Scan a compiled SPIR-V module and resolve descriptor variables through
/// OpVariable -> OpTypePointer -> block OpTypeStruct -> array element. This is
/// intentionally device-free so archive validation can fail before Vulkan or
/// spirv-cross sees a malformed ABI.
[[nodiscard]] inline SpirvLayoutScan scan_spirv_layout(
    std::span<const std::uint32_t> spirv)
{
    SpirvLayoutScan scan;
    constexpr std::uint32_t kMagic = 0x07230203u;
    constexpr std::uint16_t kOpTypeArray = 28;
    constexpr std::uint16_t kOpTypeRuntimeArray = 29;
    constexpr std::uint16_t kOpTypeStruct = 30;
    constexpr std::uint16_t kOpTypePointer = 32;
    constexpr std::uint16_t kOpVariable = 59;
    constexpr std::uint16_t kOpDecorate = 71;
    constexpr std::uint16_t kOpMemberDecorate = 72;
    constexpr std::uint32_t kDecorationArrayStride = 6;
    constexpr std::uint32_t kDecorationBinding = 33;
    constexpr std::uint32_t kDecorationDescriptorSet = 34;
    constexpr std::uint32_t kDecorationOffset = 35;
    constexpr std::uint32_t kStorageClassPushConstant = 9;

    if (spirv.size() < 5 || spirv[0] != kMagic) return scan;

    struct DecorationRecord {
        std::uint32_t id = 0;
        std::uint32_t binding = k_spirv_layout_unknown;
        std::uint32_t descriptor_set = 0;
        std::uint32_t array_stride = 0;
        bool has_binding = false;
        bool has_descriptor_set = false;
        bool has_array_stride = false;
    };
    struct MemberDecorationRecord {
        std::uint32_t struct_id = 0;
        std::vector<std::uint32_t> offsets;
    };
    struct StructTypeRecord {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> member_types;
    };
    struct ArrayTypeRecord {
        std::uint32_t id = 0;
        std::uint32_t element_type = 0;
    };
    struct PointerTypeRecord {
        std::uint32_t id = 0;
        std::uint32_t storage_class = 0;
        std::uint32_t pointee_type = 0;
    };
    struct VariableRecord {
        std::uint32_t id = 0;
        std::uint32_t result_type = 0;
        std::uint32_t storage_class = 0;
    };

    std::vector<DecorationRecord> decorations;
    std::vector<MemberDecorationRecord> member_decorations;
    std::vector<StructTypeRecord> struct_types;
    std::vector<ArrayTypeRecord> array_types;
    std::vector<PointerTypeRecord> pointer_types;
    std::vector<VariableRecord> variables;

    const auto decoration_for = [&decorations](std::uint32_t id)
        -> DecorationRecord& {
        for (auto& candidate : decorations) {
            if (candidate.id == id) return candidate;
        }
        decorations.push_back(DecorationRecord{});
        decorations.back().id = id;
        return decorations.back();
    };
    const auto member_decoration_for = [&member_decorations](std::uint32_t id)
        -> MemberDecorationRecord& {
        for (auto& candidate : member_decorations) {
            if (candidate.struct_id == id) return candidate;
        }
        member_decorations.push_back(MemberDecorationRecord{});
        member_decorations.back().struct_id = id;
        return member_decorations.back();
    };

    std::size_t word_index = 5;
    while (word_index < spirv.size()) {
        const std::uint32_t word = spirv[word_index];
        const auto op = static_cast<std::uint16_t>(word & 0xffffu);
        const auto word_count = static_cast<std::uint16_t>(word >> 16);
        if (word_count == 0 || word_index + word_count > spirv.size()) break;

        if (op == kOpDecorate && word_count >= 3) {
            const std::uint32_t target = spirv[word_index + 1];
            const std::uint32_t decoration = spirv[word_index + 2];
            auto& record = decoration_for(target);
            if (decoration == kDecorationArrayStride && word_count >= 4) {
                record.array_stride = spirv[word_index + 3];
                record.has_array_stride = true;
                scan.array_strides.push_back(record.array_stride);
            } else if (decoration == kDecorationBinding && word_count >= 4) {
                record.binding = spirv[word_index + 3];
                record.has_binding = true;
                scan.bindings.push_back(record.binding);
            } else if (decoration == kDecorationDescriptorSet
                       && word_count >= 4) {
                record.descriptor_set = spirv[word_index + 3];
                record.has_descriptor_set = true;
            }
        } else if (op == kOpMemberDecorate && word_count >= 5
                   && spirv[word_index + 3] == kDecorationOffset) {
            auto& record = member_decoration_for(spirv[word_index + 1]);
            const std::uint32_t member = spirv[word_index + 2];
            if (record.offsets.size() <= member) {
                record.offsets.resize(
                    static_cast<std::size_t>(member) + 1u,
                    k_spirv_layout_unknown);
            }
            record.offsets[member] = spirv[word_index + 4];
        } else if (op == kOpTypeArray && word_count >= 4) {
            array_types.push_back(ArrayTypeRecord{
                spirv[word_index + 1], spirv[word_index + 2]});
        } else if (op == kOpTypeRuntimeArray && word_count >= 3) {
            array_types.push_back(ArrayTypeRecord{
                spirv[word_index + 1], spirv[word_index + 2]});
        } else if (op == kOpTypeStruct && word_count >= 2) {
            StructTypeRecord record{};
            record.id = spirv[word_index + 1];
            record.member_types.reserve(word_count - 2u);
            for (std::uint16_t i = 2; i < word_count; ++i) {
                record.member_types.push_back(spirv[word_index + i]);
            }
            struct_types.push_back(std::move(record));
        } else if (op == kOpTypePointer && word_count >= 4) {
            pointer_types.push_back(PointerTypeRecord{
                spirv[word_index + 1], spirv[word_index + 2],
                spirv[word_index + 3]});
        } else if (op == kOpVariable && word_count >= 4) {
            variables.push_back(VariableRecord{
                spirv[word_index + 2], spirv[word_index + 1],
                spirv[word_index + 3]});
        }
        word_index += word_count;
    }

    const auto find_decoration = [&decorations](std::uint32_t id)
        -> const DecorationRecord* {
        for (const auto& candidate : decorations) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };
    const auto find_member_offsets = [&member_decorations](std::uint32_t id)
        -> const std::vector<std::uint32_t>* {
        for (const auto& candidate : member_decorations) {
            if (candidate.struct_id == id) return &candidate.offsets;
        }
        return nullptr;
    };
    const auto find_struct = [&struct_types](std::uint32_t id)
        -> const StructTypeRecord* {
        for (const auto& candidate : struct_types) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };
    const auto find_array = [&array_types](std::uint32_t id)
        -> const ArrayTypeRecord* {
        for (const auto& candidate : array_types) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };
    const auto find_pointer = [&pointer_types](std::uint32_t id)
        -> const PointerTypeRecord* {
        for (const auto& candidate : pointer_types) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };

    for (const auto& type : struct_types) {
        SpirvStructLayout layout{};
        layout.id = type.id;
        layout.member_offsets.assign(
            type.member_types.size(), k_spirv_layout_unknown);
        if (const auto* offsets = find_member_offsets(type.id)) {
            const std::size_t count = offsets->size() < layout.member_offsets.size()
                ? offsets->size() : layout.member_offsets.size();
            for (std::size_t i = 0; i < count; ++i) {
                layout.member_offsets[i] = (*offsets)[i];
            }
        }
        scan.structs.push_back(layout);
        if (layout.member_offsets.size() == 4u
            && layout.member_offsets[0] != k_spirv_layout_unknown
            && layout.member_offsets[1] != k_spirv_layout_unknown
            && layout.member_offsets[2] != k_spirv_layout_unknown
            && layout.member_offsets[3] != k_spirv_layout_unknown) {
            scan.structs4.push_back(std::array<std::uint32_t, 4>{
                layout.member_offsets[0], layout.member_offsets[1],
                layout.member_offsets[2], layout.member_offsets[3]});
        }
    }

    for (const auto& variable : variables) {
        const auto* pointer = find_pointer(variable.result_type);
        if (pointer == nullptr) continue;
        const auto* variable_decoration = find_decoration(variable.id);
        if (variable.storage_class == kStorageClassPushConstant) {
            if (const auto* offsets = find_member_offsets(pointer->pointee_type)) {
                scan.push_constant_offsets = *offsets;
            }
            continue;
        }
        if (variable_decoration == nullptr || !variable_decoration->has_binding) {
            continue;
        }

        std::uint32_t array_type_id = 0;
        const ArrayTypeRecord* array = find_array(pointer->pointee_type);
        if (array != nullptr) {
            array_type_id = array->id;
        } else if (const auto* block = find_struct(pointer->pointee_type)) {
            for (const std::uint32_t member_type : block->member_types) {
                if (const auto* candidate = find_array(member_type)) {
                    array = candidate;
                    array_type_id = candidate->id;
                    break;
                }
            }
        }
        if (array == nullptr) continue;
        const auto* array_decoration = find_decoration(array_type_id);

        SpirvBindingLayout layout{};
        layout.descriptor_set = variable_decoration->has_descriptor_set
            ? variable_decoration->descriptor_set : k_spirv_layout_unknown;
        layout.binding = variable_decoration->binding;
        layout.array_stride = array_decoration != nullptr
                && array_decoration->has_array_stride
            ? array_decoration->array_stride : 0u;
        if (find_struct(array->element_type) != nullptr) {
            layout.element_struct_id = array->element_type;
            if (const auto* offsets = find_member_offsets(array->element_type)) {
                layout.member_offsets = *offsets;
            }
        }
        scan.binding_layouts.push_back(std::move(layout));
    }
    return scan;
}

} // namespace bioseq::gpu::vulkan
