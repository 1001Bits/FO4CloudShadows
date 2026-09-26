# Matching source delivery

Distribute the runtime ZIP together with the matching Source ZIP and their
SHA-256 sidecars. Both archive names contain the runtime version and build ID.
The Git source is published at https://github.com/1001Bits/FO4CloudShadows;
the build-specific tag and packaged `RELEASE.json` identify the exact revision.

The Source ZIP includes the current project, build and release scripts, all five
runtime HLSL files, vendored Dear ImGui, license notices, and the exact upstream
source archives for statically linked Detours, spdlog/fmt and nlohmann-json.
Their vcpkg port recipes, triplet and installed ABI/SBOM evidence are preserved
under `third-party/vcpkg` and checked against `config/vcpkg-source-lock.json`.
`SOURCE-MANIFEST.sha256` authenticates every other source-package file.

CommonLibF4 and commonlib-shared are historical provenance and are not linked
by this build. Fallout's executables, game assets and native shader bytecode
are not part of the distribution. See `THIRD_PARTY_NOTICES.md` for component
provenance and `docs/BUILD.md` for reproduction commands.
