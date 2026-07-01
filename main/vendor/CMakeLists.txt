add_subdirectory(safestringlib)
#add_subdirectory(fmtlog)
add_subdirectory(fmt)
if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  set_property(TARGET fmt PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
add_subdirectory(seqan3)

#add_subdirectory(simd-highway)
