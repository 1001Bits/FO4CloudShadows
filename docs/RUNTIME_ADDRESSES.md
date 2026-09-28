# Built-in runtime addresses

Cloud Shadows no longer reads an Address Library database. It uses the reviewed
relative virtual addresses in `src/RuntimeAddresses.h` for exactly Fallout 4
1.10.163.0, 1.10.984.0, 1.11.221.0, 1.11.240.0 and Fallout 4 VR 1.2.72.0.
F4SE/F4SEVR is still required. Nearby versions and nonzero build revisions are
not accepted.

`RuntimeAPI::ProbeHost` validates the actual executable name and file version as
well as the extender's reported version before selecting a runtime. VR retains
the explicitly handled F4SEVR 0.6.21 proxy version. `ResolveRVA` adds the loaded
image base, so ASLR is supported, and rejects zero offsets, image/overflow bounds
violations and unexpected memory-page permissions. A missing address does not
fall back to another runtime's column.

The exported `F4SEPlugin_Version` record has both Address Library bits cleared
and lists only these supported runtimes (including the VR proxy).
[F4SE's plugin loader](https://github.com/ianpatt/f4se/blob/master/f4se/PluginManager.cpp)
checks these bits before calling the plugin's load entry point. Clearing them
is necessary in addition to replacing database lookups. The binary contract
test reads the exported record from the actual DLL and requires zero address
independence flags and the exact compatibility list.

## Migration verification, 27 September 2026

All 12 OG and all 12 AE entries match their original relocation IDs in the
locally installed version-specific databases. The IDs are retained in
`scripts/verify_runtime_addresses.py` as migration evidence. The ten active VR
addresses are unchanged from the previous source snapshot. Desktop cursor
addresses remain unsupported in VR; the cached SetShaders hook remains VR-only.

All 34 nonzero entries pass executable-version, x64 PE, image/section bounds and
readable/executable section checks against the local game images. Each function
entry is file-backed and contains code rather than an empty padding block.
The checker uses only Python's standard library and Windows version APIs.

| Private verification input | SHA-256 |
| --- | --- |
| OG Fallout4.exe 1.10.163.0 | `5b2a58004f1856e51235132ab304b20c1434017067703e4282e8b0d5bc539623` |
| OG version-1-10-163-0.bin | `d849ae8989c54a2ceebbc587c2a77f62a44f74d0c42a42e0bed30d65118d3395` |
| AE Fallout4.exe 1.11.240.0 | `fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8` |
| AE version-1-11-240-0.bin | `65985cc2259384a13cffb538d74776e422e62b0cd3766485a000620242b72b06` |
| VR Fallout4VR.exe 1.2.72.0 | `680de0e1615c238acf794ec8ca515f182e8c3c61e83c79ca2c0f584083f685f9` |

The game files and databases are verification inputs only, not distributed
payloads. Executable hashes identify this audit's inputs; the plugin continues
to use its existing exact-version and memory validation, not a file-hash gate.

To repeat the private check, substitute paths to your own installations:

```powershell
python scripts/verify_runtime_addresses.py `
  --og-exe 'D:/OG/Fallout4.exe' `
  --og-addresses 'D:/OG/Data/F4SE/Plugins/version-1-10-163-0.bin' `
  --ae-exe 'D:/AE/Fallout4.exe' `
  --ae-addresses 'D:/AE/Data/F4SE/Plugins/version-1-11-240-0.bin' `
  --vr-exe 'D:/VR/Fallout4VR.exe' `
  --ng-exe 'D:/NG/Fallout4.exe' `
  --ng-addresses 'D:/NG/Data/F4SE/Plugins/version-1-10-984-0.bin' `
  --ae221-exe 'D:/AE221/Fallout4.exe' `
  --ae221-addresses 'D:/AE221/Data/F4SE/Plugins/version-1-11-221-0.bin' `
  --output out/runtime-address-verification.json
```

The binary metadata regression was also run against the previous
`372f3a9044377c14` DLL: it correctly rejects that DLL's address flags of `6`.
The host-classification tests now cover every supported desktop pairing,
mismatched reported/actual versions, incorrect executable names, nearby
unsupported versions and nonzero build revisions.

These checks establish address parity and the absence of the loader dependency.
They do not substitute for an in-game launch without Address Library or the
remaining visual/performance acceptance matrix. No installed game files or
other mods' Address Library databases were changed for this migration.

## NG 1.10.984 and AE 1.11.221, 28 September 2026

The address table has independent NG and AE221 columns. All 12 addresses in
each new column match the corresponding local Address Library database and
pass executable-version, PE bounds and section-permission checks. Together
with the unchanged OG, AE240 and VR columns, the checker verifies 58 nonzero
addresses. The databases are private verification inputs, not dependencies.

NG uses the same renderer/function relocation IDs as AE. Its player and UI
queue IDs are respectively 2698073 and 2689091. Ghidra cross-references confirm
the NG player slot at RVA `0x0303ACA0`; the UIMessageQueue constructor at
`0x0196E1C0` writes its singleton to `0x02E665F0`.

Read-only analysis of the versioned programs in the existing Combined Ghidra
project confirmed the desktop BeginTechnique calling convention and the
renderer, camera, sky and target-manager fields used by this plugin:

| Evidence | 1.10.984 RVA | 1.11.221 RVA | Result |
| --- | --- | --- | --- |
| BSShader constructor | `0x020D2D60` | `0x022284D0` | FXP name at +0x188, type at +0x18 |
| BeginTechnique | `0x020D4940` | `0x0222A0B0` | Existing desktop five-argument ABI |
| RendererData constructor | `0x0170E310` | `0x01829190` | Existing desktop renderer prefix and target arrays |
| RenderTargetManager RT accessor | `0x017200F0` | `0x0183AF50` | Map at +0xDF4 |
| RenderTargetManager depth accessor | `0x01720110` | `0x0183AF70` | Map at +0xF84 |
| Graphics State SetCameraData | `0x01710940` | `0x0182B7C0` | Existing desktop camera-state block at +0x160 |
| DFLight SetupGeometry | `0x020E9B10` | `0x0223F1F0` | Separate allocation and scaled-viewport reciprocal dimensions |

`RuntimeLayout.h` selects these newer desktop layouts for both additions,
including the projection path already used by AE240. OG and VR retain their
existing layouts. Cloud shader authentication remains mandatory: adding a
runtime does not authorize unknown shader bytecode.

| New private executable input | SHA-256 |
| --- | --- |
| NG 1.10.984.0, unpacked analysis image | `6c54e6152b69cc976c3f2aa7bae2b99b86c056c596cbabce09ab8d60974bb537` |
| AE 1.11.221.0 | `488015e2010308bfa164d3720cb618deea7e1c098b1d61af530d258a6f17d27b` |

The extender versions come from the [official F4SE changelog](https://github.com/ianpatt/f4se/blob/master/f4se_whatsnew.txt):
0.7.2 adds 1.10.984, 0.7.3 retains that runtime, and 0.7.8 adds 1.11.221.
The release tests cover exact host matching, rejection of omitted RVA columns,
the newer shader-name/target-map layout, and actual WARP projection using each
new runtime's selected constants. Live launches and visual acceptance on NG984
and AE221 have not been performed; this is an explicit remaining validation
limit, not a claim of completed gameplay testing.
