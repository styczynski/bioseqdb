vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mity/md4c
    REF c4be8625eb11725d604232b028df10c2ddf9b577
    SHA512 663629ac1783138ba3c2b119c31c81e1aecacb7c73f230f207b898f525449a651714b9b1f56a30648e522cea03a5fa3b6efcad52489534bfd195cbe00166e4f2
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_MD2HTML_EXECUTABLE=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/md4c)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
