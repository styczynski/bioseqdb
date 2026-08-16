# The generation contract validates the complete annotation -> prepared Clang
# closure -> fixed-target module -> generated binder/registry path, including
# SneakySnake. Families extend explicit registries while target discovery
# remains Highway-owned through HWY_VISIT_TARGETS.
set(BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/highway")

# Probe the unattached scheduler source as raw tokens before constructing any
# typed Highway alignment or minimizer slots. The topology manifest is the
# single alphabet/width/shard authority shared by both generators; neither
# frontend may rediscover or multiply the other's domain.
set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE
  "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/scheduler/dispatch_processor/typed_template.cppm")
set(BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/template_flatten/dispatch_processor_typed")
cmake_path(IS_PREFIX BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}" NORMALIZE
  _bioseq_template_flatten_root_inside_highway_root)
cmake_path(IS_PREFIX BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${BIOSEQ_HIGHWAY_GENERATED_ROOT}" NORMALIZE
  _bioseq_highway_root_inside_template_flatten_root)
cmake_path(IS_PREFIX BIOSEQ_HIGHWAY_GENERATED_ROOT
  "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}" NORMALIZE
  _bioseq_pipeline_owner_root_inside_highway_root)
cmake_path(IS_PREFIX BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT
  "${BIOSEQ_HIGHWAY_GENERATED_ROOT}" NORMALIZE
  _bioseq_highway_root_inside_pipeline_owner_root)
cmake_path(IS_PREFIX BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT
  "${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}" NORMALIZE
  _bioseq_pipeline_owner_root_inside_scheduler_root)
cmake_path(IS_PREFIX BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT
  "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}" NORMALIZE
  _bioseq_scheduler_root_inside_pipeline_owner_root)
if(_bioseq_template_flatten_root_inside_highway_root
    OR _bioseq_highway_root_inside_template_flatten_root
    OR _bioseq_pipeline_owner_root_inside_highway_root
    OR _bioseq_highway_root_inside_pipeline_owner_root
    OR _bioseq_pipeline_owner_root_inside_scheduler_root
    OR _bioseq_scheduler_root_inside_pipeline_owner_root)
  message(FATAL_ERROR
    "Highway, scheduler, and pipeline-owner generated roots must be pairwise "
    "disjoint; "
    "highway='${BIOSEQ_HIGHWAY_GENERATED_ROOT}', "
    "scheduler='${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}', "
    "pipeline_owner='${BIOSEQ_PIPELINE_DISPATCH_OWNER_GENERATED_ROOT}'")
endif()
unset(_bioseq_scheduler_root_inside_pipeline_owner_root)
unset(_bioseq_pipeline_owner_root_inside_scheduler_root)
unset(_bioseq_highway_root_inside_pipeline_owner_root)
unset(_bioseq_pipeline_owner_root_inside_highway_root)
unset(_bioseq_highway_root_inside_template_flatten_root)
unset(_bioseq_template_flatten_root_inside_highway_root)
bioseq_template_flatten_probe(
  NAME dispatch_processor_typed
  SOURCE "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_TEMPLATE}"
  OUTPUT_ROOT "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  OUTPUT_MODULE bioseq.postgres.scheduler.dispatch_processor
  PROFILE "${BIOSEQ_TEMPLATE_FLATTEN_PROFILE}"
  OUT_CONFIGURE_TOPOLOGY_MANIFEST BIOSEQ_TYPED_CONFIGURE_TOPOLOGY
  OUT_TOPOLOGY_DIGEST BIOSEQ_TYPED_TOPOLOGY_DIGEST
  OUT_ENABLED_ORDINALS BIOSEQ_TYPED_ENABLED_ORDINALS
  OUT_ENABLED_SOURCES BIOSEQ_TYPED_PROBED_ENABLED_SOURCES)
bioseq_template_flatten_read_group(
  TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  ROLE identity
  GROUP alphabet
  OUT_LABELS BIOSEQ_TYPED_ALPHABET_LABELS
  OUT_REPLACEMENTS BIOSEQ_TYPED_ALPHABET_REPLACEMENTS
  OUT_ORDINALS BIOSEQ_TYPED_ALPHABET_ORDINALS)
bioseq_template_flatten_read_group(
  TOPOLOGY_MANIFEST "${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  ROLE identity
  GROUP width
  OUT_LABELS BIOSEQ_TYPED_WIDTH_LABELS
  OUT_REPLACEMENTS BIOSEQ_TYPED_WIDTH_REPLACEMENTS
  OUT_ORDINALS BIOSEQ_TYPED_WIDTH_ORDINALS)

set(BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS)
set(BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS)
foreach(_bioseq_highway_alignment_alphabet_label
    _bioseq_highway_alignment_alphabet_replacement
    _bioseq_highway_alignment_alphabet_ordinal
    IN ZIP_LISTS
      BIOSEQ_TYPED_ALPHABET_LABELS
      BIOSEQ_TYPED_ALPHABET_REPLACEMENTS
      BIOSEQ_TYPED_ALPHABET_ORDINALS)
  set(_bioseq_highway_alignment_family
    "alignment_${_bioseq_highway_alignment_alphabet_ordinal}")
  list(APPEND BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS
    "ksw2|${_bioseq_highway_alignment_family}|align_ksw2_single_affine|&::bioseq::simd::{target_namespace}::align_ksw2_single_affine<${_bioseq_highway_alignment_alphabet_replacement}>"
    "ksw2|${_bioseq_highway_alignment_family}|align_ksw2_dual_affine|&::bioseq::simd::{target_namespace}::align_ksw2_dual_affine<${_bioseq_highway_alignment_alphabet_replacement}>"
    "ksw2_batched|${_bioseq_highway_alignment_family}|align_ksw2_batched|&::bioseq::simd::{target_namespace}::align_ksw2_batched<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|prepare_block_global_gap_certificate|&::bioseq::simd::{target_namespace}::prepare_block_global_gap_certificate<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|try_block_certified_gap|&::bioseq::simd::{target_namespace}::try_block_certified_gap<${_bioseq_highway_alignment_alphabet_replacement}>"
    "block_aligner|${_bioseq_highway_alignment_family}|align_block|&::bioseq::simd::{target_namespace}::align_block<${_bioseq_highway_alignment_alphabet_replacement}>")
  list(APPEND BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS
    "${_bioseq_highway_alignment_family}|align_ksw2_single_affine"
    "${_bioseq_highway_alignment_family}|align_ksw2_dual_affine"
    "${_bioseq_highway_alignment_family}|align_ksw2_batched"
    "${_bioseq_highway_alignment_family}|prepare_block_global_gap_certificate"
    "${_bioseq_highway_alignment_family}|try_block_certified_gap"
    "${_bioseq_highway_alignment_family}|align_block")
endforeach()
unset(_bioseq_highway_alignment_family)
unset(_bioseq_highway_alignment_alphabet_label)
unset(_bioseq_highway_alignment_alphabet_replacement)
unset(_bioseq_highway_alignment_alphabet_ordinal)

# Minimizer target wrappers use the same scheduler-owned alphabet/width axes.
# Each synthetic family is one Cartesian case; the Highway renderer validates
# exact 2 x 4 closure and folds these leaves back into the neutral three-member
# MinimizerKernels table. The renderer also reconciles these records with the
# typed alignment families to emit the root table's neutral alphabet identity;
# no additional CMake alphabet registry participates.
set(BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS)
set(BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS)
foreach(_bioseq_highway_minimizer_alphabet_label
    _bioseq_highway_minimizer_alphabet_replacement
    _bioseq_highway_minimizer_alphabet_ordinal
    IN ZIP_LISTS
      BIOSEQ_TYPED_ALPHABET_LABELS
      BIOSEQ_TYPED_ALPHABET_REPLACEMENTS
      BIOSEQ_TYPED_ALPHABET_ORDINALS)
  foreach(_bioseq_highway_minimizer_width_label
      _bioseq_highway_minimizer_width_replacement
      _bioseq_highway_minimizer_width_ordinal
      IN ZIP_LISTS
        BIOSEQ_TYPED_WIDTH_LABELS
        BIOSEQ_TYPED_WIDTH_REPLACEMENTS
        BIOSEQ_TYPED_WIDTH_ORDINALS)
    if(NOT _bioseq_highway_minimizer_width_replacement
        MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR
        "scheduler topology width replacement must be a positive byte count: "
        "'${_bioseq_highway_minimizer_width_replacement}'")
    endif()
    math(EXPR _bioseq_highway_minimizer_width_minus_one
      "${_bioseq_highway_minimizer_width_replacement} - 1")
    math(EXPR _bioseq_highway_minimizer_width_power_remainder
      "${_bioseq_highway_minimizer_width_replacement} & ${_bioseq_highway_minimizer_width_minus_one}")
    if(_bioseq_highway_minimizer_width_replacement GREATER 8
        OR NOT _bioseq_highway_minimizer_width_power_remainder EQUAL 0)
      message(FATAL_ERROR
        "scheduler topology minimizer width must be a power-of-two byte count "
        "no greater than eight: "
        "'${_bioseq_highway_minimizer_width_replacement}'")
    endif()
    math(EXPR _bioseq_highway_minimizer_width_bits
      "${_bioseq_highway_minimizer_width_replacement} * 8")
    set(_bioseq_highway_minimizer_family
      "minimizer_case_${_bioseq_highway_minimizer_alphabet_ordinal}_${_bioseq_highway_minimizer_width_replacement}")
    set(_bioseq_highway_minimizer_template_arguments
      "${_bioseq_highway_minimizer_alphabet_replacement},::std::uint${_bioseq_highway_minimizer_width_bits}_t")
    list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS
      "minimizer_minimap2_reference|${_bioseq_highway_minimizer_family}|minimap2_reference|&::bioseq::simd::{target_namespace}::minimap2_reference_case<${_bioseq_highway_minimizer_template_arguments}>"
      "minimizer_two_stack_nthash|${_bioseq_highway_minimizer_family}|two_stack_nthash|&::bioseq::simd::{target_namespace}::two_stack_nthash_case<${_bioseq_highway_minimizer_template_arguments}>"
      "minimizer_bounded_nthash|${_bioseq_highway_minimizer_family}|bounded_nthash|&::bioseq::simd::{target_namespace}::bounded_nthash_case<${_bioseq_highway_minimizer_template_arguments}>")
    list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS
      "${_bioseq_highway_minimizer_family}|minimap2_reference"
      "${_bioseq_highway_minimizer_family}|two_stack_nthash"
      "${_bioseq_highway_minimizer_family}|bounded_nthash")
  endforeach()
endforeach()
unset(_bioseq_highway_minimizer_family)
unset(_bioseq_highway_minimizer_template_arguments)
unset(_bioseq_highway_minimizer_width_bits)
unset(_bioseq_highway_minimizer_width_power_remainder)
unset(_bioseq_highway_minimizer_width_minus_one)
unset(_bioseq_highway_minimizer_width_label)
unset(_bioseq_highway_minimizer_width_replacement)
unset(_bioseq_highway_minimizer_width_ordinal)
unset(_bioseq_highway_minimizer_alphabet_label)
unset(_bioseq_highway_minimizer_alphabet_replacement)
unset(_bioseq_highway_minimizer_alphabet_ordinal)

# Target-private semantic probes are four ordinary implementation
# units of the one already-generated static-target primary. They are generator
# templates, never authored/final module providers or production slot owners.
set(BIOSEQ_HIGHWAY_UNIT_PROBE_RECORDS
  "hashers|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_test_probes/hashers.cppm.in"
  "sequence_ops|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_test_probes/sequence_ops.cppm.in"
  "simd_utils|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_test_probes/simd_utils.cppm.in"
  "two_stack|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_test_probes/two_stack.cppm.in")
# One bounded ordinary owner per admitted result representation prevents every
# coarse input-width wrapper from instantiating the complete result-layout
# switch. Baseline k-mer profiles have 7 owners / 14 typed instantiations;
# extended-k-mer profiles add K7/K27 identities for exactly 13 owners / 24
# instantiations per target.
set(BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
  "minimap2_reference_l15_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<15u,15u>|u32,u64"
  "minimap2_reference_l28_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<28u,15u>|u32,u64"
  "minimap2_reference_l33_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<33u,15u>|u32,u64"
  "minimap2_reference_l63_k15|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<63u,15u>|u32,u64"
  "bounded_nthash_l15_k15|bounded_nthash|minimizer_bounded_nthash|::bioseq::minimizers::minimizer_representation<15u,15u>|u32,u64"
  "bounded_nthash_l28_k15|bounded_nthash|minimizer_bounded_nthash|::bioseq::minimizers::minimizer_representation<28u,15u>|u32,u64"
  "bounded_nthash_l63_k15|bounded_nthash|minimizer_bounded_nthash|::bioseq::minimizers::minimizer_representation<63u,15u>|u32,u64")
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
  list(APPEND BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
    "minimap2_reference_l7_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<7u,7u>|u32,u64"
    "minimap2_reference_l17_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<17u,7u>|u32,u64"
    "minimap2_reference_l49_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<49u,7u>|u32,u64"
    "minimap2_reference_l63_k7|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<63u,7u>|u32,u64"
    "minimap2_reference_l27_k27|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<27u,27u>|u64"
    "minimap2_reference_l63_k27|minimap2_reference|minimizer_minimap2_reference|::bioseq::minimizers::minimizer_representation<63u,27u>|u64")
endif()

bioseq_add_highway_codegen(
  REQUIRE_COMPLETE_TABLE
  TARGET bioseq_highway_target_modules
  POLICY_TARGET bioseq_highway_target_policy
  OUTPUT_ROOT "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
  CONTRACT_MODULE bioseq.highway.dispatch_contract
  DISPATCH_MODULE bioseq.postgres.scheduler.dispatch_processor
  HIGHWAY_VERSION 1.3.0
  # Block Aligner requires at least eight lanes. EMU128 remains the portable
  # fixed-width fallback; scalar and sizeless targets are outside this table.
  POLICY_DEFINITIONS
    HWY_COMPILE_ALL_ATTAINABLE=1
    "HWY_DISABLED_TARGETS=(HWY_SCALAR|HWY_ALL_SVE|HWY_RVV)"
  DOMAIN_TOPOLOGY_MANIFESTS
    "typed_scheduler=${BIOSEQ_TYPED_CONFIGURE_TOPOLOGY}"
  COMPILE_DATABASE "${CMAKE_BINARY_DIR}/compile_commands.json"
  SIGNATURE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_dispatch_contract.cppm"
  PCM_ROOT "${BIOSEQ_HIGHWAY_GENERATED_ROOT}/pcm"
  SOURCE_RECORDS
    "simd_utils|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_utils.cppm|bioseq.highway.sources.simd_utils|simd_utils|simd_utils|"
    "vector_stream_operations|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_vector_stream_operations.cppm|bioseq.highway.sources.vector_stream_operations|vector_stream_operations|vector_stream|"
    "vector_stream|${CMAKE_CURRENT_SOURCE_DIR}/backend/common/simd/highway_vector_stream.cppm|bioseq.highway.sources.vector_stream|vector_stream|vector_stream|simd_utils,vector_stream_operations"
    "sequence_ops|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/highway_sequence_ops.cppm|bioseq.highway.sources.sequence_ops|sequence_ops|sequence_ops|simd_utils,vector_stream"
    "hasher_base|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/hashers/highway_base_kernel.cppm|bioseq.highway.sources.hasher_base|hasher_base|hasher_base|sequence_ops,simd_utils"
    "hasher_lexic|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/hashers/highway_lexic_kernel.cppm|bioseq.highway.sources.hasher_lexic|hasher_lexic|hasher_lexic|hasher_base,simd_utils"
    "hasher_minimap_int|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/hashers/highway_minimap_int_kernel.cppm|bioseq.highway.sources.hasher_minimap_int|hasher_minimap_int|hasher_minimap_int|hasher_base,hasher_lexic,simd_utils"
    "minimizer_minimap2_reference|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/minimap2_reference_kernel.cppm|bioseq.highway.sources.minimizer_minimap2_reference|minimizer_minimap2_reference|minimizers|sequence_ops,hasher_base,hasher_minimap_int,simd_utils,vector_stream"
    "minimizer_two_stack_nthash_state|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/state.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_state|minimizer_two_stack_nthash_state|minimizers|"
    "minimizer_two_stack_nthash_resident_stream|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/resident_stream.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_resident_stream|minimizer_two_stack_nthash_resident_stream|minimizers|minimizer_two_stack_nthash_state,sequence_ops,hasher_base,hasher_lexic,hasher_minimap_int,simd_utils,vector_stream"
    "minimizer_two_stack_nthash_exact_vectors|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_vectors.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors|minimizer_two_stack_nthash_exact_vectors|minimizers|minimizer_two_stack_nthash_state"
    "minimizer_two_stack_nthash_exact_decode|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_decode.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_exact_decode|minimizer_two_stack_nthash_exact_decode|minimizers|minimizer_two_stack_nthash_resident_stream,hasher_base"
    "minimizer_two_stack_nthash_exact_k15_w10|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_k15_w10.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_exact_k15_w10|minimizer_two_stack_nthash_exact_k15_w10|minimizers|minimizer_two_stack_nthash_state,minimizer_two_stack_nthash_resident_stream,hasher_minimap_int,minimizer_two_stack_nthash_exact_vectors,minimizer_two_stack_nthash_exact_decode"
    "minimizer_two_stack_nthash_exact_chronological|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/exact_chronological.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_exact_chronological|minimizer_two_stack_nthash_exact_chronological|minimizers|minimizer_two_stack_nthash_state,minimizer_two_stack_nthash_resident_stream,hasher_minimap_int,minimizer_two_stack_nthash_exact_vectors,minimizer_two_stack_nthash_exact_decode"
    "minimizer_two_stack_nthash_continuation|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/continuation.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_continuation|minimizer_two_stack_nthash_continuation|minimizers|minimizer_two_stack_nthash_state,minimizer_two_stack_nthash_exact_vectors,minimizer_two_stack_nthash_exact_decode,minimizer_two_stack_nthash_exact_k15_w10,minimizer_two_stack_nthash_exact_chronological"
    "minimizer_two_stack_nthash_drivers|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash/drivers.cppm|bioseq.highway.sources.minimizer_two_stack_nthash_drivers|minimizer_two_stack_nthash_drivers|minimizers|minimizer_two_stack_nthash_state,minimizer_two_stack_nthash_resident_stream,minimizer_two_stack_nthash_exact_vectors,minimizer_two_stack_nthash_continuation"
    "minimizer_two_stack_nthash|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/two_stack_nthash_kernel.cppm|bioseq.highway.sources.minimizer_two_stack_nthash|minimizer_two_stack_nthash|minimizers|minimizer_two_stack_nthash_state,minimizer_two_stack_nthash_drivers"
    "minimizer_bounded_nthash|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/kmers/bounded_nthash_kernel.cppm|bioseq.highway.sources.minimizer_bounded_nthash|minimizer_bounded_nthash|minimizers|sequence_ops,hasher_base,simd_utils,vector_stream"
    "alignment_utils|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/utils_kernel.cppm|bioseq.highway.sources.alignment_utils|alignment_utils|alignment|"
    "ksw2_primitives|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/ksw2_primitives.cppm|bioseq.highway.sources.ksw2_primitives|ksw2_primitives|alignment|"
    "ksw2|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/ksw2_kernel.cppm|bioseq.highway.sources.ksw2|ksw2|alignment|alignment_utils,ksw2_primitives"
    "block_aligner_dp|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_dp.cppm|bioseq.highway.sources.block_aligner_dp|block_aligner_dp|alignment|alignment_utils,simd_utils"
    "block_aligner_traceback|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_traceback.cppm|bioseq.highway.sources.block_aligner_traceback|block_aligner_traceback|alignment|block_aligner_dp"
    "block_aligner_certificates|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_certificates.cppm|bioseq.highway.sources.block_aligner_certificates|block_aligner_certificates|alignment|alignment_utils,block_aligner_dp"
    "block_aligner|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/block_aligner_kernel.cppm|bioseq.highway.sources.block_aligner|block_aligner|alignment|alignment_utils,block_aligner_certificates,block_aligner_dp,block_aligner_traceback"
    "ksw2_batched|${CMAKE_CURRENT_SOURCE_DIR}/backend/alignment_hwy/ksw2_batched_kernel.cppm|bioseq.highway.sources.ksw2_batched|ksw2_batched|alignment|ksw2,alignment_utils"
    "prefilter|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/prefilter/sneaky_snake/kernel.cppm|bioseq.highway.sources.sneaky_snake|prefilter|common|simd_utils"
    "chaining_candidate_scoring|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/chaining/chaining_simd_candidate_scoring.cppm|bioseq.highway.sources.chaining_candidate_scoring|chaining_candidate_scoring|chaining|simd_utils"
    "chaining_forward|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/chaining/chaining_simd_forward.cppm|bioseq.highway.sources.chaining_forward|chaining_forward|chaining|chaining_candidate_scoring"
    "chaining_backtracking|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/chaining/chaining_simd_backtracking.cppm|bioseq.highway.sources.chaining_backtracking|chaining_backtracking|chaining|chaining_candidate_scoring"
    "chaining|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/chaining/chaining_simd_kernel.cppm|bioseq.highway.sources.chaining|chaining|chaining|chaining_candidate_scoring,chaining_forward,chaining_backtracking"
    "indexing|${CMAKE_CURRENT_SOURCE_DIR}/backend/sequences/indexing/highway_indexing_kernels.cppm|bioseq.highway.sources.indexing|indexing|indexing|"
  SLOT_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_SLOT_RECORDS}
    ${BIOSEQ_HIGHWAY_ALIGNMENT_SLOT_RECORDS}
    "prefilter|common|sneaky_snake_simd_batch|&bioseq::prefilter::{target_namespace}::sneaky_snake_simd_batch"
    "chaining|chaining|chain_anchors_simd|&::bioseq::simd::{target_namespace}::chain_anchors_simd"
  REQUIRED_SLOT_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_REQUIRED_SLOT_RECORDS}
    ${BIOSEQ_HIGHWAY_ALIGNMENT_REQUIRED_SLOT_RECORDS}
    "common|sneaky_snake_simd_batch"
    "chaining|chain_anchors_simd"
  UNIT_PROBE_RECORDS ${BIOSEQ_HIGHWAY_UNIT_PROBE_RECORDS}
  MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
    ${BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS}
  OUTPUT_MODULES BIOSEQ_HIGHWAY_GENERATED_MODULES
  OUTPUT_PRIMARY_MODULES BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES
  OUTPUT_TARGET_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS
  OUTPUT_DISPATCH_REGISTRY BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY
  OUTPUT_DISPATCH_INVENTORY BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY
  OUTPUT_TEST_PRIMARY_MODULE BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE
  OUTPUT_TEST_IMPLEMENTATIONS BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS
  OUTPUT_TEST_SUPPORT BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT
  OUTPUT_UNIT_PROBE_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS
  OUTPUT_MINIMIZER_REPRESENTATION_INSTANCE_IMPLEMENTATIONS
    BIOSEQ_HIGHWAY_GENERATED_MINIMIZER_REPRESENTATION_INSTANCE_IMPLEMENTATIONS
  OUTPUT_SOURCE_INPUTS BIOSEQ_HIGHWAY_REGISTERED_SOURCE_INPUTS
  OUTPUT_TARGET_RECORDS BIOSEQ_HIGHWAY_COMPILED_TARGET_RECORDS
  OUTPUT_COMPILED_MASK BIOSEQ_HIGHWAY_COMPILED_TARGET_MASK)

list(LENGTH BIOSEQ_HIGHWAY_UNIT_PROBE_RECORDS
  _bioseq_highway_unit_probe_record_count)
list(LENGTH BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS
  _bioseq_highway_unit_probe_output_count)
list(LENGTH BIOSEQ_HIGHWAY_MINIMIZER_REPRESENTATION_INSTANCE_RECORDS
  _bioseq_highway_representation_owner_count)
list(LENGTH BIOSEQ_HIGHWAY_COMPILED_TARGET_RECORDS
  _bioseq_highway_compiled_target_count)
list(LENGTH BIOSEQ_HIGHWAY_GENERATED_MINIMIZER_REPRESENTATION_INSTANCE_IMPLEMENTATIONS
  _bioseq_highway_representation_output_count)
math(EXPR _bioseq_highway_expected_representation_output_count
  "${_bioseq_highway_representation_owner_count} * ${_bioseq_highway_compiled_target_count}")
if(NOT _bioseq_highway_unit_probe_record_count EQUAL 4
    OR NOT _bioseq_highway_unit_probe_output_count EQUAL 4)
  message(FATAL_ERROR
    "Fixed-target generation requires exactly four unit validation owners/outputs")
endif()
if(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
  set(_bioseq_highway_expected_representation_owner_count 13)
else()
  set(_bioseq_highway_expected_representation_owner_count 7)
endif()
if(NOT _bioseq_highway_representation_owner_count EQUAL
      _bioseq_highway_expected_representation_owner_count
    OR NOT _bioseq_highway_representation_output_count EQUAL
      _bioseq_highway_expected_representation_output_count)
  message(FATAL_ERROR
    "Minimizer representation owner/output closure diverged: "
    "owners=${_bioseq_highway_representation_owner_count}, "
    "outputs=${_bioseq_highway_representation_output_count}, "
    "targets=${_bioseq_highway_compiled_target_count}")
endif()
set(_bioseq_highway_probe_outputs
  ${BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS})
set(_bioseq_highway_unique_probe_outputs ${_bioseq_highway_probe_outputs})
list(REMOVE_DUPLICATES _bioseq_highway_unique_probe_outputs)
list(LENGTH _bioseq_highway_probe_outputs _bioseq_highway_probe_output_count)
list(LENGTH _bioseq_highway_unique_probe_outputs
  _bioseq_highway_unique_probe_output_count)
if(NOT _bioseq_highway_probe_output_count EQUAL
    _bioseq_highway_unique_probe_output_count)
  message(FATAL_ERROR "Highway validation outputs are not disjoint")
endif()
foreach(_bioseq_highway_unit_probe
    IN LISTS BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS)
  if(_bioseq_highway_unit_probe IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS
      OR _bioseq_highway_unit_probe IN_LIST
        BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS
      OR _bioseq_highway_unit_probe STREQUAL
        BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY
      OR _bioseq_highway_unit_probe STREQUAL
        BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY)
    message(FATAL_ERROR
      "unit probe escaped its dedicated fixed-target output: "
      "${_bioseq_highway_unit_probe}")
  endif()
endforeach()
unset(_bioseq_highway_unit_probe)
unset(_bioseq_highway_probe_outputs)
unset(_bioseq_highway_unique_probe_outputs)
unset(_bioseq_highway_probe_output_count)
unset(_bioseq_highway_unique_probe_output_count)
unset(_bioseq_highway_expected_representation_owner_count)
unset(_bioseq_highway_expected_representation_output_count)
unset(_bioseq_highway_representation_output_count)
unset(_bioseq_highway_compiled_target_count)
unset(_bioseq_highway_representation_owner_count)
unset(_bioseq_highway_unit_probe_output_count)
unset(_bioseq_highway_unit_probe_record_count)

# CMake requires every named module partition provider in a CXX_MODULES file
# set. Keep binders and per-owner instantiation units as ordinary private
# sources, and attach only generated partitions alongside their primaries.
set(BIOSEQ_HIGHWAY_GENERATED_TARGET_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TARGET_ORDINARY_IMPLEMENTATIONS
  INCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS
  EXCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS
  INCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")
set(BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_IMPLEMENTATIONS})
list(FILTER BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS
  EXCLUDE REGEX "/(binder|instances/[a-z][a-z0-9_]*)\\.cppm$")

# The compiler-selected fixed target has one reusable owner. Production,
# unit tests, and benchmarks consume the same BMI/object closure by linking
# these targets instead of compiling the generated files independently.
set(BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES
  ${BIOSEQ_HIGHWAY_GENERATED_PRIMARY_MODULES})
list(REMOVE_ITEM BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES
  "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}")
set(BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_ORDINARY_IMPLEMENTATIONS})
list(REMOVE_ITEM BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS})
set(BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TARGET_PARTITIONS})
list(REMOVE_ITEM BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS})

add_library(bioseq_highway_fixed_target STATIC)
set_target_properties(bioseq_highway_fixed_target PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_highway_fixed_target
  PRIVATE ${BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS}
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES
      "${BIOSEQ_HIGHWAY_GENERATED_TEST_PRIMARY_MODULE}"
      ${BIOSEQ_HIGHWAY_GENERATED_TEST_PARTITIONS})
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_TEST_ORDINARY_IMPLEMENTATIONS}
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_link_libraries(bioseq_highway_fixed_target PUBLIC
  bioseq_modules_nopg
  bioseq_highway_target_policy)
add_dependencies(
  bioseq_highway_fixed_target bioseq_highway_target_modules)

add_library(bioseq_highway_test_support STATIC)
set_target_properties(bioseq_highway_test_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_highway_test_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES "${BIOSEQ_HIGHWAY_GENERATED_TEST_SUPPORT}")
target_link_libraries(bioseq_highway_test_support PUBLIC
  bioseq_highway_fixed_target)
add_dependencies(
  bioseq_highway_test_support bioseq_highway_target_modules)

if(NOT BIOSEQ_HIGHWAY_REGISTERED_SOURCE_INPUTS STREQUAL
    BIOSEQ_HIGHWAY_TARGET_TEMPLATE_MODULES)
  message(FATAL_ERROR
    "Highway template ownership and final-provider exclusion diverged")
endif()
