include(FetchContent)

FetchContent_Declare(
  ut
  GIT_REPOSITORY https://github.com/openalgz/ut
  GIT_TAG main
  GIT_SHALLOW TRUE
)

message(STATUS "Fetching dependencies...")
set_directory_properties(PROPERTIES EXCLUDE_FROM_ALL TRUE)
set(CMAKE_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ut)
set(CMAKE_SKIP_INSTALL_RULES OFF CACHE BOOL "" FORCE)
set_directory_properties(PROPERTIES EXCLUDE_FROM_ALL FALSE)
message(STATUS "...finished fetching dependencies.")

add_library(test_common INTERFACE)
target_compile_features(test_common INTERFACE cxx_std_23)
target_link_libraries(test_common INTERFACE ut::ut bencher::bencher)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_common INTERFACE -fno-rtti)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(test_common INTERFACE -Wall -Wextra -pedantic $<$<CONFIG:Debug>:-Werror>)
    else()
        target_compile_options(test_common INTERFACE -Wall -Wextra -pedantic)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_common INTERFACE /GR- /bigobj)
    target_compile_options(test_common INTERFACE /W4 /wd4459 /wd4805)
endif()

add_subdirectory(unit_test)

