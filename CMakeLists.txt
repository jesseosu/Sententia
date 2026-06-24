cmake_minimum_required(VERSION 3.16)

project(sententia
        VERSION 0.1.0
        DESCRIPTION "Fault-tolerant distributed order matching engine"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SENTENTIA_WERROR "Treat warnings as errors" ON)

add_library(sententia_flags INTERFACE)
if(MSVC)
    target_compile_options(sententia_flags INTERFACE /W4 /permissive-)
    if(SENTENTIA_WERROR)
        target_compile_options(sententia_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(sententia_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(SENTENTIA_WERROR)
        target_compile_options(sententia_flags INTERFACE -Werror)
    endif()
endif()

# The engine core. No I/O, no threads, no clock: see docs/determinism.md.
add_library(sententia_core
    src/types.cpp
    src/order_book.cpp
    src/event.cpp
    src/engine.cpp)
target_include_directories(sententia_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sententia_core PRIVATE sententia_flags)

add_executable(replay apps/replay/main.cpp)
target_link_libraries(replay PRIVATE sententia_core sententia_flags)

add_executable(engine_bench bench/engine_bench.cpp)
target_link_libraries(engine_bench PRIVATE sententia_core sententia_flags)

enable_testing()

# Each test is its own executable, so a failure isolates to one named
# test and a crash cannot take the rest of the suite with it.
set(SENTENTIA_TESTS
    test_order_book
    test_matching_basic
    test_matching_partial
    test_price_time_priority
    test_cancel
    test_order_types
    test_edge_cases
    test_determinism
    test_invariants)

foreach(test_name IN LISTS SENTENTIA_TESTS)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE sententia_core sententia_flags)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Replaying the sample order file twice must produce identical output.
# This is the determinism property checked end to end, through the real
# I/O path, rather than in process.
add_test(NAME replay_sample
         COMMAND ${CMAKE_COMMAND}
                 -DREPLAY=$<TARGET_FILE:replay>
                 -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/sample_orders.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/replay_determinism.cmake)
