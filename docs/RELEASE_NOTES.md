# Version 1.3 — 13 November 2026

- Cloud shadows now darken water, except in VR.
- New MCM switch for water shadows.
- New MCM switch to turn off shadows at night.
- New Current weather slider that also works with Vivid Weathers and other weather mods.

# Version 1.2 — 2 October 2026

Build `dee0d873c05fab93`. The DLL reports version **1.2.0.0**. Its SHA-256 is
`C2C2034A6171FEC4CAAF3EFAFEA3BB46037C8ACF3E88F10B91BCC0509C8E788D`.

- Add **Weather specific cloud opacity**, off by default, with individual
  opacity settings on the same 0–4 scale as Shadow opacity.
- Add sliders for 12 vanilla outdoor weathers in MCM. F4SE Menu Framework also
  lists DLC and mod weathers from the load order, marks the current weather,
  and provides a Reset button for each override.
- Blend opacity gradually during weather transitions, with additional smoothing
  for sudden changes such as console-forced weather. Menu edits apply directly.
- Store overrides in the user INI's `[Weather]` section. Weathers without an
  override follow the global Shadow opacity. DLC and mod overrides can use
  plugin names and local form IDs to remain stable across load-order changes.
- Preserve weather settings when saving through the F11 development menu.
- Avoid the newer Direct3D context query associated with a reported ENB
  save-load crash. Wrapped contexts use the standard whole-buffer calls;
  native Direct3D contexts retain support for constant-buffer ranges.
- Fix missing cloud shadows behind a pass-through `d3d11.dll` proxy that returns
  the native Windows graphics device, while retaining the ENB/ReShade guards.
- Improve logging when an unsupported graphics interceptor prevents cloud
  shader capture.
- Correct the settings tooltip to show the default Shadow opacity of 1.0.

Validation: the Release build and all 14 core checks pass, including binary
version, F4SE metadata, weather settings and graphics-wrapper contracts.
Weather gameplay behavior and the original tester's ENB crash fix still await
confirmation. The preceding weather test build loaded on OG with ENB 0.501
and registered its F4SE Menu Framework settings page successfully.

# Version 1.1 — 28 September 2026

Build `0c5f8ead508b2e31`. The DLL reports version **1.1.0.0**. Its SHA-256 is
`89F333E9E59150A3E24BB9C864836816316D9D9D22E18EA771EBB37D65C2146C`.

- Set default shadow opacity to **1.0** in the DLL, MCM and legacy JSON settings.
- Keep **Development menu off by default**. F11 opens the optional menu and FPS
  test controls; F10 toggles cloud shadows for a manual comparison. F7/F8 are
  unused. The MCM page contains cloud enable, opacity and Development menu only.
- Include the FO4 Community Shaders 0.1.1 adapter, entirely in this plugin.
  No CS file replacement or upstream plugin change is needed.
- Include gradual travel re-anchoring on desktop and VR, blending over four
  seconds to avoid a sudden projection jump.
- Support Fallout 4 1.10.163, 1.10.984, 1.11.221, 1.11.240 and VR 1.2.72 in one
  DLL, with the matching script extender and no Address Library requirement.
- Update the DLL resource, F4SE metadata, development menu and package versions
  consistently to 1.1.

Existing saved user preferences take precedence over the new defaults; the
package does not include or overwrite `MCM/Settings/FO4CloudShadows.ini`.
The rendering implementation is unchanged from build `92c53ede901e26f1` below.
That build's loaded-save CS test confirms capture, mask dispatch and sunlight
submissions; full visual and combined-performance acceptance remain outstanding.

# Previous build 92c53ede901e26f1 — 28 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`F8AA5D17CE168ECF6FAC1DFC93CF9F76753DFE472F7FAE10829C59CE6113F916`.

- Add an adapter for the audited FO4 Community Shaders 0.1.1 cloud shader
  variants. Cloud layers 5/6/7 retain their original texture, interpolation,
  opacity and fade behavior. Unknown replacements remain rejected.
- Apply our screen-space cloud mask to the direct-light colour in CS's active
  deferred-light shader. Preserve CS's original instructions, shadow factors,
  ambient additions, extra lighting lobes and output alpha. No CS files or
  upstream plugin changes are required.
- Recognize `FO4CommunityShaders.dll` and permit its shader-creation hooks as
  a validated downstream owner, keeping an existing outer hook in place.
- Retain the standalone renderer, menu settings and runtime requirements.
- Add GPU composition and lifecycle tests plus optional CS shader corpus checks.

Validation: all 14 core checks pass. All 65 matching directional shaders from
166 captured CS lighting variants patch and create successfully. A further 128
combinations of directional shader family, ambient light, screen-space shadows,
terrain shadows, wetness and water effects compile, patch and create on WARP.
The GPU composition fixture verifies neutral/partial/full cloud attenuation,
unchanged ambient and output alpha, swizzles and operand modifiers. Local-light
descriptors, unknown layouts, missing reflection and an occupied t47 are rejected.
The stock 1,243-shader corpus retains its exact 138 directional matches.

Live validation on Fallout 4 1.11.240 / F4SE 0.7.9 with FO4 CS 0.1.1 and its
Screen Space Shadows feature enabled reached an exterior save. The log confirms
authenticated CS cloud capture, a 1920x1080 shadow prepass and at least 15,000
submitted shadowed sunlight draws, including recovery after a load transition.
Neither plugin logged errors. One startup menu-device deferral and three
cloud-field initialization/reset deferrals were followed by successful setup
and rendering. The F11 menu also opened and saved settings during the run.
Only our DLL was replaced; all 74 installed CS DLL/shader files remained
byte-for-byte identical to the official 0.1.1 archive.

This confirms the rendering path operates together in a loaded save; full
visual and combined-performance acceptance remain outstanding. The adapter
does not add cloud-shadow consumers to water surfaces,
fog or volumetric/forward lighting. Source reference: Mute/northaxosky's FO4 CS
commit `1a0439a01dabdbd1ad65b0dd9547160d6125b53e`; see THIRD_PARTY_NOTICES.md.

# Previous build d2e1ad80d10a5584 — 28 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`450DA0436F159EBDDC862A40223F28ADA098A73D8DAE8B6007FD012443FCA778`.

- Rename the F11 development menu window and heading from **Open Shaders FO4**
  to **Cloud Shadows FO4**. Hotkeys, controls and rendering behavior are unchanged.
- Record the joint test of the previous build with FO4 Community Shaders 0.1.1
  on Fallout 4 1.11.240, with CS Screen Space Shadows enabled. Both plugins
  loaded and an exterior save ran, but our cloud capture rejected CS's sky
  shaders and cloud shadows did not render. The observed vertex and pixel
  hashes exactly match the compiled BSSky entries in the CS shader cache.
  This documents a confirmed incompatibility; this build does not fix it.

The DLL rebuild and package checks pass. In-game visual confirmation of the
renamed title awaits installation and restart. Re-anchor visual acceptance
remains pending because the joint setup did not produce cloud shadows.

# Previous build 9b9302cd6ecad960 — 28 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`5B7D106B84994A7CAF07BE94426DD807258127A87EE62DFC3EFDFF88BC41BDB5`.

- Enable the existing VR travel re-anchoring policy on all four supported
  desktop runtimes. Read the authenticated main-view GPU camera asynchronously
  about twice per second, instead of retaining the first origin until a reset.
- Re-anchor after more than 1.5 cloud heights of horizontal travel or half a
  cloud height vertically, and blend from the previous projection for four
  seconds. At the default height these are about 214 m and 71 m. Nearby movement
  keeps the current anchor; this bounds travel drift rather than providing
  continuous camera-centred cloud alignment.
- Send the previous-origin constants on desktop and enable the same shader
  blend used by VR. Updating the CPU anchor alone would otherwise cause a jump.
- Document fog behavior and the current unsupported FO4 Community Shaders
  combination. The existing startup check still misses `FO4CommunityShaders.dll`;
  this build does not add a new exclusion or compatibility policy.

All 14 core checks pass. The GPU regression first failed against the old desktop
shader, then passed with the fix; it verifies previous-origin blend weights on
desktop and VR, production/diagnostic agreement, and clear-sky neutrality. Camera
readback tests also cover a later desktop camera without a world reset.
Developer tools and private profiling remain compiled off.

Long-distance in-game visual testing and a joint FO4 Community Shaders run have
not been performed for this build. Re-anchoring remains a periodic approximation;
sky alignment can differ between re-anchors and during the four-second blend.

# Previous build 48c2784b65fcc7f0 — 28 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`B448F162B7492DCCCB503D81BF735F668DB6B9C0A1CD37A0E982FE52EC97529A`.

- Add Fallout 4 **1.10.984** and **1.11.221** to the same DLL alongside
  1.10.163, 1.11.240 and VR 1.2.72. Other versions remain rejected.
- Include a separate reviewed address table for each added executable. Address
  Library remains unnecessary; install the matching F4SE for your game.
- Select the newer shader-name and render-target map offsets on both versions,
  and use the corresponding depth-reconstruction layout. The development menu
  and log identify the exact runtime.
- Retain the preceding Present-wrapper fix and existing release settings.

All 58 active runtime addresses pass checks against local executables and
version-specific databases. All 14 core checks pass, including exact host
matching, omission/mismatch rejection, the new object-layout fixtures,
GPU projection for both added runtime paths, and the exported F4SE version
list with zero Address Library flags. Developer tools and private profiling
remain compiled off.

NG 1.10.984 and AE 1.11.221 have not yet been launched or visually tested with
this build. Static executable review and WARP tests do not replace that
remaining in-game validation. See [address and layout evidence](RUNTIME_ADDRESSES.md).

# Previous build 6cbead4449ed1249 — 28 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`4CB99CE14F420714E4EB9A0C6630CB0E87C6DB69B3D2AC7C508A9B29C40904B7`.

- Fix missing cloud shadows when a graphics wrapper replaces the native swap
  chain after its Present hook was installed. Install the renderer-selected
  object's actual Present method with its own immutable trampoline.
- Keep one shared nesting guard across wrapper, overlay and native routes.
  Publish each main frame once; keep auxiliary and TEST presents excluded.
- Notice later changes to the selected chain's Present vtable entry during
  normal renderer maintenance. Failed attachments leave that boundary inactive
  and retry with backoff, without replacing established trampolines.
- Remove the assumption that any existing native Present hook makes a newly
  selected wrapper ready. No weather, shader or saved-setting changes are needed.

All 14 core checks pass. Real WARP/Detours integration tests cover native-first
and wrapper-first installation, 120 successive wrapped frames, nested wrappers,
a later overlay, auxiliary windows, TEST calls, receiver replacement, original
HRESULT propagation and failed-attachment recovery. The reporter's exact mod
setup and an in-game visual matrix have not been rerun for this build.
Release developer-tools/private-profiling flags remain off. Runtime support
and Address Library requirements are unchanged from the preceding build.

# Previous build 3824b403d0b12757 — 28 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`EC7CB38C31AFA31636CD7E2CFC1B605F5B98CF0303A54036A88340F16AFA5E33`.

- Add FPS testing to the release Development menu, following the Fallout
  mirrors mod's ON/OFF averages, frame-time distribution and automatic-test
  workflow. F11 provides Start FPS test, Start manual comparison, stop and
  hide-display controls. F10 remains the manual shadow toggle.
- Run OFF/ON/ON/OFF in about 57 seconds, including countdown and settling.
  Restore the original shadow setting after completion or cancellation; never
  save temporary benchmark phases. Report live application FPS, mean frame
  time, p95/p99 and ON-minus-OFF results in the menu and log.
- Reject incomplete samples after focus loss, menu visibility or absent world
  rendering. Cancel automatic tests on camera/world, settings, output or render
  thread changes. Check Present counters, independent wall time, ON lighting
  coverage and the repeated OFF baseline before accepting the comparison.
- Remove F7/F8 handlers, sky-preview controls and their MCM help text. MCM still
  contains only cloud enable, opacity and the Development menu checkbox.

All 12 core checks pass, including automatic completion/restoration, manual
comparisons, interruption recovery, baseline drift and missing ON lighting.
Release developer-tools/private-profiling flags remain off. The shaders and
runtime version list are unchanged. Gameplay validation of the new FPS menu
is pending at packaging time. Reported frame times include caps and Present
waits and do not represent isolated CPU/GPU execution time.

# Previous build d8912e3322fc8cc3 — 27 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`CDF7C4C334F81E15863E5D08D2627FF5A25A83770D83C60D7F194F483914D3CE`.

- Remove Debug view, Isolate one cloud and Isolation radius from MCM. The
  Development menu checkbox enables desktop hotkeys without adding controls.
  Its description explains F11, F10, F8 and F7 individually.
- Ignore saved values from the removed controls and remove those keys on the
  next settings save. Existing cloud enable and opacity preferences are kept.
- Fix the F11 menu being rejected with ENB 0.501. ENB's device wrapper and its
  output device have different interface addresses but share COM identity.
  Compare that identity while continuing to reject different devices and
  separately wrapped resources.

All 12 core checks pass, including regressions for obsolete MCM settings,
ENB-style device aliases, distinct wrapper identities and reference lifetimes.
The shaders, runtime version list and release profiling flags are unchanged.
In-game confirmation of this menu fix remains pending at packaging time.

# Previous build f202d70ed7dbb9b1 — 27 September 2026

The DLL reports version **1.0.0.0**. Its SHA-256 is
`B9A01BD3315CB905F246DEF2BA37159C74A3F2E99E1DC556132B6C7E1ED2C1E0`.
Use the runtime and matching source archives bearing build ID
`f202d70ed7dbb9b1` for the release without Address Library.

## Changes

- Remove the Address Library requirement on OG and AE by including the exact
  reviewed addresses for 1.10.163.0 and 1.11.240.0. VR 1.2.72.0 continues to use
  its existing built-in addresses. F4SE/F4SEVR is still required.
- Remove database loading and clear the F4SE metadata flags that previously
  required a database before the plugin could load. Retain the exact supported
  executable-version list, ASLR handling and memory validation.
- Add package verification of the actual exported F4SE metadata and additional
  unsupported-version regressions. The renderer, shaders, default settings and
  earlier rendering-state fix are retained.

## Validation and limits

All 12 core checks pass, including the renderer fixtures, binary metadata and
release-feature contracts. Each of the 12 OG and 12 AE addresses matches its
original database lookup. All 34 active OG/AE/VR entries pass checks against
the local executables for exact versions, image bounds and expected PE sections.
The new binary check correctly rejects the previous DLL's Address Library
flags and accepts this DLL. See [address verification](RUNTIME_ADDRESSES.md).

This build has not been launched in-game without Address Library. Full visual,
compatibility, long-session and performance acceptance remains outstanding;
the gameplay and hardware observations below apply to their recorded builds.

# Previous build 372f3a9044377c14 — 27 September 2026

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
