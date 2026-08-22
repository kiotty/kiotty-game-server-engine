# -----------------------------------------------------------------------------
# The single owner of the unit test build. The root CMakeLists only calls
# add_subdirectory here - the gtest dependency, the target and its options stay
# in the directory that needs them.
#
# Everything built here is free of the operating system: core/ opens no socket,
# starts no thread and touches no clock, so the same binary is expected to give
# the same result on Windows, Linux and Android. Anything that needs a socket
# belongs in test/integration.
# -----------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2   # pinned: following a branch makes yesterday's green build red today
)

# MSVC refuses to link two objects built against different CRTs, and gtest
# defaults to the static one while this project uses the dynamic one.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# The engine's install rules should not grow a gtest half.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Sources are listed, not globbed. A new .cpp that is not added here does not
# compile at all, which is louder than a test that quietly never runs.
add_executable(kiotty_unit_tests
    implementation/kiotty_test_account_id.cpp
    implementation/kiotty_test_block_pool.cpp
    implementation/kiotty_test_bytes.cpp
    implementation/kiotty_test_channel_binder.cpp
    implementation/kiotty_test_channel_id.cpp
    implementation/kiotty_test_connection_info.cpp
    implementation/kiotty_test_constant_time.cpp
    implementation/kiotty_test_cryptor.cpp
    implementation/kiotty_test_datasource.cpp
    implementation/kiotty_test_game_channel.cpp
    implementation/kiotty_test_game_channel_pool.cpp
    implementation/kiotty_test_holder.cpp
    implementation/kiotty_test_packet_codec.cpp
    implementation/kiotty_test_packet_concept.cpp
    implementation/kiotty_test_password_hasher.cpp
    implementation/kiotty_test_receive_buffer.cpp
    implementation/kiotty_test_result.cpp
    implementation/kiotty_test_result_ref.cpp
    implementation/kiotty_test_ring_buffer.cpp
    implementation/kiotty_test_send_buffer.cpp
    implementation/kiotty_test_session_repository.cpp
    implementation/kiotty_test_stream.cpp
    implementation/kiotty_test_usecase_dispatcher.cpp
)

# So the tests can say #include "support/kiotty_test_tracked.h".
target_include_directories(kiotty_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# GoogleTest 1.15 needs C++14. The engine itself stays at the C++11 the root
# CMakeLists sets, and this raise applies to the test binary only - said
# explicitly here rather than inherited by accident from gtest.
target_compile_features(kiotty_unit_tests PRIVATE cxx_std_14)

target_link_libraries(kiotty_unit_tests PRIVATE kiotty GTest::gtest_main)

target_compile_options(kiotty_unit_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Discovery rather than add_test: a new TEST would otherwise stay outside ctest
# without anyone noticing.
include(GoogleTest)
gtest_discover_tests(kiotty_unit_tests)
