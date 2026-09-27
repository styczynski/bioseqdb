cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/../../bioseq_highway_codegen.cmake")

function(_contract_require_equal actual expected message_text)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "Highway target-order contract failed: ${message_text}\n"
      "actual='${actual}'\nexpected='${expected}'")
  endif()
endfunction()

# HWY_VISIT_FALLBACK can publish the static ARM target before the remaining
# concrete ARM visits. Canonicalizing that alias must retain its visitor slot;
# sorting by the normalized slug would move NEON_WITHOUT_AES to the end.
set(_canonical_visitor_records
  "HWY_NEON_WITHOUT_AES|N_NEON_WITHOUT_AES|536870912"
  "HWY_NEON|N_NEON|268435456"
  "HWY_NEON_BF16|N_NEON_BF16|67108864")
_bioseq_highway_finalize_target_inventory(
  _records _order _mask _static_record
  aarch64 536870912 ${_canonical_visitor_records})

set(_expected_records
  "aarch64|neon_without_aes|HWY_NEON_WITHOUT_AES|N_NEON_WITHOUT_AES|536870912"
  "aarch64|neon|HWY_NEON|N_NEON|268435456"
  "aarch64|neon_bf16|HWY_NEON_BF16|N_NEON_BF16|67108864")
_contract_require_equal(
  "${_records}" "${_expected_records}"
  "normalization changed HWY_VISIT_TARGETS order")
_contract_require_equal(
  "${_order}"
  "HWY_NEON_WITHOUT_AES;HWY_NEON;HWY_NEON_BF16"
  "target token order diverged from HWY_VISIT_TARGETS")
_contract_require_equal(
  "${_mask}" "872415232"
  "compiled target mask changed during ordered normalization")
_contract_require_equal(
  "${_static_record}"
  "aarch64|neon_without_aes|HWY_NEON_WITHOUT_AES|N_NEON_WITHOUT_AES|536870912"
  "static target selection changed during ordered normalization")

message(STATUS "Highway target-order script contract passed")
