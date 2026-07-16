add_subdirectory(safestringlib)
#add_subdirectory(fmtlog)
add_subdirectory(fmt)
# fmt is a static archive consumed by the PostgreSQL shared modules on every
# supported toolchain, so its objects must always be position-independent.
set_property(TARGET fmt PROPERTY POSITION_INDEPENDENT_CODE ON)
add_subdirectory(seqan3)

#add_subdirectory(simd-highway)
