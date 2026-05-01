# Enabling `assert` by setting the build type to 'Debug' for test module
set(CMAKE_BUILD_TYPE "Debug")

# Defining target kseq++-test
add_executable(kseq++-test src/kseq++_test.cpp)
target_compile_options(kseq++-test PRIVATE -g -Wall -Wpedantic -Werror)
target_include_directories(kseq++-test
  PRIVATE ${PROJECT_SOURCE_DIR}/test/include
  PRIVATE kseq++::kseq++)
target_link_libraries(kseq++-test
  PRIVATE kseq++::kseq++)

# Defining target seqio-test
add_executable(seqio-test src/seqio_test.cpp)
target_compile_options(seqio-test PRIVATE -g -Wall -Wpedantic -Werror)
target_include_directories(seqio-test
  PRIVATE ${PROJECT_SOURCE_DIR}/test/include
  PRIVATE kseq++::kseq++)
target_link_libraries(seqio-test
  PRIVATE kseq++::kseq++)

add_custom_target(test
  COMMAND ./test/kseq++-test ${PROJECT_SOURCE_DIR}/test/kseq++_test.dat
  COMMAND ./test/seqio-test ${PROJECT_SOURCE_DIR}/test/kseq++_test.dat
  DEPENDS kseq++-test seqio-test
  WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
  )
