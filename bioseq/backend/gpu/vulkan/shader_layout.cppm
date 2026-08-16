/**
 * @file shader_layout.cppm
 * @brief Device-free SPIR-V decoration scanner for Vulkan shader ABI checks.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

export module bioseq.gpu.vulkan.shader_layout;

export namespace bioseq::gpu::vulkan {

/// @brief Sentinel used when a SPIR-V decoration has no resolved unsigned value.
inline constexpr std::uint32_t k_spirv_layout_unknown = 0xffffffffu;

/// @brief Member-offset decorations resolved for one SPIR-V structure type.
struct spirv_struct_layout {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> member_offsets;
};

/// @brief Layout resolved through one descriptor variable, its pointer/block type,
/// and the binding's array element type. `member_offsets` are therefore
/// binding-specific instead of an unsafe global "some struct looked right"
/// match. Empty offsets mean a scalar/vector array element.
struct spirv_binding_layout {
    std::uint32_t descriptor_set = 0;
    std::uint32_t binding = 0;
    std::uint32_t array_stride = 0;
    std::uint32_t element_struct_id = 0;
    std::vector<std::uint32_t> member_offsets;
};

/// @brief Decoded SPIR-V decorations used to validate storage-buffer and push-constant
/// ABIs. Aggregate views support broad checks. Binding-specific records support
/// exact descriptor validation with arbitrary member counts.
struct spirv_layout_scan {
    std::vector<std::uint32_t> array_strides;
    std::vector<std::array<std::uint32_t, 4>> structs4;
    std::vector<std::uint32_t> bindings;
    std::vector<spirv_struct_layout> structs;
    std::vector<spirv_binding_layout> binding_layouts;
    std::vector<std::uint32_t> push_constant_offsets;

    /// @brief Reports whether any reflected array uses the requested element stride.
    /// @param[in] stride Element stride in bytes.
    /// @return `true` when a reflected array has the requested stride.
    [[nodiscard]] bool has_array_stride(std::uint32_t stride) const noexcept {
        for (const std::uint32_t candidate : array_strides) {
            if (candidate == stride) return true;
        }
        return false;
    }

    /// @brief Reports whether any four-member structure has the requested offsets.
    /// @param[in] offsets Ordered member offsets in bytes.
    /// @return `true` when a reflected structure has the requested layout.
    [[nodiscard]] bool has_struct_offsets(
        const std::array<std::uint32_t, 4>& offsets) const noexcept {
        for (const auto& candidate : structs4) {
            if (candidate == offsets) return true;
        }
        return false;
    }

    /// @brief Reports whether any descriptor variable uses the requested binding.
    /// @param[in] binding Descriptor binding number.
    /// @return `true` when the binding is present.
    [[nodiscard]] bool has_binding(std::uint32_t binding) const noexcept {
        for (const std::uint32_t candidate : bindings) {
            if (candidate == binding) return true;
        }
        return false;
    }

    /// @brief Resolves the layout for one descriptor-set binding.
    /// @param[in] descriptor_set Descriptor set number.
    /// @param[in] binding Descriptor binding number.
    /// @return Borrowed layout, or `nullptr` when the binding is absent.
    [[nodiscard]] const spirv_binding_layout* binding_layout(
        std::uint32_t descriptor_set,
        std::uint32_t binding) const noexcept
    {
        for (const auto& candidate : binding_layouts) {
            if (candidate.descriptor_set == descriptor_set
                && candidate.binding == binding) return &candidate;
        }
        return nullptr;
    }

    /// @brief Reports whether one descriptor binding has the requested array stride.
    /// @param[in] descriptor_set Descriptor set number.
    /// @param[in] binding Descriptor binding number.
    /// @param[in] stride Array element stride in bytes.
    /// @return `true` when the binding exists with the requested stride.
    [[nodiscard]] bool has_binding_array_stride(
        std::uint32_t descriptor_set,
        std::uint32_t binding,
        std::uint32_t stride) const noexcept
    {
        const auto* layout = binding_layout(descriptor_set, binding);
        return layout != nullptr && layout->array_stride == stride;
    }

    /// @brief Reports whether one descriptor binding exposes the requested member offsets.
    /// @param[in] descriptor_set Descriptor set number.
    /// @param[in] binding Descriptor binding number.
    /// @param[in] offsets Ordered structure-member offsets in bytes.
    /// @return `true` when the binding layout matches every offset.
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

    /// @brief Reports whether the push-constant block exposes the requested offsets.
    /// @param[in] offsets Ordered push-constant member offsets in bytes.
    /// @return `true` when the push-constant layout matches every offset.
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

/// @brief Scan a compiled SPIR-V module and resolve descriptor variables through
/// OpVariable -> OpTypePointer -> block OpTypeStruct -> array element.
///
/// @param[in] spirv Compiled SPIR-V words, including the five-word module header.
/// @return Reflected descriptor, structure, and push-constant decorations.
///
/// The scan is device-free so malformed layouts are rejected before Vulkan or
/// SPIRV-Cross consumes the module.
[[nodiscard]] inline spirv_layout_scan scan_spirv_layout(
    std::span<const std::uint32_t> spirv)
{
    spirv_layout_scan scan;
    constexpr std::uint32_t k_spirv_magic = 0x07230203u;
    constexpr std::uint16_t k_op_type_array = 28;
    constexpr std::uint16_t k_op_type_runtime_array = 29;
    constexpr std::uint16_t k_op_type_struct = 30;
    constexpr std::uint16_t k_op_type_pointer = 32;
    constexpr std::uint16_t k_op_variable = 59;
    constexpr std::uint16_t k_op_decorate = 71;
    constexpr std::uint16_t k_op_member_decorate = 72;
    constexpr std::uint32_t k_decoration_array_stride = 6;
    constexpr std::uint32_t k_decoration_binding = 33;
    constexpr std::uint32_t k_decoration_descriptor_set = 34;
    constexpr std::uint32_t k_decoration_offset = 35;
    constexpr std::uint32_t k_storage_class_push_constant = 9;

    if (spirv.size() < 5 || spirv[0] != k_spirv_magic) return scan;

    // Accumulates object-level descriptor and array-stride decorations by SPIR-V id.
    struct decoration_record {
        std::uint32_t id = 0;
        std::uint32_t binding = k_spirv_layout_unknown;
        std::uint32_t descriptor_set = 0;
        std::uint32_t array_stride = 0;
        bool has_binding = false;
        bool has_descriptor_set = false;
        bool has_array_stride = false;
    };
    // Accumulates member offsets for one reflected structure type.
    struct member_decoration_record {
        std::uint32_t struct_id = 0;
        std::vector<std::uint32_t> offsets;
    };
    // Retains the member type ids of one OpTypeStruct declaration.
    struct struct_type_record {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> member_types;
    };
    // Binds an array type id to its element type for descriptor traversal.
    struct array_type_record {
        std::uint32_t id = 0;
        std::uint32_t element_type = 0;
    };
    // Records the pointee and storage class of one pointer type.
    struct pointer_type_record {
        std::uint32_t id = 0;
        std::uint32_t storage_class = 0;
        std::uint32_t pointee_type = 0;
    };
    // Connects one variable id to its pointer type and storage class.
    struct variable_record {
        std::uint32_t id = 0;
        std::uint32_t result_type = 0;
        std::uint32_t storage_class = 0;
    };

    std::vector<decoration_record> decorations;
    std::vector<member_decoration_record> member_decorations;
    std::vector<struct_type_record> struct_types;
    std::vector<array_type_record> array_types;
    std::vector<pointer_type_record> pointer_types;
    std::vector<variable_record> variables;

    // Finds or creates the mutable decoration aggregate for an object id.
    const auto decoration_for = [&decorations](std::uint32_t id)
        -> decoration_record& {
        for (auto& candidate : decorations) {
            if (candidate.id == id) return candidate;
        }
        decorations.push_back(decoration_record{});
        decorations.back().id = id;
        return decorations.back();
    };
    // Finds or creates the mutable member-offset aggregate for a structure id.
    const auto member_decoration_for = [&member_decorations](std::uint32_t id)
        -> member_decoration_record& {
        for (auto& candidate : member_decorations) {
            if (candidate.struct_id == id) return candidate;
        }
        member_decorations.push_back(member_decoration_record{});
        member_decorations.back().struct_id = id;
        return member_decorations.back();
    };

    std::size_t word_index = 5;
    while (word_index < spirv.size()) {
        const std::uint32_t word = spirv[word_index];
        const auto op = static_cast<std::uint16_t>(word & 0xffffu);
        const auto word_count = static_cast<std::uint16_t>(word >> 16);
        if (word_count == 0 || word_index + word_count > spirv.size()) break;

        if (op == k_op_decorate && word_count >= 3) {
            const std::uint32_t target = spirv[word_index + 1];
            const std::uint32_t decoration = spirv[word_index + 2];
            auto& record = decoration_for(target);
            if (decoration == k_decoration_array_stride && word_count >= 4) {
                record.array_stride = spirv[word_index + 3];
                record.has_array_stride = true;
                scan.array_strides.push_back(record.array_stride);
            } else if (decoration == k_decoration_binding && word_count >= 4) {
                record.binding = spirv[word_index + 3];
                record.has_binding = true;
                scan.bindings.push_back(record.binding);
            } else if (decoration == k_decoration_descriptor_set
                       && word_count >= 4) {
                record.descriptor_set = spirv[word_index + 3];
                record.has_descriptor_set = true;
            }
        } else if (op == k_op_member_decorate && word_count >= 5
                   && spirv[word_index + 3] == k_decoration_offset) {
            auto& record = member_decoration_for(spirv[word_index + 1]);
            const std::uint32_t member = spirv[word_index + 2];
            if (record.offsets.size() <= member) {
                record.offsets.resize(
                    static_cast<std::size_t>(member) + 1u,
                    k_spirv_layout_unknown);
            }
            record.offsets[member] = spirv[word_index + 4];
        } else if (op == k_op_type_array && word_count >= 4) {
            array_types.push_back(array_type_record{
                spirv[word_index + 1], spirv[word_index + 2]});
        } else if (op == k_op_type_runtime_array && word_count >= 3) {
            array_types.push_back(array_type_record{
                spirv[word_index + 1], spirv[word_index + 2]});
        } else if (op == k_op_type_struct && word_count >= 2) {
            struct_type_record record{};
            record.id = spirv[word_index + 1];
            record.member_types.reserve(word_count - 2u);
            for (std::uint16_t i = 2; i < word_count; ++i) {
                record.member_types.push_back(spirv[word_index + i]);
            }
            struct_types.push_back(std::move(record));
        } else if (op == k_op_type_pointer && word_count >= 4) {
            pointer_types.push_back(pointer_type_record{
                spirv[word_index + 1], spirv[word_index + 2],
                spirv[word_index + 3]});
        } else if (op == k_op_variable && word_count >= 4) {
            variables.push_back(variable_record{
                spirv[word_index + 2], spirv[word_index + 1],
                spirv[word_index + 3]});
        }
        word_index += word_count;
    }

    // Resolves object-level decorations without extending the parsed inventory.
    const auto find_decoration = [&decorations](std::uint32_t id)
        -> const decoration_record* {
        for (const auto& candidate : decorations) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };
    // Resolves the ordered member offsets recorded for a structure id.
    const auto find_member_offsets = [&member_decorations](std::uint32_t id)
        -> const std::vector<std::uint32_t>* {
        for (const auto& candidate : member_decorations) {
            if (candidate.struct_id == id) return &candidate.offsets;
        }
        return nullptr;
    };
    // Resolves a parsed structure declaration by id.
    const auto find_struct = [&struct_types](std::uint32_t id)
        -> const struct_type_record* {
        for (const auto& candidate : struct_types) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };
    // Resolves a parsed array declaration by id.
    const auto find_array = [&array_types](std::uint32_t id)
        -> const array_type_record* {
        for (const auto& candidate : array_types) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };
    // Resolves a parsed pointer declaration by id.
    const auto find_pointer = [&pointer_types](std::uint32_t id)
        -> const pointer_type_record* {
        for (const auto& candidate : pointer_types) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    };

    for (const auto& type : struct_types) {
        spirv_struct_layout layout{};
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
        if (variable.storage_class == k_storage_class_push_constant) {
            if (const auto* offsets = find_member_offsets(pointer->pointee_type)) {
                scan.push_constant_offsets = *offsets;
            }
            continue;
        }
        if (variable_decoration == nullptr || !variable_decoration->has_binding) {
            continue;
        }

        std::uint32_t array_type_id = 0;
        const array_type_record* array = find_array(pointer->pointee_type);
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

        spirv_binding_layout layout{};
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
