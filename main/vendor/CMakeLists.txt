add_subdirectory(safestringlib)
#add_subdirectory(fmtlog)
add_subdirectory(fmt)
# fmt is linked into the shared postgresql_bioseq extension module in every core
# configuration - the GCC 16.1 extension build and the Clang-22 website tree alike -
# so its objects must be position-independent or the shared-object link fails with
# "relocation R_X86_64_PC32 ... can not be used when making a shared object".
set_property(TARGET fmt PROPERTY POSITION_INDEPENDENT_CODE ON)
add_subdirectory(seqan3)

#add_subdirectory(simd-highway)
