# Alternative to the Makefile (the Makefile is the primary, tested build).
#   cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Debug [-DTCPSYNC_SANITIZE=thread|address]
#   cmake --build cmake-build -j && ctest --test-dir cmake-build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(tcpsync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(TCPSYNC_SANITIZE "" CACHE STRING "Sanitizer to enable: address or thread")

find_package(Threads REQUIRED)

add_library(tcpsync_core STATIC
    src/net.cpp src/protocol.cpp src/file_store.cpp src/server.cpp src/client.cpp)
target_include_directories(tcpsync_core PUBLIC include)
target_link_libraries(tcpsync_core PUBLIC Threads::Threads)
target_compile_options(tcpsync_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow)

if(TCPSYNC_SANITIZE STREQUAL "address")
    target_compile_options(tcpsync_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tcpsync_core PUBLIC -fsanitize=address,undefined)
elseif(TCPSYNC_SANITIZE STREQUAL "thread")
    target_compile_options(tcpsync_core PUBLIC -fsanitize=thread)
    target_link_options(tcpsync_core PUBLIC -fsanitize=thread)
endif()

add_executable(tcpsync-server src/server_main.cpp)
target_link_libraries(tcpsync-server PRIVATE tcpsync_core)

add_executable(tcpsync-client src/client_main.cpp)
target_link_libraries(tcpsync-client PRIVATE tcpsync_core)

add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE tcpsync_core)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
add_test(NAME integration
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.sh ${CMAKE_CURRENT_BINARY_DIR})
