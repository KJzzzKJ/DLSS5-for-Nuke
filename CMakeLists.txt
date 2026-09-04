cmake_minimum_required(VERSION 3.20)
project(DLSS5Live VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Option to also build worker executable
option(BUILD_DLSS_WORKER "Build DLSS worker process" OFF)

if(NOT DEFINED NUKE_INSTALL_DIR)
    set(NUKE_INSTALL_DIR "C:/Program Files/Nuke15.0v2")
endif()

if(NOT DEFINED NUKE_INCLUDE_DIR)
    set(NUKE_INCLUDE_DIR "${NUKE_INSTALL_DIR}/include")
endif()

if(NOT DEFINED NUKE_LIB_DIR)
    set(NUKE_LIB_DIR "${NUKE_INSTALL_DIR}")
endif()

add_library(DLSS5Live SHARED
    src/DLSS5Live.cpp
    src/DLSS5Live.h
    src/WorkerBridge.cpp
    src/WorkerBridge.h
)

set_target_properties(DLSS5Live PROPERTIES PREFIX "")
set_target_properties(DLSS5Live PROPERTIES SUFFIX ".dll")

target_include_directories(DLSS5Live PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${NUKE_INCLUDE_DIR}"
)

target_compile_definitions(DLSS5Live PRIVATE
    DLSS5_VERSION_STRING="v${PROJECT_VERSION}"
    WIN32
    _WINDOWS
    NOMINMAX
    _USE_MATH_DEFINES
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS
    NDEBUG
    _USRDLL
    _EXPORT=__declspec(dllexport)
)

target_link_directories(DLSS5Live PRIVATE "${NUKE_LIB_DIR}")
target_link_libraries(DLSS5Live PRIVATE DDImage)

if(MSVC)
    target_compile_options(DLSS5Live PRIVATE /wd4251 /wd4275 /MD /Zi)
    target_link_options(DLSS5Live PRIVATE /DEBUG)
endif()

if(BUILD_DLSS_WORKER)
    add_subdirectory(worker)
endif()
