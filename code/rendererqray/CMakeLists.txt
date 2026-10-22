cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(rendererqray LANGUAGES C CXX)
endif()

set(Q3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Quake 3 source root")
set(QUAKERAY_SDK_DIR "" CACHE PATH "QuakeRay SDK source directory")
set(QR_NVRHI_SOURCE_DIR "" CACHE PATH "NVRHI source directory")

if(NOT QUAKERAY_SDK_DIR OR NOT EXISTS "${QUAKERAY_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set QUAKERAY_SDK_DIR to the QuakeRay SDK source tree")
endif()

if(NOT QR_NVRHI_SOURCE_DIR OR NOT EXISTS "${QR_NVRHI_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set QR_NVRHI_SOURCE_DIR to the NVRHI source tree")
endif()

find_package(Vulkan REQUIRED)

set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)

add_subdirectory("${QR_NVRHI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/nvrhi")
add_subdirectory("${QUAKERAY_SDK_DIR}" "${CMAKE_BINARY_DIR}/qray_sdk")

add_library(rendererqray SHARED
    qray_host.cpp
)

target_compile_features(rendererqray PRIVATE cxx_std_20)
target_compile_definitions(rendererqray PRIVATE
    QR_STATIC
    QR_USE_SURFACE_WIN32
    VK_USE_PLATFORM_WIN32_KHR
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
target_include_directories(rendererqray PRIVATE
    "${Q3_ROOT}/code/renderercommon"
    "${Q3_ROOT}/code/qcommon"
    "${QUAKERAY_SDK_DIR}/Source/Stb"
)
target_link_libraries(rendererqray PRIVATE
    renderer
    user32
)

if(MSVC)
    target_compile_options(rendererqray PRIVATE /W3 /EHsc /utf-8 /MP)
endif()

set_target_properties(rendererqray PROPERTIES
    OUTPUT_NAME "quake3e_qray_x86_64"
    PREFIX ""
)

if(WIN32 AND DEFINED QR_FFX_SDK_ROOT)
    add_custom_command(TARGET rendererqray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
            "$<TARGET_FILE_DIR:rendererqray>/amd_fidelityfx_vk.dll"
    )
endif()
