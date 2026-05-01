cmake_minimum_required(VERSION 3.10)
project(kseq++ VERSION 1.1.2 LANGUAGES CXX)

#options
option(BUILD_TESTING "Build test programs" OFF)  # ignored by default
option(BUILD_BENCHMARKING "Build benchmark program" OFF)  # ignored by default

# Include external modules
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Finding dependencies.
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(Threads REQUIRED)

# Creating an INTERFACE library
add_library(kseq++ INTERFACE)
# Defining include directories
target_include_directories(kseq++
  INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>;$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>
  INTERFACE $<TARGET_PROPERTY:ZLIB::ZLIB,INTERFACE_INCLUDE_DIRECTORIES>
  INTERFACE $<TARGET_PROPERTY:BZip2::BZip2,INTERFACE_INCLUDE_DIRECTORIES>
  INTERFACE $<TARGET_PROPERTY:Threads::Threads,INTERFACE_INCLUDE_DIRECTORIES>)
# Defining link libraries
target_link_libraries(kseq++
  INTERFACE $<BUILD_INTERFACE:ZLIB::ZLIB>;$<INSTALL_INTERFACE:ZLIB::ZLIB>
  INTERFACE $<BUILD_INTERFACE:BZip2::BZip2>;$<INSTALL_INTERFACE:BZip2::BZip2>
  INTERFACE $<BUILD_INTERFACE:Threads::Threads>;$<INSTALL_INTERFACE:Threads::Threads>)
# Use C++17
target_compile_features(kseq++ INTERFACE cxx_std_11)
# Generating the configure header file
configure_file(include/kseq++/config.hpp.in ${PROJECT_SOURCE_DIR}/include/kseq++/config.hpp @ONLY)
# Generating pkgconfig file
configure_file(kseq++.pc.in ${PROJECT_SOURCE_DIR}/kseq++.pc @ONLY)
# Setting header files
file(GLOB HEADER_FILES RELATIVE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/include/kseq++/*.h")
list(TRANSFORM HEADER_FILES PREPEND "${PROJECT_SOURCE_DIR}/include/" OUTPUT_VARIABLE BUILD_HEADER_FILES)
list(TRANSFORM HEADER_FILES PREPEND "${CMAKE_INSTALL_FULL_INCLUDEDIR}/" OUTPUT_VARIABLE INSTALL_HEADER_FILES)
# Defining target sources
target_sources(kseq++
  INTERFACE "$<BUILD_INTERFACE:${BUILD_HEADER_FILES}>;$<INSTALL_INTERFACE:${INSTALL_HEADER_FILES}>")
# Defining `kseq++::kseq++` alias
add_library(kseq++::kseq++ ALIAS kseq++)
# Install targets
install(DIRECTORY include/kseq++ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN "*.hpp.in" EXCLUDE)
install(FILES kseq++.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
# Exporting targets
install(TARGETS kseq++ EXPORT kseq++-targets)
install(EXPORT kseq++-targets NAMESPACE kseq++:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kseq++)
# Creating the package configuration file
configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/kseq++-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/kseq++-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kseq++)
# Generating the version file for the configuration file
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/kseq++-config-version.cmake"
  VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
  COMPATIBILITY AnyNewerVersion)
# Install generated configuration files
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/kseq++-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/kseq++-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kseq++)

# Adding test submodule
if(BUILD_TESTING)
  add_subdirectory(test)
endif(BUILD_TESTING)

# Adding benchmark submodule
if(BUILD_BENCHMARKING)
  add_subdirectory(benchmark)
endif(BUILD_BENCHMARKING)

# Packaging configuration
set(CPACK_PACKAGE_NAME "kseqpp")
set(CPACK_PACKAGE_VENDOR "cartoonist")
set(CPACK_GENERATOR "STGZ;TGZ;RPM")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast FASTA/Q parser and writer (C++ re-implementation of kseq library)")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_SYSTEM_NAME "noarch")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
include(CPack)
