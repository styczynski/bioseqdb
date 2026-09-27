/**
 * @file shader_layout.cppm
 * @brief Read SPIR-V buffer layouts so the runtime can check them against C++ types.
 */
module;

export module bioseq.gpu.vulkan.shader_layout;

import std;

export namespace bioseq::gpu::vulkan {

/// @brief Mark a missing member offset or descriptor-set number.
inline constexpr std::uint32_t k_spirv_layout_unknown = 0xffffffffu;

/// @brief Byte offsets of the members in one SPIR-V structure.
struct spirv_struct_layout {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> member_offsets;
};

/// @brief Array layout for a specific descriptor-set binding. Follow the
/// variable's type to find the element stride and structure-member offsets.
/// element_struct_id is zero for a scalar or vector element. Missing offset
/// decorations can also leave member_offsets empty.
struct spirv_binding_layout {
    std::uint32_t descriptor_set = 0;
    std::uint32_t binding = 0;
    std::uint32_t array_stride = 0;
    std::uint32_t element_struct_id = 0;
    std::vector<std::uint32_t> member_offsets;
};

/// @brief Buffer and push-parameter layouts read from SPIR-V. Callers can
/// check layouts across the module or check a particular descriptor binding
/// against the C++ structure used to upload its data.
struct spirv_layout_scan {
    std::vector<std::uint32_t> array_strides;
    std::vector<std::array<std::uint32_t, 4>> structs4;
    std::vector<std::uint32_t> bindings;
    std::vector<spirv_struct_layout> structs;
    std::vector<spirv_binding_layout> binding_layouts;
    std::vector<std::uint32_t> push_constant_offsets;

    /// @brief Check whether any array has this byte stride between elements.
    /// @param[in] stride Expected distance between array elements in bytes.
    /// @return true if a matching stride was found.
    [[nodiscard]] bool has_array_stride(std::uint32_t stride) const noexcept {
        for (const std::uint32_t candidate : array_strides) {
            if (candidate == stride)
                return true;
        }
        return false;
    }

    /// @brief Check whether any four-member structure has these byte offsets.
    /// @param[in] offsets Expected member offsets in declaration order.
    /// @return true if a matching structure was found.
    [[nodiscard]] bool
    has_struct_offsets(const std::array<std::uint32_t, 4>& offsets) const noexcept {
        for (const auto& candidate : structs4) {
            if (candidate == offsets)
                return true;
        }
        return false;
    }

    /// @brief Check whether a descriptor uses this binding number in any set.
    /// @param[in] binding Descriptor binding number.
    /// @return true if the binding was found.
    [[nodiscard]] bool has_binding(std::uint32_t binding) const noexcept {
        for (const std::uint32_t candidate : bindings) {
            if (candidate == binding)
                return true;
        }
        return false;
    }

    /// @brief Find the array layout for a descriptor-set binding.
    /// @param[in] descriptor_set Descriptor set number.
    /// @param[in] binding Binding number within that set.
    /// @return A pointer into this scan, or nullptr if no matching layout was read.
    [[nodiscard]] const spirv_binding_layout* binding_layout(std::uint32_t descriptor_set,
                                                             std::uint32_t binding) const noexcept {
        for (const auto& candidate : binding_layouts) {
            if (candidate.descriptor_set == descriptor_set && candidate.binding == binding)
                return &candidate;
        }
        return nullptr;
    }

    /// @brief Check the array element stride at a descriptor-set binding.
    /// @param[in] descriptor_set Descriptor set number.
    /// @param[in] binding Binding number within that set.
    /// @param[in] stride Expected distance between elements in bytes.
    /// @return true if the binding layout has this stride.
    [[nodiscard]] bool has_binding_array_stride(std::uint32_t descriptor_set,
                                                std::uint32_t binding,
                                                std::uint32_t stride) const noexcept {
        const auto* layout = binding_layout(descriptor_set, binding);
        return layout != nullptr && layout->array_stride == stride;
    }

    /// @brief Check every structure-member offset at a descriptor-set binding.
    /// @param[in] descriptor_set Descriptor set number.
    /// @param[in] binding Binding number within that set.
    /// @param[in] offsets Expected byte offsets in declaration order.
    /// @return true if the member count and all offsets match.
    [[nodiscard]] bool
    has_binding_struct_offsets(std::uint32_t descriptor_set,
                               std::uint32_t binding,
                               std::span<const std::uint32_t> offsets) const noexcept {
        const auto* layout = binding_layout(descriptor_set, binding);
        if (layout == nullptr || layout->member_offsets.size() != offsets.size()) {
            return false;
        }
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            if (layout->member_offsets[i] != offsets[i])
                return false;
        }
        return true;
    }

    /// @brief Check the layout of the shader's push parameters.
    /// @param[in] offsets Expected byte offsets in declaration order.
    /// @return true if the member count and all offsets match.
    [[nodiscard]] bool
    has_push_constant_offsets(std::span<const std::uint32_t> offsets) const noexcept {
        if (push_constant_offsets.size() != offsets.size())
            return false;
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            if (push_constant_offsets[i] != offsets[i])
                return false;
        }
        return true;
    }
};

/// @brief Read array strides, member offsets and descriptor bindings from SPIR-V.
/// Follow variable types to associate each array layout with its binding.
/// This needs no Vulkan device. Callers compare the result with expected layouts.
///
/// @param[in] spirv Compiled SPIR-V words, including the five-word header.
/// @return Layouts collected before a zero-length or truncated instruction.
/// A missing header or bad magic gives an empty result. This function does
/// not fully validate SPIR-V.
[[nodiscard]] inline spirv_layout_scan scan_spirv_layout(std::span<const std::uint32_t> spirv) {
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

    if (spirv.size() < 5 || spirv[0] != k_spirv_magic)
        return scan;

    // Descriptor numbers and array strides attached to a SPIR-V object.
    struct decoration_record {
        std::uint32_t id = 0;
        std::uint32_t binding = k_spirv_layout_unknown;
        std::uint32_t descriptor_set = 0;
        std::uint32_t array_stride = 0;
        bool has_binding = false;
        bool has_descriptor_set = false;
        bool has_array_stride = false;
    };
    // Member offsets attached to a structure type.
    struct member_decoration_record {
        std::uint32_t struct_id = 0;
        std::vector<std::uint32_t> offsets;
    };
    // Store the member type ids of one OpTypeStruct declaration.
    struct struct_type_record {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> member_types;
    };
    // Array type and the type of its elements.
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

    // Find or add the decorations for an object.
    const auto decoration_for = [&decorations](std::uint32_t id) -> decoration_record& {
        for (auto& candidate : decorations) {
            if (candidate.id == id)
                return candidate;
        }
        decorations.push_back(decoration_record{});
        decorations.back().id = id;
        return decorations.back();
    };
    // Find or add the member offsets for a structure.
    const auto member_decoration_for =
        [&member_decorations](std::uint32_t id) -> member_decoration_record& {
        for (auto& candidate : member_decorations) {
            if (candidate.struct_id == id)
                return candidate;
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
        if (word_count == 0 || word_index + word_count > spirv.size())
            break;

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
            } else if (decoration == k_decoration_descriptor_set && word_count >= 4) {
                record.descriptor_set = spirv[word_index + 3];
                record.has_descriptor_set = true;
            }
        } else if (op == k_op_member_decorate && word_count >= 5 &&
                   spirv[word_index + 3] == k_decoration_offset) {
            auto& record = member_decoration_for(spirv[word_index + 1]);
            const std::uint32_t member = spirv[word_index + 2];
            if (record.offsets.size() <= member) {
                record.offsets.resize(static_cast<std::size_t>(member) + 1u,
                                      k_spirv_layout_unknown);
            }
            record.offsets[member] = spirv[word_index + 4];
        } else if (op == k_op_type_array && word_count >= 4) {
            array_types.push_back(array_type_record{spirv[word_index + 1], spirv[word_index + 2]});
        } else if (op == k_op_type_runtime_array && word_count >= 3) {
            array_types.push_back(array_type_record{spirv[word_index + 1], spirv[word_index + 2]});
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
                spirv[word_index + 1], spirv[word_index + 2], spirv[word_index + 3]});
        } else if (op == k_op_variable && word_count >= 4) {
            variables.push_back(variable_record{
                spirv[word_index + 2], spirv[word_index + 1], spirv[word_index + 3]});
        }
        word_index += word_count;
    }

    // Find the decorations already read for an object.
    const auto find_decoration = [&decorations](std::uint32_t id) -> const decoration_record* {
        for (const auto& candidate : decorations) {
            if (candidate.id == id)
                return &candidate;
        }
        return nullptr;
    };
    // Find a structure's member offsets.
    const auto find_member_offsets =
        [&member_decorations](std::uint32_t id) -> const std::vector<std::uint32_t>* {
        for (const auto& candidate : member_decorations) {
            if (candidate.struct_id == id)
                return &candidate.offsets;
        }
        return nullptr;
    };
    // Find a structure type by its SPIR-V id.
    const auto find_struct = [&struct_types](std::uint32_t id) -> const struct_type_record* {
        for (const auto& candidate : struct_types) {
            if (candidate.id == id)
                return &candidate;
        }
        return nullptr;
    };
    // Find an array type by its SPIR-V id.
    const auto find_array = [&array_types](std::uint32_t id) -> const array_type_record* {
        for (const auto& candidate : array_types) {
            if (candidate.id == id)
                return &candidate;
        }
        return nullptr;
    };
    // Find a pointer type by its SPIR-V id.
    const auto find_pointer = [&pointer_types](std::uint32_t id) -> const pointer_type_record* {
        for (const auto& candidate : pointer_types) {
            if (candidate.id == id)
                return &candidate;
        }
        return nullptr;
    };

    for (const auto& type : struct_types) {
        spirv_struct_layout layout{};
        layout.id = type.id;
        layout.member_offsets.assign(type.member_types.size(), k_spirv_layout_unknown);
        if (const auto* offsets = find_member_offsets(type.id)) {
            const std::size_t count = offsets->size() < layout.member_offsets.size()
                                          ? offsets->size()
                                          : layout.member_offsets.size();
            for (std::size_t i = 0; i < count; ++i) {
                layout.member_offsets[i] = (*offsets)[i];
            }
        }
        scan.structs.push_back(layout);
        if (layout.member_offsets.size() == 4u &&
            layout.member_offsets[0] != k_spirv_layout_unknown &&
            layout.member_offsets[1] != k_spirv_layout_unknown &&
            layout.member_offsets[2] != k_spirv_layout_unknown &&
            layout.member_offsets[3] != k_spirv_layout_unknown) {
            scan.structs4.push_back(std::array<std::uint32_t, 4>{layout.member_offsets[0],
                                                                 layout.member_offsets[1],
                                                                 layout.member_offsets[2],
                                                                 layout.member_offsets[3]});
        }
    }

    for (const auto& variable : variables) {
        const auto* pointer = find_pointer(variable.result_type);
        if (pointer == nullptr)
            continue;
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
        if (array == nullptr)
            continue;
        const auto* array_decoration = find_decoration(array_type_id);

        spirv_binding_layout layout{};
        layout.descriptor_set = variable_decoration->has_descriptor_set
                                    ? variable_decoration->descriptor_set
                                    : k_spirv_layout_unknown;
        layout.binding = variable_decoration->binding;
        layout.array_stride = array_decoration != nullptr && array_decoration->has_array_stride
                                  ? array_decoration->array_stride
                                  : 0u;
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
