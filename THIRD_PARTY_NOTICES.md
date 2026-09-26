# License and third-party notices

FO4CloudShadows is distributed under GNU GPL version 3; see [`LICENSE`](LICENSE).
The additional permissions in [`EXCEPTIONS.md`](EXCEPTIONS.md) apply only to
material for which the relevant copyright holders granted those permissions.
They do not turn GPL-covered material into MIT code and do not remove source
delivery obligations.

## Derived project material

Current build note (1.0.0, build acc6bcd8dfab4548): CMake compiles the project-owned
RuntimeAPI/EngineAPI/F4SECompat bridge and does not link CommonLibF4 or
commonlib-shared. Their revisions and license texts below remain historical
provenance. The current source archive contains the complete current project
and linked dependency inputs. Optional CommonLib source-script arguments
preserve the corresponding-source path for historical builds using those
libraries; the historical library list is not the current link graph.

The following provenance is part of the release license contract:

| Material in FO4CloudShadows | Source | Revision | Terms | Bundled text |
|---|---|---|---|---|
| `src/ShaderTools/*` and adaptations | local audited Dynamic Reflections for Fallout 4 source tree (the checkout has no configured upstream URL) | `3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7` | GPLv3 with its Modding and Linking Exceptions | `licenses/Dynamic-Reflections-GPL-3.0.txt`, `licenses/Dynamic-Reflections-EXCEPTIONS.txt` |
| Runtime HLSL formulas/layouts adapted from `BSSkyShader.hlsl` and `BSDFLightShader.hlsl` | [Fallout 4 Community Shaders](https://github.com/northaxosky/fallout4-community-shaders) | `d1ccd465cb38b81baad90973953be1a95e025644` | GPL-3.0-or-later; the cited upstream shader files do not carry the FO4-CS exception | `licenses/Community-Shaders-GPL-3.0.txt` |

The package ships the complete preferred source form of all five runtime HLSL
files. A binary release must additionally publish or accompany the complete
Corresponding Source for the DLL at the exact released revision, including the
build scripts and the adapted sources used from Dynamic Reflections. Builds
using CommonLibF4/commonlib-shared must include those exact sources as well.
Because the vcpkg triplet links Detours, spdlog/fmt, and
nlohmann-json statically, the source archive also preserves their exact
upstream source archives and the port/patch recipes and ABI evidence used to
build them. The `release-package` target creates the adjacent source artifact
specified in `SOURCE_DELIVERY.md`. Commit identifiers are provenance records,
not a substitute for making that source available as required by GPLv3 section
6.

## Current and historical component provenance

| Component | Version / pinned revision | License | Bundled text |
|---|---|---|---|
| [CommonLibF4](https://github.com/Dear-Modding-FO4/commonlibf4) | `eaa917736be34c4853302beea222c65641ec662e` | MIT | `licenses/CommonLibF4-MIT.txt` |
| [commonlib-shared](https://github.com/Dear-Modding-FO4/commonlib-shared) | `dfd28a41a832108c4de1367608bd42e1ba477212` | GPL version 3 with the project's Modding Exception and GPL-3.0 Linking Exception | `licenses/commonlib-shared-GPL-3.0.txt`, `licenses/commonlib-shared-EXCEPTIONS.txt` |
| [Microsoft Detours](https://github.com/microsoft/Detours) | vcpkg port version 2025-06-20 | MIT | `licenses/Detours-MIT.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.90.0 | MIT | `licenses/Dear-ImGui-MIT.txt` |
| Dear ImGui embedded ProggyClean font | bundled with Dear ImGui 1.90.0 | MIT | `licenses/ProggyClean-MIT.txt` |
| Dear ImGui embedded stb headers | bundled with Dear ImGui 1.90.0 | MIT or public domain | `licenses/stb-dual-license.txt` |
| [spdlog](https://github.com/gabime/spdlog) | 1.17.0 | MIT | `licenses/spdlog-MIT.txt` |
| [fmt](https://github.com/fmtlib/fmt) | 12.1.0, used transitively by spdlog | MIT | `licenses/fmt-MIT.txt` |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.12.0 | MIT | `licenses/nlohmann-json-MIT.txt` |
| Jost font | bundled 2020 Braille Institute release | SIL Open Font License 1.1 | `licenses/Jost-OFL-1.1.txt` |

The copied license texts come from the audited source checkouts, the packages
installed from this repository's locked vcpkg baseline, Dear ImGui's v1.90
upstream tag, and the bundled Jost font directory. Dear ImGui embeds
ProggyClean and identifies it as MIT with Tristan Grimmer's 2004–2005
copyright; the original `upperbounds.net` license URL is no longer resolvable,
so the bundle retains that attribution with the full standard MIT terms. Keep
all of these notices and exact texts with every binary distribution.

## Historical commonlib-shared source obligation

This subsection records the requirements for builds that link commonlib-shared.
The current universal CMake target does not link that library. Preserve this
record for the earlier binary releases to which it applies.

`commonlib-shared` is not MIT-licensed. Its GPLv3 license is accompanied by two
additional permissions in `commonlib-shared-EXCEPTIONS.txt`. The Modding
Exception permits eligible Modded Code to retain its own license when linked
with commonlib-shared. The linking exception also requires Corresponding Source
for the covered work and the Modding Libraries used in a conveyed non-source
combination.

Release records must therefore retain and make available at least:

- the exact FO4CloudShadows release source and build scripts;
- the adapted source from Dynamic Reflections revision
  `3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7`;
- Community Shaders source at
  `d1ccd465cb38b81baad90973953be1a95e025644`;
- CommonLibF4 at `eaa917736be34c4853302beea222c65641ec662e`;
- commonlib-shared at `dfd28a41a832108c4de1367608bd42e1ba477212`;
- the exact locked source and port/patch recipes for statically linked Detours,
  spdlog, fmt, and nlohmann-json;
- all patches, build definitions, dependency versions, and other material
  needed by the applicable licenses to regenerate the binary.

Before publication, the maintainer must review the release channel's actual
source-delivery mechanism against GPLv3 section 6. This notice records the
obligation; it is not a substitute for supplying Corresponding Source.

## Game-owned material

FO4CloudShadows does not distribute Fallout 4's vanilla DFLight DXBC corpus.
Maintainer tooling may extract and hash a user-owned Fallout 4 1.10.163 corpus
for compatibility validation, but the corpus is excluded from release
archives. Fallout, Fallout 4, and Bethesda names and assets remain the property
of their respective owners.
