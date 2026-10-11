# AudioFileSource (--audio option) depends on miniaudio (single header library, optional).
# Find miniaudio in two ways: first search for miniaudio.h directly, then fall back to pkgconf (for environments such as Termux
# that provide only pkg-config). Treat it as missing only if both searches fail; then print DISABLED and skip
# AudioFileSource (sine/fourier sources are unaffected), following the same approach as mock_uvc_ffmpeg.
# Layout differences of  stb_vorbis.c (vcpkg include root/Termux include/stb/ subdirectory)
# are handled adaptively by __has_include in AudioFileSource.cpp; no detection is needed here.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if (NOT MINIAUDIO_INCLUDE_DIR)
    # Homebrew 0.11.25+ installs the header file in the include/miniaudio/ subdirectory, and its miniaudio.pc
    # Cflags also points to the include root (the pkgconf branch cannot be found either), so the subdirectory must be found directly.
    # PATH_SUFFIXES causes find_path to return <prefix>/include/miniaudio, which is the directory required by -I
    find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio)
endif ()
if (MINIAUDIO_INCLUDE_DIR)
    set(MINIAUDIO_INCLUDE_DIRS ${MINIAUDIO_INCLUDE_DIR})
else ()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(MINIAUDIO_PKGCONF QUIET miniaudio)
        if (MINIAUDIO_PKGCONF_FOUND)
            set(MINIAUDIO_INCLUDE_DIRS ${MINIAUDIO_PKGCONF_INCLUDE_DIRS})
        endif ()
    endif ()
endif ()

file(GLOB_RECURSE src *.cpp *.c)
# test files are compiled separately into the test target and do not participate in the main program.
list(FILTER src EXCLUDE REGEX "test_.*\\.cpp")
if (MINIAUDIO_INCLUDE_DIRS)
    message(STATUS "miniaudio: ENABLED (include: ${MINIAUDIO_INCLUDE_DIRS})")
else ()
    list(FILTER src EXCLUDE REGEX "AudioFileSource")
    message(STATUS "miniaudio: DISABLED (--audio option unavailable)")
endif ()
add_executable(mock_audio ${src})
target_link_libraries(mock_audio usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_audio PRIVATE .)
if (MINIAUDIO_INCLUDE_DIRS)
    target_include_directories(mock_audio PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
    target_compile_definitions(mock_audio PRIVATE USBIPDCPP_USE_MINIAUDIO)
endif ()

# AudioFileSource unit tests (implementation accompanies the example: add when building tests and miniaudio is available)
if (USBIPDCPP_BUILD_TESTS AND MINIAUDIO_INCLUDE_DIRS)
    find_package(GTest QUIET)
    if (GTest_FOUND)
        include(GoogleTest)
        enable_testing()
        add_executable(test_mock_audio_file test_audio_file_source.cpp AudioFileSource.cpp)
        target_link_libraries(test_mock_audio_file PRIVATE usbipdcpp_virtual_device GTest::gtest GTest::gtest_main)
        target_include_directories(test_mock_audio_file PRIVATE . ${MINIAUDIO_INCLUDE_DIRS})
        target_compile_definitions(test_mock_audio_file PRIVATE USBIPDCPP_USE_MINIAUDIO)
        # PRE_TEST: move test discovery to the build stage (same approach as tests/CMakeLists.txt, avoiding parallel-build races)
        gtest_discover_tests(test_mock_audio_file DISCOVERY_MODE PRE_TEST)
    endif ()
endif ()
