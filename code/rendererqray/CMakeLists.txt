cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(rendererqray LANGUAGES C CXX)
endif()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "rendererqray supports Windows and Linux (X11) only")
endif()

set(Q3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Quake 3 source root")
set(QUAKERAY_SDK_DIR "${Q3_ROOT}/code/quakeraysdk" CACHE PATH "QuakeRay SDK source directory")
set(QR_NVRHI_SOURCE_DIR "" CACHE PATH "NVRHI source directory")

if(NOT QUAKERAY_SDK_DIR OR NOT EXISTS "${QUAKERAY_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "QuakeRay SDK is missing at ${QUAKERAY_SDK_DIR}. "
        "Run `git submodule update --init --recursive`, or set QUAKERAY_SDK_DIR explicitly.")
endif()

if(NOT QR_NVRHI_SOURCE_DIR OR NOT EXISTS "${QR_NVRHI_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set QR_NVRHI_SOURCE_DIR to the NVRHI source tree")
endif()

# The SDK and NVRHI are static libraries linked into the renderer module, so on
# Linux they must be compiled as position-independent code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
target_include_directories(rendererqray PRIVATE
    "${Q3_ROOT}/code/renderercommon"
    "${Q3_ROOT}/code/qcommon"
    "${QUAKERAY_SDK_DIR}/Source/Stb"
)
target_link_libraries(rendererqray PRIVATE qray::renderer)

if(WIN32)
    target_compile_definitions(rendererqray PRIVATE
        VK_USE_PLATFORM_WIN32_KHR
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(rendererqray PRIVATE user32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(rendererqray PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    # Only GetRefAPI is exported; the SDK, NVRHI and KTX symbols stay private so
    # they cannot clash with the engine or other renderer modules.
    set_target_properties(rendererqray PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        # libamd_fidelityfx_vk.so is deployed next to the renderer module.
        BUILD_RPATH "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
    )
    target_link_options(rendererqray PRIVATE "LINKER:--no-undefined" "LINKER:--exclude-libs,ALL")
endif()

if(MSVC)
    target_compile_options(rendererqray PRIVATE /W3 /EHsc /utf-8 /MP)
else()
    target_compile_options(rendererqray PRIVATE -Wall -Wno-unused-function -Wno-missing-field-initializers)
endif()

# Module name the client loads: quake3e_qray_x86_64.dll / quake3e_qray_x86_64.so
set_target_properties(rendererqray PROPERTIES
    OUTPUT_NAME "quake3e_qray_x86_64"
    PREFIX ""
)

# Deploy the source-built FidelityFX provider next to the renderer module.
if(WIN32 AND DEFINED QR_FFX_VK_RUNTIME_DLL AND EXISTS "${QR_FFX_VK_RUNTIME_DLL}")
    add_custom_command(TARGET rendererqray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_VK_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:rendererqray>/amd_fidelityfx_vk.dll"
    )
elseif(NOT WIN32 AND DEFINED QR_FFX_VK_SO AND EXISTS "${QR_FFX_VK_SO}")
    add_custom_command(TARGET rendererqray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_VK_SO}"
            "$<TARGET_FILE_DIR:rendererqray>/libamd_fidelityfx_vk.so"
    )
endif()
