# Build acc6bcd8dfab4548 — 26 September 2026

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
