# rendererqray: curve shading investigation

Status: investigation only. No renderer code was changed.

Scope: retail `q3dm1.bsp` (BSP version 46) and the diagnostic captures from
2026-10-23, including `cull0.bmp`, `cull1.bmp`, and `cull_ref.bmp`.

## Code path

- `MST_PATCH` surfaces are handled at `code/rendererqray/qray_host.cpp:3184`
  and leave the loop at `:3245`.
- Patch vertices receive a placeholder normal `(0, 0, 1)` (`:3202`), then the
  whole batch is uploaded with `QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT`
  (`:3377`). The SDK replaces the placeholder with a flat per-triangle normal
  (`QuakeRaySDK/Source/Shaders/VertexPreprocessPartial.hlsli:60-93`).
- The first-triangle cross product at `qray_host.cpp:3267` and the winding
  flip at `:3340-3353` are **not executed for patches** because of the early
  `continue` at `:3245`. They apply to planar and triangle-soup surfaces.
- Patch indices (`:3225-3232`) are intentionally the reverse of Q3's
  `(i0,i2,i1)/(i1,i2,i3)` order, matching the flipped planar/soup convention.
- Q3 itself does not use the BSP control-point normals for patch rendering:
  `R_SubdividePatchToGrid` recomputes them with `MakeMeshNormals`
  (`code/renderer/tr_curve.c:513-516`).

## Measured facts

### Surface inventory

`q3dm1.bsp` contains 2097 surfaces:

- 1942 `MST_PLANAR`
- 113 `MST_PATCH`
- 42 `MST_TRIANGLE_SOUP`

16 planar sky surfaces are skipped; 2081 surfaces are uploaded. All 113
patches are uploaded. Patch dimensions are mostly 3x3, with 5x3, 9x3, 5x5,
9x5, 3x7, and 9x7 variants present.

### Patch normals

- 1917 control-point normals: all unit length, zero zero-length normals.
- Control-point normals vs analytic Bezier normal: mean dot 0.970, only 2 of
  1379 comparable points inverted. Both are on `textures/skin/tongue_trans`
  at degenerate/pole positions.
- 134208 tessellated patch triangles: all 134208 have a positive dot with the
  analytic Bezier normal. Mean dot 0.99985, minimum 0.482, inverted count 0.
- Q3 smooth normals vs qray flat triangle normals: mean angle 0.85 degrees,
  p95 1.45 degrees, maximum 46.97 degrees at one degenerate edge.

Conclusion: patches are not systematically inside-out on q3dm1. The visible
difference is flat versus smooth shading, not a flipped normal.

### Winding and culling

- All 1942 planar surfaces have their raw first-triangle winding opposite the
  BSP plane normal. The host flips them; one surface (surface 312,
  `textures/gothic_trim/pitted_rust3`, first-triangle dot +1.0) is left as-is.
  This matches the log line `planarWindingKept=1`.
- Triangle soups are decorative props, not architectural curves. 2644 of 2658
  raw soup triangles are wound opposite their authored normals; the host flips
  them. Authored per-vertex normals are not preserved in the default
  `GENERATE_NORMALS` path.
- The saved culling pair differs by 6726 pixels (0.855% of 1024x768); 6137 of
  those pixels are brighter with culling enabled. The largest connected
  difference is a 2944-pixel region in the upper-right. This is not the
  signature of whole walls disappearing.
- The 2026-10-23 11:55 test frame was captured with `cullBackFaces=0`.

### Lighting and materials

- Every one of the 113 patches has a valid lightmap (`lightmapNum >= 0`,
  non-zero lightmap area).
- The host does not read `LUMP_LIGHTMAPS` or `lightmapNum`; `lightmapVecs[2]`
  is only used as the planar plane normal. World lighting comes from the sun
  and map light entities, not baked lightmaps.
- In the test view (`setviewpos 212 2360 82 315`), 19 patch centers are in the
  frustum. They use opaque shaders:
  - 8x `textures/gothic_door/skullarch_c`
  - 4x `textures/gothic_door/km_arena1archfinald_bot`
  - 4x `textures/gothic_door/xian_tourneyarch_inside2`
  - 2x `textures/gothic_trim/skullsvertgray02b`
  - 1x `textures/gothic_door/skull_door_b`
- No `cull disable` was found for the patch shaders that have shader scripts.
- `textures/skin/surface8_trans` and `textures/skin/tongue_trans` are
  `surfaceparm nonsolid`, not alpha-blended transparency. Their scripts use a
  lightmap stage plus a `GL_DST_COLOR GL_ZERO` texture stage.
- `CreateMaterialForShader` loads the raw shader name instead of resolving the
  shader definition's `map` stage. `surface8_trans` and `tongue_trans` have no
  raw texture under that name, so 10 patches (8 + 2) fall back to the default
  material. These patches are not in the test view and are not the dark arches.

### Screen coverage

Projecting the actual patch and soup geometry for the test camera (no
occlusion/z-buffer) gives:

- patch coverage: 156687 pixels, 19.92% of the frame
- soup coverage: 9691 pixels, 1.23%
- 60% of patch-covered pixels are dark (`luminance < 8`)
- up to 40.3% of all dark pixels lie under patch coverage

The last number is an upper bound because the projection does not account for
occlusion. It shows that curves can contribute to the dark frame, but does not
support the inverted-normal explanation.

## Open items

- Capture `qray_normalMode` 0/1/2/3/4 A/B frames at the same view; the existing
  `nm_auto.cfg` was not run to completion and `nm0..nm3` are absent.
- Separate missing baked lighting from material errors by varying
  `qray_lightScale` and `qray_sunIntensity` at the test view, then comparing the
  dark-pixel mask.
- If smooth curves are required, reproduce Q3's `MakeMeshNormals` on the
  tessellated patch grid and upload with exact normals, instead of relying on
  the SDK's flat per-face normals.
- Resolve world materials through the shader definition map rather than
  `LoadQ3Texture(shaderName)`; this also avoids the `surface8_trans` /
  `tongue_trans` default-material fallback.
