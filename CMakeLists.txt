cmake_minimum_required(VERSION 3.16)
project(compositor_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

include_directories(include)

add_executable(compositor_sim src/main.cpp)
target_link_libraries(compositor_sim PRIVATE Threads::Threads)

enable_testing()
add_executable(run_tests tests/test_latest_slot.cpp)
target_link_libraries(run_tests PRIVATE Threads::Threads)
add_test(NAME unit_tests COMMAND run_tests)
