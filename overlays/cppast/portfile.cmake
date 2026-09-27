vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO standardese/cppast
    REF 3790bf3fba9ba989086d4198689266452cd5d74a
    SHA512 4ac6e6d9cd1e07a0e9da1f867b11c1dcbe185e818825c9a63f238e4ffa1758f3b714f8d485ac295ddf3b29459f1353435ec92030f6fea93b6eb7289079c204bd
    HEAD_REF main
)

if(DEFINED ENV{LLVM_CONFIG_BINARY}
    AND EXISTS "$ENV{LLVM_CONFIG_BINARY}")
    set(_cppast_llvm_config "$ENV{LLVM_CONFIG_BINARY}")
else()
    find_program(_cppast_llvm_config
        NAMES llvm-config-22 llvm-config
        REQUIRED
    )
endif()
execute_process(
    COMMAND "${_cppast_llvm_config}" --version
    OUTPUT_VARIABLE _cppast_llvm_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(_cppast_llvm_version VERSION_LESS "15")
    message(FATAL_ERROR
        "cppast requires llvm-config 15 or newer; found ${_cppast_llvm_version}"
    )
endif()
execute_process(
    COMMAND "${_cppast_llvm_config}" --includedir
    OUTPUT_VARIABLE _cppast_libclang_include_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

vcpkg_cmake_configure(
    SOURCE_PATH "${CURRENT_PORT_DIR}"
    OPTIONS
        "-DCPPAST_SOURCE_DIR=${SOURCE_PATH}"
        "-DCPPAST_LIBCLANG_INCLUDE_DIR=${_cppast_libclang_include_dir}"
        "-DCPPAST_LIBCLANG_VERSION=${_cppast_llvm_version}"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME cppast
    CONFIG_PATH lib/cmake/cppast
)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
