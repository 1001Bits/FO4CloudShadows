# Build 372f3a9044377c14 — 27 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`6092986C47E514E7D68323C5BEDDE0A21716894B2B49632D4C1C66C68F463E1F`.
Use the runtime and matching source archives bearing build ID
`372f3a9044377c14`; older archives do not contain the fix below.

## Changes

- Preserve native rendering state when a cloud capture is rejected. Previously,
  cleanup could clear another renderer's pixel UAV or shader class bindings
  even though capture had not changed the pipeline.
- Add regression coverage for rejected captures and correct the GPU preview
  test's float-to-UNORM tolerance to the Direct3D specification.
- Keep the previous build's cloud projection, opacity defaults and runtime
  compatibility behavior.

## Validation and limits

All 12 core checks pass, including the binary and release-feature contracts.
Flat/VR rendering fixtures pass on an RTX 4070 Laptop GPU. The synthetic shader
class-linkage case runs on WARP because it reproduces an NVIDIA compiler crash
in an independent program; hardware mode explicitly reports that skip. The
complete available OG lighting corpus passes: 1,243 shaders, 138 directional
variants and 66 unique matched hashes. Native sky pixels, ambient lighting and
output alpha preservation are covered by GPU fixtures.

The OG launch at 15:12 on 27 September confirms this build loads through F4SE
and submits patched sunlight draws. Full visual acceptance, long sessions,
matched performance measurements and the AE/VR game matrices for this exact
build remain outstanding. Developer tools, private profiling and experimental
godray occlusion remain disabled. See the
[graphics audit](GRAPHICS_AUDIT_2026-09-27.md) for findings and evidence limits.

# Previous build acc6bcd8dfab4548 — 26 September 2026

The tested DLL reports version **1.0.0.0**. Its SHA-256 is
`B266593FA734FEA5614216189CE00FEDBE7A353FDDDD2E1A1A39FD8FE0AC50F0`.
Archive names include the build ID to distinguish this snapshot from older
packages, including the separately named 1.0.7 archive.

## Changes

- Retain the confirmed cloud-field origin so camera travel does not drag the
  projected shadows across the landscape.
- Correct AE reconstruction with upscaling: depth sampling uses the allocation
  scale while world projection uses the rendered viewport scale.
- Capture shaders through ReShade's native device and preserve Upscaling's
  presentation chain and COM receiver.
- Accept proportionally reduced desktop sky viewports after validating the
  main-view resources; retain VR stereo and reflection guards.
- Keep MCM options to a name and one concise description and add the optional
  desktop Menu Framework page.

## Validation and limits

All 12 registered core checks passed, including the restored binary and release
feature contracts. The fixed-world receiver
GPU regression failed before the AE correction and passes afterward, covering
camera translation, pitch, yaw, roll, near/far depth and reduced render sizes.
Binary contract, release-feature and runtime asset verification also passed.
Developer tools and private profiling are compiled off.

The user confirmed **stable during movement and melee** in AE 1.11.240 with
ReShade and Upscaling enabled. The test rendered at 1280x720 into 1920x1080
output. Successful shadow and sunlight draw counters reached at least 5,000,
with no Cloud Shadows errors in that session.

OG and VR visual smoke results apply to earlier build `42439231b2fbcd1f`.
The current build has not completed those runtime acceptance matrices,
long-session testing or matched performance measurements. VR's later save-load
crash ended in an invalid-argument `memcpy_s` call from game code; its source is
not established and no Cloud Shadows frame appears on the crashing thread.
Godray occlusion is not enabled in this release.

The final OG launch of this build reached plugin initialization but exited while
loading a save. The session recorded an access violation after NVIDIA driver
errors, and Windows memory-allocation failures affected other processes around
the same time. No current Fallout crash dump was produced. The cause remains
unattributed; this OG run is not a visual acceptance pass.

Release tooling and notices have been restored alongside the tested source.
This record documents the evidence available for the snapshot; packaging does
not imply completion of the remaining runtime checks.
