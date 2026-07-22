set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)

set(_bioseq_tsan_clang "/opt/homebrew/opt/llvm@22/bin/clang")
set(_bioseq_tsan_clangxx "/opt/homebrew/opt/llvm@22/bin/clang++")
if(NOT EXISTS "${_bioseq_tsan_clang}" OR
   NOT EXISTS "${_bioseq_tsan_clangxx}")
    message(FATAL_ERROR
        "Bioseq's TSan oneTBB overlay requires Homebrew LLVM 22")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO oneapi-src/oneTBB
    REF "v${VERSION}"
    SHA512 fdc50589785b1949ca1dd4429bbcedb180be4b8966da5243ddd1f8e9f97310dd603681e0bb83c1d6c2d3e27932f577ef6739e4e82f3c54af147f4d6d906b39f1
    HEAD_REF master
    PATCHES
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    INVERTED_FEATURES
        hwloc TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DTBB_TEST=OFF
        -DTBB_STRICT=OFF
        -DTBB_SANITIZE=thread
        "-DCMAKE_C_COMPILER:FILEPATH=${_bioseq_tsan_clang}"
        "-DCMAKE_CXX_COMPILER:FILEPATH=${_bioseq_tsan_clangxx}"
)

set(_bioseq_tbb_validated_cache_count 0)
foreach(_bioseq_tbb_variant IN ITEMS dbg rel)
    set(_bioseq_tbb_cache
        "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-${_bioseq_tbb_variant}/CMakeCache.txt")
    if(NOT EXISTS "${_bioseq_tbb_cache}")
        continue()
    endif()

    math(EXPR _bioseq_tbb_validated_cache_count
        "${_bioseq_tbb_validated_cache_count} + 1")
    file(STRINGS "${_bioseq_tbb_cache}" _bioseq_tbb_cache_lines)
    list(FIND _bioseq_tbb_cache_lines
        "TBB_SANITIZE:STRING=thread"
        _bioseq_tbb_sanitize_cache_entry_index)
    if(_bioseq_tbb_sanitize_cache_entry_index EQUAL -1)
        message(FATAL_ERROR
            "Bioseq's TSan oneTBB ${_bioseq_tbb_variant} cache does not "
            "attest 'TBB_SANITIZE:STRING=thread': ${_bioseq_tbb_cache}")
    endif()

    foreach(_bioseq_tbb_compiler_kind IN ITEMS C CXX)
        if(_bioseq_tbb_compiler_kind STREQUAL "C")
            set(_bioseq_tbb_expected_compiler "${_bioseq_tsan_clang}")
        else()
            set(_bioseq_tbb_expected_compiler "${_bioseq_tsan_clangxx}")
        endif()
        set(_bioseq_tbb_compiler_cache_entries
            "${_bioseq_tbb_cache_lines}")
        list(FILTER _bioseq_tbb_compiler_cache_entries INCLUDE REGEX
            "^CMAKE_${_bioseq_tbb_compiler_kind}_COMPILER:(STRING|FILEPATH)=")
        list(LENGTH _bioseq_tbb_compiler_cache_entries
            _bioseq_tbb_compiler_cache_entry_count)
        if(NOT _bioseq_tbb_compiler_cache_entry_count EQUAL 1)
            message(FATAL_ERROR
                "Bioseq's TSan oneTBB ${_bioseq_tbb_variant} cache does not "
                "contain exactly one ${_bioseq_tbb_compiler_kind} compiler "
                "STRING or FILEPATH entry: ${_bioseq_tbb_cache}")
        endif()
        list(GET _bioseq_tbb_compiler_cache_entries 0
            _bioseq_tbb_compiler_cache_entry)
        if(NOT _bioseq_tbb_compiler_cache_entry STREQUAL
              "CMAKE_${_bioseq_tbb_compiler_kind}_COMPILER:STRING=${_bioseq_tbb_expected_compiler}"
           AND NOT _bioseq_tbb_compiler_cache_entry STREQUAL
              "CMAKE_${_bioseq_tbb_compiler_kind}_COMPILER:FILEPATH=${_bioseq_tbb_expected_compiler}")
            message(FATAL_ERROR
                "Bioseq's TSan oneTBB ${_bioseq_tbb_variant} cache does not "
                "attest the exact ${_bioseq_tbb_compiler_kind} compiler "
                "path '${_bioseq_tbb_expected_compiler}' as STRING or FILEPATH: "
                "${_bioseq_tbb_cache}")
        endif()
    endforeach()
endforeach()
if(_bioseq_tbb_validated_cache_count EQUAL 0)
    message(FATAL_ERROR
        "Bioseq's TSan oneTBB overlay produced no CMakeCache to validate")
endif()

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/TBB")
vcpkg_copy_pdbs()

if(NOT VCPKG_BUILD_TYPE)
    if(VCPKG_TARGET_ARCHITECTURE MATCHES "^(x86|arm|wasm32)$")
        set(arch_suffix "32")
    endif()
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/tbb${arch_suffix}.pc" "-ltbb12" "-ltbb12_debug")
    else()
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/tbb${arch_suffix}.pc" "-ltbb" "-ltbb_debug")
    endif()
    unset(arch_suffix)
endif()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/share/doc"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    # These are duplicate libraries provided on Windows -- users should use the tbb12 libraries instead
    "${CURRENT_PACKAGES_DIR}/lib/tbb.lib"
    "${CURRENT_PACKAGES_DIR}/debug/lib/tbb_debug.lib"
)

file(READ "${CURRENT_PACKAGES_DIR}/share/tbb/TBBConfig.cmake" _contents)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/tbb/TBBConfig.cmake" "
include(CMakeFindDependencyMacro)
find_dependency(Threads)
${_contents}")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/bioseq-tsan-instrumented.txt"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/tbb")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
