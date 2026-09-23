// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Derived in part from Dynamic Reflections Fallout 4 commit
// 3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7. Licensed under GPLv3 with
// that project's exceptions; see /THIRD_PARTY_NOTICES.md and /LICENSE.

#include "DXBCPatch.h"

#include <d3d11.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SIE
{
	/// Loads VanillaDXBC DFLight pixel shaders, applies a DXBC binary patch
	/// that multiplies only the exact direct-sun intermediate(s) by the
	/// screen-space cloud factor bound at t47, and caches the resulting
	/// ID3D11PixelShader objects for runtime swap. Environment and ambient
	/// addends remain vanilla.
	///
	/// Standalone cloud-shadow variant — single-texture MUL at t47 only.
	class DFLightPatcher
	{
	public:
		~DFLightPatcher();

		/// Load the runtime-specific VanillaDXBC corpus
		/// (DFLight/PS for flat/AE, DFLightVR/PS for VR), apply the patch, and
		/// create ID3D11PixelShader objects. Call once after the D3D device is
		/// available.
		void Initialize(ID3D11Device* device);

		/// Look up a patched PS by DXBC bytecode hash. Returns one retained
		/// reference which the caller must Release.
		ID3D11PixelShader* GetPatchedPSByHash(uint64_t hash) const;

		/// Ensure that the hash associated with `vanillaPS` has a strict patched
		/// replacement. Shader identity is stored as D3D private data rather than
		/// in a raw-pointer registry, so destroyed/reused COM addresses cannot
		/// select a stale replacement.
		void RegisterVanillaPS(uint64_t hash, ID3D11PixelShader* vanillaPS);

		/// Associate a successfully-bound DFLight descriptor with the exact
		/// strictly-patched shader object. Returns one retained reference, or
		/// nullptr when the descriptor/hash is not an audited directional-sun
		/// variant. The caller must Release a non-null result.
		ID3D11PixelShader* RegisterDescriptor(
			uint32_t pixelDescriptor,
			ID3D11PixelShader* vanillaPS);

		/// Acquire a retained replacement for an authenticated descriptor. The
		/// caller owns one reference and must Release it. The AddRef occurs while
		/// device teardown is excluded, so the returned shader cannot race
		/// Release().
		ID3D11PixelShader* AcquireDescriptor(
			uint32_t pixelDescriptor,
			ID3D11PixelShader* vanillaPS);

		/// Record the DXBC hash for every PS the game creates, so we can later
		/// diagnose why a given bound PS wasn't matched to our patched pool.
		void RecordPSHash(ID3D11PixelShader* vanillaPS, uint64_t hash);

		/// Reverse lookup — returns 0 if no hash was recorded for this PS.
		uint64_t LookupPSHash(ID3D11PixelShader* vanillaPS) const;

		/// Attach/read the immutable bytecode identity used by every shader stage.
		/// The first successful writer owns the device child's identity; a later
		/// attempt to change it is rejected rather than overwriting trusted data.
		static bool RecordShaderHash(
			ID3D11DeviceChild* shader, uint64_t hash) noexcept;
		static uint64_t LookupShaderHash(
			ID3D11DeviceChild* shader) noexcept;

		/// True if this hash is in our pre-patched pool.
		bool IsKnownHash(uint64_t hash) const;

		/// Strictly inspect all bytecode captured before Initialize(). This
		/// retroactively prepares sunlight shaders without retaining raw shader
		/// pointers. Returns the number of newly-created patched variants.
		uint32_t RematchRecordedShaders();

		/// Given a vanilla PS pointer (as returned by PSGetShader), acquire the
		/// patched replacement. The caller must Release a non-null result.
		ID3D11PixelShader* GetSwapForVanillaPS(ID3D11PixelShader* vanillaPS);

		/// Explicit retained alias used by the live draw hook.
		ID3D11PixelShader* AcquireSwapForVanillaPS(
			ID3D11PixelShader* vanillaPS);

		/// Compute FNV-1a hash of DXBC bytecode.
		static uint64_t HashDXBC(const void* data, size_t size);

		/// Record the raw bytecode for a pixel shader the game created, keyed
		/// by hash, so strict runtime patching works without loose shader files.
		void StoreBytecode(uint64_t hash, const void* bytes, size_t size);

		/// Release device-owned shader objects while retaining validated source
		/// bytecode so Initialize() can recreate them on the next D3D device.
		void Release();

		/// Final/full reset. Releases device objects and discards captured bytecode.
		/// Normal renderer device teardown must call Release(), not Reset().
		void Reset();

		/// True once the strict patch payload and device are initialized.
		bool IsInitialized() const { return initialized.load(std::memory_order_acquire); }

		/// True only after at least one strictly-validated sunlight shader exists.
		bool IsReady() const { return ready.load(std::memory_order_acquire); }

	private:
		/// Build the t47 cloud-load payload. The DXBC patcher inserts it at the
		/// strictly matched terminal sunlight site and then multiplies only the
		/// signature's direct-sun targets. Returns the payload's relative factor
		/// temp by reference.
		DXBCPatch BuildCloudShadowPatch(uint32_t& factorTempRegister);

		/// Pre-patch the optional loose vanilla corpus. Runtime bytecode capture is
		/// authoritative for standalone installs that do not ship this directory.
		void ProcessDXBCDirectory(
			ID3D11Device* device,
			const std::filesystem::path& dir,
			const DXBCPatch& patch,
			uint32_t factorTempRegister);

		/// Strictly patch/create a runtime-captured shader if supported, or cache
		/// the hash as unsupported. Never accepts a non-sunlight terminal layout.
		ID3D11PixelShader* GetOrCreatePatchedShader(uint64_t hash);
		ID3D11PixelShader* AcquirePatchedShader(uint64_t hash);
		void DiscardBytecode(uint64_t hash);
		void ReleaseResources(bool clearCapturedBytecode);

		// vanilla DXBC hash → patched PS
		std::unordered_map<uint64_t, ID3D11PixelShader*> patchedByHash;
		std::unordered_set<uint64_t> unsupportedHashes;
		mutable std::shared_mutex patchedMapMutex;

		// Raw DXBC bytecode for every PS the game creates, keyed by hash. Runtime
		// strict patching makes loose VanillaDXBC files an optional optimization.
		// Unsupported entries are discarded as soon as they are classified.
		// Supported entries are retained so a D3D device rebuild can recreate the
		// patched COM objects without requiring loose corpus files.
		std::unordered_map<uint64_t, std::vector<uint8_t>> bytecodeByHash;
		mutable std::shared_mutex bytecodeMutex;
		size_t capturedBytecodeBytes = 0;

		DXBCPatch cachedSunPatch;
		uint32_t cachedFactorTempRegister = 0xFFFFFFFF;
		ID3D11Device* patchDevice = nullptr;

		// Shader creation is rare and serialized. Recursive acquisition is needed
		// because Initialize() performs the captured-bytecode rematch inline.
		std::recursive_mutex lifecycleMutex;
		std::atomic<bool> initialized{ false };
		std::atomic<bool> ready{ false };
	};
}
