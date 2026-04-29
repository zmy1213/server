cmake_minimum_required(VERSION 3.20)

project(cpp20_server
    VERSION 0.1.0
    DESCRIPTION "A portable C++20 TCP server foundation"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CPP20_SERVER_BUILD_EXAMPLES "Build example programs" ON)
option(CPP20_SERVER_BUILD_TESTS "Build test programs" ON)
option(CPP20_SERVER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CPP20_SERVER_BACKEND "auto" CACHE STRING "I/O backend: auto, epoll, kqueue, iocp, select")
set_property(CACHE CPP20_SERVER_BACKEND PROPERTY STRINGS auto epoll kqueue iocp select)

if(CPP20_SERVER_BACKEND STREQUAL "auto")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(CPP20_SERVER_RESOLVED_BACKEND "epoll")
    elseif(APPLE)
        set(CPP20_SERVER_RESOLVED_BACKEND "kqueue")
    elseif(WIN32)
        set(CPP20_SERVER_RESOLVED_BACKEND "iocp")
    else()
        set(CPP20_SERVER_RESOLVED_BACKEND "select")
    endif()
else()
    set(CPP20_SERVER_RESOLVED_BACKEND "${CPP20_SERVER_BACKEND}")
endif()

if(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "epoll" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "epoll backend is only available on Linux")
endif()

if(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "kqueue" AND NOT APPLE)
    message(FATAL_ERROR "kqueue backend is enabled here only for macOS")
endif()

if(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "iocp" AND NOT WIN32)
    message(FATAL_ERROR "iocp backend is only available on Windows")
endif()

set(CPP20_SERVER_SOURCES
    src/net/buffer.cpp
    src/net/socket.cpp
    src/net/tcp_server.cpp
)

if(NOT CPP20_SERVER_RESOLVED_BACKEND STREQUAL "iocp")
    list(APPEND CPP20_SERVER_SOURCES
        src/net/acceptor.cpp
        src/net/channel.cpp
        src/net/connection.cpp
        src/net/event_loop.cpp
        src/net/poller.cpp
    )
endif()

add_library(cpp20_server ${CPP20_SERVER_SOURCES})

target_include_directories(cpp20_server
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(cpp20_server PUBLIC cxx_std_20)

if(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "epoll")
    target_compile_definitions(cpp20_server PRIVATE CPP20_SERVER_USE_EPOLL=1)
elseif(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "kqueue")
    target_compile_definitions(cpp20_server PRIVATE CPP20_SERVER_USE_KQUEUE=1)
elseif(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "iocp")
    target_compile_definitions(cpp20_server PRIVATE CPP20_SERVER_USE_IOCP=1)
elseif(CPP20_SERVER_RESOLVED_BACKEND STREQUAL "select")
    target_compile_definitions(cpp20_server PRIVATE CPP20_SERVER_USE_SELECT=1)
else()
    message(FATAL_ERROR "Unsupported backend: ${CPP20_SERVER_RESOLVED_BACKEND}")
endif()

target_compile_definitions(cpp20_server
    PUBLIC
        CPP20_SERVER_BACKEND_NAME="${CPP20_SERVER_RESOLVED_BACKEND}"
)

if(WIN32)
    target_link_libraries(cpp20_server PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(cpp20_server PRIVATE /W4 /permissive-)
    if(CPP20_SERVER_WARNINGS_AS_ERRORS)
        target_compile_options(cpp20_server PRIVATE /WX)
    endif()
else()
    target_compile_options(cpp20_server PRIVATE -Wall -Wextra -Wpedantic)
    if(CPP20_SERVER_WARNINGS_AS_ERRORS)
        target_compile_options(cpp20_server PRIVATE -Werror)
    endif()
endif()

if(CPP20_SERVER_BUILD_EXAMPLES)
    add_executable(echo_server examples/echo_server.cpp)
    target_link_libraries(echo_server PRIVATE cpp20_server)
endif()

if(CPP20_SERVER_BUILD_TESTS)
    enable_testing()

    add_executable(server_tests tests/server_tests.cpp)
    target_link_libraries(server_tests PRIVATE cpp20_server)
    add_test(NAME server_tests COMMAND server_tests)
endif()

message(STATUS "cpp20_server backend: ${CPP20_SERVER_RESOLVED_BACKEND}")
