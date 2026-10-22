# rendererqray

`rendererqray` is the Quake 3 sidecar renderer that hosts the QuakeRay SDK. It is a
separate Windows DLL (`quake3e_qray_x86_64.dll`) loaded through `cl_renderer qray`.
The QuakeRay SDK continues to own the Vulkan instance, device, surface, swapchain
and presentation path; Quake 3 only supplies the native window handle.

## Current scope

This tree implements the first integration stage:

- `GetWindowHandle` in `refimport_t` and Win32 window export from `win_glimp.c`.
- Correct qray instance lifetime across `REF_KEEP_CONTEXT`, `REF_KEEP_WINDOW` and
  window destruction.
- World geometry upload for Quake 3 BSP planar, triangle-soup and Bezier patch
  surfaces.
- Direct Q3 texture loading into qray materials, with `.jpg`/`.png` fallback
  for model assets that reference a missing `.tga`, plus basic Q3 `.shader`
  script parsing for model and item materials.
- Configurable directional sun and Quake 3 `light` entity upload as spherical
  lights.
- MD3 model loading, frame interpolation, tags, bounds, model skins and dynamic
  scene geometry for world entities and first-person view models.
- Forced-geometry rendering for sprites, polygon effects, beams, rail cores and
  lightning.
- Rasterized 2D overlay for Q3 UI/HUD assets and a bigchars-based fallback font.
- Asynchronous final-frame readback and RGB24 conversion for Quake 3 AVI capture.
- Full `refexport_t` wiring, with explicit no-op stubs for features that are not
  implemented yet.

Not implemented yet: IQM/MDR models, BSP inline models (bmodels), mirrors and
portals, full multi-stage shader effects, lightmaps/DTAL relighting, authored PBR
overrides, FSR/DLSS selection and automatic fallback. Those are follow-up stages.

## Build

Requirements:

- Windows x64 and Visual Studio 2022 (MSVC).
- Vulkan SDK 1.4.318 or newer.
- NVRHI source tree.
- QuakeRaySDK source at API `1.04.0000` or newer, with its FidelityFX SDK
  dependency present at `Source/FidelityFX_SDK_1.1.4`.

Configure and build from this directory:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DQUAKERAY_SDK_DIR="C:/path/to/QuakeRaySDK" `
  -DQR_NVRHI_SOURCE_DIR="C:/path/to/nvrhi"
cmake --build build --config Debug --target rendererqray
```

`QR_FFX_SDK_ROOT` can be passed explicitly if the SDK uses a different
FidelityFX location. The post-build step copies `amd_fidelityfx_vk.dll` next to
the renderer DLL.

The renderer DLL is built at `build/Debug/quake3e_qray_x86_64.dll`. Copy it,
`amd_fidelityfx_vk.dll`, and the Quake 3 client into the same directory.

## Runtime assets

The host takes all QuakeRay assets from cvars, so the shader build can either be
shipped in a game directory or referenced from a development checkout:

- `qray_shaderDir`: directory containing the compiled `.spv` files. It must end
  with a slash. The shaders must be regenerated from the matching QuakeRaySDK
  source with `Source/Shaders/GenerateShaders.py`; stale shader sets can crash
  the Vulkan driver during pipeline creation.
- `qray_blueNoise`: blue-noise PNG used by QuakeRay.
- `qray_waterNormal`: optional water normal PNG.
- `qray_config`: optional QuakeRay settings file.
- `qray_sunColor`: directional sun RGB, default `1.0 0.95 0.85`.
- `qray_sunIntensity`: directional sun intensity, default `3.0`.
- `qray_lightScale`: radiometric scale applied to Quake 3 map light entities,
  default `1600`.
- `qray_lightRadiusScale`: radius scale applied to Quake 3 map light entities,
  default `1.0`.
- `qray_debugLog`: set to `1` to write host diagnostics to `qray_host.log` in
  the system temporary directory.

Example development launch, after copying the client and renderer DLLs into a
directory whose `baseq3` points at a retail Quake 3 installation:

```powershell
.\quake3e.x64.exe +set cl_renderer qray `
  +set qray_shaderDir "C:/path/to/QuakeRaySDK/Build/" `
  +set qray_blueNoise "C:/path/to/QuakeRaySDK/gfx/textures/BlueNoise_LDR_RGBA_128.png" `
  +set qray_waterNormal "C:/path/to/QuakeRaySDK/gfx/textures/WaterNormal_n.png" `
  +set r_fullscreen 0 +map q3dm1
```

## Lifetime contract

The qray instance is destroyed before `VKimp_Shutdown` destroys the Win32 window.
`REF_KEEP_CONTEXT` keeps the qray instance and the loaded world alive because the
window and Vulkan surface remain valid. `REF_KEEP_WINDOW`, `REF_DESTROY_WINDOW`
and `REF_UNLOAD_DLL` destroy the qray instance first, then release the window or
DLL through the Quake 3 platform layer.

## Readback

Readback uses the asynchronous QuakeRay API added in API `1.04.0000`:

- `qrGetFrameReadbackRequirements`
- `qrRequestFrameReadback`
- `qrGetFrameReadbackStatus`

The host requests RGBA8 with UI included, never calls `vkDeviceWaitIdle` in the
frame loop, and converts completed frames to RGB24 for `RE_TakeVideoFrame`.
Resize and minimize failures reset the cached requirements on the next frame.

The console command `qray_readback [file.bmp]` writes the latest completed
readback frame as a 32-bit BMP. `screenshot [file.png]` uses the SDK screenshot
request path instead. With the same frame on screen, the BMP and PNG should match
apart from timing and HUD animation.
