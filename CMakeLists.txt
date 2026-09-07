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

# Swap the mutex-based LatestSlot for the lock-free triple buffer
# everywhere the simulation uses an inter-thread frame slot.
option(COMPOSITOR_SIM_LOCKFREE "Use the lock-free triple-buffer slot in the sim" OFF)

add_executable(compositor_sim src/main.cpp)
target_link_libraries(compositor_sim PRIVATE Threads::Threads)
if(COMPOSITOR_SIM_LOCKFREE)
    target_compile_definitions(compositor_sim PRIVATE COMPOSITOR_SIM_LOCKFREE)
endif()

enable_testing()
# Force asserts/checks on for the test binary regardless of build type.
add_executable(run_tests tests/test_latest_slot.cpp)
target_link_libraries(run_tests PRIVATE Threads::Threads)
target_compile_options(run_tests PRIVATE -UNDEBUG)
add_test(NAME unit_tests COMMAND run_tests)
