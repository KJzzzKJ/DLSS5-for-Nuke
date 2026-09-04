cmake_minimum_required(VERSION 3.16)
project(DLSS_Nuke_Worker CXX)

set(CMAKE_CXX_STANDARD 17)

# Windows only
if(NOT WIN32)
    message(FATAL_ERROR "Worker only supports Windows (D3D12 + DLSS)")
endif()

# 1. Main Worker Executable
add_executable(DLSS_Nuke_Worker WIN32
    main.cpp
    DLSSWorker.cpp
    DISOpticalFlow.cpp
)

# Switch to CONSOLE subsystem for stdin/stdout communication with Nuke
set_target_properties(DLSS_Nuke_Worker PROPERTIES
    WIN32_EXECUTABLE FALSE
)

target_compile_definitions(DLSS_Nuke_Worker PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE
    UNICODE
)

target_link_libraries(DLSS_Nuke_Worker PRIVATE
    d3d12.lib
    dxgi.lib
    dxguid.lib
)

# 2. Native Caller Shim DLL (named nvngx.dll to satisfy NGX snippet return address check)
add_library(nvngx SHARED
    caller_shim.cpp
)

set_target_properties(nvngx PROPERTIES
    OUTPUT_NAME "nvngx"
    PREFIX ""
)

target_compile_definitions(nvngx PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(nvngx PRIVATE
    d3d12.lib
)

# Release optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(DLSS_Nuke_Worker PRIVATE /O2 /GL)
    target_link_options(DLSS_Nuke_Worker PRIVATE /LTCG)
    target_compile_options(nvngx PRIVATE /O2)
endif()

install(TARGETS DLSS_Nuke_Worker nvngx
        RUNTIME DESTINATION .)
