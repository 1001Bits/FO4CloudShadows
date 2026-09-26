# Cloud Shadows for Fallout 4

Cloud Shadows adapts the Skyrim Community Shaders technique to Fallout 4. It
captures the game's actual cloud geometry and textures and projects their
coverage along the visible sun's direction, dimming direct sunlight beneath
cloud cover and allowing it through gaps.

The cloud map is a small cubemap. A compute pass reconstructs surface positions
from scene depth, projects them into the world-anchored cloud field and supplies
a shadow mask to the game's sunlight pass. Fallout's implementation uses
dedicated capture draws; it does not have Skyrim's zero-additional-capture-draw
property. Volumetric godray occlusion is not included in the release build.

## Requirements and runtime targets

| Runtime | Game version | Script extender | Address Library |
| --- | --- | --- | --- |
| OG | 1.10.163 | F4SE 0.6.23 | Matching desktop database required |
| AE | 1.11.240 | F4SE 0.7.9 | Matching desktop database required |
| VR | 1.2.72 | F4SEVR 0.6.21 | Not required by this plugin |

Use [F4SE](https://f4se.silverlock.org/) and, on desktop,
[Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327).
Other executable versions, including 1.10.980/984, are not runtime targets of
this build. One DLL contains the three supported runtime paths.

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
settings page. The default opacity is 2.0. Development controls are off by
default. User settings are stored in `Data/MCM/Settings/FO4CloudShadows.ini`;
the package supplies defaults under `Data/MCM/Config/FO4CloudShadows` and does
not ship a user-settings file. Legacy JSON preferences are migrated when no
user INI exists.

Close the game before updating or removing this DLL. Replace the installed mod
when updating, preserving your user INI. Remove the mod through the mod manager
to uninstall it.

## Compatibility and current validation

- ENB: if ENB's cloud shadows are enabled at startup, this plugin stays inactive.
  Set `[EFFECT] EnableCloudShadows=false` in `enbseries.ini` and restart the game
  to use this implementation.
- ReShade and Upscaling: build `acc6bcd8dfab4548` passed an AE visual test with
  both enabled, including sideways movement, turning and melee camera motion.
  That test used ReShade 6.8.0, 1280x720 rendering and 1920x1080 output. It does
  not certify every ReShade effect, upscaler or frame-generation mode.
- Earlier OG and VR builds passed visual smoke checks. The current universal
  DLL still needs the full OG/VR acceptance matrix. A later VR save-load crash
  remains unattributed; the available dump does not directly implicate this
  plugin. The latest OG retest also exited during save loading amid memory and
  graphics-driver errors, without a crash dump. See [release notes](docs/RELEASE_NOTES.md).
- No comprehensive weather-mod compatibility list or matched performance
  measurements have been established for this build.

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
