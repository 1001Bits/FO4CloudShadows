# Cloud Shadows for Fallout 4

Cloud Shadows adapts the Skyrim Community Shaders technique to Fallout 4. It
captures the game's actual cloud geometry and textures and projects their
coverage along the visible sun's direction, dimming direct sunlight beneath
cloud cover and allowing it through gaps. Shadows also fall on moonlit ground at
night. The "Shadows at night" switch can instead fade them out at sunset and back
in at sunrise, following the current climate, with nothing running in between.
On desktop they also fall on water: sun highlights go out and the water's colour
darkens. Water is unchanged in VR.

Weather specific cloud opacity gives each weather its own strength. MCM's Current
weather slider sets whichever weather is active, so it works with DLC and weather
mods such as Vivid Weathers, which replace the vanilla weathers.

The cloud map is a small cubemap. A compute pass reconstructs surface positions
from scene depth, projects them into the world-anchored cloud field and supplies
a shadow mask to the game's sunlight pass. Fallout's implementation uses
dedicated capture draws; it does not have Skyrim's zero-additional-capture-draw
property. Volumetric godray occlusion is not included in the release build.

Desktop and VR keep the projection origin fixed during nearby movement, then
re-anchor it after travelling more than 1.5 cloud heights horizontally or half
a cloud height vertically. The transition blends over four seconds. At the
default 10,000-unit height, those distances are about 214 m and 71 m. This bounds
travel drift; the projection is still an approximation between re-anchors.
Fog does not disable cloud shadows on terrain and objects, but the fog itself
does not receive cloud occlusion. Dense fog or weak sunlight can hide the effect.

## Requirements and runtime targets

| Runtime | Game version | Script extender |
| --- | --- | --- |
| OG | 1.10.163 | F4SE 0.6.23 |
| NG | 1.10.984 | F4SE 0.7.2 / 0.7.3 |
| AE | 1.11.221 | F4SE 0.7.8 |
| AE | 1.11.240 | F4SE 0.7.9 |
| VR | 1.2.72 | F4SEVR 0.6.21 |

Use the matching [F4SE](https://f4se.silverlock.org/) and the
[Microsoft Visual C++ v14 x64 Redistributable](https://aka.ms/vc14/vc_redist.x64.exe).
**Address Library is not required** on OG, NG, AE or VR. The DLL contains addresses
for the exact versions above; other mods may still need Address Library.
Other executable versions, including 1.10.980, are not runtime targets of
this build. Supporting a different executable version requires a mod update.
One DLL supports all five exact game versions above.

[MCM](https://www.nexusmods.com/fallout4/mods/21497) provides the settings page.
The optional [F4SE Menu Framework](https://www.nexusmods.com/fallout4/mods/105090)
provides an additional desktop settings page. The effect also runs with its
default settings without either menu framework.

## Installation and settings

Install the runtime ZIP as a mod in your mod manager and launch through the
appropriate script extender. For a manual install, copy the ZIP's `F4SE`,
`Shaders`, `MCM` and `Interface` directories into the game's `Data` directory.
The package does not contain an ESP or ESL.

Enable cloud shadows and adjust **Shadow opacity** in the Cloud Shadows
settings page. The default opacity is 1.0. Development controls are off by
default. User settings are stored in `Data/MCM/Settings/FO4CloudShadows.ini`;
the package supplies defaults under `Data/MCM/Config/FO4CloudShadows` and does
not ship a user-settings file. Legacy JSON preferences are migrated when no
user INI exists.

Enable **Development menu** in MCM, close MCM, and use the desktop hotkeys:

- **F11** opens or closes the development menu, including the FPS test controls.
- **F10** toggles cloud shadows and starts a manual FPS comparison.

The Development menu checkbox is off by default and adds no extra MCM controls.
Turning it off closes the development menu and stops any active FPS test.

Choose **Start FPS test** in F11, then keep the same outdoor view. The menu
closes and the test runs OFF/ON/ON/OFF in about 57 seconds: a five-second
countdown, then three seconds of settling and ten seconds of measurement per
step. It restores the original shadow setting when complete or cancelled;
test phases are never saved to your settings. **Start manual comparison** lets
you switch modes with F10 instead. Reopening F11 pauses sampling and provides
the stop and hide-display buttons. F7 and F8 are unused.

The display and `FO4CloudShadows.log` report application FPS, average frame time,
95th/99th percentile frame times and the ON-minus-OFF difference. These include
Present waits, VSync and frame caps; they do not isolate CPU or GPU execution
time. OFF keeps the plugin installed; the comparison measures toggling the
shadow effect. The automatic comparison checks the repeated OFF baseline and confirms
that cloud lighting was submitted throughout ON samples. Focus loss, absent
world rendering and the F11 menu discard partial windows. Moving the camera or
changing settings cancels an automatic test. Keep other game menus closed.

Close the game before updating or removing this DLL. Replace the installed mod
when updating, preserving your user INI. Remove the mod through the mod manager
to uninstall it.

## Compatibility and current validation

- Fallout 4 Community Shaders (`FO4CommunityShaders.dll`): an adapter for the
  audited FO4 CS 0.1.1 shaders is included. It accepts the three validated cloud
  shader pairs and applies cloud attenuation to CS's direct sunlight colour,
  preserving its lighting features and ambient contributions. It requires no
  CS file replacement or CS-side plugin changes. The standalone path remains
  available when CS is absent. Unknown cloud shader pairs or lighting layouts
  are rejected. Shader validation covers 128 combinations of contact shadows,
  terrain shadows, wetness and water effects. A loaded exterior save on
  1.11.240 with CS Screen Space Shadows enabled confirmed cloud capture, mask
  dispatch and shadowed sunlight submissions, including recovery after a load
  transition, with no errors in either plugin log. Full visual and combined
  performance acceptance remain outstanding. This does not add cloud
  attenuation to fog or other forward/volumetric lighting passes. FO4 CS 0.4.0
  and later are not supported: their renamed lighting constants are rejected, so
  ground shadows do not appear with them.
- ENB: if ENB's cloud shadows are enabled at startup, this plugin stays inactive.
  Set `[EFFECT] EnableCloudShadows=false` in `enbseries.ini` and restart the game
  to use this implementation.
- ReShade and Upscaling: build `acc6bcd8dfab4548` passed an AE visual test with
  both enabled, including sideways movement, turning and melee camera motion.
  That test used ReShade 6.8.0, 1280x720 rendering and 1920x1080 output. It does
  not certify every ReShade effect, upscaler or frame-generation mode.
- This build removes the Address Library requirement and retains the rendering
  state fix from `372f3a9044377c14`. That earlier build passed hardware rendering
  checks and submitted shadowed sunlight draws in OG. Built-in desktop addresses
  match the previous database lookups; see [address verification](docs/RUNTIME_ADDRESSES.md).
  Full OG/AE/VR visual acceptance and long-session testing for the new DLL remain
  outstanding. Earlier OG/VR save-load failures remain unattributed. See
  [release notes](docs/RELEASE_NOTES.md) and the
  [graphics audit](docs/GRAPHICS_AUDIT_2026-09-27.md).
- No comprehensive weather-mod compatibility list or matched performance
  measurements have been established for this build.
- NG 1.10.984 and AE 1.11.221 addresses and the required engine layouts were
  checked against the exact executables. Host-selection, binary-metadata and
  GPU projection tests cover the added runtime paths. In-game testing on these
  two versions is still outstanding; see [runtime verification](docs/RUNTIME_ADDRESSES.md).

For bug reports, include the game and mod versions, reproduction steps,
`FO4CloudShadows.log`, and a crash report if one exists. Logs are under
`Documents/My Games/Fallout4/F4SE` or `Fallout4VR/F4SE`.

## Building and source

See [BUILD.md](docs/BUILD.md) for the Windows build and packaging commands.
The source is maintained at
[1001Bits/FO4CloudShadows](https://github.com/1001Bits/FO4CloudShadows).
Runtime and matching source ZIPs are produced together with SHA-256 checksums.
The source package includes the exact archives and build recipes for linked
vcpkg dependencies; see [SOURCE_DELIVERY.md](SOURCE_DELIVERY.md).

## Credits and licensing

ProfJack and doodlum developed the original
[Skyrim Cloud Shadows technique](https://www.nexusmods.com/skyrimspecialedition/mods/139185).
This implementation also contains adaptations from Fallout 4 Community Shaders
and Dynamic Reflections for Fallout 4. Exact provenance and dependency notices
are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The project is licensed under **GNU GPL version 3**. See [LICENSE](LICENSE) and
[EXCEPTIONS.md](EXCEPTIONS.md). The scope of additional
permissions varies by component as documented in the notices.
