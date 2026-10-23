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
  for model assets that reference a missing `.tga`, plus Q3 `.shader` script
  parsing for world, model and item materials.
- Shader stages drive the qray blend state: `blendFunc` factor pairs and
  mnemonics map onto `QrBlendFactor`, so additive effects, alpha blending and
  the `GL_ZERO`/`GL_ONE_MINUS_SRC_COLOR` darken blend used by impact marks all
  keep their authored behaviour. Darken-blend decals are converted into a black
  texture whose alpha carries the source luminance, because a layer with alpha in
  slot 0 is treated as opaque by the qray compositor.
- `alphaFunc` stages upload their surfaces into the qray alpha-tested hit group,
  so grate and fence cut-outs are seen through by camera, shadow and reflection
  rays instead of rendering as solid quads.
- Configurable directional sun and Quake 3 `light` entity upload as spherical
  lights, additionally aggregated into per-cluster light lists from the BSP
  cluster tree and PVS so direct lighting only samples the lights that can reach
  the hit point.
- MD3 model loading, frame interpolation, tags, bounds, model skins and dynamic
  scene geometry for world entities and first-person view models. Surfaces whose
  shader stage is a `tcGen environment` pass get rebuilt sphere-map texcoords; both the
  `ONE/ZERO` bodies and the additive shells run unlit on the raster overlay by default,
  see `qray_envMapRaster` and `qray_envShellTrace`.
- Forced-geometry rendering for sprites, polygon effects, beams, rail cores and
  lightning.
- Rasterized 2D overlay for Q3 UI/HUD assets and a bigchars-based fallback font.
- Asynchronous final-frame readback and RGB24 conversion for Quake 3 AVI capture.
- Full `refexport_t` wiring, with explicit no-op stubs for features that are not
  implemented yet.

Not implemented yet: IQM/MDR models, BSP inline models (bmodels), mirrors and
portals, `surfaceparm trans` blend stages on world geometry, lightmaps/DTAL
relighting, authored PBR overrides, FSR/DLSS selection and automatic fallback.
Those are follow-up stages.

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
  default `200`.
- `qray_lightRadius`: emission sphere radius of a Quake 3 map light in Quake
  units, default `16`. Small values behave like point lights.
- `qray_lightTopUp`: distance in Quake units within which a cluster also
  receives lights its own PVS row does not list, default `1024`.
- `qray_lightRadiusScale`: radius scale applied to Quake 3 map light entities,
  default `1.0`.
- `qray_alphaTest`: set to `0` to upload `alphaFunc` surfaces as opaque geometry,
  default `1`. Useful for A/B comparison of cut-out behaviour.
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

## Sky and sun

Retail Quake 3 bakes the `q3map_sun` directive of the sky shader into the lightmaps, so an RT
renderer has to place the sun itself. The sidecar now parses `q3map_sun` / `q3map_sunExt`
(`<r> <g> <b> <intensity> <degrees> <elevation>`) from the sky shader of the loaded map and
drives the directional light with it. `intensity` is normalized against the value retail maps
use most (80) and scaled by `qray_sunIntensity`; `qray_sunColor` still overrides the color and
must be empty (the default) to keep the map values.

The retail sky textures carry no sun disc, because the sun only ever existed in the lightmaps.
The sidecar therefore burns one into the sky atlas: a saturated core of about one degree with a
seven degree glow, placed on the direction taken from the same `q3map_sun` that feeds the
directional light, so light and disc can not drift apart. Skies without `q3map_sun` get no disc.

QuakeRay only samples the sky from the cubemap its own sky pass rasterizes, which a sidecar
cannot feed, so `DrawFrame` reported `QR_SKY_TYPE_CUBEMAP` and the traced pass fell through to
`skyColorDefault`. The composed sky faces are therefore also packed into a single atlas and
drawn as six inward facing quads at `qray_skyboxRadius`, which makes the sky visible without
touching the SDK.

| Cvar | Default | Description |
| --- | --- | --- |
| `qray_skybox` | `1` | Draw the map sky as rasterized geometry. |
| `qray_skyboxRadius` | `2000` | Distance of the sky box in map units, must stay inside the far plane. |

## Model environment stages

Pickups are built from a solid body plus a glass shell, and both are `tcGen environment` maps
that only differ in their blend factors. The texcoords are rebuilt per vertex as a `GL_SPHERE_MAP`
of the eye reflection vector for either flavour, and the authored factors decide how the surface is
drawn:

- `blendFunc GL_ONE GL_ONE` is the glass shell of the health and armour spheres. Additive over the
  frame gives the see-through bubble that the pickup artwork expects, so it runs unlit on the raster
  overlay.
- No `blendFunc` (the commented out one of `models/powerups/health/yellow`, which Q3 defaults to
  `GL_ONE GL_ZERO`) is the body under that shell. `ONE/ZERO` replaces what is under it, so the stage
  is the surface itself: it becomes the traced material albedo and the map lighting dims it exactly
  like every other model surface.

The `ONE/ZERO` body keeps `envmapyel` as its albedo, which is why the medkit reads as yellow chrome.
Both environment stages are composited on the raster overlay, like the fixed function pipeline did:
traced, the baked map is dimmed by the map lighting and the additive shell then washes the body out.

The HDR pipeline still does not reproduce the 8 bit blend for free, so both stages are adjusted
before they reach the overlay. The shell is added into an unclamped buffer where the retail blend
clamps, which makes it far too bright, and the baked body map reads too dark and too desaturated once
it sits under that shell. `qray_envShellScale`, `qray_envMapScale` and `qray_envMapSaturation` scale
the two textures to match the retail pickup colours; defaults are tuned against the retail medkit and
should be a good starting point for the other powerups.

| Cvar | Default | Description |
| --- | --- | --- |
| `qray_envMapRaster` | `1` | Draw opaque environment stages on the unlit overlay; `0` uploads them as traced surfaces. |
| `qray_envMapScale` | `1.15` | Brightness of the opaque environment stages. |
| `qray_envMapSaturation` | `1.6` | Saturation of the opaque environment stages, the additive shell desaturates the bodies. |
| `qray_envShellScale` | `0.1` | Brightness of the additive glass shells. |
| `qray_envShellTrace` | `0` | `1` traces the shells instead of rasterizing them, `2` skips them for A/B comparison. |

## Tone mapping and exposure

Quake 3 was authored for a fixed 8 bit exposure, while QuakeRay renders an HDR image. The host has to
supply the tonemap parameters explicitly, because the SDK otherwise clips every highlight at a white
point of one.

| Cvar | Default | Description |
| --- | --- | --- |
| `qray_tonemapType` | `4` | Operator: `0` none, `1` Reinhard, `2` extended Reinhard, `3` ACES, `4` AgX. |
| `qray_tonemapPower` | `0.9` | Blend towards the operator; `0` keeps the filmic curve alone. |
| `qray_whitePoint` | `10.0` | Luminance mapped to white, higher values keep more highlights. |
| `qray_exposureBias` | `-2.5` | Exposure bias in stops; `-2.5` matches the baked Quake 3 brightness. |
| `qray_eyeAdaptation` | `0` | Let QuakeRay adapt exposure to the frame. Retail had a fixed exposure, and adaptation also makes measurements drift between frames, so it is off by default. |


## Effect blending

Impact marks in `baseq3` are frequently plain images without a shader script, and several of them
are black-background glow maps stored as JPEG. Treating such an image as an alpha blended decal
turns its opaque black border into a solid card, so a mark (or sprite) whose texture has no alpha
channel is blended additively instead. `GL_ZERO GL_ONE_MINUS_SRC_COLOR` darken decals ignore the
vertex color and darken by their image, which is what the retail shadow blobs expect.
