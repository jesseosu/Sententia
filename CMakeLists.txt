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

# Networking layer. POSIX only: the engine core stays portable and CI
# still builds and tests it on Windows and macOS, but the transport
# targets the platform this project deploys to. See docs/wire-protocol.md.
if(UNIX)
    add_library(sententia_net
        src/net/message.cpp
        src/net/framing.cpp
        src/net/socket.cpp
        src/net/cluster_config.cpp
        src/net/transport.cpp
        src/replication/command_log.cpp
        src/replication/replicator.cpp)
    target_include_directories(sententia_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(sententia_net PUBLIC sententia_core PRIVATE sententia_flags)

    add_executable(node apps/node/main.cpp)
    target_link_libraries(node PRIVATE sententia_net sententia_flags)

    add_executable(replication_bench bench/replication_bench.cpp)
    target_link_libraries(replication_bench PRIVATE sententia_net sententia_flags)
endif()

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

# Networking tests, POSIX only. The first four need no sockets at all;
# only test_transport opens one.
set(SENTENTIA_NET_TESTS
    test_wire
    test_message_codec
    test_framing
    test_cluster_config
    test_transport
    test_backpressure
    test_command_log
    test_replication
    test_replication_chaos)

foreach(test_name IN LISTS SENTENTIA_TESTS)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE sententia_core sententia_flags)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

if(UNIX)
    foreach(test_name IN LISTS SENTENTIA_NET_TESTS)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE sententia_net sententia_flags)
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # Two real node processes exchange heartbeats and shut down cleanly.
    add_test(NAME two_node_demo
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/two_node_demo.sh
                     $<TARGET_FILE:node>)
    set_tests_properties(two_node_demo PROPERTIES TIMEOUT 60)

    # A primary replicates an order file to a backup and both end at the
    # same state checksum, in each acknowledgement mode.
    foreach(repl_mode IN ITEMS sync async)
        add_test(NAME replication_demo_${repl_mode}
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/replication_demo.sh
                         $<TARGET_FILE:node>
                         ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sample_orders.txt
                         ${repl_mode})
        set_tests_properties(replication_demo_${repl_mode} PROPERTIES TIMEOUT 90)
    endforeach()
endif()

# Replaying the sample order file twice must produce identical output.
# This is the determinism property checked end to end, through the real
# I/O path, rather than in process.
add_test(NAME replay_sample
         COMMAND ${CMAKE_COMMAND}
                 -DREPLAY=$<TARGET_FILE:replay>
                 -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/scripts/sample_orders.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/replay_determinism.cmake)
