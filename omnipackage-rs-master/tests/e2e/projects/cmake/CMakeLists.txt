cmake_minimum_required(VERSION 3.10)
project(helloworld-cmake VERSION 1.2.3 LANGUAGES CXX)

add_executable(helloworld-cmake main.cpp)
target_compile_definitions(helloworld-cmake PRIVATE VERSION="${PROJECT_VERSION}")
install(TARGETS helloworld-cmake RUNTIME DESTINATION bin)
