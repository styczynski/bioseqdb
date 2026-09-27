vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jinja2cpp/Jinja2Cpp
    REF 79af49f6f9a8a552edb9c1932d4a73a5895b1d32
    SHA512 ecc8bbc2915eb3480e04e51b1a2ad8a90853838c384fb8bd6854d9a1fb68e15700ca5fec64a8f2ff7111fd61e17213079f950998c5bb1ade3ce1dce63b79ef56
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${CURRENT_PORT_DIR}"
    OPTIONS
        "-DJINJA2CPP_SOURCE_DIR=${SOURCE_PATH}"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME jinja2cpp
    CONFIG_PATH lib/cmake/jinja2cpp
)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
