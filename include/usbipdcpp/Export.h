#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
    // If it is a compiled static library
    #ifdef USBIPDCPP_STATIC
        #define USBIPDCPP_API
    // If not defined, it is a compiled dynamic library
    // Determines whether the library is being compiled or used based on whether USBIPDCPP_EXPORTS is defined
    #else
        #ifdef USBIPDCPP_EXPORTS
            #define USBIPDCPP_API __declspec(dllexport)
        #else
            #define USBIPDCPP_API __declspec(dllimport)
        #endif
    #endif
#else
    #define USBIPDCPP_API __attribute__((visibility("default")))
#endif
