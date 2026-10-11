# PlaybackSink (native playback) depends on miniaudio (single header file library, optional).
# Search as in mock_audio: first find miniaudio.h directly (including the Homebrew include/miniaudio/
# subdirectory layout), then fall back to pkgconf (for environments such as Termux that provide only pkg-config). If both fail,
# mock_speaker still builds normally (--output WavFileSink and discard mode are unaffected);
# only local playback options are unavailable
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if (NOT MINIAUDIO_INCLUDE_DIR)
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

file(GLOB_RECURSE src *.cpp)
# test files are compiled separately into the test target and do not participate in the main program.
list(FILTER src EXCLUDE REGEX "test_.*\\.cpp")
if (MINIAUDIO_INCLUDE_DIRS)
    message(STATUS "miniaudio: ENABLED (include: ${MINIAUDIO_INCLUDE_DIRS})")
else ()
    list(FILTER src EXCLUDE REGEX "PlaybackSink")
    message(STATUS "miniaudio: DISABLED (no local playback)")
endif ()
add_executable(mock_speaker ${src})
target_link_libraries(mock_speaker usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_speaker PRIVATE .)
if (MINIAUDIO_INCLUDE_DIRS)
    target_include_directories(mock_speaker PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
    target_compile_definitions(mock_speaker PRIVATE USBIPDCPP_USE_MINIAUDIO)
endif ()
