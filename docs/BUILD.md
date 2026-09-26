# Building and packaging

Requirements: Windows x64, Visual Studio 2022 with Desktop development with C++,
a Windows SDK, CMake 3.25 or later, Git, PowerShell 7 and vcpkg. The manifest pins
vcpkg baseline `36118ef68885436fd2a999188216337365856ad4` and uses the
`x64-windows-static-md` triplet. Dear ImGui is vendored; CommonLib is not a build
dependency of the current renderer.

Set `VCPKG_ROOT` to the vcpkg checkout and use the pinned baseline. From the
repository root in PowerShell:

```powershell
cmake --preset validation -DFO4CS_ENABLE_DEVELOPER_TOOLS=OFF -DFO4CS_ENABLE_PRIVATE_PROFILING=OFF
cmake --build --preset validation
ctest --preset validation
cmake --build out/build/validation --config Release --target release-package
```

The package target runs the core tests, verifies the DLL's exports, version,
release feature flags and runtime assets, and creates matching runtime/source
ZIPs plus checksums under `out/build/validation`. It requires the pinned vcpkg
checkout, installed package metadata and cached source archives listed in
`config/vcpkg-source-lock.json`. No game installation is needed for the core
tests, which use generated GPU fixtures and D3D11 WARP.

The runtime ZIP has a mod-manager layout with `F4SE`, `Shaders`, `MCM` and
`Interface` at its root. The source ZIP includes build scripts, vendored sources,
dependency source archives and a manifest that verifies all packaged files.
Compiled symbols and game-owned shader dumps are excluded from both ZIPs.

For build `acc6bcd8dfab4548`, the recorded compiler was MSVC 19.40.33813.0 and the
Windows SDK was 10.0.22621.0. Release used `/O2 /Ob2 /DNDEBUG` and the default
MSVC runtime setting `MultiThreadedDLL`. The build ID fingerprints source,
configuration and toolchain inputs; different toolchains may produce a different
ID and binary even when building the same Git commit. Documentation and release
tooling are outside the runtime fingerprint.

On hosts where the environment contains both `PATH` and `Path`, MSBuild can
reject the duplicate names. Normalize the environment before invoking CMake,
or start a clean Developer PowerShell session. Do not run the deployment or game
launch helpers from unrelated recovered work trees as part of a normal build.
