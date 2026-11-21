#!/usr/bin/env bash
#
# Build quake3e_qray_x86_64.so for Linux/X11.
#
#   QR_NVRHI_SOURCE_DIR=/path/to/nvrhi ./build_linux.sh [Debug|Release|RelWithDebInfo]
#
# Optional environment:
#   GLSLANG_VALIDATOR   glslangValidator used for the FidelityFX provider shaders
#   VULKAN_HEADERS_DIR  Vulkan headers (defaults to $VULKAN_SDK/include or /usr/include)
#   QR_BUILD_DIR        build directory (default: build/linux-<config>)
#   CMAKE_GENERATOR     e.g. Ninja
#
set -euo pipefail

config="${1:-Release}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk_dir="${QUAKERAY_SDK_DIR:-$script_dir/../quakeraysdk}"
build_dir="${QR_BUILD_DIR:-$script_dir/build/linux-${config,,}}"

if [[ -z "${QR_NVRHI_SOURCE_DIR:-}" || ! -f "$QR_NVRHI_SOURCE_DIR/CMakeLists.txt" ]]; then
    echo "error: set QR_NVRHI_SOURCE_DIR to the NVRHI source tree" >&2
    exit 1
fi
if [[ ! -f "$sdk_dir/CMakeLists.txt" ]]; then
    echo "error: QuakeRay SDK not found at $sdk_dir; run 'git submodule update --init --recursive'" >&2
    exit 1
fi

mkdir -p "$build_dir"
build_dir="$(cd "$build_dir" && pwd)"

# The SDK expects the provider at <build>/ffx/libamd_fidelityfx_vk.so by default.
ffx_so="$build_dir/ffx/libamd_fidelityfx_vk.so"
if [[ ! -f "$ffx_so" ]]; then
    echo "== Building FidelityFX Vulkan provider"
    QR_FFX_OUT_DIR="$build_dir/ffx" \
        "$sdk_dir/Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh"
fi

echo "== Configuring rendererqray ($config)"
extra_args=()
if [[ -n "${VULKAN_HEADERS_DIR:-}" ]]; then
    # NVRHI needs Vulkan headers >= 1.4.318; distro headers are often older.
    extra_args+=("-DVulkan_INCLUDE_DIR=$VULKAN_HEADERS_DIR")
fi
cmake -S "$script_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$config" \
    -DQR_NVRHI_SOURCE_DIR="$QR_NVRHI_SOURCE_DIR" \
    -DQUAKERAY_SDK_DIR="$sdk_dir" \
    -DQR_FFX_VK_SO="$ffx_so" \
    "${extra_args[@]}"

echo "== Building rendererqray"
cmake --build "$build_dir" --target rendererqray --parallel

echo "== Done: $build_dir/quake3e_qray_x86_64.so"
