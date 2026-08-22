cmake_minimum_required(VERSION 3.16)

project(kiotty_game_server_engine CXX)

# C++11 is the floor the engine targets (docs/requirement.md). Extensions are off
# so a GNU-only spelling cannot creep in and break the MSVC build.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clang-tidy reads this. Only the Ninja and Makefile generators produce it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KIOTTY_BUILD_TESTS "Build the test targets" ON)
option(KIOTTY_USE_IO_URING "Prefer io_uring over epoll on Linux when liburing is present" ON)

# -----------------------------------------------------------------------------
# Backend selection
#
# Exactly one multiplexer is compiled in. Every backend source guards itself on
# the same condition, so a file listed for the wrong platform would compile to
# nothing rather than to a broken object - but they are listed per platform
# anyway, so a missing guard shows up as a link error instead of silence.
# -----------------------------------------------------------------------------

set(KIOTTY_SOURCES
    src/implementation/core/kiotty_bytes.cpp
    src/implementation/presentation/event/kiotty_io_event.cpp
    src/implementation/presentation/endpoint/kiotty_endpoint.cpp
    src/implementation/presentation/connection/kiotty_connection.cpp
    src/implementation/presentation/connection/kiotty_connection_table.cpp
    src/implementation/domain/channel/kiotty_game_channel_pool.cpp
    src/implementation/domain/usecase/kiotty_usecase_registry.cpp
    src/implementation/domain/usecase/kiotty_usecase_dispatcher.cpp
    src/implementation/presentation/io_loop/kiotty_io_loop.cpp
)

set(KIOTTY_BACKEND_LIBRARIES "")
set(KIOTTY_BACKEND_DEFINITIONS "")

if(WIN32)
    set(KIOTTY_BACKEND_NAME "iocp")
    list(APPEND KIOTTY_SOURCES
        src/implementation/presentation/socket/backend/kiotty_windows_socket.cpp
        src/implementation/presentation/event_listener/backend/kiotty_iocp_io_event_listener.cpp
    )
    list(APPEND KIOTTY_BACKEND_LIBRARIES ws2_32)

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND KIOTTY_SOURCES
        src/implementation/presentation/socket/backend/kiotty_linux_socket.cpp
    )

    find_path(KIOTTY_LIBURING_INCLUDE_DIR liburing.h)
    find_library(KIOTTY_LIBURING_LIBRARY uring)

    if(KIOTTY_USE_IO_URING AND KIOTTY_LIBURING_INCLUDE_DIR AND KIOTTY_LIBURING_LIBRARY)
        set(KIOTTY_BACKEND_NAME "io_uring")
        list(APPEND KIOTTY_SOURCES
            src/implementation/presentation/socket/backend/kiotty_io_uring_socket.cpp
            src/implementation/presentation/event_listener/backend/kiotty_io_uring_io_event_listener.cpp
        )
        list(APPEND KIOTTY_BACKEND_LIBRARIES ${KIOTTY_LIBURING_LIBRARY})
        list(APPEND KIOTTY_BACKEND_DEFINITIONS KIOTTY_HAS_IO_URING)
    else()
        set(KIOTTY_BACKEND_NAME "epoll")
        list(APPEND KIOTTY_SOURCES
            src/implementation/presentation/socket/backend/kiotty_epoll_socket.cpp
            src/implementation/presentation/event_listener/backend/kiotty_epoll_io_event_listener.cpp
        )
    endif()

else()
    message(FATAL_ERROR "kiotty: no IO multiplexer backend for ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "kiotty: IO backend = ${KIOTTY_BACKEND_NAME}")

add_library(kiotty STATIC ${KIOTTY_SOURCES})

# PUBLIC: headers include each other as <presentation/...>, so anything linking
# kiotty needs the same root on its include path.
target_include_directories(kiotty PUBLIC src/implementation)

target_compile_definitions(kiotty PUBLIC ${KIOTTY_BACKEND_DEFINITIONS})
target_link_libraries(kiotty PUBLIC ${KIOTTY_BACKEND_LIBRARIES})

target_compile_options(kiotty PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

find_package(Threads REQUIRED)
target_link_libraries(kiotty PUBLIC Threads::Threads)

if(KIOTTY_BUILD_TESTS)
    enable_testing()

    # Unit tests own their own build definition, including how they get gtest.
    add_subdirectory(test/unit)

    # An end-to-end check, not a unit test: it drives one real client through
    # accept -> receive -> send and compares the bytes. Unit tests are a separate
    # job and belong under test/unit.
    add_executable(kiotty_io_event_listener_smoke
        test/integration/kiotty_io_event_listener_smoke.cpp)

    target_link_libraries(kiotty_io_event_listener_smoke PRIVATE kiotty)

    target_compile_options(kiotty_io_event_listener_smoke PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )

    add_test(NAME io_event_listener_smoke COMMAND kiotty_io_event_listener_smoke)

    # It binds a fixed loopback port, so two copies cannot run at once.
    set_tests_properties(io_event_listener_smoke PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 30
    )
endif()
