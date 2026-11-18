cmake_minimum_required(VERSION 3.24)
get_filename_component(_repository_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# Probe the real package entry point before dependency/toolchain provisioning.
# A stale production cache must fail rather than emit an API-disabled package.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _package_guard_nonce)
if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
  set(_package_guard_root "$ENV{TMPDIR}/bioseq-package-guard-${_package_guard_nonce}")
else()
  set(_package_guard_root "/tmp/bioseq-package-guard-${_package_guard_nonce}")
endif()
foreach(_disabled IN ITEMS EXTERNAL_API_CLIENTS CACHE)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${_repository_root}" -B "${_package_guard_root}/${_disabled}"
    -DBIOSEQ_PACKAGE_BUILD=ON -DBIOSEQ_BUILD_CACHE=ON
    -DBIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON "-DBIOSEQ_BUILD_${_disabled}=OFF"
    RESULT_VARIABLE _guard_result OUTPUT_VARIABLE _guard_out ERROR_VARIABLE _guard_err)
  if(_guard_result EQUAL 0 OR NOT "${_guard_out}${_guard_err}" MATCHES "Production packages require")
    message(FATAL_ERROR "Production package guard did not reject ${_disabled}=OFF: ${_guard_out}${_guard_err}")
  endif()
endforeach()
file(REMOVE_RECURSE "${_package_guard_root}")

message(STATUS "Production packages reject disabled cache and external API")
