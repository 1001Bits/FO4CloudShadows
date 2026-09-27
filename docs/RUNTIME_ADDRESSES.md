# Built-in runtime addresses

Cloud Shadows no longer reads an Address Library database. It uses the reviewed
relative virtual addresses in `src/RuntimeAddresses.h` for exactly Fallout 4
1.10.163.0, Fallout 4 1.11.240.0 and Fallout 4 VR 1.2.72.0. F4SE/F4SEVR is still
required. This does not broaden the supported executable versions.

`RuntimeAPI::ProbeHost` validates the actual executable name and file version as
well as the extender's reported version before selecting a runtime. VR retains
the explicitly handled F4SEVR 0.6.21 proxy version. `ResolveRVA` adds the loaded
image base, so ASLR is supported, and rejects zero offsets, image/overflow bounds
violations and unexpected memory-page permissions. A missing address does not
fall back to another runtime's column.

The exported `F4SEPlugin_Version` record has both Address Library bits cleared
and lists only the existing supported runtimes (including the VR proxy).
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
  --output out/runtime-address-verification.json
```

The binary metadata regression was also run against the previous
`372f3a9044377c14` DLL: it correctly rejects that DLL's address flags of `6`.
The host-classification tests cover unsupported desktop versions, including
1.10.980/984, 1.11.221, a future 1.11.241 and nonzero build revisions.

These checks establish address parity and the absence of the loader dependency.
They do not substitute for an in-game launch without Address Library or the
remaining visual/performance acceptance matrix. No installed game files or
other mods' Address Library databases were changed for this migration.
