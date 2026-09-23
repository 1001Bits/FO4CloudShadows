// SPDX-License-Identifier: GPL-3.0-only
#include "DFLightPatcher.h"

// Derived in part from Dynamic Reflections Fallout 4 commit
// 3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7. Licensed under GPLv3 with
// that project's exceptions; see /THIRD_PARTY_NOTICES.md and /LICENSE.

#include "DXBCPatcher.h"
#include "DFLightDescriptor.h"
#include "RuntimeAPI.h"

#include <array>
#include <atomic>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <spdlog/spdlog.h>
#include <vector>

namespace SIE
{
	namespace
	{
		thread_local bool g_creatingPatchedShader = false;

		// ID3D11DeviceChild private-data key used to bind a live shader object to
		// its immutable DXBC identity. D3D owns/copies the value with the object,
		// so no raw COM-pointer lifetime registry is required.
		constexpr GUID kCloudShadowDXBCHashGuid{
			0x7f62e43f, 0xa75a, 0x48e1,
			{ 0x9d, 0x96, 0x35, 0x1c, 0x59, 0x92, 0x11, 0xa7 }
		};

		constexpr size_t kMaxCapturedShaderBytes = 4u * 1024u * 1024u;
		constexpr size_t kMaxCapturedBytecodeCache = 128u * 1024u * 1024u;

		struct ScopedPatchedShaderCreation
		{
			ScopedPatchedShaderCreation() : previous(g_creatingPatchedShader)
			{
				g_creatingPatchedShader = true;
			}
			~ScopedPatchedShaderCreation() { g_creatingPatchedShader = previous; }
			bool previous;
		};

		// Parse one operand and record immediate TEMP/INPUT register-index DWORDs.
		// Relative addressing cannot be rebased safely when the payload is spliced
		// into a target shader, so compiler output using it is rejected fail-closed.
		bool ScanOperandForFixups(
			const std::vector<uint32_t>& instr,
			uint32_t& pos,
			std::vector<uint32_t>& tempPositions,
			std::vector<uint32_t>& inputPositions,
			uint32_t depth = 0)
		{
			if (pos >= instr.size() || depth > 8)
				return false;

			const uint32_t operandToken = instr[pos++];
			uint32_t extendedToken = operandToken;
			while ((extendedToken >> 31) != 0) {
				if (pos >= instr.size())
					return false;
				extendedToken = instr[pos++];
			}

			const uint32_t type = (operandToken >> 12) & 0xFF;
			const uint32_t componentCount = operandToken & 0x3;
			if (type == 4 || type == 5) { // immediate32 / immediate64
				uint32_t values = 0;
				if (componentCount == 1)
					values = 1;
				else if (componentCount == 2)
					values = 4;
				else
					return false;
				if (type == 5)
					values *= 2;
				if (values > instr.size() - pos)
					return false;
				pos += values;
				return true;
			}

			const uint32_t indexDimension = (operandToken >> 20) & 0x3;
			if ((type == 0 || type == 1) && indexDimension != 1)
				return false;
			for (uint32_t dimension = 0; dimension < indexDimension; dimension++) {
				const uint32_t representation =
					(operandToken >> (22 + 3 * dimension)) & 0x7;
				switch (representation) {
				case 0: // immediate32
					if (pos >= instr.size())
						return false;
					if (type == 0)
						tempPositions.push_back(pos);
					else if (type == 1)
						inputPositions.push_back(pos);
					pos++;
					break;
				case 1: // immediate64
					if (type == 0 || type == 1 || instr.size() - pos < 2)
						return false;
					pos += 2;
					break;
				case 2: // relative
					if (type == 0 || type == 1 ||
						!ScanOperandForFixups(instr, pos, tempPositions, inputPositions, depth + 1)) {
						return false;
					}
					break;
				case 3: // immediate32 + relative
					if (type == 0 || type == 1 || pos >= instr.size())
						return false;
					pos++;
					if (!ScanOperandForFixups(instr, pos, tempPositions, inputPositions, depth + 1))
						return false;
					break;
				case 4: // immediate64 + relative
					if (type == 0 || type == 1 || instr.size() - pos < 2)
						return false;
					pos += 2;
					if (!ScanOperandForFixups(instr, pos, tempPositions, inputPositions, depth + 1))
						return false;
					break;
				default:
					return false;
				}
			}
			return true;
		}

		// Scan a complete instruction. This is deliberately a validator as well as
		// a fixup finder: malformed or unsupported FXC output is never injected.
		bool ScanInstructionForFixups(
			const std::vector<uint32_t>& instr,
			std::vector<uint32_t>& tempPositions,
			std::vector<uint32_t>& inputPositions)
		{
			if (instr.size() < 2)
				return false;

			uint32_t pos = 1; // skip opcode token and its extension chain
			uint32_t extendedToken = instr[0];
			while ((extendedToken >> 31) != 0) {
				if (pos >= instr.size())
					return false;
				extendedToken = instr[pos++];
			}
			while (pos < instr.size()) {
				if (!ScanOperandForFixups(instr, pos, tempPositions, inputPositions))
					return false;
			}
			return pos == instr.size();
		}

		bool IsDirectionalSunDescriptor(uint32_t descriptor)
		{
			return IsPotentialDirectionalSunDescriptor(descriptor);
		}
	}

	DXBCPatch DFLightPatcher::BuildCloudShadowPatch(uint32_t& factorTempRegister)
	{
		DXBCPatch patch;
		factorTempRegister = 0xFFFFFFFF;

		// Sample cloud visibility at SV_Position. The snippet's o0 write is
		// rewritten below to an added temp; vanilla DFLight outputs are never
		// redirected or post-multiplied.
		const char* snippetSrc =
			"Texture2D<float> T47 : register(t47);\n"
			"float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
			"  float cloud = T47.Load(int3(pos.xy, 0));\n"
			"  return float4(cloud, cloud, cloud, cloud);\n"
			"}\n";

		ID3DBlob* snippetBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		HRESULT hr = D3DCompile(snippetSrc, strlen(snippetSrc), nullptr, nullptr, nullptr,
			"main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &snippetBlob, &errorBlob);
		if (errorBlob) {
			SPDLOG_WARN("[DFLightPatcher] Snippet compile warnings: {}",
				static_cast<const char*>(errorBlob->GetBufferPointer()));
			errorBlob->Release();
		}
		if (FAILED(hr) || !snippetBlob) {
			SPDLOG_ERROR("[DFLightPatcher] Failed to compile cloud-shadow snippet (hr=0x{:X})",
				static_cast<uint32_t>(hr));
			return patch;
		}
		if (snippetBlob->GetBufferSize() > std::numeric_limits<uint32_t>::max()) {
			snippetBlob->Release();
			return patch;
		}

		DXBCPatcher::ParsedDXBC snippetDXBC{};
		if (!DXBCPatcher::Parse(
				static_cast<const uint8_t*>(snippetBlob->GetBufferPointer()),
				static_cast<uint32_t>(snippetBlob->GetBufferSize()), snippetDXBC) ||
			snippetDXBC.shex.usesDynamicLinkage ||
			snippetDXBC.shex.declaredResourceRegisters.size() != 1 ||
			!snippetDXBC.shex.declaredResourceRegisters.contains(47)) {
			SPDLOG_ERROR("[DFLightPatcher] Cloud payload resource ABI is not exactly Texture2D t47");
			snippetBlob->Release();
			return patch;
		}

		patch.newDeclarations = DXBCPatcher::ExtractDeclarations(
			snippetBlob, { DXBCOpcodes::DCL_RESOURCE });
		if (patch.newDeclarations.empty()) {
			SPDLOG_ERROR("[DFLightPatcher] No resource declarations found in snippet");
			snippetBlob->Release();
			return patch;
		}

		auto bodyInstrs = DXBCPatcher::CompileSnippetInstructions(snippetSrc, "main", "ps_5_0");
		snippetBlob->Release();

		if (bodyInstrs.empty()) {
			SPDLOG_ERROR("[DFLightPatcher] No body instructions from snippet");
			return patch;
		}

		uint32_t maxSnippetTemp = 0;
		for (const auto& instr : bodyInstrs) {
			std::vector<uint32_t> tempPos, inputPos;
			if (!ScanInstructionForFixups(instr, tempPos, inputPos)) {
				SPDLOG_ERROR("[DFLightPatcher] Unsupported operand encoding in cloud payload");
				return patch;
			}
			for (uint32_t p : tempPos) {
				if (instr[p] >= 4096) {
					SPDLOG_ERROR("[DFLightPatcher] Cloud payload temp register is out of range");
					return patch;
				}
				if (instr[p] + 1 > maxSnippetTemp)
					maxSnippetTemp = instr[p] + 1;
			}
		}

		// Temp register layout relative to the target shader's temp base:
		//   0..maxSnippetTemp-1: snippet's own temps
		//   maxSnippetTemp:      sampled cloud factor
		uint32_t factorReg = maxSnippetTemp;
		patch.additionalTempRegisters = maxSnippetTemp + 1;

		// Redirect only the snippet's o0 write to factorReg.
		uint32_t outputRedirectCount = 0;
		for (auto& instr : bodyInstrs) {
			if (instr.size() < 2)
				continue;
			uint32_t firstOpPos = 1;
			{
				uint32_t tok = instr[0];
				while ((tok >> 31) && firstOpPos < static_cast<uint32_t>(instr.size())) {
					tok = instr[firstOpPos];
					firstOpPos++;
				}
			}
			if (firstOpPos >= instr.size())
				continue;

			uint32_t operandToken = instr[firstOpPos];
			uint32_t opType = (operandToken >> 12) & 0xFF;
			if (opType != 2)
				continue;

			if (((operandToken >> 20) & 0x3) != 1 ||
				((operandToken >> 22) & 0x7) != 0) {
				SPDLOG_ERROR("[DFLightPatcher] Cloud payload output is not immediately indexed");
				return patch;
			}
			uint32_t idxPos = firstOpPos + 1;
			uint32_t extendedOperandToken = operandToken;
			while ((extendedOperandToken >> 31) != 0) {
				if (idxPos >= instr.size())
					return patch;
				extendedOperandToken = instr[idxPos++];
			}
			if (idxPos >= instr.size() || instr[idxPos] != 0) {
				SPDLOG_ERROR("[DFLightPatcher] Cloud payload must write exactly SV_Target0");
				return patch;
			}

			instr[firstOpPos] = (operandToken & ~(0xFFu << 12));
			instr[idxPos] = factorReg;
			outputRedirectCount++;
		}

		if (outputRedirectCount != 1) {
			SPDLOG_ERROR("[DFLightPatcher] Expected one output write in snippet, found {}",
				outputRedirectCount);
			return patch;
		}

		for (auto& instr : bodyInstrs) {
			std::vector<uint32_t> tempPos, inputPos;
			if (!ScanInstructionForFixups(instr, tempPos, inputPos)) {
				SPDLOG_ERROR("[DFLightPatcher] Rewritten cloud payload failed operand validation");
				return patch;
			}
			patch.preRetInstructions.push_back(instr);
			patch.tempRemapPositions.push_back(tempPos);
			patch.svPositionFixupPositions.push_back(inputPos);
		}

		factorTempRegister = factorReg;
		SPDLOG_INFO("[DFLightPatcher] Sunlight payload built: {} decls, {} site instrs, "
			"snippetTemps={}, factorReg=r{}",
			patch.newDeclarations.size(), patch.preRetInstructions.size(),
			maxSnippetTemp, factorReg);

		return patch;
	}


	void DFLightPatcher::ProcessDXBCDirectory(
		ID3D11Device* device,
		const std::filesystem::path& dir,
		const DXBCPatch& patch,
		uint32_t factorTempRegister)
	{
		std::error_code fsError;
		if (!std::filesystem::exists(dir, fsError) || fsError) {
			SPDLOG_WARN("[DFLightPatcher] DFLight DXBC path not found: {}", dir.string());
			return;
		}

		uint32_t loaded = 0, patched = 0, signatureSkipped = 0;
		uint32_t failed = 0, disasmFailed = 0, duplicates = 0;
		std::array<uint32_t, 10> variants{};

		std::filesystem::directory_iterator it(dir, fsError), end;
		for (; !fsError && it != end; it.increment(fsError)) {
			const auto& entry = *it;
			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError ||
				entry.path().extension() != ".dxbc") {
				continue;
			}

			std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
			if (!file.is_open())
				continue;

			const std::streamoff fileSize = file.tellg();
			if (fileSize <= 0 ||
				static_cast<uint64_t>(fileSize) > kMaxCapturedShaderBytes ||
				static_cast<uint64_t>(fileSize) >
					static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
				failed++;
				continue;
			}
			file.seekg(0, std::ios::beg);

			std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
			file.read(reinterpret_cast<char*>(fileData.data()),
				static_cast<std::streamsize>(fileSize));
			if (!file || file.gcount() != static_cast<std::streamsize>(fileSize)) {
				failed++;
				continue;
			}
			loaded++;

			ID3DBlob* vanillaBlob = nullptr;
			if (FAILED(D3DCreateBlob(fileData.size(), &vanillaBlob)))
				continue;
			memcpy(vanillaBlob->GetBufferPointer(), fileData.data(), fileData.size());

			DXBCPatcher::SunShadowPatchInfo patchInfo;
			ID3DBlob* patchedBlob = DXBCPatcher::PatchSunShadowShader(
				vanillaBlob, patch, factorTempRegister, &patchInfo);
			vanillaBlob->Release();

			if (!patchedBlob) {
				if (patchInfo.signatureMatched)
					failed++;
				else
					signatureSkipped++;
				continue;
			}
			if (patchInfo.signatureVariant >= 1 && patchInfo.signatureVariant <= variants.size())
				variants[patchInfo.signatureVariant - 1]++;
			else {
				patchedBlob->Release();
				failed++;
				continue;
			}

			{
				ID3DBlob* disasmBlob = nullptr;
				HRESULT disasmHr = D3DDisassemble(
					patchedBlob->GetBufferPointer(),
					patchedBlob->GetBufferSize(),
					0, nullptr, &disasmBlob);
				if (disasmBlob)
					disasmBlob->Release();
				if (FAILED(disasmHr)) {
					disasmFailed++;
					patchedBlob->Release();
					continue;
				}
			}

			ID3D11PixelShader* ps = nullptr;
			HRESULT createHr;
			{
				ScopedPatchedShaderCreation guard;
				createHr = device->CreatePixelShader(
					patchedBlob->GetBufferPointer(),
					patchedBlob->GetBufferSize(),
					nullptr, &ps);
			}
			patchedBlob->Release();

			if (FAILED(createHr) || !ps) {
				failed++;
				continue;
			}

			uint64_t hash = HashDXBC(fileData.data(), fileData.size());
			bool inserted = false;
			{
				std::unique_lock mapLock(patchedMapMutex);
				auto [existing, wasInserted] = patchedByHash.emplace(hash, ps);
				inserted = wasInserted;
				if (!wasInserted) {
					ps->Release();
					duplicates++;
				}
			}
			// Retain the validated stock bytes so a new D3D device can recreate
			// the strict shader even when this optional loose corpus disappears.
			StoreBytecode(hash, fileData.data(), fileData.size());
			if (inserted)
				patched++;
		}
		if (fsError) {
			SPDLOG_ERROR("[DFLightPatcher] Error scanning {}: {}", dir.string(), fsError.message());
		}

		SPDLOG_INFO("[DFLightPatcher] DFLight strict sunlight splice: {} blobs loaded, {} patched, "
			"{} duplicates, {} signature-skipped, {} failed, {} disasm-failed, "
			"variants={}/{}/{}/{}/{}/{}/{}/{}/{}/{}",
			loaded, patched, duplicates, signatureSkipped, failed, disasmFailed,
			variants[0], variants[1], variants[2], variants[3], variants[4],
			variants[5], variants[6], variants[7], variants[8], variants[9]);
	}

	void DFLightPatcher::Initialize(ID3D11Device* device)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		if (!device) {
			SPDLOG_ERROR("[DFLightPatcher] Initialize called with null device");
			return;
		}
		if (initialized.load(std::memory_order_acquire) && patchDevice == device) {
			RematchRecordedShaders();
			return;
		}
		if (initialized.load(std::memory_order_acquire) || patchDevice) {
			SPDLOG_WARN("[DFLightPatcher] D3D device changed; rebuilding all patched shaders");
			ReleaseResources(false);
		}

		uint32_t factorTempRegister = 0xFFFFFFFF;
		auto dfLightPatch = BuildCloudShadowPatch(factorTempRegister);
		if (dfLightPatch.preRetInstructions.empty() || factorTempRegister == 0xFFFFFFFF) {
			SPDLOG_ERROR("[DFLightPatcher] DFLight patch is empty, aborting init");
			return;
		}

		cachedSunPatch = dfLightPatch;
		cachedFactorTempRegister = factorTempRegister;
		if (patchDevice)
			patchDevice->Release();
		patchDevice = device;
		patchDevice->AddRef();
		// Initialization and operational readiness are deliberately separate.
		// A valid payload with zero mapped sunlight shaders is not usable yet.
		initialized.store(true, std::memory_order_release);
		ready.store(false, std::memory_order_release);

		auto& runtime = FO4CS::RuntimeAPI::GetSingleton();
		const bool vrTarget =
			runtime.Target() ==
			FO4CS::F4SECompat::RuntimeTarget::kVR;
		const auto host = runtime.Host();
		if (!host.executablePath.empty() &&
			host.executablePath.has_parent_path()) {
			const auto corpusPath = host.executablePath.parent_path() /
				(vrTarget
					? std::filesystem::path("Data/Shaders/VanillaDXBC/DFLightVR/PS")
					: std::filesystem::path("Data/Shaders/VanillaDXBC/DFLight/PS"));
			ProcessDXBCDirectory(
				device,
				corpusPath,
				dfLightPatch,
				factorTempRegister);
		} else {
			SPDLOG_WARN(
				"[DFLightPatcher] Optional DFLight corpus skipped: "
				"validated host executable directory is unavailable");
		}

		// Bootstrap initialization can occur after the game created its DFLight
		// shaders. Strictly patch and register every captured candidate now.
		uint32_t rematched = RematchRecordedShaders();
		uint32_t hashCount = 0;
		{
			std::shared_lock mapLock(patchedMapMutex);
			hashCount = static_cast<uint32_t>(patchedByHash.size());
		}
		ready.store(hashCount != 0, std::memory_order_release);
		if (hashCount == 0) {
			if (vrTarget) {
				SPDLOG_WARN("[DFLightPatcher] Initialized but NOT READY: no exact VR DFLight sunlight terminal match was attested in the loaded corpus or captured runtime bytecode; VR sunlight patching remains fail-closed");
			} else {
				SPDLOG_WARN("[DFLightPatcher] Initialized but NOT READY: no strictly validated sunlight shader is available yet");
			}
		} else {
			SPDLOG_INFO("[DFLightPatcher] READY: {} unique strict hashes, {} runtime variants prepared",
				hashCount, rematched);
		}
	}

	uint64_t DFLightPatcher::HashDXBC(const void* data, size_t size)
	{
		uint64_t hash = 0xcbf29ce484222325ULL;
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; i++) {
			hash ^= bytes[i];
			hash *= 0x100000001b3ULL;
		}
		return hash;
	}

	ID3D11PixelShader* DFLightPatcher::GetPatchedPSByHash(uint64_t hash) const
	{
		std::shared_lock mapLock(patchedMapMutex);
		auto it = patchedByHash.find(hash);
		if (it != patchedByHash.end()) {
			it->second->AddRef();
			return it->second;
		}
		return nullptr;
	}

	ID3D11PixelShader* DFLightPatcher::GetOrCreatePatchedShader(uint64_t hash)
	{
		if (g_creatingPatchedShader)
			return nullptr;

		bool unsupported = false;
		{
			std::shared_lock mapLock(patchedMapMutex);
			auto existing = patchedByHash.find(hash);
			if (existing != patchedByHash.end())
				return existing->second;
			unsupported = unsupportedHashes.contains(hash);
		}
		if (unsupported) {
			DiscardBytecode(hash);
			return nullptr;
		}

		// Protect the borrowed device pointer and cached payload against Release()
		// while the slow patch/create path is active. Initialize() invokes this
		// path recursively during rematching, hence the recursive mutex.
		std::lock_guard lifecycleLock(lifecycleMutex);
		if (!initialized.load(std::memory_order_acquire) || !patchDevice ||
			cachedSunPatch.preRetInstructions.empty() ||
			cachedFactorTempRegister == 0xFFFFFFFF) {
			return nullptr;
		}

		{
			std::shared_lock mapLock(patchedMapMutex);
			auto existing = patchedByHash.find(hash);
			if (existing != patchedByHash.end())
				return existing->second;
			unsupported = unsupportedHashes.contains(hash);
		}
		if (unsupported) {
			DiscardBytecode(hash);
			return nullptr;
		}

		auto markUnsupported = [this, hash]() {
			// Keep lock ordering map -> bytecode consistent with StoreBytecode().
			std::unique_lock mapLock(patchedMapMutex);
			unsupportedHashes.insert(hash);
			DiscardBytecode(hash);
		};
		auto logRetryableFailure = [hash](const char* stage, HRESULT hr) {
			static std::atomic<uint32_t> failureCount{ 0 };
			const uint32_t n = ++failureCount;
			if (n <= 10 || (n % 50) == 0) {
				SPDLOG_WARN("[DFLightPatcher] Retryable runtime patch failure #{} at {} "
					"for hash=0x{:016x} (hr=0x{:08X})",
					n, stage, hash, static_cast<uint32_t>(hr));
			}
		};

		std::vector<uint8_t> bytes;
		{
			std::shared_lock bytecodeLock(bytecodeMutex);
			auto it = bytecodeByHash.find(hash);
			if (it == bytecodeByHash.end())
				return nullptr;
			bytes = it->second;
		}

		ID3DBlob* vanillaBlob = nullptr;
		const HRESULT blobHr = D3DCreateBlob(bytes.size(), &vanillaBlob);
		if (FAILED(blobHr) || !vanillaBlob) {
			logRetryableFailure("D3DCreateBlob", blobHr);
			return nullptr;
		}
		memcpy(vanillaBlob->GetBufferPointer(), bytes.data(), bytes.size());

		DXBCPatcher::SunShadowPatchInfo patchInfo;
		ID3DBlob* patchedBlob = DXBCPatcher::PatchSunShadowShader(
			vanillaBlob, cachedSunPatch, cachedFactorTempRegister, &patchInfo);
		vanillaBlob->Release();
		if (!patchedBlob) {
			if (!patchInfo.signatureMatched)
				markUnsupported();
			else
				logRetryableFailure("strict splice", E_FAIL);
			return nullptr;
		}

		ID3DBlob* disasmBlob = nullptr;
		HRESULT disasmHr = D3DDisassemble(
			patchedBlob->GetBufferPointer(), patchedBlob->GetBufferSize(),
			0, nullptr, &disasmBlob);
		if (disasmBlob)
			disasmBlob->Release();
		if (FAILED(disasmHr)) {
			patchedBlob->Release();
			logRetryableFailure("D3DDisassemble", disasmHr);
			return nullptr;
		}

		ID3D11PixelShader* created = nullptr;
		HRESULT createHr;
		{
			ScopedPatchedShaderCreation guard;
			createHr = patchDevice->CreatePixelShader(
				patchedBlob->GetBufferPointer(), patchedBlob->GetBufferSize(),
				nullptr, &created);
		}
		patchedBlob->Release();
		if (FAILED(createHr) || !created) {
			logRetryableFailure("CreatePixelShader", createHr);
			return nullptr;
		}

		ID3D11PixelShader* result = created;
		{
			std::unique_lock mapLock(patchedMapMutex);
			auto [it, inserted] = patchedByHash.emplace(hash, created);
			if (!inserted) {
				created->Release();
				result = it->second;
			}
		}
		// Keep the validated bytes for a future device generation. Unsupported
		// bytecode is the only classified data removed from this bounded cache.
		ready.store(true, std::memory_order_release);

		static std::atomic<uint32_t> runtimePatchCount = 0;
		uint32_t n = ++runtimePatchCount;
		if (n <= 10 || (n % 25) == 0) {
			SPDLOG_INFO("[DFLightPatcher] Strict runtime sunlight patch #{} hash=0x{:016x} variant={}",
				n, hash, patchInfo.signatureVariant);
		}
		return result;
	}

	void DFLightPatcher::DiscardBytecode(uint64_t hash)
	{
		std::unique_lock bytecodeLock(bytecodeMutex);
		auto it = bytecodeByHash.find(hash);
		if (it == bytecodeByHash.end())
			return;
		capturedBytecodeBytes -= it->second.size();
		bytecodeByHash.erase(it);
	}

	void DFLightPatcher::RegisterVanillaPS(uint64_t hash, ID3D11PixelShader* vanillaPS)
	{
		if (!vanillaPS || g_creatingPatchedShader)
			return;
		ID3D11PixelShader* patched = GetOrCreatePatchedShader(hash);
		if (!patched)
			return;
		static std::atomic<uint32_t> hitCount = 0;
		uint32_t n = ++hitCount;
		if (n == 1 || n == 10 || n == 50 || n == 100 || (n % 50) == 0) {
			SPDLOG_INFO("[DFLightPatcher] Registered DFLight PS #{} hash=0x{:016x} ps={}",
				n, hash, (void*)vanillaPS);
		}
	}

	ID3D11PixelShader* DFLightPatcher::RegisterDescriptor(
		uint32_t pixelDescriptor,
		ID3D11PixelShader* vanillaPS)
	{
		if (!vanillaPS || !IsDirectionalSunDescriptor(pixelDescriptor))
			return nullptr;

		const uint64_t hash = LookupPSHash(vanillaPS);
		if (!hash)
			return nullptr;

		return AcquirePatchedShader(hash);
	}

	ID3D11PixelShader* DFLightPatcher::AcquirePatchedShader(uint64_t hash)
	{
		// GetOrCreatePatchedShader nests this lock. Keep it held through AddRef so
		// ReleaseResources cannot erase/release the cache entry in between.
		std::lock_guard lifecycleLock(lifecycleMutex);
		auto* shader = GetOrCreatePatchedShader(hash);
		if (shader)
			shader->AddRef();
		return shader;
	}

	ID3D11PixelShader* DFLightPatcher::AcquireDescriptor(
		uint32_t pixelDescriptor,
		ID3D11PixelShader* vanillaPS)
	{
		if (!vanillaPS || !IsDirectionalSunDescriptor(pixelDescriptor))
			return nullptr;
		const uint64_t hash = LookupPSHash(vanillaPS);
		return hash ? AcquirePatchedShader(hash) : nullptr;
	}

	ID3D11PixelShader* DFLightPatcher::GetSwapForVanillaPS(ID3D11PixelShader* vanillaPS)
	{
		return AcquireSwapForVanillaPS(vanillaPS);
	}

	ID3D11PixelShader* DFLightPatcher::AcquireSwapForVanillaPS(
		ID3D11PixelShader* vanillaPS)
	{
		const uint64_t hash = LookupPSHash(vanillaPS);
		return hash ? AcquirePatchedShader(hash) : nullptr;
	}

	void DFLightPatcher::RecordPSHash(ID3D11PixelShader* vanillaPS, uint64_t hash)
	{
		if (!vanillaPS || g_creatingPatchedShader) return;
		if (!RecordShaderHash(vanillaPS, hash)) {
			static std::atomic<uint32_t> failures{ 0 };
			const uint32_t n = ++failures;
			if (n <= 5) {
				SPDLOG_ERROR("[DFLightPatcher] Immutable shader identity rejected for PS={}",
					(void*)vanillaPS);
			}
		}
	}

	uint64_t DFLightPatcher::LookupPSHash(ID3D11PixelShader* vanillaPS) const
	{
		return LookupShaderHash(vanillaPS);
	}

	bool DFLightPatcher::RecordShaderHash(
		ID3D11DeviceChild* shader, uint64_t hash) noexcept
	{
		if (!shader || hash == 0)
			return false;

		uint64_t existing = 0;
		UINT size = static_cast<UINT>(sizeof(existing));
		const HRESULT lookup = shader->GetPrivateData(
			kCloudShadowDXBCHashGuid, &size, &existing);
		if (SUCCEEDED(lookup))
			return size == sizeof(existing) && existing == hash;
		if (lookup != DXGI_ERROR_NOT_FOUND)
			return false;

		return SUCCEEDED(shader->SetPrivateData(
			kCloudShadowDXBCHashGuid,
			static_cast<UINT>(sizeof(hash)), &hash));
	}

	uint64_t DFLightPatcher::LookupShaderHash(
		ID3D11DeviceChild* shader) noexcept
	{
		if (!shader)
			return 0;
		uint64_t hash = 0;
		UINT size = static_cast<UINT>(sizeof(hash));
		return SUCCEEDED(shader->GetPrivateData(kCloudShadowDXBCHashGuid, &size, &hash)) &&
			size == sizeof(hash) ? hash : 0;
	}

	bool DFLightPatcher::IsKnownHash(uint64_t hash) const
	{
		std::shared_lock mapLock(patchedMapMutex);
		return patchedByHash.find(hash) != patchedByHash.end();
	}

	uint32_t DFLightPatcher::RematchRecordedShaders()
	{
		// Keep the before/after accounting and the complete rematch generation
		// coherent with Release()/Initialize(). This nests during Initialize().
		std::lock_guard lifecycleLock(lifecycleMutex);
		std::vector<uint64_t> capturedHashes;
		{
			std::shared_lock readLock(bytecodeMutex);
			capturedHashes.reserve(bytecodeByHash.size());
			for (const auto& [hash, bytes] : bytecodeByHash)
				capturedHashes.push_back(hash);
		}

		size_t before = 0;
		{
			std::shared_lock mapLock(patchedMapMutex);
			before = patchedByHash.size();
		}
		for (uint64_t hash : capturedHashes)
			(void)GetOrCreatePatchedShader(hash);

		size_t after = 0;
		{
			std::shared_lock mapLock(patchedMapMutex);
			after = patchedByHash.size();
		}
		const uint32_t added = static_cast<uint32_t>(after - before);
		SPDLOG_INFO("[DFLightPatcher] Retroactive strict scan: classified {} captured hashes, created {} sunlight variants",
			capturedHashes.size(), added);
		return added;
	}

	void DFLightPatcher::StoreBytecode(uint64_t hash, const void* bytes, size_t size)
	{
		if (!bytes || size == 0 || size > kMaxCapturedShaderBytes || g_creatingPatchedShader)
			return;

		// Keep the map lock through insertion so a concurrent unsupported
		// classification cannot discard the entry immediately before we add it.
		std::shared_lock mapLock(patchedMapMutex);
		if (unsupportedHashes.contains(hash))
			return;
		std::unique_lock writeLock(bytecodeMutex);
		if (bytecodeByHash.find(hash) != bytecodeByHash.end())
			return;
		if (capturedBytecodeBytes > kMaxCapturedBytecodeCache - size) {
			static std::atomic<bool> logged{ false };
			if (!logged.exchange(true)) {
				SPDLOG_ERROR("[DFLightPatcher] Runtime bytecode cache reached {} MiB; rejecting additional shaders until classification",
					kMaxCapturedBytecodeCache / (1024u * 1024u));
			}
			return;
		}
		auto [it, inserted] = bytecodeByHash.try_emplace(hash);
		if (inserted) {
			it->second.assign(
				static_cast<const uint8_t*>(bytes),
				static_cast<const uint8_t*>(bytes) + size);
			capturedBytecodeBytes += size;
		}
	}

	void DFLightPatcher::ReleaseResources(bool clearCapturedBytecode)
	{
		initialized.store(false, std::memory_order_release);
		ready.store(false, std::memory_order_release);
		{
			std::unique_lock mapLock(patchedMapMutex);
			for (auto& [hash, ps] : patchedByHash) {
				if (ps)
					ps->Release();
			}
			patchedByHash.clear();
			if (clearCapturedBytecode)
				unsupportedHashes.clear();
		}
		if (clearCapturedBytecode) {
			std::unique_lock bytecodeLock(bytecodeMutex);
			bytecodeByHash.clear();
			capturedBytecodeBytes = 0;
		}
		if (patchDevice) {
			patchDevice->Release();
			patchDevice = nullptr;
		}
		cachedSunPatch = {};
		cachedFactorTempRegister = 0xFFFFFFFF;
	}

	void DFLightPatcher::Release()
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		ReleaseResources(false);
	}

	void DFLightPatcher::Reset()
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		ReleaseResources(true);
	}

	DFLightPatcher::~DFLightPatcher()
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		ReleaseResources(true);
	}
}
