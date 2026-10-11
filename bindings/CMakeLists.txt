find_package(pybind11 REQUIRED)

file(GLOB BINDING_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*.cpp
)

# Internal build target that actually produces the .pyd
pybind11_add_module(_usbipdcpp_py ${BINDING_SOURCES})

set_target_properties(_usbipdcpp_py PROPERTIES OUTPUT_NAME usbipdcpp)
set_target_properties(_usbipdcpp_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python_package
)

target_link_libraries(_usbipdcpp_py PRIVATE
    usbipdcpp
    usbipdcpp_virtual_device
)

target_include_directories(_usbipdcpp_py PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# Copy the required shared-library DLLs to python_package so the .pyd can be loaded directly
if(USBIPDCPP_BUILD_SHARED_LIBS AND WIN32)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.copy_dlls_done"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:usbipdcpp>"
            "${CMAKE_BINARY_DIR}/python_package"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:usbipdcpp_virtual_device>"
            "${CMAKE_BINARY_DIR}/python_package"
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/.copy_dlls_done"
        COMMENT "Copying DLLs to python_package"
        DEPENDS "$<TARGET_FILE:_usbipdcpp_py>" "$<TARGET_FILE:usbipdcpp>" "$<TARGET_FILE:usbipdcpp_virtual_device>"
    )
    add_custom_target(_copy_python_dlls DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.copy_dlls_done")
    set(STUBGEN_DEP "_copy_python_dlls")
else()
    set(STUBGEN_DEP "_usbipdcpp_py")
endif()

# Automatically generate .pyi stub files after the build to support IDE code completion
find_program(PYBIND11_STUBGEN_EXE pybind11-stubgen)
if(PYBIND11_STUBGEN_EXE)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.stubgen_done"
        COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=${CMAKE_BINARY_DIR}/python_package"
                ${PYBIND11_STUBGEN_EXE} usbipdcpp
                -o "${CMAKE_BINARY_DIR}/python_package"
                --ignore-all-errors
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/.stubgen_done"
        COMMENT "Generating .pyi stubs with pybind11-stubgen"
        DEPENDS ${STUBGEN_DEP}
    )
    add_custom_target(_python_stubgen DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.stubgen_done")
    set(USBPY_FINAL_DEP _python_stubgen)
else()
    if(USBIPDCPP_BUILD_SHARED_LIBS AND WIN32)
        set(USBPY_FINAL_DEP _copy_python_dlls)
    else()
        set(USBPY_FINAL_DEP _usbipdcpp_py)
    endif()
    message(WARNING "pybind11-stubgen not found, .pyi stubs will not be generated. "
                    "Install with: pip install pybind11-stubgen")
endif()

# Public target: building --target usbipdcpp_python triggers the entire chain
add_custom_target(usbipdcpp_python DEPENDS ${USBPY_FINAL_DEP})
