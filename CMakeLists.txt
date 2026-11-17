cmake_minimum_required(VERSION 3.24)
project(SkyrimPainter VERSION 0.9.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- CommonLibSSE-NG (git submodule: alandtse/CommonLibVR, branch ng - same pin as Hearthframe)
# Two downloads from the same code: SE / AE (default), or Skyrim VR (-DSKYRIMPAINTER_VR=ON). No unit tests.
option(SKYRIMPAINTER_VR "Build the Skyrim VR version" OFF)
if (SKYRIMPAINTER_VR)
    set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
else()
    set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(COMMONLIB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

# ---- other dependencies (vcpkg)
find_package(imgui CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "evodube"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp src/Art.cpp src/Dds.cpp src/Items.cpp src/Menus.cpp src/Overlay.cpp src/Paint.cpp src/Place.cpp
            src/Spectral.cpp src/Store.cpp src/Studio.cpp src/Theme.cpp src/Live.cpp src/VR.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE src ${STB_INCLUDE_DIRS})
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if (SKYRIMPAINTER_VR)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SKYRIMPAINTER_VR=1)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 dxgi dxguid ole32 shell32 uuid)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /permissive- /Zc:preprocessor)
    target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
endif()
