# Experimental XeSS Super Resolution implementation

Project Helghast has an opt-in experimental presentation path built around Intel XeSS Super Resolution (XeSS-SR).
It stages the unmodified `libxess.dll` from Intel XeSS SDK 3.0.2 and uses the SR API reported by that runtime,
`2.0.2`. This is an **ENHANCEMENT**: the PS3 game has no XeSS path, and Bilinear remains the default filter.

In brief, the implementation:

- keeps the game rendering its original 1280x720 scene;
- identifies the game's primary deferred pass and full-resolution depth;
- applies XeSS jitter to scene geometry;
- reconstructs camera motion from the current and previous camera poses plus depth;
- compares coherent opaque and final scene captures to generate XeSS's responsive-pixel mask;
- dispatches XeSS-SR into the selected larger output resolution; and
- falls back to the existing Bilinear presentation path if any prerequisite fails.

The current implementation is Super Resolution only. It does not include XeSS Frame Generation or Xe Low Latency.
Moving objects do not yet have their own motion vectors, and the final scene, post-processing, and UI still share
one color input.

## Enabling it

Use **Options > Display > Scaling filter > XeSS-SR (experimental)** and accept the warning. The selection is stored
in `display.ini`:

```ini
[display]
filter = xess
```

For one run without changing the saved setting:

```powershell
$env:KZ3_DISPLAY_FILTER = "xess"
```

XeSS-SR is attempted only when the output is larger than 1280x720 and has the same aspect ratio. A 1920x1080
output is the primary test case. The runtime chooses a XeSS quality mode from the requested scale; there is no
separate quality selector yet.

## Architecture

The Intel ABI and Killzone 3 hooks are kept outside the general ps3recomp runtime. The KZ3 target defines
`KZ3_XESS` and supplies a port-owned adapter. Other ps3recomp users do not gain an Intel runtime dependency.

```text
Recompiled game camera hooks
  -> current pose, effective FOV, guest flip and view ID
  -> validated near/far planes from the primary scene projection
  -> port-owned camera bridge

RSX draw replay
  -> identify the full-resolution four-target deferred pass
  -> remember and jitter its depth-tested geometry
  -> jitter later depth-tested draws into the scene target
  -> capture the opaque deferred resolve
  -> capture the scene immediately before temporal presentation

Presentation
  -> use the deferred R32_FLOAT depth surface
  -> generate R16G16_FLOAT current-to-previous NDC motion
  -> generate an R8_UNORM responsive-pixel mask
  -> dispatch XeSS-SR into an RGBA8 output
  -> apply scan-out gamma and the existing display blit
  -> present

Any missing, stale, unsupported, or incoherent input
  -> log the reason
  -> restore the normal resource states
  -> use Bilinear presentation
```

### Components

| Component | Role |
|---|---|
| [`port/kz3/xess.cpp`](../port/kz3/xess.cpp) | Secure runtime loading, exact API validation, XeSS context and resource ownership, motion and responsive-mask compute passes, scene captures, and dispatch |
| [`port/kz3/xess_api.h`](../port/kz3/xess_api.h) | Minimal independently authored XeSS-SR 2.0 ABI declarations used for dynamic loading |
| [`port/kz3/xess.h`](../port/kz3/xess.h) | C boundary between the KZ3 adapter and the patched C renderer |
| [`port/kz3/xess_motion.h`](../port/kz3/xess_motion.h) | Platform-independent depth reconstruction and camera reprojection |
| [`port/kz3/xess_inputs.h`](../port/kz3/xess_inputs.h) | Pure deferred-pass, scene-resolve, jitter, and capture-pair selection rules |
| [`port/kz3/main.cpp`](../port/kz3/main.cpp) | Camera, FOV, and projection-plane publication |
| [`port/patches/2139-xess-super-resolution.patch`](../port/patches/2139-xess-super-resolution.patch) | Renderer integration, input selection, presentation ordering, UI, and fallback |
| [`tools/release/stage_xess.py`](../tools/release/stage_xess.py) | Release-time validation and staging of Intel's unmodified runtime and required notices |
| [`tools/smoke/test_xess.cpp`](../tools/smoke/test_xess.cpp) | Native D3D12 context, dispatch, responsive-mask, readback, and fallback smoke test |

## Frame pipeline

### 1. Camera and projection publication

The camera hook publishes the final interpolated pose after the port's camera adjustments. The pose stores the
right, forward, and up vectors followed by position. Horizontal and vertical FOV values include the port's
widescreen adjustment.

Pose, FOV, projection planes, guest flip, and view ID are published atomically only after both camera and projection
data for that guest flip are available. Until then, the last coherent snapshot remains visible. The renderer
snapshots that state when it reaches the primary deferred draw. This avoids both a camera/projection publication
race and any assumption that the PPU flip counter and the renderer's FIFO frame counter have a fixed offset.

The primary 1.14 scene caller of `Camera::vf17` supplies the near and far values used for depth reconstruction. An
observed campaign frame used near `0.2` and far `2048`. A different projection path using `0.1` and `512` is ignored.
The equivalent 1.00 caller is mapped but has not received the same runtime validation.

Camera history resets when:

- there is no coherent immediately previous guest flip;
- the view ID changes;
- more than one view appears in one guest flip;
- the position changes by more than 5 guest units; or
- the camera rotates by more than 1.5 radians.

The two numeric cut thresholds are experimental adapter policy, not recovered gameplay constants.

### 2. Deferred input selection

The stable full-resolution deferred pass contains four color targets and one primary depth target:

| Local offset | Validated use |
|---|---|
| `0x02200000` | member of the four-target deferred MRT set; channel semantics remain unidentified |
| `0x01E40000` | deferred MRT and source sampled by the first scene-target resolve |
| `0x016C0000` | packed view normals |
| `0x01A80000` | member of the deferred MRT set; channel semantics remain unidentified |
| zeta `0x025C0000` | primary full-resolution depth |

No unidentified G-buffer target is treated as velocity. Slot number and texture format do not establish motion
direction, units, or jitter convention.

The renderer selects the primary deferred pass structurally: a local full-render-size target, full viewport, four
or more color targets, a bound depth surface, and depth testing. It records that depth for the current renderer
frame. The same XeSS jitter is applied to the primary pass and to later depth-tested draws into the validated scene
target that reuse that depth.

Jitter starts only after the runtime, context, camera, projection, and output size are ready. A missing or rejected
XeSS runtime therefore cannot leave a visibly jittered frame before Bilinear fallback.

### 3. Motion vectors

The adapter generates `R16G16_FLOAT` motion at input resolution. For each pixel it:

1. removes the current XeSS jitter from pixel NDC;
2. reconstructs view-space position from normal depth, near/far planes, and current FOV;
3. transforms the point through the current camera pose into world space;
4. projects that point into the previous camera pose; and
5. writes `previousNdc - currentNdc`.

This matches the current-to-previous NDC velocity convention requested through
`XESS_INIT_FLAG_USE_NDC_VELOCITY`. Jitter is excluded from motion.

Depth makes camera translation spatially correct: nearby geometry moves farther than distant geometry. Camera
rotation and translation are represented. Object, skeletal, vegetation, particle, and shader-driven motion are
not. Those pixels currently receive camera motion only.

### 4. Responsive-pixel mask

The game resolves deferred lighting into the local scene target at `0x01300000`, then adds translucent effects and
other forward composition before presentation. The adapter captures:

- the first validated opaque lighting resolve; and
- the final same-resource scene color immediately before temporal presentation.

The pair is accepted only when frame number, resource identity, dimensions, and XeSS context generation match.
A compute pass writes an input-resolution `R8_UNORM` responsive-pixel mask:

```text
max(abs(final.rgb - opaque.rgb)) >= 0.2 ? 0.8 : 0.0
```

The threshold and mask value are an experimental heuristic, not game-derived transparency semantics.
This marks composition that changed after the opaque resolve so XeSS favors the current frame there. If the pair is
missing or stale, the mask is cleared instead of reusing old data.

### 5. XeSS dispatch and presentation

The adapter initializes XeSS with:

- responsive-pixel-mask input;
- NDC velocity;
- LDR input color; and
- fixed exposure scale `1.0`.

The color and output are both `R8G8B8A8_UNORM`. Depth is `R32_FLOAT`; motion is `R16G16_FLOAT`; the responsive mask
has a float-readable R channel. Input resources enter XeSS in
`D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`; output enters in
`D3D12_RESOURCE_STATE_UNORDERED_ACCESS`.

After a successful dispatch, the output enters the renderer's normal presentation sequence. The renderer applies
the existing scan-out gamma and display rectangle logic. Bilinear and Nearest behavior is unchanged when XeSS is
not selected.

The initial quality policy is:

| Output/input scale | XeSS quality |
|---:|---|
| `1.0` | Native AA |
| up to `1.3` | Ultra Quality Plus |
| up to `1.5` | Ultra Quality |
| up to `1.7` | Quality |
| up to `2.0` | Balanced |
| up to `2.3` | Performance |
| up to `3.0` | Ultra Performance |

Before initialization, the adapter asks XeSS for the minimum, optimal, and maximum input resolution for the chosen
output and quality. It rejects the context if the game's actual input is outside that range.

## Runtime loading and redistribution

The adapter does not use Intel's import library. It dynamically resolves only the XeSS-SR exports it needs and
loads `libxess.dll` only from beside `kz3.exe`, with Windows system dependencies restricted to System32.

The SDK package version is 3.0.2, while the runtime's exported XeSS-SR API and Windows file version are 2.0.2. The
adapter requires exactly API 2.0.2 and fails closed on a different ABI.

Release staging pins these official Intel files:

| File | Bytes | SHA-256 |
|---|---:|---|
| `libxess.dll` | 77,795,704 | `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7` |
| `LICENSE.txt` | 4,194 | `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df` |
| `third-party-programs.txt` | 35,788 | `e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c` |

The release workflow also pins the SDK archive SHA-256 and verifies the runtime's Intel Authenticode signature. The
DLL is redistributed unmodified with Intel's licence and third-party notices. Intel's SDK headers are not bundled;
the small ABI declarations in `xess_api.h` were independently authored and their layouts checked against the
official headers.

XeSS-SR does not contact the network.

## Fallback behavior

Selecting XeSS never makes it a startup requirement. The renderer uses Bilinear for the frame and logs the reason
when any of these conditions applies:

- `libxess.dll` is missing, invalid, or exposes a different API;
- D3D12 or the current driver cannot create an XeSS context;
- input and output do not share an aspect ratio;
- output is not larger, or exceeds the supported 3x scale;
- XeSS rejects the actual input resolution for the selected quality;
- the deferred depth, camera snapshot, projection, or target is stale;
- resources have incompatible dimensions or formats; or
- motion generation, responsive-mask generation, or XeSS dispatch fails.

The Options page warns before enabling the experimental path. Cancel restores the currently active filter.

## Validation

Validation that needs no game data:

- the adapter compiles as C++20 with strict warnings;
- local XeSS ABI sizes and offsets match Intel's official 2.0 headers;
- the official runtime reports XeSS-SR API 2.0.2;
- motion tests cover identity, rotation, depth-aware translation, FOV changes, and invalid projection data;
- input tests cover deferred-pass selection, jitter eligibility, opaque resolve selection, and capture coherence;
- display tests cover parsing, `display.ini`, the options row, warning/cancel behavior, reset, and no unsupported
  sharpness control;
- release tests cover exact hashes, AMD64 PE validation, required notices, payload assembly, portable ZIP inclusion,
  and launcher placement; and
- the native D3D12 smoke test dispatches 1280x720 to 1920x1080, uses the responsive-mask path, reads back a non-empty
  output, and verifies missing, wrong, and unsupported-aspect fallback.

Native Windows gameplay validation on 2026-10-31 used the hardware D3D12 path on an NVIDIA RTX 5070, a 1280x720
game image, a 1920x1080 Fit output, and `KZ3_DISPLAY_FILTER=xess`. The log showed:

- XeSS-SR API 2.0.2 dispatching 1280x720 to 1920x1080;
- matched opaque and opaque-plus-translucent captures with the generated responsive mask;
- 600 dispatches with 11 history resets during startup and camera transitions; and
- no XeSS fallback after the pre-scene frame, dispatch failure, or device removal.

The captured gameplay window was inspected and contained the expected scene and HUD rather than a black or corrupt
output. Broader image-quality, performance, hardware, level, and Proton validation remains.

## Known issues

- Motion is camera-only. Moving characters, weapons, particles, vegetation, and shader animation can ghost or
  shimmer.
- UI and scene color are combined before XeSS, so HUD text and icons receive temporal treatment.
- The responsive mask is inferred from two scene captures. It is not a semantic transparency or composition mask.
- Exposure is fixed for the LDR input; the game's exposure buffer is not integrated.
- The game still renders at 1280x720. XeSS does not increase geometry, shadow, or material resolution.
- Output must keep the 16:9 input aspect ratio. Ultrawide temporal output needs a separate policy.
- The primary scene projection mapping is runtime-validated for game version 1.14 only.
- Split-screen and multiple views bypass the temporal path.
- There is no in-game quality selector or debug visualization.

## Future work

1. Publish coherent per-object and skeletal transforms and generate object motion vectors.
2. Separate scene and UI composition so the HUD can be drawn after temporal upscaling.
3. Replace the difference mask with semantic transparency/reactive information where the renderer exposes it.
4. Integrate stable scene exposure or pre-exposure if it improves image stability.
5. Add a user-facing quality policy after visual and performance measurements.
6. Validate more Intel, NVIDIA, and AMD GPUs and the supported Windows/Proton paths.
7. Investigate an ultrawide render-resolution policy without stretching the 16:9 temporal input.
8. Evaluate XeSS Frame Generation and Xe Low Latency only as separate opt-in experiments after XeSS-SR is stable.

## Intel references

Implementation behavior was written from the public
[Intel XeSS SDK](https://github.com/intel/xess), the
[XeSS SDK 3.0.2 release](https://github.com/intel/xess/releases/tag/v3.0.2), and Intel's
[XeSS-SR developer guide](https://github.com/intel/xess/blob/v3.0.2/doc/xess_sr_developer_guide_english.md).
No Intel source code is copied into the adapter.
