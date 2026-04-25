if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std:c++20")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Werror -ggdb")
endif()

if (SQLGEN_BUILD_DRY_TESTS_ONLY)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DSQLGEN_BUILD_DRY_TESTS_ONLY")
endif()

if(SQLGEN_DUCKDB)
    add_subdirectory(duckdb)
endif()

if(SQLGEN_MYSQL)
    add_subdirectory(mysql)
endif()

if(SQLGEN_POSTGRES)
    add_subdirectory(postgres)
endif()

if(SQLGEN_SQLITE3)
    add_subdirectory(sqlite)
endif()
