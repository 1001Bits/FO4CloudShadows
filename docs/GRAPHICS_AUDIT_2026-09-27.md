# Graphics preservation audit — 27 September 2026

One graphics-state defect was reproduced and fixed. The updated build passes
all 12 core checks, the hardware renderer fixtures on an RTX 4070 Laptop GPU,
and the complete available OG deferred-light shader corpus. This establishes
the tested rendering contracts; it does not certify every in-game weather,
material, mod combination or long session.

## Fixed defect

`CloudGeometryCapture::SavedState` restored VS/PS and output targets even when
validation rejected a capture before it changed any state. Its snapshot does
not contain shader class instances or pixel UAVs because those configurations
are deliberately unsupported. That unnecessary restoration therefore cleared
foreign bindings and could affect subsequent native or third-party rendering.

The state guard now starts restoration only immediately before capture makes
its first binding change. Rejected requests release their acquired references
without calling pipeline setters. Successful capture retains its restoration
path. No cloud appearance parameters or production shader math changed.

A new pixel-UAV regression failed against the original code and passes with
the fix. It checks the exact binding and confirms that rejection submits no
geometry. WARP also checks preservation of a dynamically linked pixel shader
and its class instance. These cases run with flat/VR layouts and normal/zero
initial cloud fade.

Implementation: [CloudGeometryCapture.cpp](../src/CloudGeometryCapture.cpp).
Regression: [CloudRendererTests.cpp](../tests/CloudRendererTests.cpp).

## What the normal-graphics checks establish

| Area | Evidence and remaining boundary |
| --- | --- |
| Visible sky and clouds | GPU fixtures alternate 27 capture-disabled/capture-enabled frames per configuration, for cloud techniques 5/6/7. Native sky pixels remain identical. Both flat and VR layouts pass on WARP and NVIDIA hardware. |
| Ambient light and alpha | The real sunlight patch renders generated direct-plus-ambient fixtures with factors 0, 0.5 and 1. Ambient diffuse/specular and both output alpha channels are preserved. |
| Shader selection | The stock OG corpus contains 1,243 shaders; precisely 138 directional variants, representing 66 unique hashes, match the expected signatures. Patched matches disassemble and create successfully on WARP. Other descriptors remain excluded. This was not a complete AE/VR corpus run. |
| Reflections and other views | Source review finds main-resource, viewport, shader and technique checks before capture and lighting substitution. The water-reflection hook forwards the native call. The OG log records rejection of 512/1024-square offscreen targets against a 1920x1080 main view. This supports isolation but is not a visual comparison of all reflection mods. |
| Camera movement and stereo | Flat/VR projection, world anchoring, motion, reduced rendering sizes and sky-alignment fixtures pass. A generated VR fixture does not replace headset testing. |
| Clear sky, darkness and transitions | Invalid receivers and absent cloud coverage produce a neutral factor. The lifecycle checks cover withdrawal/reset and publication of completed cloud fields. The production mask is bounded to 0–1. |
| Menus and debug preview | The cubemap-preview test preserves source pixels, native compute bindings and predication, and verifies face orientation/live updates. Main-view guards protect unrelated draws. In-game menu comparisons remain part of visual acceptance. |

The lighting change is limited to authenticated deferred sunlight. Materials
outside that path, including emissive/additive or some forward-rendered
surfaces, can retain their brightness while nearby sunlit surfaces darken.
Preserving those materials is not proof that every object will have matching
perceived contrast under cloud shadows.

## Skyrim reports checked

The accessible first page of [Cloud Shadows — Community Shaders comments](https://www.nexusmods.com/skyrimspecialedition/mods/139185?tab=posts)
was reviewed, including its older pinned discussions. The page lists 177
comments across seven pages. Older-page requests could not be retrieved, and
direct access encountered a Cloudflare challenge. This is not a claim to have
reviewed all 177 comments.

| Report on that page | Assessment of this implementation |
| --- | --- |
| VendeClarion, 12 February 2025: shrubs appear bright with little ambient light; the follow-up distinguishes placed shrubs from grass. | Ambient preservation is tested, but differently lit materials can still stand out as described above. A matched in-game vegetation comparison is required to exclude this appearance. |
| baronvonellis22, 19 February 2026: apparent 10–15 fps shadow movement and sharp boundaries. | Capture is evaluated each authenticated main-Sky frame and publishes a completed field at Present; there is no deliberate 10–15 Hz cloud-animation cap. Motion tests pass. Sampling is bilinear at mip zero, not a soft solar-penumbra integration, so hard-looking boundaries cannot be ruled out. |
| FransGamingLow, 18 June 2026: a personal 2–4 fps loss. | Cloud capture and projection have a cost. Instruction counts and successful tests do not measure the game's frame-time impact. Matched enabled/disabled game measurements remain outstanding. |
| August/September 2026 comments: confusion about updates and integration into core Community Shaders. | These concern the Skyrim distribution. This plugin has its own runtime/build checks and does not depend on Community Shaders. |

The accessible [Skyrim bug list](https://www.nexusmods.com/skyrimspecialedition/mods/139185?tab=bugs)
also contains a compilation report for version 1.2.0, closed as **Not a bug**.
The FO4 production flat/VR shaders compile and execute in the fixtures, and
the OG log reaches successful sunlight draws. That does not establish the
cause of the Skyrim report.

The OG settings used in the log have opacity **4.0**, while the shipped default
is **2.0**. The stronger setting can increase contrast and make boundaries
look harsher. No user settings were changed during this audit.

## Crash dialog during the audit

The screenshot names `FO4CloudShadowsRendererTests.exe`, the standalone test
process launched during this audit. Its access violation occurred in
`nvgpucomp64.dll` while exercising the newly added synthetic dynamic-linkage
fixture. A minimal program linking only D3D11 and D3DCompiler reproduces the
same exception without any Cloud Shadows code. The same probe passes on WARP.
This isolates that observed test crash; it does not explain unrelated game
crashes or establish behavior on other driver versions.

The linkage regression now runs on WARP. Hardware mode explicitly reports
that one fixture as skipped; pixel-UAV preservation and the production
capture/projection fixtures still run on hardware. Automated tests suppress
modal Windows error dialogs and report their current stage.

A separate hardware preview assertion assumed a maximum 0.5-ULP error when
converting float to UNORM. The observed error was about 0.55 ULP, within the
0.6-ULP allowance in [Direct3D section 3.2.3.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
The test now uses that bound and retains its channel, alpha and orientation
checks. Preview rendering code was not changed.

## OG log and build status

The reviewed log is `Documents/My Games/Fallout4/F4SE/FO4CloudShadows.log`, from
the 14:11–14:14 session on 27 September. It reports OG 1.10.163, F4SE 0.6.23,
build `acc6bcd8dfab4548`, developer tools/profiling disabled, and ENB 0.501 with
ENB cloud shadows disabled. Initialization completes, 66 strict sunlight hashes
become ready, and actual patched sunlight draws are submitted.

There are no `[E]` entries and three `committed-field` rejection warnings.
The first two are followed by renewed rendering activity; the log ends at the
third, after a world reset. Rejection retains native lighting while cloud data
is unavailable. The log alone cannot prove recovery after its final line or
a clean game exit. No new Fallout crash dump was found during this audit.

The rebuilt universal DLL is at
`out/build/validation/bin/Release/FO4CloudShadows.dll`:

- Build ID: `372f3a9044377c14`.
- SHA-256: `6092986C47E514E7D68323C5BEDDE0A21716894B2B49632D4C1C66C68F463E1F`.
- Core validation: **12/12 passed** after the final test changes.
- Hardware renderer: **passed**, with the WARP-only linkage case disclosed.
- OG shader corpus: **passed**, 1,243 total / 138 matched / 66 unique.

At audit completion, this DLL had **not been deployed to the games**. OG still
had build `acc6bcd8dfab4548`, SHA-256
`B266593FA734FEA5614216189CE00FEDBE7A353FDDDD2E1A1A39FD8FE0AC50F0`.
Existing release notes describe the older build and were not promoted to a
new runtime acceptance claim.

Local, untracked evidence is under
`out/build/validation/graphics-audit-20260927/`: `ctest-final.log`,
`renderer-hardware-final.log`, `og-stock-shaders.log`,
`rejected-capture-before.log`, `rejected-capture-after.log`, and the standalone
`linkage-probe` source with hardware/WARP logs. Game-owned shader dumps remain
local build artifacts.

### Follow-up OG launch

At the user's request, build `372f3a9044377c14` was installed into OG and
launched through F4SE at 15:12 on 27 September 2026 (Asia/Bangkok). The previous
DLL and PDB were backed up under `out/build/validation/og-audit-20260927/backup/`;
the deployment plan and launch receipt are in that parent directory. Installed
shaders matched the audited source, and settings were preserved.

The new startup log confirms OG 1.10.163, F4SE 0.6.23, the expected build ID,
plugin load completion and successful shadow prepass dispatch. This is startup
evidence, not completion of the visual acceptance work below. AE and VR were
not updated in this launch.

At 15:15 the user requested a comparison with the mod disabled. The OG DLL was
renamed to `FO4CloudShadows.dll.disabled-graphics-audit-20260927` and OG was
relaunched through F4SE. The original DLL path is absent for that session;
settings were not changed. Its receipt is under
`out/build/validation/og-disabled-20260927/`. This local comparison does not
disable the release package, which is built from the audited build output.

## Remaining acceptance work

A full visual acceptance pass needs the rebuilt DLL in OG, AE and VR, with
matched enabled/disabled views of vegetation, emissive effects, water,
reflections and menus; clear/cloudy weather, night and interior transitions;
and movement, save/load and longer sessions. Edge appearance should be checked
at both default opacity and the user's stronger setting. Performance needs
matched frame-time measurements. These are open evidence gaps, not additional
defects established by this audit.
