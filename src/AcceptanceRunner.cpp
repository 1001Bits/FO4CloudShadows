#include "PCH.h"

#include "AcceptanceRunner.h"
#include "CloudShadows.h"
#include "RuntimeAPI.h"

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace CloudShadows::AcceptanceRunner
{
	namespace
	{
		using SteadyClock = std::chrono::steady_clock;

		constexpr std::uint32_t kSchemaVersion = 1;
		constexpr std::uint64_t kDefaultTimeoutMilliseconds = 120000;
		constexpr std::uint64_t kDefaultMinimumObservationMilliseconds = 3000;
		constexpr std::uint64_t kDefaultMinimumObservationFrames = 180;
		constexpr std::uint64_t kDefaultMinimumEpochAdvances = 2;
		constexpr std::uint64_t kMinimumValidMaskFrames = 3;
		constexpr std::uint64_t kMinimumTelemetrySamples = 2;
		constexpr std::uint64_t kMinimumValidReceiverSamples = 64;
		constexpr std::uint64_t kMinimumReceiverClassSamples = 4;
		constexpr std::uintmax_t kMaximumProtocolFileBytes = 64u * 1024u;
		constexpr std::size_t kMaximumProtocolQueueDepth = 8;
		constexpr std::size_t kMaximumRememberedRequestIds = 128;
		constexpr auto kProtocolPollInterval = std::chrono::milliseconds(100);
		constexpr auto kResultRetryInterval = std::chrono::milliseconds(250);
		constexpr auto kResultRetryLimit = std::chrono::seconds(30);

		struct Request
		{
			std::string requestId;
			std::string requestedUtc;
			std::string sessionId;
			std::string targetExecutablePath;
			std::string targetRuntime;
			std::uint32_t targetProcessId{ 0 };
			std::uint64_t timeoutMilliseconds{ kDefaultTimeoutMilliseconds };
			std::uint64_t minimumObservationMilliseconds{
				kDefaultMinimumObservationMilliseconds };
			std::uint64_t minimumObservationFrames{
				kDefaultMinimumObservationFrames };
			std::uint64_t minimumEpochAdvances{
				kDefaultMinimumEpochAdvances };
			bool local{ false };
		};

		struct ProtocolIdentity
		{
			fs::path directory;
			fs::path executablePath;
			std::string executablePathUtf8;
			std::wstring normalizedExecutablePath;
			std::string runtimeTarget;
			std::string sessionId;
			std::string processCreationTime100ns;
			std::uint32_t processId{ 0 };
		};

		struct PhysicalTelemetrySample
		{
			std::uint64_t epoch{ 0 };
			std::uint64_t worldGeneration{ 0 };
			std::uint32_t dispatchOrdinal{ 0 };
			float receiverOpacity{ 0.0f };
			float localMinimum{ 0.0f };
			float localMean{ 0.0f };
			float localMaximum{ 0.0f };
			float shadowedFraction{ 0.0f };
			std::uint32_t sampleCount{ 0 };
			bool sunEligible{ false };
			bool fieldCommitted{ false };
			bool hasExactClear{ false };
			bool hasCloud{ false };
			bool hasBrokenMix{ false };
		};

		struct Observation
		{
			std::uint64_t frames{ 0 };
			SteadyClock::time_point startedAt{};
			std::uint64_t elapsedMilliseconds{ 0 };
			std::uint64_t baselineActualSwaps{ 0 };
			std::uint64_t baselineCommittedEpoch{ 0 };
			std::uint64_t baselineWorldGeneration{ 0 };
			std::uint64_t baselineTelemetryEpoch{ 0 };
			std::uint32_t baselineTelemetryDispatchOrdinal{ 0 };
			std::uint64_t baselineScreenEvidenceGeneration{ 0 };
			std::uint64_t baselineScreenEvidenceEpoch{ 0 };
			std::uint64_t baselineCubeEvidenceGeneration{ 0 };
			std::uint64_t baselineCubeEvidenceEpoch{ 0 };
			std::uint64_t requestedScreenEvidenceId{ 0 };
			std::uint64_t requestedCubeEvidenceId{ 0 };
			std::uint64_t cubeEvidenceRequestedForCommittedEpoch{ 0 };
			std::uint32_t evidencePairRetryCount{ 0 };
			std::uint32_t baselineSkyDraws{ 0 };
			std::uint32_t baselineCloudDraws{ 0 };
			std::uint32_t baselineTileCaptures{ 0 };
			std::uint32_t baselineCaptureOverflow{ 0 };
			std::uint64_t firstCommittedEpoch{ 0 };
			std::uint64_t lastCommittedEpoch{ 0 };
			std::uint64_t committedEpochAdvances{ 0 };
			std::uint64_t lastTelemetryEpoch{ 0 };
			std::uint64_t lastTelemetryWorldGeneration{ 0 };
			std::uint32_t lastTelemetryDispatchOrdinal{ 0 };
			std::uint64_t telemetrySamples{ 0 };
			std::uint64_t validMaskFrames{ 0 };
			std::uint32_t maximumActiveLayers{ 0 };
			bool everWorldReady{ false };
			bool everMaskValid{ false };
			bool everPatcherReady{ false };
			bool everSunEligible{ false };
			bool everFieldCommitted{ false };
			bool hasFreshScreenEvidence{ false };
			bool hasFreshCubeEvidence{ false };
			bool screenEvidenceValid{ false };
			bool cubeEvidenceValid{ false };
			bool screenHasExactNeutralReceiver{ false };
			bool screenHasAttenuatedReceiver{ false };
			bool screenHasReceiverVariation{ false };
			bool cubeHasClearOpacity{ false };
			bool cubeHasCloudOpacity{ false };
			bool cubeHasBrokenCloudMix{ false };
			bool sawClearSample{ false };
			bool sawCloudSample{ false };
			bool sawBrokenCloudMix{ false };
			float receiverOpacityMinimum{ 1.0f };
			float receiverOpacityMaximum{ 0.0f };
			float localOpacityMinimumMinimum{ 1.0f };
			float localOpacityMinimumMaximum{ 0.0f };
			float localOpacityMeanMinimum{ 1.0f };
			float localOpacityMeanMaximum{ 0.0f };
			float localOpacityMaximum{ 0.0f };
			float shadowedFractionMinimum{ 1.0f };
			float shadowedFractionMaximum{ 0.0f };
			std::uint32_t lastTelemetrySampleCount{ 0 };
			std::array<PhysicalTelemetrySample, 64> telemetryHistory{};
			std::uint32_t telemetryHistoryCount{ 0 };
			std::uint32_t telemetryHistoryWriteIndex{ 0 };
			float initialOpacity{ 0.0f };
			float initialCloudHeight{ 0.0f };
			ScreenMaskEvidenceSnapshot screenEvidence{};
			WorldCloudCubeEvidenceSnapshot cubeEvidence{};
			PresentEvidence latestPresent{};
		};

		struct RunnerState
		{
			Status status{ Status::kIdle };
			Request request{};
			Request pendingLocalRequest{};
			Observation observation{};
			std::vector<std::string> reasons;
			std::string statusText{ "Idle" };
			std::uint64_t presentOrdinal{ 0 };
			bool hasPendingLocalRequest{ false };
			bool verdictArmed{ false };
			bool resultQueued{ false };
			Status armedVerdict{ Status::kIdle };
			std::string armedStatusText;
			std::optional<nlohmann::json> pendingResultDocument;
		};

		std::mutex s_mutex;
		RunnerState s_state;
		std::atomic<std::uint64_t> s_localRequestCounter{ 0 };
		std::atomic<bool> s_runnerBusy{ false };

		struct ResultWriteJob
		{
			std::string requestId;
			Status verdict{ Status::kFail };
			nlohmann::json document;
		};

		struct ResultCompletion
		{
			std::string requestId;
			Status verdict{ Status::kFail };
			bool succeeded{ false };
			std::string error;
		};

		class ProtocolWorker;
		std::unique_ptr<ProtocolWorker> s_protocolWorker;
		ProtocolIdentity s_protocolIdentity;
		[[nodiscard]] bool TryQueueProtocolResult(ResultWriteJob&& job) noexcept;
		[[nodiscard]] bool EvidencePairIsCorrelated() noexcept;
		[[nodiscard]] const PhysicalTelemetrySample*
		FindCorrelatedPhysicalTelemetry() noexcept;

		[[nodiscard]] const char* StatusName(Status status) noexcept
		{
			switch (status) {
			case Status::kRunning:
				return "RUNNING";
			case Status::kPass:
				return "PASS";
			case Status::kFail:
				return "FAIL";
			case Status::kInconclusiveWeather:
				return "INCONCLUSIVE_WEATHER";
			default:
				return "IDLE";
			}
		}

		[[nodiscard]] std::string NowUtc()
		{
			SYSTEMTIME time{};
			GetSystemTime(&time);
			char value[32]{};
			std::snprintf(
				value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
				time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
				time.wSecond, time.wMilliseconds);
			return value;
		}

		[[nodiscard]] std::uint64_t Mix64(std::uint64_t value) noexcept
		{
			value += 0x9E3779B97F4A7C15ULL;
			value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ULL;
			value = (value ^ (value >> 27u)) * 0x94D049BB133111EBULL;
			return value ^ (value >> 31u);
		}

		[[nodiscard]] std::string MakeRequestId()
		{
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			FILETIME fileTime{};
			GetSystemTimeAsFileTime(&fileTime);
			const std::uint64_t clock =
				(static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32u) |
				fileTime.dwLowDateTime;
			std::uint64_t first = Mix64(
				static_cast<std::uint64_t>(counter.QuadPart) ^ clock ^
				(static_cast<std::uint64_t>(GetCurrentProcessId()) << 32u) ^
				s_localRequestCounter.fetch_add(1, std::memory_order_relaxed));
			std::uint64_t second = Mix64(
				first ^ static_cast<std::uint64_t>(GetCurrentThreadId()));
			std::array<std::uint8_t, 16> bytes{};
			std::memcpy(bytes.data(), &first, sizeof(first));
			std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));
			bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0Fu) | 0x40u);
			bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3Fu) | 0x80u);

			char value[40]{};
			std::snprintf(
				value, sizeof(value),
				"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
				"%02x%02x%02x%02x%02x%02x",
				bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
				bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
				bytes[12], bytes[13], bytes[14], bytes[15]);
			return value;
		}

		[[nodiscard]] bool IsGuidString(std::string_view value) noexcept
		{
			if (value.size() != 36)
				return false;
			for (std::size_t i = 0; i < value.size(); ++i) {
				const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
				if (separator) {
					if (value[i] != '-')
						return false;
				} else if (!((value[i] >= '0' && value[i] <= '9') ||
					(value[i] >= 'a' && value[i] <= 'f') ||
					(value[i] >= 'A' && value[i] <= 'F'))) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] const char* RuntimeTargetName(
			FO4CS::F4SECompat::RuntimeTarget target) noexcept;

		[[nodiscard]] std::string Utf8FromWide(std::wstring_view value)
		{
			if (value.empty())
				return {};
			const int count = WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			if (count <= 0)
				throw std::runtime_error("cannot encode executable path as UTF-8");
			std::string result(static_cast<std::size_t>(count), '\0');
			if (WideCharToMultiByte(
					CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
					static_cast<int>(value.size()), result.data(), count, nullptr,
					nullptr) != count) {
				throw std::runtime_error("cannot encode executable path as UTF-8");
			}
			return result;
		}

		[[nodiscard]] std::wstring WideFromUtf8(std::string_view value)
		{
			if (value.empty())
				return {};
			const int count = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0);
			if (count <= 0)
				throw std::runtime_error("path is not valid UTF-8");
			std::wstring result(static_cast<std::size_t>(count), L'\0');
			if (MultiByteToWideChar(
					CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
					static_cast<int>(value.size()), result.data(), count) != count) {
				throw std::runtime_error("path is not valid UTF-8");
			}
			return result;
		}

		[[nodiscard]] std::wstring NormalizePathForComparison(
			const fs::path& value)
		{
			if (!value.is_absolute())
				throw std::runtime_error("executable path must be absolute");
			const fs::path normalized = value.lexically_normal();
			std::wstring result = normalized.native();
			for (wchar_t& character : result) {
				if (character == L'/')
					character = L'\\';
				character = static_cast<wchar_t>(std::towlower(character));
			}
			while (result.size() > 3 && result.back() == L'\\')
				result.pop_back();
			return result;
		}

		[[nodiscard]] std::string CurrentProcessCreationTime100ns()
		{
			FILETIME creation{}, exit{}, kernel{}, user{};
			if (!GetProcessTimes(
					GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
				throw std::runtime_error("GetProcessTimes failed");
			}
			const std::uint64_t value =
				(static_cast<std::uint64_t>(creation.dwHighDateTime) << 32u) |
				creation.dwLowDateTime;
			return std::to_string(value);
		}

		[[nodiscard]] ProtocolIdentity BuildProtocolIdentity()
		{
			const auto host = FO4CS::RuntimeAPI::GetSingleton().Host();
			if (host.executablePath.empty())
				throw std::runtime_error("runtime executable path is unavailable");
			ProtocolIdentity identity;
			identity.executablePath = host.executablePath;
			identity.directory = host.executablePath.parent_path() / "Data" /
				"F4SE" / "Plugins";
			identity.executablePathUtf8 = Utf8FromWide(
				identity.executablePath.native());
			identity.normalizedExecutablePath =
				NormalizePathForComparison(identity.executablePath);
			identity.runtimeTarget = RuntimeTargetName(host.target);
			if (identity.runtimeTarget == "UNSUPPORTED")
				throw std::runtime_error("unsupported runtime target");
			identity.sessionId = MakeRequestId();
			identity.processId = GetCurrentProcessId();
			identity.processCreationTime100ns = CurrentProcessCreationTime100ns();
			return identity;
		}

		[[nodiscard]] fs::path RequestPath(const ProtocolIdentity& identity)
		{
			return identity.directory /
				"FO4CloudShadows.acceptance.request.json";
		}

		[[nodiscard]] fs::path EnablePath(const ProtocolIdentity& identity)
		{
			return identity.directory /
				("FO4CloudShadows.acceptance.enable." +
				 std::to_string(identity.processId));
		}

		[[nodiscard]] fs::path ReadyPath(const ProtocolIdentity& identity)
		{
			return identity.directory /
				("FO4CloudShadows.acceptance.ready." +
				 std::to_string(identity.processId) + ".json");
		}

		[[nodiscard]] fs::path ResultPath(
			const ProtocolIdentity& identity,
			std::string_view requestId)
		{
			if (!IsGuidString(requestId))
				throw std::runtime_error("unsafe result request ID");
			return identity.directory /
				("FO4CloudShadows.acceptance.result." +
				 std::string(requestId) + ".json");
		}

		[[nodiscard]] std::string VersionString(
			FO4CS::F4SECompat::Version version)
		{
			return std::to_string(version.major) + "." +
				std::to_string(version.minor) + "." +
				std::to_string(version.patch) + "." +
				std::to_string(version.build);
		}

		[[nodiscard]] const char* RuntimeTargetName(
			FO4CS::F4SECompat::RuntimeTarget target) noexcept
		{
			using FO4CS::F4SECompat::RuntimeTarget;
			switch (target) {
			case RuntimeTarget::kLegacy:
				return "OG";
			case RuntimeTarget::kAE:
				return "AE";
			case RuntimeTarget::kVR:
				return "VR";
			default:
				return "UNSUPPORTED";
			}
		}

		[[nodiscard]] std::string Hex64(std::uint64_t value)
		{
			char text[24]{};
			std::snprintf(
				text, sizeof(text), "0x%016llX",
				static_cast<unsigned long long>(value));
			return text;
		}

		[[nodiscard]] std::string Hex32(std::uint32_t value)
		{
			char text[16]{};
			std::snprintf(text, sizeof(text), "0x%08X", value);
			return text;
		}

		template <class T>
		[[nodiscard]] constexpr T SaturatingDelta(T current, T baseline) noexcept
		{
			return current >= baseline ? current - baseline : T{};
		}

		[[nodiscard]] nlohmann::json DistributionEvidence(
			const EvidenceDistributionSummary& value)
		{
			return {
				{ "SampleCount", value.sampleCount },
				{ "FiniteCount", value.finiteCount },
				{ "NonFiniteCount", value.nonFiniteCount },
				{ "OutOfRangeCount", value.outOfRangeCount },
				{ "ExactZeroCount", value.exactZeroCount },
				{ "ExactOneCount", value.exactOneCount },
				{ "NeutralCount", value.neutralCount },
				{ "StrongShadowCount", value.strongShadowCount },
				{ "ClearCount", value.clearCount },
				{ "OpaqueCount", value.opaqueCount },
				{ "Minimum", value.minimum },
				{ "Maximum", value.maximum },
				{ "Mean", value.mean },
				{ "StandardDeviation", value.standardDeviation }
			};
		}

		[[nodiscard]] bool DistributionIsValid(
			const EvidenceDistributionSummary& value) noexcept
		{
			constexpr double kTolerance = 1.0e-5;
			if (value.sampleCount == 0 || value.finiteCount == 0 ||
				value.finiteCount + value.nonFiniteCount != value.sampleCount ||
				value.nonFiniteCount != 0 || value.outOfRangeCount != 0) {
				return false;
			}
			const std::uint64_t classifiedCounts[]{
				value.exactZeroCount, value.exactOneCount, value.neutralCount,
				value.strongShadowCount, value.clearCount, value.opaqueCount
			};
			for (const std::uint64_t count : classifiedCounts) {
				if (count > value.finiteCount)
					return false;
			}
			return std::isfinite(value.minimum) &&
				std::isfinite(value.maximum) && std::isfinite(value.mean) &&
				std::isfinite(value.standardDeviation) &&
				value.minimum >= -kTolerance &&
				value.maximum <= 1.0 + kTolerance &&
				value.minimum <= value.maximum + kTolerance &&
				value.mean >= static_cast<double>(value.minimum) - kTolerance &&
				value.mean <= static_cast<double>(value.maximum) + kTolerance &&
				value.standardDeviation >= -kTolerance &&
				value.standardDeviation <= 1.0 + kTolerance;
		}

		[[nodiscard]] const EvidenceDistributionSummary& ScreenScope(
			const ScreenMaskEvidenceSnapshot& snapshot,
			ScreenMaskEvidenceScope scope) noexcept
		{
			return snapshot.scopes[static_cast<std::uint32_t>(scope)];
		}

		[[nodiscard]] const ValidReceiverEvidenceSummary& ValidReceiverScope(
			const ScreenMaskEvidenceSnapshot& snapshot,
			ScreenMaskEvidenceScope scope) noexcept
		{
			return snapshot.validReceivers[static_cast<std::uint32_t>(scope)];
		}

		[[nodiscard]] bool EmptyDistributionIsCanonical(
			const EvidenceDistributionSummary& value) noexcept
		{
			return value.sampleCount == 0 && value.finiteCount == 0 &&
				value.nonFiniteCount == 0 && value.outOfRangeCount == 0 &&
				value.exactZeroCount == 0 && value.exactOneCount == 0 &&
				value.neutralCount == 0 && value.strongShadowCount == 0 &&
				value.clearCount == 0 && value.opaqueCount == 0 &&
				value.minimum == 0.0f && value.maximum == 0.0f &&
				value.mean == 0.0 && value.standardDeviation == 0.0;
		}

		[[nodiscard]] std::uint64_t StridedCount(
			std::uint64_t begin, std::uint64_t end,
			std::uint64_t stride) noexcept
		{
			if (begin >= end || stride == 0)
				return 0;
			const std::uint64_t first = ((begin + stride - 1u) / stride) * stride;
			return first >= end ? 0u : 1u + (end - 1u - first) / stride;
		}

		[[nodiscard]] bool ScreenEvidenceIsValid(
			const ScreenMaskEvidenceSnapshot& snapshot) noexcept
		{
			if (!snapshot.valid || snapshot.width == 0 || snapshot.height == 0 ||
				snapshot.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
				snapshot.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
				snapshot.sampleStride == 0 ||
				snapshot.sampleStride > (std::max)(snapshot.width, snapshot.height) ||
				!std::isfinite(snapshot.debugMode) ||
				snapshot.debugMode != 0.0f) {
				return false;
			}
			const std::uint64_t stride = snapshot.sampleStride;
			const std::uint64_t sampledX =
				StridedCount(0, snapshot.width, stride);
			const std::uint64_t sampledY =
				StridedCount(0, snapshot.height, stride);
			const std::array<std::uint64_t, kScreenMaskEvidenceScopeCount>
				expectedCounts{
					sampledX * sampledY,
					sampledX * StridedCount(
						snapshot.height / 2u, snapshot.height, stride),
					StridedCount(
						snapshot.width / 4u,
						(static_cast<std::uint64_t>(snapshot.width) * 3u) / 4u,
						stride) *
						StridedCount(
							snapshot.height / 2u, snapshot.height, stride)
				};
			for (std::uint32_t index = 0;
				index < kScreenMaskEvidenceScopeCount; ++index) {
				const auto& scope = snapshot.scopes[index];
				const auto& receivers = snapshot.validReceivers[index];
				if (!DistributionIsValid(scope) ||
					scope.sampleCount != expectedCounts[index] ||
					receivers.validReceiverCount > scope.sampleCount ||
					receivers.validReceiverExactOneCount >
						receivers.validReceiverCount ||
					receivers.validReceiverAttenuatedCount >
						receivers.validReceiverCount ||
					receivers.validReceiverExactOneCount +
						receivers.validReceiverAttenuatedCount !=
						receivers.validReceiverCount ||
					receivers.distribution.sampleCount !=
						receivers.validReceiverCount) {
					return false;
				}
				if (receivers.validReceiverCount == 0) {
					if (!EmptyDistributionIsCanonical(receivers.distribution))
						return false;
				} else if (!DistributionIsValid(receivers.distribution) ||
					receivers.distribution.exactOneCount !=
						receivers.validReceiverExactOneCount) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool CheckedProduct(
			std::initializer_list<std::uint64_t> factors,
			std::uint64_t& result) noexcept
		{
			result = 1;
			for (const std::uint64_t factor : factors) {
				if (factor != 0 && result >
					(std::numeric_limits<std::uint64_t>::max)() / factor) {
					return false;
				}
				result *= factor;
			}
			return true;
		}

		[[nodiscard]] bool CubeEvidenceIsValid(
			const WorldCloudCubeEvidenceSnapshot& snapshot) noexcept
		{
			if (!snapshot.valid || snapshot.width == 0 || snapshot.height == 0 ||
				snapshot.width != snapshot.height ||
				snapshot.width > D3D11_REQ_TEXTURECUBE_DIMENSION ||
				snapshot.mipLevels == 0 || snapshot.mipLevels > 32 ||
				snapshot.coarseMip >= snapshot.mipLevels ||
				snapshot.activeLayerCount > kMaxWorldCloudLayers ||
				snapshot.sampledLayerCount == 0 ||
				snapshot.sampledLayerCount > kMaxWorldCloudLayers ||
				snapshot.sampledLayerCount !=
					(std::max)(1u, snapshot.activeLayerCount) ||
				!DistributionIsValid(snapshot.baseAllFaces) ||
				!DistributionIsValid(snapshot.coarseAllFaces)) {
				return false;
			}
			std::uint64_t expectedFace = 0;
			std::uint64_t expectedAllFaces = 0;
			const std::uint64_t coarseWidth =
				(std::max)(1u, snapshot.width >> snapshot.coarseMip);
			const std::uint64_t coarseHeight =
				(std::max)(1u, snapshot.height >> snapshot.coarseMip);
			std::uint64_t expectedCoarse = 0;
			if (!CheckedProduct(
					{ snapshot.width, snapshot.height,
						snapshot.sampledLayerCount }, expectedFace) ||
				!CheckedProduct({ expectedFace, 6u }, expectedAllFaces) ||
				!CheckedProduct(
					{ coarseWidth, coarseHeight, snapshot.sampledLayerCount, 6u },
					expectedCoarse) ||
				snapshot.baseAllFaces.sampleCount != expectedAllFaces ||
				snapshot.coarseAllFaces.sampleCount != expectedCoarse) {
				return false;
			}
			std::uint64_t summedSamples = 0;
			std::uint64_t summedFinite = 0;
			std::uint64_t summedNonFinite = 0;
			std::uint64_t summedOutOfRange = 0;
			std::uint64_t summedExactZero = 0;
			std::uint64_t summedExactOne = 0;
			std::uint64_t summedNeutral = 0;
			std::uint64_t summedStrongShadow = 0;
			std::uint64_t summedClear = 0;
			std::uint64_t summedOpaque = 0;
			for (const auto& face : snapshot.baseFaces) {
				if (!DistributionIsValid(face) || face.sampleCount != expectedFace)
					return false;
				summedSamples += face.sampleCount;
				summedFinite += face.finiteCount;
				summedNonFinite += face.nonFiniteCount;
				summedOutOfRange += face.outOfRangeCount;
				summedExactZero += face.exactZeroCount;
				summedExactOne += face.exactOneCount;
				summedNeutral += face.neutralCount;
				summedStrongShadow += face.strongShadowCount;
				summedClear += face.clearCount;
				summedOpaque += face.opaqueCount;
			}
			return summedSamples == snapshot.baseAllFaces.sampleCount &&
				summedFinite == snapshot.baseAllFaces.finiteCount &&
				summedNonFinite == snapshot.baseAllFaces.nonFiniteCount &&
				summedOutOfRange == snapshot.baseAllFaces.outOfRangeCount &&
				summedExactZero == snapshot.baseAllFaces.exactZeroCount &&
				summedExactOne == snapshot.baseAllFaces.exactOneCount &&
				summedNeutral == snapshot.baseAllFaces.neutralCount &&
				summedStrongShadow == snapshot.baseAllFaces.strongShadowCount &&
				summedClear == snapshot.baseAllFaces.clearCount &&
				summedOpaque == snapshot.baseAllFaces.opaqueCount;
		}

		void AddReason(std::string reason)
		{
			if (std::find(
					s_state.reasons.begin(), s_state.reasons.end(), reason) ==
				s_state.reasons.end()) {
				s_state.reasons.push_back(std::move(reason));
			}
		}

		[[nodiscard]] bool WriteJsonAtomically(
			const fs::path& destination,
			const nlohmann::json& document,
			std::string_view temporaryTag,
			std::string& error)
		{
			std::error_code directoryError;
			if (!fs::is_directory(
					destination.parent_path(), directoryError) || directoryError) {
				error = "protocol directory is unavailable";
				return false;
			}

			const std::string payload = document.dump(2) + '\n';
			if (payload.size() > kMaximumProtocolFileBytes) {
				error = "serialized protocol document exceeds 64 KiB";
				return false;
			}

			fs::path temporary = destination;
			temporary += "." + std::string(temporaryTag) + ".tmp";
			HANDLE output = CreateFileW(
				temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
			if (output == INVALID_HANDLE_VALUE) {
				const DWORD code = GetLastError();
				error = "cannot create unique temporary protocol file: " +
					std::to_string(code);
				return false;
			}

			bool succeeded = true;
			std::size_t offset = 0;
			while (offset < payload.size()) {
				const DWORD requested = static_cast<DWORD>((std::min)(
					payload.size() - offset,
					static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
				DWORD written = 0;
				if (!WriteFile(
						output, payload.data() + offset, requested, &written, nullptr) ||
					written == 0) {
					error = "cannot write temporary protocol file: " +
						std::to_string(GetLastError());
					succeeded = false;
					break;
				}
				offset += written;
			}
			if (succeeded && !FlushFileBuffers(output)) {
				error = "cannot flush temporary protocol file: " +
					std::to_string(GetLastError());
				succeeded = false;
			}
			CloseHandle(output);
			if (!succeeded) {
				DeleteFileW(temporary.c_str());
				return false;
			}

			if (!MoveFileExW(
					temporary.c_str(), destination.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				error = "MoveFileExW failed with " +
					std::to_string(GetLastError());
				DeleteFileW(temporary.c_str());
				return false;
			}
			return true;
		}

		[[nodiscard]] nlohmann::json BuildEvidence()
		{
			const auto& observation = s_state.observation;
			const std::uint64_t actualSwapDelta = SaturatingDelta(
				observation.latestPresent.actualDfLightSwapHits,
				observation.baselineActualSwaps);
			const std::uint32_t skyDraws =
				g_skyDrawsSeen.load(std::memory_order_relaxed);
			const std::uint32_t cloudDraws =
				g_cloudDrawCount.load(std::memory_order_relaxed);
			const std::uint32_t tileCaptures =
				g_reRenderCount.load(std::memory_order_relaxed);
			const std::uint32_t captureOverflow =
				g_worldCloudCaptureOverflow.load(std::memory_order_relaxed);
			const auto host = FO4CS::RuntimeAPI::GetSingleton().Host();

			nlohmann::json evidence;
			evidence["Runtime"] = {
				{ "Target", RuntimeTargetName(host.target) },
				{ "ExecutableVersion", VersionString(host.executableVersion) },
				{ "F4SEVersion", VersionString(host.f4seVersion) },
				{ "PluginVersion", FO4CLOUDSHADOWS_VERSION_STR },
				{ "ProcessId", s_protocolIdentity.processId },
				{ "ProcessCreationTime100ns",
					s_protocolIdentity.processCreationTime100ns },
				{ "ExecutablePath", s_protocolIdentity.executablePathUtf8 },
				{ "SessionId", s_protocolIdentity.sessionId }
			};
			evidence["Observation"] = {
				{ "Frames", observation.frames },
				{ "ElapsedMilliseconds", observation.elapsedMilliseconds },
				{ "MinimumMilliseconds",
					s_state.request.minimumObservationMilliseconds },
				{ "TimeoutMilliseconds",
					s_state.request.timeoutMilliseconds },
				{ "MinimumFrames", s_state.request.minimumObservationFrames },
				{ "Clock", "steady_clock" }
			};
			evidence["ProductionConfiguration"] = {
				{ "ShadowsEnabled",
					g_shadowsEnabled.load(std::memory_order_relaxed) },
				{ "DebugMode", g_settings.DebugMode },
				{ "SingleCloudIsolation",
					g_singleCloudIsolationEnabled.load(
						std::memory_order_acquire) },
				{ "Opacity", g_settings.Opacity },
				{ "CloudHeight", g_settings.CloudHeight }
			};
			evidence["WorldCloud"] = {
				{ "ReadyObserved", observation.everWorldReady },
				{ "CurrentActiveLayers",
					g_worldCloudActiveLayers.load(std::memory_order_acquire) },
				{ "MaximumActiveLayers", observation.maximumActiveLayers },
				{ "BaselineCommittedEpoch",
					observation.baselineCommittedEpoch },
				{ "FirstCommittedEpoch", observation.firstCommittedEpoch },
				{ "LastCommittedEpoch", observation.lastCommittedEpoch },
				{ "CommittedEpochAdvances", observation.committedEpochAdvances },
				{ "RequiredEpochAdvances", s_state.request.minimumEpochAdvances },
				{ "SkyDrawDelta", SaturatingDelta(
					skyDraws, observation.baselineSkyDraws) },
				{ "CloudDrawDelta", SaturatingDelta(
					cloudDraws, observation.baselineCloudDraws) },
				{ "TileCaptureDelta", SaturatingDelta(
					tileCaptures, observation.baselineTileCaptures) },
				{ "CaptureOverflowDelta", SaturatingDelta(
					captureOverflow, observation.baselineCaptureOverflow) }
			};
			evidence["WorldCloud"]["GpuCubeEvidence"] = {
				{ "RequestedCaptureId", observation.requestedCubeEvidenceId },
				{ "BaselineGeneration",
					observation.baselineCubeEvidenceGeneration },
				{ "BaselineEpoch", observation.baselineCubeEvidenceEpoch },
				{ "FreshPostRequest", observation.hasFreshCubeEvidence },
				{ "Valid", observation.cubeEvidenceValid },
				{ "Generation", observation.cubeEvidence.generation },
				{ "CaptureRequestId", observation.cubeEvidence.requestId },
				{ "Epoch", observation.cubeEvidence.epoch },
				{ "WorldGeneration", observation.cubeEvidence.worldGeneration },
				{ "Width", observation.cubeEvidence.width },
				{ "Height", observation.cubeEvidence.height },
				{ "MipLevels", observation.cubeEvidence.mipLevels },
				{ "CoarseMip", observation.cubeEvidence.coarseMip },
				{ "ActiveLayerCount",
					observation.cubeEvidence.activeLayerCount },
				{ "SampledLayerCount",
					observation.cubeEvidence.sampledLayerCount },
				{ "HasExactClearOpacity", observation.cubeHasClearOpacity },
				{ "HasCloudOpacity", observation.cubeHasCloudOpacity },
				{ "HasBrokenCloudMix", observation.cubeHasBrokenCloudMix },
				{ "BaseAllFaces",
					DistributionEvidence(observation.cubeEvidence.baseAllFaces) },
				{ "CoarseAllFaces",
					DistributionEvidence(observation.cubeEvidence.coarseAllFaces) }
			};
			for (std::uint32_t face = 0; face < 6; ++face) {
				evidence["WorldCloud"]["GpuCubeEvidence"]["BaseFaces"].push_back(
					DistributionEvidence(observation.cubeEvidence.baseFaces[face]));
			}
			evidence["ScreenMask"] = {
				{ "ValidObserved", observation.everMaskValid },
				{ "ValidCompletedFrames", observation.validMaskFrames },
				{ "RequestedCaptureId", observation.requestedScreenEvidenceId },
				{ "BaselineGeneration",
					observation.baselineScreenEvidenceGeneration },
				{ "BaselineEpoch", observation.baselineScreenEvidenceEpoch },
				{ "FreshPostRequest", observation.hasFreshScreenEvidence },
				{ "GpuEvidenceValid", observation.screenEvidenceValid },
				{ "Generation", observation.screenEvidence.generation },
				{ "CaptureRequestId", observation.screenEvidence.requestId },
				{ "Epoch", observation.screenEvidence.epoch },
				{ "WorldGeneration", observation.screenEvidence.worldGeneration },
				{ "DispatchOrdinal", observation.screenEvidence.dispatchOrdinal },
				{ "Width", observation.screenEvidence.width },
				{ "Height", observation.screenEvidence.height },
				{ "SampleStride", observation.screenEvidence.sampleStride },
				{ "DebugMode", observation.screenEvidence.debugMode },
				{ "ReceiverHasExactOne",
					observation.screenHasExactNeutralReceiver },
				{ "ReceiverHasAttenuation",
					observation.screenHasAttenuatedReceiver },
				{ "ReceiverHasVariation",
					observation.screenHasReceiverVariation },
				{ "LowerHalf", DistributionEvidence(ScreenScope(
					observation.screenEvidence,
					ScreenMaskEvidenceScope::kLowerHalf)) },
				{ "CentreGround", DistributionEvidence(ScreenScope(
					observation.screenEvidence,
					ScreenMaskEvidenceScope::kCentreGround)) }
			};
			auto validReceiverEvidence = [](const ValidReceiverEvidenceSummary& value) {
				return nlohmann::json{
					{ "ValidReceiverCount", value.validReceiverCount },
					{ "ExactOneCount", value.validReceiverExactOneCount },
					{ "AttenuatedCount", value.validReceiverAttenuatedCount },
					{ "Distribution", DistributionEvidence(value.distribution) }
				};
			};
			evidence["ScreenMask"]["ValidReceiversLowerHalf"] =
				validReceiverEvidence(ValidReceiverScope(
					observation.screenEvidence,
					ScreenMaskEvidenceScope::kLowerHalf));
			evidence["ScreenMask"]["ValidReceiversCentreGround"] =
				validReceiverEvidence(ValidReceiverScope(
					observation.screenEvidence,
					ScreenMaskEvidenceScope::kCentreGround));
			evidence["FieldCorrelation"] = {
				{ "Correlated", EvidencePairIsCorrelated() },
				{ "AutomaticRetryCount", observation.evidencePairRetryCount },
				{ "AutomaticRetryLimit", s_state.request.local ? 0u : 1u },
				{ "ScreenEpoch", observation.screenEvidence.epoch },
				{ "CubeEpoch", observation.cubeEvidence.epoch },
				{ "ScreenWorldGeneration",
					observation.screenEvidence.worldGeneration },
				{ "CubeWorldGeneration",
					observation.cubeEvidence.worldGeneration }
			};
			evidence["PhysicalTelemetry"] = {
				{ "ValidSamples", observation.telemetrySamples },
				{ "BaselineEpoch", observation.baselineTelemetryEpoch },
				{ "BaselineDispatchOrdinal",
					observation.baselineTelemetryDispatchOrdinal },
				{ "LastEpoch", observation.lastTelemetryEpoch },
				{ "LastDispatchOrdinal",
					observation.lastTelemetryDispatchOrdinal },
				{ "ReceiverOpacityMinimum", observation.receiverOpacityMinimum },
				{ "ReceiverOpacityMaximum", observation.receiverOpacityMaximum },
				{ "LocalOpacityMinimumObservedMinimum",
					observation.localOpacityMinimumMinimum },
				{ "LocalOpacityMinimumObservedMaximum",
					observation.localOpacityMinimumMaximum },
				{ "LocalOpacityMeanMinimum", observation.localOpacityMeanMinimum },
				{ "LocalOpacityMeanMaximum", observation.localOpacityMeanMaximum },
				{ "LocalOpacityMaximum", observation.localOpacityMaximum },
				{ "ShadowedFractionMinimum", observation.shadowedFractionMinimum },
				{ "ShadowedFractionMaximum", observation.shadowedFractionMaximum },
				{ "LastSampleCount", observation.lastTelemetrySampleCount },
				{ "SunEligibleObserved", observation.everSunEligible },
				{ "CommittedFieldObserved", observation.everFieldCommitted },
				{ "ClearSampleObserved", observation.sawClearSample },
				{ "CloudSampleObserved", observation.sawCloudSample },
				{ "BrokenCloudMixObserved", observation.sawBrokenCloudMix }
			};
			if (const auto* correlated = FindCorrelatedPhysicalTelemetry()) {
				evidence["PhysicalTelemetry"]["CorrelatedToGpuEvidence"] = true;
				evidence["PhysicalTelemetry"]["CorrelatedEpoch"] = correlated->epoch;
				evidence["PhysicalTelemetry"]["CorrelatedWorldGeneration"] =
					correlated->worldGeneration;
				evidence["PhysicalTelemetry"]["CorrelatedDispatchOrdinal"] =
					correlated->dispatchOrdinal;
				evidence["PhysicalTelemetry"]["CorrelatedSunEligible"] =
					correlated->sunEligible;
				evidence["PhysicalTelemetry"]["CorrelatedFieldCommitted"] =
					correlated->fieldCommitted;
				evidence["PhysicalTelemetry"]["CorrelatedExactClear"] =
					correlated->hasExactClear;
				evidence["PhysicalTelemetry"]["CorrelatedCloud"] =
					correlated->hasCloud;
				evidence["PhysicalTelemetry"]["CorrelatedBrokenMix"] =
					correlated->hasBrokenMix;
			} else {
				evidence["PhysicalTelemetry"]["CorrelatedToGpuEvidence"] = false;
			}
			if (observation.telemetrySamples == 0) {
				for (const char* name : {
						"ReceiverOpacityMinimum", "ReceiverOpacityMaximum",
						"LocalOpacityMinimumObservedMinimum",
						"LocalOpacityMinimumObservedMaximum",
						"LocalOpacityMeanMinimum", "LocalOpacityMeanMaximum",
						"LocalOpacityMaximum", "ShadowedFractionMinimum",
						"ShadowedFractionMaximum" }) {
					evidence["PhysicalTelemetry"][name] = nullptr;
				}
			}
			evidence["DFLight"] = {
				{ "StrictPatcherReadyObserved", observation.everPatcherReady },
				{ "ActualSwapBaseline", observation.baselineActualSwaps },
				{ "ActualSwapCurrent",
					observation.latestPresent.actualDfLightSwapHits },
				{ "ActualSwapDelta", actualSwapDelta },
				{ "LastVanillaShaderHash",
					Hex64(observation.latestPresent.lastVanillaShaderHash) },
				{ "LastPixelDescriptor",
					Hex32(observation.latestPresent.lastPixelDescriptor) }
			};
			evidence["AutomationScope"] = {
				"Does not control the game, camera, player, weather, or input",
				"PASS is structural evidence, not perceptual sky-to-ground silhouette certification",
				"Direct-sun-only composition is statically attested by the strict DXBC matcher; this runner observes one correlated live field"
			};
			return evidence;
		}

		void Finalize(Status status, std::string text)
		{
			if (s_state.verdictArmed)
				return;
			// Once a verdict is armed, no further full cube/screen evidence can
			// contribute to it. Retire only unarmed request tokens; GPU work which
			// is already pending continues to be polled safely.
			CancelPendingAcceptanceEvidenceRequests();
			if (s_state.request.local) {
				s_state.armedVerdict = status;
				s_state.armedStatusText = std::move(text);
				s_state.verdictArmed = true;
				s_state.status = Status::kRunning;
				s_state.statusText = std::string(StatusName(status)) +
					": awaiting successful Present";
				return;
			}

			try {
				nlohmann::json result{
					{ "SchemaVersion", kSchemaVersion },
					{ "RequestId", s_state.request.requestId },
					{ "RequestedUtc", s_state.request.requestedUtc },
					{ "CompletedUtc", NowUtc() },
					{ "Status", StatusName(status) },
					{ "ProcessId", s_protocolIdentity.processId },
					{ "ProcessCreationTime100ns",
						s_protocolIdentity.processCreationTime100ns },
					{ "SessionId", s_protocolIdentity.sessionId },
					{ "RuntimeTarget", s_protocolIdentity.runtimeTarget },
					{ "ExecutablePath", s_protocolIdentity.executablePathUtf8 },
					{ "Evidence", BuildEvidence() },
					{ "Reasons", s_state.reasons }
				};
				s_state.pendingResultDocument = std::move(result);
				s_state.armedVerdict = status;
				s_state.armedStatusText = std::move(text);
				s_state.verdictArmed = true;
				s_state.resultQueued = false;
				s_state.status = Status::kRunning;
				s_state.statusText = std::string(StatusName(status)) +
					": awaiting successful Present before durable result";
			} catch (const std::exception& exception) {
				AddReason(std::string("RESULT_CONSTRUCTION_FAILED: ") +
					exception.what());
				s_state.status = Status::kFail;
				s_state.statusText = "FAIL: acceptance result could not be constructed";
				s_runnerBusy.store(false, std::memory_order_release);
			} catch (...) {
				AddReason("RESULT_CONSTRUCTION_FAILED: unknown exception");
				s_state.status = Status::kFail;
				s_state.statusText = "FAIL: acceptance result could not be constructed";
				s_runnerBusy.store(false, std::memory_order_release);
			}
		}

		void BeginRequest(Request request, const PresentEvidence& evidence)
		{
			s_state.request = std::move(request);
			s_state.observation = {};
			s_state.observation.startedAt = SteadyClock::now();
			ScreenMaskEvidenceSnapshot screenBaseline{};
			if (GetScreenMaskEvidenceSnapshot(screenBaseline)) {
				s_state.observation.baselineScreenEvidenceGeneration =
					screenBaseline.generation;
				s_state.observation.baselineScreenEvidenceEpoch =
					screenBaseline.epoch;
			}
			WorldCloudCubeEvidenceSnapshot cubeBaseline{};
			if (GetWorldCloudCubeEvidenceSnapshot(cubeBaseline)) {
				s_state.observation.baselineCubeEvidenceGeneration =
					cubeBaseline.generation;
				s_state.observation.baselineCubeEvidenceEpoch =
					cubeBaseline.epoch;
			}
			s_state.observation.baselineActualSwaps =
				evidence.actualDfLightSwapHits;
			s_state.observation.baselineCommittedEpoch =
				g_worldCloudCommittedEpoch.load(std::memory_order_acquire);
			s_state.observation.baselineWorldGeneration =
				g_worldCloudResetGeneration.load(std::memory_order_acquire);
			s_state.observation.firstCommittedEpoch =
				s_state.observation.baselineCommittedEpoch;
			s_state.observation.lastCommittedEpoch =
				s_state.observation.baselineCommittedEpoch;
			s_state.observation.baselineTelemetryEpoch =
				g_cloudTelemetryEpoch.load(std::memory_order_acquire);
			s_state.observation.baselineTelemetryDispatchOrdinal =
				g_cloudTelemetryDispatchOrdinal.load(std::memory_order_acquire);
			s_state.observation.lastTelemetryEpoch =
				s_state.observation.baselineTelemetryEpoch;
			s_state.observation.lastTelemetryDispatchOrdinal =
				s_state.observation.baselineTelemetryDispatchOrdinal;
			s_state.observation.baselineSkyDraws =
				g_skyDrawsSeen.load(std::memory_order_relaxed);
			s_state.observation.baselineCloudDraws =
				g_cloudDrawCount.load(std::memory_order_relaxed);
			s_state.observation.baselineTileCaptures =
				g_reRenderCount.load(std::memory_order_relaxed);
			s_state.observation.baselineCaptureOverflow =
				g_worldCloudCaptureOverflow.load(std::memory_order_relaxed);
			s_state.observation.initialOpacity = g_settings.Opacity;
			s_state.observation.initialCloudHeight = g_settings.CloudHeight;
			s_state.observation.latestPresent = evidence;
			// The incremental cloud cube is published only after six Sky frames.
			// Request it first; once its exact publication epoch is known,
			// ObserveGpuEvidence requests the screen mask while that committed cube
			// remains active. Simultaneous requests sampled unrelated epochs.
			s_state.observation.requestedScreenEvidenceId = 0;
			s_state.observation.requestedCubeEvidenceId =
				RequestWorldCloudCubeEvidenceCapture();
			s_state.observation.cubeEvidenceRequestedForCommittedEpoch =
				s_state.observation.baselineCommittedEpoch;
			s_state.reasons.clear();
			s_state.verdictArmed = false;
			s_state.resultQueued = false;
			s_state.armedVerdict = Status::kIdle;
			s_state.armedStatusText.clear();
			s_state.pendingResultDocument.reset();
			s_state.status = Status::kRunning;
			s_state.statusText = "Observing production cloud shadows";
			s_runnerBusy.store(true, std::memory_order_release);
			SPDLOG_INFO(
				"[CloudShadows][Acceptance] Started request={} timeoutMs={} "
				"minimumFrames={} minimumEpochAdvances={} screenEvidenceRequest={} "
				"cubeEvidenceRequest={}",
				s_state.request.requestId,
				s_state.request.timeoutMilliseconds,
				s_state.request.minimumObservationFrames,
				s_state.request.minimumEpochAdvances,
				s_state.observation.requestedScreenEvidenceId,
				s_state.observation.requestedCubeEvidenceId);
		}

		enum class RequestReadResult
		{
			kAccepted,
			kNotPresent,
			kNotForThisProcess,
			kInvalid
		};

		[[nodiscard]] bool IsStrictUtcTimestamp(std::string_view value) noexcept
		{
			if (value.size() < 20 || value.size() > 28 || value.back() != 'Z')
				return false;
			constexpr std::size_t separators[]{ 4, 7, 10, 13, 16 };
			constexpr char expected[]{ '-', '-', 'T', ':', ':' };
			for (std::size_t i = 0; i < std::size(separators); ++i) {
				if (value[separators[i]] != expected[i])
					return false;
			}
			for (std::size_t i = 0; i < value.size() - 1; ++i) {
				const bool separator = i == 4 || i == 7 || i == 10 || i == 13 ||
					i == 16 || i == 19;
				if (separator) {
					if (i == 19 && value.size() > 20 && value[i] != '.')
						return false;
					continue;
				}
				if (value[i] < '0' || value[i] > '9')
					return false;
			}
			auto twoDigits = [&](std::size_t offset) {
				return (value[offset] - '0') * 10 + (value[offset + 1] - '0');
			};
			SYSTEMTIME time{};
			time.wYear = static_cast<WORD>(
				(value[0] - '0') * 1000 + (value[1] - '0') * 100 +
				(value[2] - '0') * 10 + (value[3] - '0'));
			time.wMonth = static_cast<WORD>(twoDigits(5));
			time.wDay = static_cast<WORD>(twoDigits(8));
			time.wHour = static_cast<WORD>(twoDigits(11));
			time.wMinute = static_cast<WORD>(twoDigits(14));
			time.wSecond = static_cast<WORD>(twoDigits(17));
			if (time.wSecond > 59)
				return false;
			FILETIME converted{};
			return SystemTimeToFileTime(&time, &converted) != FALSE;
		}

		[[nodiscard]] bool JsonNestingWithinLimit(
			std::string_view text, std::uint32_t maximumDepth) noexcept
		{
			std::uint32_t depth = 0;
			bool quoted = false;
			bool escaped = false;
			for (const char character : text) {
				if (quoted) {
					if (escaped)
						escaped = false;
					else if (character == '\\')
						escaped = true;
					else if (character == '"')
						quoted = false;
					continue;
				}
				if (character == '"') {
					quoted = true;
				} else if (character == '{' || character == '[') {
					if (++depth > maximumDepth)
						return false;
				} else if (character == '}' || character == ']') {
					if (depth == 0)
						return false;
					--depth;
				}
			}
			return !quoted && depth == 0;
		}

		[[nodiscard]] std::uint64_t Fnv1a64(std::string_view value) noexcept
		{
			std::uint64_t hash = 14695981039346656037ULL;
			for (const unsigned char character : value) {
				hash ^= character;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		[[nodiscard]] RequestReadResult ReadRequest(
			const ProtocolIdentity& identity,
			Request& request,
			std::uint64_t& fingerprint,
			std::string& error)
		{
			const fs::path path = RequestPath(identity);
			std::error_code sizeError;
			const std::uintmax_t size = fs::file_size(path, sizeError);
			if (sizeError)
				return RequestReadResult::kNotPresent;
			if (size == 0 || size > kMaximumProtocolFileBytes) {
				error = "request size must be between 1 byte and 64 KiB";
				return RequestReadResult::kInvalid;
			}
			std::ifstream input(path, std::ios::binary);
			if (!input)
				return RequestReadResult::kNotPresent;
			std::string text(static_cast<std::size_t>(size), '\0');
			input.read(text.data(), static_cast<std::streamsize>(text.size()));
			if (input.gcount() != static_cast<std::streamsize>(text.size()))
				return RequestReadResult::kNotPresent;
			fingerprint = Fnv1a64(text);
			if (!JsonNestingWithinLimit(text, 8)) {
				error = "request JSON nesting is invalid or exceeds 8 levels";
				return RequestReadResult::kInvalid;
			}

			const nlohmann::json document = nlohmann::json::parse(
				text, nullptr, false, true);
			if (document.is_discarded() || !document.is_object()) {
				error = "request root must be one valid JSON object";
				return RequestReadResult::kInvalid;
			}
			static constexpr std::string_view allowedFields[]{
				"SchemaVersion", "RequestId", "RequestedUtc", "SessionId",
				"TargetProcessId", "TargetExecutablePath", "TargetRuntime",
				"TimeoutSeconds", "MinimumObservationSeconds",
				"MinimumObservationFrames", "MinimumEpochAdvances"
			};
			for (auto iterator = document.begin(); iterator != document.end();
				++iterator) {
				const std::string_view key = iterator.key();
				if (std::find(std::begin(allowedFields), std::end(allowedFields), key) ==
					std::end(allowedFields)) {
					error = "unknown request field: " + iterator.key();
					return RequestReadResult::kInvalid;
				}
			}
			auto requireString = [&](const char* name, std::size_t maximum) {
				if (!document.contains(name) || !document[name].is_string())
					throw std::runtime_error(std::string(name) + " must be a string");
				std::string value = document[name].get<std::string>();
				if (value.empty() || value.size() > maximum)
					throw std::runtime_error(std::string(name) + " has invalid length");
				return value;
			};
			auto requireUnsigned = [&](const char* name,
				std::uint64_t minimum, std::uint64_t maximum) {
				if (!document.contains(name) || !document[name].is_number_unsigned())
					throw std::runtime_error(
						std::string(name) + " must be an unsigned integer");
				const std::uint64_t value = document[name].get<std::uint64_t>();
				if (value < minimum || value > maximum)
					throw std::runtime_error(std::string(name) + " is out of range");
				return value;
			};

			try {
				if (!document.contains("SchemaVersion") ||
					!document["SchemaVersion"].is_number_unsigned() ||
					document["SchemaVersion"].get<std::uint64_t>() != kSchemaVersion) {
					throw std::runtime_error("SchemaVersion must be unsigned integer 1");
				}
				request.requestId = requireString("RequestId", 36);
				request.sessionId = requireString("SessionId", 36);
				if (!IsGuidString(request.requestId) ||
					!IsGuidString(request.sessionId)) {
					throw std::runtime_error("RequestId and SessionId must be canonical GUIDs");
				}
				request.targetProcessId = static_cast<std::uint32_t>(requireUnsigned(
					"TargetProcessId", 1,
					(std::numeric_limits<std::uint32_t>::max)()));
				request.targetRuntime = requireString("TargetRuntime", 8);
				request.targetExecutablePath =
					requireString("TargetExecutablePath", 4096);
				if (request.sessionId != identity.sessionId ||
					request.targetProcessId != identity.processId ||
					request.targetRuntime != identity.runtimeTarget ||
					NormalizePathForComparison(fs::path(WideFromUtf8(
						request.targetExecutablePath))) !=
						identity.normalizedExecutablePath) {
					return RequestReadResult::kNotForThisProcess;
				}
				request.requestedUtc = requireString("RequestedUtc", 32);
				if (!IsStrictUtcTimestamp(request.requestedUtc))
					throw std::runtime_error("RequestedUtc must be an RFC3339 UTC timestamp");
				const std::uint64_t timeoutSeconds =
					requireUnsigned("TimeoutSeconds", 5, 3600);
				const std::uint64_t minimumSeconds = requireUnsigned(
					"MinimumObservationSeconds", 1, timeoutSeconds);
				request.timeoutMilliseconds = timeoutSeconds * 1000;
				request.minimumObservationMilliseconds = minimumSeconds * 1000;
				request.minimumObservationFrames = requireUnsigned(
					"MinimumObservationFrames", 1, 36000);
				request.minimumEpochAdvances = requireUnsigned(
					"MinimumEpochAdvances", 1, 120);
				request.local = false;
				return RequestReadResult::kAccepted;
			} catch (const std::exception& exception) {
				error = exception.what();
				return RequestReadResult::kInvalid;
			}
		}

		class ProtocolWorker
		{
		public:
			explicit ProtocolWorker(ProtocolIdentity identity) :
				identity_(std::move(identity)),
				thread_([this](std::stop_token stop) { Run(stop); })
			{}

			~ProtocolWorker()
			{
				thread_.request_stop();
				condition_.notify_all();
			}

			ProtocolWorker(const ProtocolWorker&) = delete;
			ProtocolWorker& operator=(const ProtocolWorker&) = delete;

			[[nodiscard]] bool TryTakeRequest(Request& request) noexcept
			{
				try {
					std::unique_lock lock(mutex_, std::try_to_lock);
					if (!lock.owns_lock() || requests_.empty())
						return false;
					request = std::move(requests_.front());
					requests_.pop_front();
					return true;
				} catch (...) {
					return false;
				}
			}

			[[nodiscard]] bool TryQueueResult(ResultWriteJob&& job) noexcept
			{
				try {
					std::unique_lock lock(mutex_, std::try_to_lock);
					if (!lock.owns_lock() ||
						results_.size() >= kMaximumProtocolQueueDepth) {
						return false;
					}
					results_.push_back(std::move(job));
					condition_.notify_one();
					return true;
				} catch (...) {
					return false;
				}
			}

			[[nodiscard]] bool TryTakeCompletion(ResultCompletion& completion) noexcept
			{
				try {
					std::unique_lock lock(mutex_, std::try_to_lock);
					if (!lock.owns_lock() || completions_.empty())
						return false;
					completion = std::move(completions_.front());
					completions_.pop_front();
					return true;
				} catch (...) {
					return false;
				}
			}

		private:
			struct ActiveResult
			{
				ResultWriteJob job;
				SteadyClock::time_point firstAttempt{};
				SteadyClock::time_point nextAttempt{};
				std::uint64_t attempts{ 0 };
			};

			void RememberRequest(std::string requestId)
			{
				remembered_.insert(requestId);
				rememberedOrder_.push_back(std::move(requestId));
				while (rememberedOrder_.size() > kMaximumRememberedRequestIds) {
					remembered_.erase(rememberedOrder_.front());
					rememberedOrder_.pop_front();
				}
			}

			[[nodiscard]] nlohmann::json ReadyDocument() const
			{
				return {
					{ "SchemaVersion", kSchemaVersion },
					{ "Ready", true },
					{ "ReadyUtc", NowUtc() },
					{ "SessionId", identity_.sessionId },
					{ "ProcessId", identity_.processId },
					{ "ProcessCreationTime100ns",
						identity_.processCreationTime100ns },
					{ "ExecutablePath", identity_.executablePathUtf8 },
					{ "RuntimeTarget", identity_.runtimeTarget },
					{ "PluginVersion", FO4CLOUDSHADOWS_VERSION_STR }
				};
			}

			[[nodiscard]] ResultWriteJob BusyResult(const Request& request) const
			{
				ResultWriteJob job;
				job.requestId = request.requestId;
				job.verdict = Status::kFail;
				job.document = {
					{ "SchemaVersion", kSchemaVersion },
					{ "RequestId", request.requestId },
					{ "RequestedUtc", request.requestedUtc },
					{ "CompletedUtc", NowUtc() },
					{ "Status", "FAIL" },
					{ "ProcessId", identity_.processId },
					{ "ProcessCreationTime100ns",
						identity_.processCreationTime100ns },
					{ "SessionId", identity_.sessionId },
					{ "RuntimeTarget", identity_.runtimeTarget },
					{ "ExecutablePath", identity_.executablePathUtf8 },
					{ "Evidence", { { "Protocol", "request rejected while another acceptance run was active" } } },
					{ "Reasons", { "RUNNER_BUSY" } }
				};
				return job;
			}

			void PollRequest()
			{
				Request request;
				std::uint64_t fingerprint = 0;
				std::string error;
				RequestReadResult result = RequestReadResult::kInvalid;
				try {
					result = ReadRequest(identity_, request, fingerprint, error);
				} catch (const std::exception& exception) {
					error = exception.what();
				} catch (...) {
					error = "unknown request-read exception";
				}
				if (result == RequestReadResult::kNotPresent ||
					result == RequestReadResult::kNotForThisProcess) {
					return;
				}
				if (result == RequestReadResult::kInvalid) {
					if (fingerprint != 0 && fingerprint != lastInvalidFingerprint_) {
						lastInvalidFingerprint_ = fingerprint;
						SPDLOG_WARN(
							"[CloudShadows][Acceptance] Ignoring invalid request: {}",
							error);
					}
					return;
				}

				std::lock_guard lock(mutex_);
				if (remembered_.contains(request.requestId))
					return;
				if (s_runnerBusy.exchange(true, std::memory_order_acq_rel)) {
					if (results_.size() >= kMaximumProtocolQueueDepth)
						return;
					results_.push_back(BusyResult(request));
					RememberRequest(request.requestId);
					condition_.notify_one();
					return;
				}
				if (requests_.size() >= kMaximumProtocolQueueDepth) {
					s_runnerBusy.store(false, std::memory_order_release);
					return;
				}
				RememberRequest(request.requestId);
				requests_.push_back(std::move(request));
			}

			void PublishCompletion(ResultCompletion completion)
			{
				std::lock_guard lock(mutex_);
				if (completions_.size() >= kMaximumProtocolQueueDepth)
					completions_.pop_front();
				completions_.push_back(std::move(completion));
			}

			void Run(std::stop_token stop) noexcept
			{
				try {
					RunLoop(stop);
				} catch (const std::exception& exception) {
					try {
						SPDLOG_ERROR(
							"[CloudShadows][Acceptance] Protocol worker stopped: {}",
							exception.what());
					} catch (...) {
					}
				} catch (...) {
					try {
						SPDLOG_ERROR(
							"[CloudShadows][Acceptance] Protocol worker stopped with an unknown exception");
					} catch (...) {
					}
				}
			}

			void RunLoop(std::stop_token stop)
			{
				bool readyWritten = false;
				std::uint64_t readyAttempts = 0;
				auto nextReadyAttempt = SteadyClock::now();
				auto nextRequestPoll = SteadyClock::now();
				std::optional<ActiveResult> active;
				while (!stop.stop_requested()) {
					const auto now = SteadyClock::now();
					if (!readyWritten && now >= nextReadyAttempt) {
						try {
							std::string error;
							const std::string tag = identity_.sessionId + "." +
								std::to_string(++readyAttempts);
							readyWritten = WriteJsonAtomically(
								ReadyPath(identity_), ReadyDocument(), tag, error);
							if (!readyWritten)
								SPDLOG_WARN("[CloudShadows][Acceptance] Ready write failed: {}", error);
						} catch (const std::exception& exception) {
							SPDLOG_WARN(
								"[CloudShadows][Acceptance] Ready construction failed: {}",
								exception.what());
						} catch (...) {
							SPDLOG_WARN("[CloudShadows][Acceptance] Ready construction failed");
						}
						nextReadyAttempt = now + std::chrono::seconds(1);
					}
					if (now >= nextRequestPoll) {
						PollRequest();
						nextRequestPoll = now + kProtocolPollInterval;
					}

					if (!active) {
						std::lock_guard lock(mutex_);
						if (!results_.empty()) {
							active.emplace();
							active->job = std::move(results_.front());
							results_.pop_front();
							active->firstAttempt = now;
							active->nextAttempt = now;
						}
					}
					if (active && now >= active->nextAttempt) {
						std::string error;
						bool written = false;
						try {
							const std::string tag = identity_.sessionId + "." +
								active->job.requestId + "." +
								std::to_string(++active->attempts);
							written = WriteJsonAtomically(
								ResultPath(identity_, active->job.requestId),
								active->job.document, tag, error);
						} catch (const std::exception& exception) {
							error = exception.what();
						} catch (...) {
							error = "unknown result-write exception";
						}
						if (written || now - active->firstAttempt >= kResultRetryLimit) {
							PublishCompletion({
								.requestId = active->job.requestId,
								.verdict = active->job.verdict,
								.succeeded = written,
								.error = std::move(error) });
							active.reset();
						} else {
							active->nextAttempt = now + kResultRetryInterval;
						}
					}

					std::unique_lock lock(mutex_);
					condition_.wait_for(lock, std::chrono::milliseconds(25), [&] {
						return stop.stop_requested() || !results_.empty();
					});
				}
			}

			ProtocolIdentity identity_;
			std::mutex mutex_;
			std::condition_variable condition_;
			std::deque<Request> requests_;
			std::deque<ResultWriteJob> results_;
			std::deque<ResultCompletion> completions_;
			std::unordered_set<std::string> remembered_;
			std::deque<std::string> rememberedOrder_;
			std::uint64_t lastInvalidFingerprint_{ 0 };
			std::jthread thread_;
		};

		[[nodiscard]] bool TryStartExternalProtocolWorker(
			std::uint64_t presentOrdinal)
		{
			if (s_protocolWorker)
				return true;
			// External acceptance is a developer-only service. Normal gameplay must
			// not keep a thread waking every 25 ms and polling the plugin directory.
			// The harness creates a PID-specific enable marker; checking for it
			// about once per second, independent of frame rate (and of a slow
			// mod-manager virtual file system), keeps the service fully opt-in.
			static ULONGLONG nextMarkerPoll = 0;
			const ULONGLONG now = GetTickCount64();
			if (presentOrdinal != 1u && now < nextMarkerPoll)
				return false;
			nextMarkerPoll = now + 1000;
			if (s_protocolIdentity.executablePath.empty())
				s_protocolIdentity = BuildProtocolIdentity();
			std::error_code markerError;
			const fs::path enablePath = EnablePath(s_protocolIdentity);
			if (!fs::is_regular_file(enablePath, markerError) || markerError)
				return false;
			// A PID-specific marker is single-use. Failure to remove it is harmless:
			// s_protocolWorker prevents a second worker in this process.
			std::error_code removeError;
			(void)fs::remove(enablePath, removeError);
			s_protocolWorker =
				std::make_unique<ProtocolWorker>(s_protocolIdentity);
			SPDLOG_INFO(
				"[CloudShadows][Acceptance] External protocol enabled explicitly");
			return true;
		}

		[[nodiscard]] bool TryQueueProtocolResult(ResultWriteJob&& job) noexcept
		{
			return s_protocolWorker &&
				s_protocolWorker->TryQueueResult(std::move(job));
		}

		[[nodiscard]] bool IsDirectionalDescriptor(std::uint32_t descriptor) noexcept
		{
			const std::uint32_t kind = descriptor & 0x7Fu;
			return (descriptor & 0x100u) == 0u && (kind == 1u || kind == 2u);
		}

		void ObserveTelemetry()
		{
			auto& observation = s_state.observation;
			observation.everSunEligible = observation.everSunEligible ||
				g_cloudTelemetrySunEligible.load(std::memory_order_acquire);
			observation.everFieldCommitted = observation.everFieldCommitted ||
				g_cloudTelemetryFieldCommitted.load(std::memory_order_acquire);
			if (!g_cloudTelemetryValid.load(std::memory_order_acquire))
				return;
			const std::uint64_t epoch =
				g_cloudTelemetryEpoch.load(std::memory_order_relaxed);
			const std::uint64_t worldGeneration =
				g_cloudTelemetryWorldGeneration.load(std::memory_order_relaxed);
			const std::uint32_t dispatchOrdinal =
				g_cloudTelemetryDispatchOrdinal.load(std::memory_order_relaxed);
			if (epoch < observation.baselineTelemetryEpoch ||
				(epoch == observation.baselineTelemetryEpoch &&
				 dispatchOrdinal <=
					observation.baselineTelemetryDispatchOrdinal)) {
				return;
			}
			if (epoch == observation.lastTelemetryEpoch &&
				worldGeneration == observation.lastTelemetryWorldGeneration &&
				dispatchOrdinal == observation.lastTelemetryDispatchOrdinal) {
				return;
			}
			if (worldGeneration != observation.baselineWorldGeneration) {
				AddReason("PHYSICAL_TELEMETRY_WORLD_GENERATION_CHANGED");
				return;
			}
			if ((epoch < observation.lastTelemetryEpoch ||
				 (epoch == observation.lastTelemetryEpoch &&
				  dispatchOrdinal < observation.lastTelemetryDispatchOrdinal)) &&
				worldGeneration == observation.lastTelemetryWorldGeneration) {
				AddReason("PHYSICAL_TELEMETRY_EPOCH_REGRESSED");
				return;
			}

			const float receiverOpacity =
				g_receiverSunRayCloudOpacity.load(std::memory_order_relaxed);
			const float localMean =
				g_localCloudOpacityMean.load(std::memory_order_relaxed);
			const float localMinimum =
				g_localCloudOpacityMinimum.load(std::memory_order_relaxed);
			const float localMaximum =
				g_localCloudOpacityMaximum.load(std::memory_order_relaxed);
			const float shadowedFraction =
				g_localCloudShadowedFraction.load(std::memory_order_relaxed);
			const std::uint32_t sampleCount =
				g_localCloudTelemetrySampleCount.load(std::memory_order_relaxed);
			const bool sunEligible =
				g_cloudTelemetrySunEligible.load(std::memory_order_relaxed);
			const bool fieldCommitted =
				g_cloudTelemetryFieldCommitted.load(std::memory_order_relaxed);
			if (!g_cloudTelemetryValid.load(std::memory_order_acquire) ||
				epoch != g_cloudTelemetryEpoch.load(std::memory_order_relaxed) ||
				worldGeneration !=
					g_cloudTelemetryWorldGeneration.load(std::memory_order_relaxed) ||
				dispatchOrdinal !=
					g_cloudTelemetryDispatchOrdinal.load(std::memory_order_relaxed)) {
				return;
			}
			const float values[]{
				receiverOpacity, localMinimum, localMean, localMaximum,
				shadowedFraction
			};
			for (float value : values) {
				if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
					AddReason("PHYSICAL_TELEMETRY_OUT_OF_RANGE");
					return;
				}
			}
			if (sampleCount == 0 || sampleCount > 81) {
				AddReason("PHYSICAL_TELEMETRY_SAMPLE_COUNT_INVALID");
				return;
			}

			observation.lastTelemetryEpoch = epoch;
			observation.lastTelemetryWorldGeneration = worldGeneration;
			observation.lastTelemetryDispatchOrdinal = dispatchOrdinal;
			++observation.telemetrySamples;
			observation.receiverOpacityMinimum = (std::min)(
				observation.receiverOpacityMinimum, receiverOpacity);
			observation.receiverOpacityMaximum = (std::max)(
				observation.receiverOpacityMaximum, receiverOpacity);
			observation.localOpacityMinimumMinimum = (std::min)(
				observation.localOpacityMinimumMinimum, localMinimum);
			observation.localOpacityMinimumMaximum = (std::max)(
				observation.localOpacityMinimumMaximum, localMinimum);
			observation.localOpacityMeanMinimum = (std::min)(
				observation.localOpacityMeanMinimum, localMean);
			observation.localOpacityMeanMaximum = (std::max)(
				observation.localOpacityMeanMaximum, localMean);
			observation.localOpacityMaximum = (std::max)(
				observation.localOpacityMaximum, localMaximum);
			observation.shadowedFractionMinimum = (std::min)(
				observation.shadowedFractionMinimum, shadowedFraction);
			observation.shadowedFractionMaximum = (std::max)(
				observation.shadowedFractionMaximum, shadowedFraction);
			observation.lastTelemetrySampleCount = sampleCount;
			observation.sawClearSample = observation.sawClearSample ||
				receiverOpacity == 0.0f || localMinimum == 0.0f;
			observation.sawCloudSample = observation.sawCloudSample ||
				receiverOpacity > kCloudTelemetryOpacityThreshold ||
				localMaximum > kCloudTelemetryOpacityThreshold;
			observation.sawBrokenCloudMix = observation.sawBrokenCloudMix ||
				((receiverOpacity == 0.0f || localMinimum == 0.0f) &&
				 shadowedFraction > 0.0f && shadowedFraction < 1.0f &&
				 localMaximum > kCloudTelemetryOpacityThreshold);

			PhysicalTelemetrySample sample;
			sample.epoch = epoch;
			sample.worldGeneration = worldGeneration;
			sample.dispatchOrdinal = dispatchOrdinal;
			sample.receiverOpacity = receiverOpacity;
			sample.localMinimum = localMinimum;
			sample.localMean = localMean;
			sample.localMaximum = localMaximum;
			sample.shadowedFraction = shadowedFraction;
			sample.sampleCount = sampleCount;
			sample.sunEligible = sunEligible;
			sample.fieldCommitted = fieldCommitted;
			sample.hasExactClear =
				receiverOpacity == 0.0f || localMinimum == 0.0f;
			sample.hasCloud =
				receiverOpacity > kCloudTelemetryOpacityThreshold ||
				localMaximum > kCloudTelemetryOpacityThreshold;
			sample.hasBrokenMix = sample.hasExactClear &&
				shadowedFraction > 0.0f && shadowedFraction < 1.0f &&
				localMaximum > kCloudTelemetryOpacityThreshold;
			observation.telemetryHistory[observation.telemetryHistoryWriteIndex] =
				sample;
			observation.telemetryHistoryWriteIndex =
				(observation.telemetryHistoryWriteIndex + 1u) %
				static_cast<std::uint32_t>(observation.telemetryHistory.size());
			observation.telemetryHistoryCount = (std::min)(
				observation.telemetryHistoryCount + 1u,
				static_cast<std::uint32_t>(observation.telemetryHistory.size()));
		}

		[[nodiscard]] bool ObserveGpuEvidence()
		{
			auto& observation = s_state.observation;

			ScreenMaskEvidenceSnapshot screen{};
			if (observation.requestedScreenEvidenceId != 0 &&
				GetScreenMaskEvidenceSnapshot(screen) &&
				screen.generation >
					observation.baselineScreenEvidenceGeneration &&
				screen.requestId >= observation.requestedScreenEvidenceId &&
				screen.generation != observation.screenEvidence.generation) {
				observation.hasFreshScreenEvidence = true;
				observation.screenEvidence = screen;
				if (screen.epoch < observation.baselineScreenEvidenceEpoch) {
					AddReason("SCREEN_MASK_EVIDENCE_EPOCH_REGRESSED");
					return false;
				}
				observation.screenEvidenceValid = ScreenEvidenceIsValid(screen);
				if (!observation.screenEvidenceValid) {
					AddReason("SCREEN_MASK_GPU_EVIDENCE_INVALID");
					return false;
				}

				const auto& lower = ValidReceiverScope(
					screen, ScreenMaskEvidenceScope::kLowerHalf);
				const auto& centre = ValidReceiverScope(
					screen, ScreenMaskEvidenceScope::kCentreGround);
				observation.screenHasExactNeutralReceiver =
					centre.validReceiverExactOneCount >=
						kMinimumReceiverClassSamples ||
					lower.validReceiverExactOneCount >=
						kMinimumReceiverClassSamples;
				observation.screenHasAttenuatedReceiver =
					centre.validReceiverAttenuatedCount >=
						kMinimumReceiverClassSamples ||
					lower.validReceiverAttenuatedCount >=
						kMinimumReceiverClassSamples;
				auto varied = [](const ValidReceiverEvidenceSummary& receivers) {
					return receivers.validReceiverCount >=
							kMinimumValidReceiverSamples &&
						receivers.validReceiverExactOneCount >=
							kMinimumReceiverClassSamples &&
						receivers.validReceiverAttenuatedCount >=
							kMinimumReceiverClassSamples &&
						receivers.distribution.maximum -
							receivers.distribution.minimum > 1.0e-3f &&
						receivers.distribution.standardDeviation > 1.0e-4;
				};
				observation.screenHasReceiverVariation =
					varied(centre) || varied(lower);
			}

			WorldCloudCubeEvidenceSnapshot cube{};
			if (GetWorldCloudCubeEvidenceSnapshot(cube) &&
				cube.generation > observation.baselineCubeEvidenceGeneration &&
				cube.requestId >= observation.requestedCubeEvidenceId &&
				cube.generation != observation.cubeEvidence.generation) {
				observation.hasFreshCubeEvidence = true;
				observation.cubeEvidence = cube;
				if (cube.epoch < observation.baselineCubeEvidenceEpoch) {
					AddReason("WORLD_CLOUD_GPU_EVIDENCE_EPOCH_REGRESSED");
					return false;
				}
				observation.cubeEvidenceValid = CubeEvidenceIsValid(cube);
				if (!observation.cubeEvidenceValid) {
					AddReason("WORLD_CLOUD_GPU_EVIDENCE_INVALID");
					return false;
				}

				const auto& distribution = cube.baseAllFaces;
				observation.cubeHasClearOpacity =
					distribution.exactZeroCount > 0;
				observation.cubeHasCloudOpacity =
					distribution.maximum > kCloudTelemetryOpacityThreshold;
				observation.cubeHasBrokenCloudMix =
					observation.cubeHasClearOpacity &&
					observation.cubeHasCloudOpacity &&
					distribution.maximum - distribution.minimum > 1.0e-5f &&
					distribution.standardDeviation > 1.0e-7;
				if (cube.activeLayerCount == 0 &&
					observation.cubeHasCloudOpacity) {
					AddReason("ZERO_ACTIVE_LAYERS_WITH_NONZERO_CUBE_OPACITY");
					return false;
				}

				// Cube publication armed this screen capture before the DFLight
				// dispatch which consumed that exact committed cube. Do not issue a
				// later request here: the committed epoch may already have advanced by
				// the time asynchronous cube readback reaches the CPU.
				observation.requestedScreenEvidenceId =
					cube.pairedScreenRequestId;
				observation.hasFreshScreenEvidence = false;
				observation.screenEvidenceValid = false;
				observation.screenHasExactNeutralReceiver = false;
				observation.screenHasAttenuatedReceiver = false;
				observation.screenHasReceiverVariation = false;
			}
			return true;
		}

		[[nodiscard]] bool EvidencePairIsCorrelated() noexcept
		{
			const auto& observation = s_state.observation;
			return observation.hasFreshScreenEvidence &&
				observation.screenEvidenceValid &&
				observation.hasFreshCubeEvidence &&
				observation.cubeEvidenceValid &&
				observation.screenEvidence.requestId >=
					observation.requestedScreenEvidenceId &&
				observation.cubeEvidence.requestId >=
					observation.requestedCubeEvidenceId &&
				observation.screenEvidence.epoch >
					observation.baselineScreenEvidenceEpoch &&
				observation.cubeEvidence.epoch >
					observation.baselineCubeEvidenceEpoch &&
				observation.screenEvidence.epoch ==
					observation.cubeEvidence.epoch &&
				observation.screenEvidence.worldGeneration ==
					observation.cubeEvidence.worldGeneration &&
				observation.screenEvidence.worldGeneration ==
					observation.baselineWorldGeneration;
		}

		[[nodiscard]] const PhysicalTelemetrySample*
		FindCorrelatedPhysicalTelemetry() noexcept
		{
			if (!EvidencePairIsCorrelated())
				return nullptr;
			const auto& observation = s_state.observation;
			const auto capacity = static_cast<std::uint32_t>(
				observation.telemetryHistory.size());
			for (std::uint32_t offset = 0;
				offset < observation.telemetryHistoryCount; ++offset) {
				const std::uint32_t index =
					(observation.telemetryHistoryWriteIndex +
					 capacity - 1u - offset) % capacity;
				const auto& sample = observation.telemetryHistory[index];
				if (sample.epoch == observation.screenEvidence.epoch &&
					sample.worldGeneration ==
						observation.screenEvidence.worldGeneration &&
					sample.dispatchOrdinal ==
						observation.screenEvidence.dispatchOrdinal) {
					return &sample;
				}
			}
			return nullptr;
		}

		void RequestFreshCorrelatedEvidencePair()
		{
			auto& observation = s_state.observation;
			// Cube first, then screen. See ObserveGpuEvidence for the hand-off.
			observation.requestedScreenEvidenceId = 0;
			observation.requestedCubeEvidenceId =
				RequestWorldCloudCubeEvidenceCapture();
			observation.cubeEvidenceRequestedForCommittedEpoch =
				g_worldCloudCommittedEpoch.load(std::memory_order_acquire);
			observation.hasFreshScreenEvidence = false;
			observation.hasFreshCubeEvidence = false;
			observation.screenEvidenceValid = false;
			observation.cubeEvidenceValid = false;
			observation.screenHasExactNeutralReceiver = false;
			observation.screenHasAttenuatedReceiver = false;
			observation.screenHasReceiverVariation = false;
			observation.cubeHasClearOpacity = false;
			observation.cubeHasCloudOpacity = false;
			observation.cubeHasBrokenCloudMix = false;
		}

		[[nodiscard]] bool RefreshGpuEvidenceForCommittedEpoch(
			std::uint64_t committedEpoch)
		{
			auto& observation = s_state.observation;
			if (committedEpoch <= observation.baselineCommittedEpoch)
				return true;
			const bool screenStale = observation.hasFreshScreenEvidence &&
				(observation.screenEvidence.epoch <=
					observation.baselineScreenEvidenceEpoch ||
				 observation.screenEvidence.worldGeneration !=
					observation.baselineWorldGeneration);
			const bool cubeStale = observation.hasFreshCubeEvidence &&
				(observation.cubeEvidence.epoch <=
					observation.baselineCubeEvidenceEpoch ||
				 observation.cubeEvidence.worldGeneration !=
					observation.baselineWorldGeneration);
			const bool pairMismatch = observation.hasFreshScreenEvidence &&
				observation.screenEvidenceValid &&
				observation.hasFreshCubeEvidence &&
				observation.cubeEvidenceValid && !EvidencePairIsCorrelated();
			// A screen request now forces telemetry in its exact dispatch. The
			// 128-byte telemetry query can finish after the two texture queries, so
			// wait for it instead of repeatedly requesting whole cube/screen copies.
			if (!screenStale && !cubeStale && !pairMismatch)
				return true;

			constexpr std::uint32_t kExternalEvidencePairRetryLimit = 1;
			const std::uint32_t retryLimit = s_state.request.local
				? 0u
				: kExternalEvidencePairRetryLimit;
			if (observation.evidencePairRetryCount < retryLimit) {
				++observation.evidencePairRetryCount;
				SPDLOG_WARN(
					"[CloudShadows][Acceptance] GPU evidence pair did not "
					"correlate request={} screenEpoch={} cubeEpoch={} "
					"retry={}/{}",
					s_state.request.requestId,
					observation.screenEvidence.epoch,
					observation.cubeEvidence.epoch,
					observation.evidencePairRetryCount, retryLimit);
				RequestFreshCorrelatedEvidencePair();
				s_state.statusText =
					"GPU evidence crossed field epochs; collecting the one "
					"permitted retry";
				return true;
			}

			if (screenStale)
				AddReason("SCREEN_MASK_GPU_EVIDENCE_STALE");
			if (cubeStale)
				AddReason("WORLD_CLOUD_GPU_EVIDENCE_STALE");
			if (pairMismatch)
				AddReason("GPU_EVIDENCE_FIELD_IDENTITY_MISMATCH");
			AddReason("GPU_EVIDENCE_AUTOMATIC_RETRY_LIMIT_REACHED");
			SPDLOG_ERROR(
				"[CloudShadows][Acceptance] GPU evidence pair rejected "
				"request={} screenEpoch={} cubeEpoch={} retry={}/{}",
				s_state.request.requestId,
				observation.screenEvidence.epoch,
				observation.cubeEvidence.epoch,
				observation.evidencePairRetryCount, retryLimit);
			Finalize(
				Status::kFail,
				"FAIL: screen/cube evidence did not identify one field");
			return false;
		}

		[[nodiscard]] bool CoreEvidenceComplete()
		{
			const auto& observation = s_state.observation;
			const auto* telemetry = FindCorrelatedPhysicalTelemetry();
			const std::uint64_t actualSwapDelta = SaturatingDelta(
				observation.latestPresent.actualDfLightSwapHits,
				observation.baselineActualSwaps);
			return observation.everWorldReady && observation.everMaskValid &&
				observation.everPatcherReady &&
				observation.validMaskFrames >= kMinimumValidMaskFrames &&
				observation.telemetrySamples >= kMinimumTelemetrySamples &&
				EvidencePairIsCorrelated() && telemetry &&
				telemetry->sunEligible && telemetry->fieldCommitted &&
				actualSwapDelta > 0 &&
				observation.latestPresent.lastVanillaShaderHash != 0 &&
				IsDirectionalDescriptor(
					observation.latestPresent.lastPixelDescriptor);
		}

		[[nodiscard]] bool FreshExactClearSkyEvidenceComplete()
		{
			const auto& observation = s_state.observation;
			const auto& base = observation.cubeEvidence.baseAllFaces;
			const auto& coarse = observation.cubeEvidence.coarseAllFaces;
			return observation.everWorldReady &&
				observation.hasFreshCubeEvidence &&
				observation.cubeEvidenceValid &&
				observation.cubeEvidence.requestId >=
					observation.requestedCubeEvidenceId &&
				observation.cubeEvidence.epoch >=
					observation.baselineCubeEvidenceEpoch &&
				observation.cubeEvidence.worldGeneration ==
					observation.baselineWorldGeneration &&
				// The current producer publishes one composite cube even when
				// every live cloud has zero opacity. Its pixels prove clear sky;
				// zero published fields instead means capture was unavailable.
				observation.cubeEvidence.activeLayerCount == 1 &&
				observation.cubeEvidence.sampledLayerCount == 1 &&
				base.finiteCount != 0 &&
				base.exactZeroCount == base.finiteCount &&
				base.maximum == 0.0f && coarse.finiteCount != 0 &&
				coarse.exactZeroCount == coarse.finiteCount &&
				coarse.maximum == 0.0f;
		}

		[[nodiscard]] bool BrokenCloudPassEvidenceComplete()
		{
			const auto& observation = s_state.observation;
			const auto* telemetry = FindCorrelatedPhysicalTelemetry();
			const std::uint32_t cloudDrawDelta = SaturatingDelta(
				g_cloudDrawCount.load(std::memory_order_relaxed),
				observation.baselineCloudDraws);
			return CoreEvidenceComplete() &&
				telemetry && telemetry->sunEligible && telemetry->fieldCommitted &&
				telemetry->hasExactClear && telemetry->hasCloud &&
				telemetry->hasBrokenMix &&
				observation.cubeHasBrokenCloudMix &&
				observation.screenHasExactNeutralReceiver &&
				observation.screenHasAttenuatedReceiver &&
				observation.screenHasReceiverVariation &&
				observation.maximumActiveLayers > 0 && cloudDrawDelta > 0 &&
				// Warm frames reuse the geometry mapping. Fresh authenticated
				// cloud draws and committed resolves prove motion; requiring a
				// new legacy tile capture would reject every warmed-up scene.
				observation.committedEpochAdvances >=
					s_state.request.minimumEpochAdvances &&
				observation.screenEvidence.epoch >
					observation.baselineScreenEvidenceEpoch &&
				observation.cubeEvidence.epoch >
					observation.baselineCubeEvidenceEpoch &&
				EvidencePairIsCorrelated();
		}

		void AddMissingCoreReasons()
		{
			const auto& observation = s_state.observation;
			if (!observation.everWorldReady)
				AddReason("WORLD_CLOUD_RESOURCES_NEVER_READY");
			if (!observation.everMaskValid ||
				observation.validMaskFrames < kMinimumValidMaskFrames) {
				AddReason("NO_STABLE_VALID_SCREEN_MASK");
			}
			if (!observation.everPatcherReady)
				AddReason("STRICT_DFLIGHT_PATCHER_NOT_READY");
			if (observation.telemetrySamples < kMinimumTelemetrySamples)
				AddReason("NO_POST_REQUEST_PHYSICAL_CLOUD_TELEMETRY");
			if (!observation.everSunEligible)
				AddReason("NO_ELIGIBLE_ABOVE_HORIZON_SUN_SAMPLE");
			if (!observation.hasFreshScreenEvidence)
				AddReason("NO_POST_REQUEST_SCREEN_MASK_GPU_EVIDENCE");
			else if (!observation.screenEvidenceValid)
				AddReason("SCREEN_MASK_GPU_EVIDENCE_INVALID");
			if (!observation.hasFreshCubeEvidence)
				AddReason("NO_POST_REQUEST_WORLD_CLOUD_GPU_EVIDENCE");
			else if (!observation.cubeEvidenceValid)
				AddReason("WORLD_CLOUD_GPU_EVIDENCE_INVALID");
			if (observation.hasFreshScreenEvidence &&
				observation.hasFreshCubeEvidence &&
				!EvidencePairIsCorrelated()) {
				AddReason("GPU_EVIDENCE_FIELD_IDENTITY_MISMATCH");
			}
			if (EvidencePairIsCorrelated() &&
				!FindCorrelatedPhysicalTelemetry()) {
				AddReason("NO_PHYSICAL_TELEMETRY_FOR_GPU_EVIDENCE_FIELD");
			}
			if (observation.latestPresent.actualDfLightSwapHits <=
				observation.baselineActualSwaps) {
				AddReason("NO_LATER_ACTUAL_DFLIGHT_T47_CONSUMPTION");
			}
			if (observation.latestPresent.lastVanillaShaderHash == 0)
				AddReason("ACTUAL_DFLIGHT_SHADER_HASH_MISSING");
			if (!IsDirectionalDescriptor(
					observation.latestPresent.lastPixelDescriptor)) {
				AddReason("ACTUAL_DFLIGHT_DESCRIPTOR_NOT_DIRECTIONAL_SUN");
			}
		}

		void AddMissingBrokenCloudPassReasons()
		{
			const auto& observation = s_state.observation;
			if (!observation.cubeHasClearOpacity)
				AddReason("VISIBLE_CLOUD_CUBE_HAS_NO_EXACT_CLEAR_TEXEL");
			if (!observation.cubeHasCloudOpacity)
				AddReason("VISIBLE_CLOUD_CUBE_HAS_NO_NONZERO_CLOUD_OPACITY");
			if (!observation.cubeHasBrokenCloudMix)
				AddReason("VISIBLE_CLOUD_CUBE_HAS_NO_BROKEN_CLOUD_MIX");
			if (!observation.screenHasExactNeutralReceiver)
				AddReason("RECEIVER_MASK_HAS_NO_EXACT_ONE_SAMPLE");
			if (!observation.screenHasAttenuatedReceiver)
				AddReason("RECEIVER_MASK_HAS_NO_CLOUD_ATTENUATION");
			if (!observation.screenHasReceiverVariation)
				AddReason("RECEIVER_MASK_HAS_NO_CLOUD_SHAPE_VARIATION");
			const auto* telemetry = FindCorrelatedPhysicalTelemetry();
			if (!telemetry)
				AddReason("NO_CORRELATED_PHYSICAL_TELEMETRY");
			else {
				if (!telemetry->sunEligible)
					AddReason("CORRELATED_TELEMETRY_SUN_NOT_ELIGIBLE");
				if (!telemetry->fieldCommitted)
					AddReason("CORRELATED_TELEMETRY_HAS_NO_COMMITTED_FIELD");
				if (!telemetry->hasExactClear)
					AddReason("CORRELATED_TELEMETRY_HAS_NO_EXACT_CLEAR_RAY");
				if (!telemetry->hasCloud)
					AddReason("CORRELATED_TELEMETRY_HAS_NO_CLOUD_RAY");
				if (!telemetry->hasBrokenMix)
					AddReason("CORRELATED_TELEMETRY_HAS_NO_BROKEN_CLOUD_MIX");
			}
			if (observation.maximumActiveLayers == 0)
				AddReason("NO_AUTHENTICATED_VISIBLE_CLOUD_LAYER");
			if (observation.committedEpochAdvances <
				s_state.request.minimumEpochAdvances) {
				AddReason("INSUFFICIENT_POST_REQUEST_COMMITTED_EPOCH_ADVANCES");
			}
			if (g_cloudDrawCount.load(std::memory_order_relaxed) <=
				observation.baselineCloudDraws) {
				AddReason("NO_NEW_AUTHENTICATED_CLOUD_DRAW");
			}
			if (observation.screenEvidence.epoch <=
				observation.baselineScreenEvidenceEpoch) {
				AddReason("SCREEN_MASK_EVIDENCE_EPOCH_NOT_NEWER");
			}
			if (observation.cubeEvidence.epoch <=
				observation.baselineCubeEvidenceEpoch) {
				AddReason("WORLD_CLOUD_EVIDENCE_EPOCH_NOT_NEWER");
			}
			if (!EvidencePairIsCorrelated())
				AddReason("SCREEN_AND_CUBE_EVIDENCE_NOT_SAME_FIELD");
		}

		void ObserveRunning(const PresentEvidence& evidence)
		{
			auto& observation = s_state.observation;
			++observation.frames;
			observation.elapsedMilliseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					SteadyClock::now() - observation.startedAt).count());
			observation.latestPresent = evidence;
			s_state.statusText = "Observing production cloud shadows (" +
				std::to_string(observation.elapsedMilliseconds / 1000u) + "/" +
				std::to_string(s_state.request.timeoutMilliseconds / 1000u) +
				" seconds, " + std::to_string(observation.frames) + " frames)";

			const bool productionConfiguration =
				g_shadowsEnabled.load(std::memory_order_relaxed) &&
				std::isfinite(g_settings.DebugMode) &&
				g_settings.DebugMode == 0.0f &&
				!g_singleCloudIsolationEnabled.load(std::memory_order_acquire) &&
				std::isfinite(g_settings.Opacity) && g_settings.Opacity > 0.0f &&
				std::isfinite(g_settings.CloudHeight) &&
				g_settings.CloudHeight > 0.0f;
			if (!productionConfiguration) {
				if (!g_shadowsEnabled.load(std::memory_order_relaxed))
					AddReason("NON_PRODUCTION_SHADOWS_DISABLED");
				if (!std::isfinite(g_settings.DebugMode) ||
					g_settings.DebugMode != 0.0f) {
					AddReason("NON_PRODUCTION_DEBUG_MODE");
				}
				if (g_singleCloudIsolationEnabled.load(std::memory_order_acquire))
					AddReason("NON_PRODUCTION_SINGLE_CLOUD_ISOLATION");
				if (!std::isfinite(g_settings.Opacity) || g_settings.Opacity <= 0.0f)
					AddReason("NON_PRODUCTION_OPACITY");
				if (!std::isfinite(g_settings.CloudHeight) ||
					g_settings.CloudHeight <= 0.0f) {
					AddReason("NON_PRODUCTION_CLOUD_HEIGHT");
				}
				Finalize(Status::kFail, "FAIL: production configuration required");
				return;
			}
			if (std::abs(g_settings.Opacity - observation.initialOpacity) > 1.0e-6f ||
				std::abs(g_settings.CloudHeight - observation.initialCloudHeight) >
					1.0e-4f) {
				AddReason("PRODUCTION_SETTINGS_CHANGED_DURING_RUN");
				Finalize(Status::kFail, "FAIL: settings changed during acceptance");
				return;
			}
			if (g_worldCloudResetGeneration.load(std::memory_order_acquire) !=
				observation.baselineWorldGeneration) {
				AddReason("WORLD_GENERATION_CHANGED_DURING_RUN");
				Finalize(Status::kFail, "FAIL: world changed during acceptance");
				return;
			}

			observation.everWorldReady = observation.everWorldReady ||
				g_worldCloudReady.load(std::memory_order_acquire);
			const bool maskValid =
				g_lastCompletedShadowMaskValid.load(std::memory_order_acquire);
			observation.everMaskValid = observation.everMaskValid || maskValid;
			observation.validMaskFrames += maskValid ? 1u : 0u;
			observation.everPatcherReady = observation.everPatcherReady ||
				g_dfLightPatcher.IsReady();
			const std::uint32_t activeLayers =
				g_worldCloudActiveLayers.load(std::memory_order_acquire);
			observation.maximumActiveLayers = (std::max)(
				observation.maximumActiveLayers, activeLayers);
			if (activeLayers > kMaxWorldCloudLayers) {
				AddReason("ACTIVE_LAYER_COUNT_OUT_OF_RANGE");
				Finalize(Status::kFail, "FAIL: active layer count is invalid");
				return;
			}

			const std::uint32_t overflow =
				g_worldCloudCaptureOverflow.load(std::memory_order_relaxed);
			if (overflow != observation.baselineCaptureOverflow) {
				AddReason("CLOUD_CAPTURE_OVERFLOW_DURING_RUN");
				Finalize(Status::kFail, "FAIL: cloud capture overflow");
				return;
			}

			const std::uint64_t committedEpoch =
				g_worldCloudCommittedEpoch.load(std::memory_order_acquire);
			if (observation.firstCommittedEpoch == 0 && committedEpoch != 0)
				observation.firstCommittedEpoch = committedEpoch;
			if (observation.lastCommittedEpoch != 0 &&
				committedEpoch < observation.lastCommittedEpoch) {
				AddReason("COMMITTED_EPOCH_REGRESSED");
				Finalize(Status::kFail, "FAIL: committed epoch regressed");
				return;
			}
			if (committedEpoch > observation.lastCommittedEpoch) {
				if (observation.lastCommittedEpoch != 0)
					++observation.committedEpochAdvances;
				observation.lastCommittedEpoch = committedEpoch;
			}
			if (activeLayers == 0 && observation.hasFreshCubeEvidence &&
				!FreshExactClearSkyEvidenceComplete() &&
				committedEpoch >
					observation.cubeEvidenceRequestedForCommittedEpoch) {
				// Do not replace an outstanding GPU request every neutral epoch.
				// A completed exact-zero cube remains authoritative for its captured
				// epoch and world generation even if later neutral epochs advance while
				// the asynchronous staging readback is pending.
				observation.requestedCubeEvidenceId =
					RequestWorldCloudCubeEvidenceCapture();
				observation.cubeEvidenceRequestedForCommittedEpoch = committedEpoch;
				observation.hasFreshCubeEvidence = false;
				observation.cubeEvidenceValid = false;
				observation.cubeHasClearOpacity = false;
				observation.cubeHasCloudOpacity = false;
				observation.cubeHasBrokenCloudMix = false;
			}

			ObserveTelemetry();
			if (std::find(
					s_state.reasons.begin(), s_state.reasons.end(),
					"PHYSICAL_TELEMETRY_OUT_OF_RANGE") != s_state.reasons.end() ||
				std::find(
					s_state.reasons.begin(), s_state.reasons.end(),
					"PHYSICAL_TELEMETRY_SAMPLE_COUNT_INVALID") !=
					s_state.reasons.end() ||
				std::find(
					s_state.reasons.begin(), s_state.reasons.end(),
					"PHYSICAL_TELEMETRY_EPOCH_REGRESSED") !=
					s_state.reasons.end() ||
				std::find(
					s_state.reasons.begin(), s_state.reasons.end(),
					"PHYSICAL_TELEMETRY_WORLD_GENERATION_CHANGED") !=
					s_state.reasons.end()) {
				Finalize(Status::kFail, "FAIL: physical telemetry is invalid");
				return;
			}
			if (!ObserveGpuEvidence()) {
				Finalize(Status::kFail, "FAIL: GPU evidence is invalid");
				return;
			}
			if (!RefreshGpuEvidenceForCommittedEpoch(committedEpoch))
				return;

			if (observation.frames >=
					s_state.request.minimumObservationFrames &&
				observation.elapsedMilliseconds >=
					s_state.request.minimumObservationMilliseconds &&
				BrokenCloudPassEvidenceComplete()) {
				AddReason(
					"CORRELATED_CUBE_HAS_EXACT_CLEAR_AND_NONZERO_CLOUD_OPACITY");
				AddReason(
					"DEPTH_VALID_RECEIVERS_HAVE_EXACT_ONE_AND_VARIED_ATTENUATION");
				AddReason(
					"CORRELATED_PHYSICAL_TELEMETRY_HAS_CLEAR_AND_CLOUD_RAYS");
				AddReason("ALL_AUTOMATED_INVARIANTS_SATISFIED");
				Finalize(Status::kPass, "PASS: automated invariants satisfied");
				return;
			}

			if (observation.elapsedMilliseconds <
				s_state.request.timeoutMilliseconds) {
				return;
			}
			if (!observation.everSunEligible) {
				AddReason("NO_FINITE_ABOVE_HORIZON_SUN_DURING_OBSERVATION");
				Finalize(
					Status::kInconclusiveWeather,
					"INCONCLUSIVE_WEATHER: no eligible daylight sun observed");
				return;
			}
			if (FreshExactClearSkyEvidenceComplete()) {
				AddReason("FRESH_GPU_CUBE_IS_EXACT_CLEAR_SKY");
				AddReason("NO_VISIBLE_CLOUD_OPACITY_DURING_OBSERVATION");
				Finalize(
					Status::kInconclusiveWeather,
					"INCONCLUSIVE_WEATHER: captured sky was exactly clear");
				return;
			}
			if (!CoreEvidenceComplete()) {
				AddMissingCoreReasons();
				Finalize(Status::kFail, "FAIL: required runtime evidence missing");
				return;
			}
			if (!observation.cubeHasBrokenCloudMix) {
				AddReason("BROKEN_CLOUD_MIX_NOT_OBSERVED");
				if (!observation.cubeHasClearOpacity)
					AddReason("WEATHER_HAS_NO_EXACT_CLEAR_CLOUD_TEXEL");
				if (!observation.cubeHasCloudOpacity)
					AddReason("WEATHER_HAS_NO_NONZERO_CLOUD_OPACITY");
				Finalize(
					Status::kInconclusiveWeather,
					"INCONCLUSIVE_WEATHER: clear/cloud mix not observed");
				return;
			}
			AddMissingBrokenCloudPassReasons();
			Finalize(
				Status::kFail,
				"FAIL: broken clouds did not produce a valid receiver mask");
		}
	}

	void TickAtPresent(const PresentEvidence& evidence) noexcept
	{
		try {
			std::unique_lock lock(s_mutex, std::try_to_lock);
			if (!lock.owns_lock())
				return;
			++s_state.presentOrdinal;
			(void)TryStartExternalProtocolWorker(s_state.presentOrdinal);

			ResultCompletion completion;
			while (s_protocolWorker &&
				s_protocolWorker->TryTakeCompletion(completion)) {
				if (completion.requestId != s_state.request.requestId ||
					!s_state.resultQueued) {
					continue;
				}
				if (completion.succeeded) {
					s_state.status = completion.verdict;
					s_state.statusText = s_state.armedStatusText;
					SPDLOG_INFO(
						"[CloudShadows][Acceptance] {} request={} frames={} reasons={} result=durable",
						StatusName(completion.verdict), completion.requestId,
						s_state.observation.frames, s_state.reasons.size());
				} else {
					AddReason("RESULT_DELIVERY_FAILED: " + completion.error);
					s_state.status = Status::kFail;
					s_state.statusText =
						"FAIL: durable acceptance result delivery failed";
					SPDLOG_ERROR(
						"[CloudShadows][Acceptance] Result delivery failed request={}: {}",
						completion.requestId, completion.error);
				}
				s_state.verdictArmed = false;
				s_state.resultQueued = false;
				s_state.pendingResultDocument.reset();
				s_runnerBusy.store(false, std::memory_order_release);
			}

			Request protocolRequest;
			if (s_protocolWorker &&
				s_protocolWorker->TryTakeRequest(protocolRequest)) {
				BeginRequest(std::move(protocolRequest), evidence);
			}

			if (s_state.hasPendingLocalRequest) {
				Request request = std::move(s_state.pendingLocalRequest);
				s_state.hasPendingLocalRequest = false;
				BeginRequest(std::move(request), evidence);
			}
			if (s_state.status == Status::kRunning) {
				if (s_state.verdictArmed || s_state.resultQueued)
					return;
				ObserveRunning(evidence);
				return;
			}
		} catch (const std::exception& exception) {
			try {
				SPDLOG_ERROR(
					"[CloudShadows][Acceptance] Present tick failed: {}",
					exception.what());
			} catch (...) {
			}
		} catch (...) {
			try {
				SPDLOG_ERROR(
					"[CloudShadows][Acceptance] Present tick failed with an unknown exception");
			} catch (...) {
			}
		}
	}

	void CompletePresent(bool succeeded) noexcept
	{
		try {
			std::unique_lock lock(s_mutex, std::try_to_lock);
			if (!lock.owns_lock() || !s_state.verdictArmed ||
				s_state.resultQueued) {
				return;
			}
			if (!succeeded) {
				s_state.verdictArmed = false;
				s_state.pendingResultDocument.reset();
				s_state.armedVerdict = Status::kIdle;
				s_state.armedStatusText.clear();
				s_state.reasons.clear();
				s_state.status = Status::kRunning;
				s_state.statusText =
					"Present failed; discarded verdict and resumed observation";
				return;
			}
			if (s_state.request.local) {
				SPDLOG_INFO(
					"[CloudShadows][Acceptance] {} local request={} frames={} "
					"elapsedMs={} reasons={} screenEvidenceRequest={} "
					"cubeEvidenceRequest={} correlatedDispatch={} "
					"result=present-confirmed",
					StatusName(s_state.armedVerdict), s_state.request.requestId,
					s_state.observation.frames,
					s_state.observation.elapsedMilliseconds,
					s_state.reasons.size(),
					s_state.observation.requestedScreenEvidenceId,
					s_state.observation.requestedCubeEvidenceId,
					FindCorrelatedPhysicalTelemetry()
						? s_state.observation.screenEvidence.dispatchOrdinal
						: 0u);
				s_state.status = s_state.armedVerdict;
				s_state.statusText = s_state.armedStatusText;
				s_state.verdictArmed = false;
				s_state.armedVerdict = Status::kIdle;
				s_state.armedStatusText.clear();
				CancelPendingAcceptanceEvidenceRequests();
				s_runnerBusy.store(false, std::memory_order_release);
				return;
			}
			if (!s_state.pendingResultDocument)
				return;
			ResultWriteJob job;
			job.requestId = s_state.request.requestId;
			job.verdict = s_state.armedVerdict;
			job.document = std::move(*s_state.pendingResultDocument);
			if (!TryQueueProtocolResult(std::move(job))) {
				*s_state.pendingResultDocument = std::move(job.document);
				s_state.statusText = std::string(StatusName(s_state.armedVerdict)) +
					": successful Present confirmed; result queue busy";
				return;
			}
			s_state.pendingResultDocument.reset();
			s_state.resultQueued = true;
			s_state.statusText = std::string(StatusName(s_state.armedVerdict)) +
				": successful Present confirmed; writing durable result";
		} catch (const std::exception& exception) {
			try {
				SPDLOG_ERROR(
					"[CloudShadows][Acceptance] Present completion failed: {}",
					exception.what());
			} catch (...) {
			}
		} catch (...) {
			try {
				SPDLOG_ERROR(
					"[CloudShadows][Acceptance] Present completion failed with an unknown exception");
			} catch (...) {
			}
		}
	}

	bool StartLocalRequest() noexcept
	{
		try {
			bool expected = false;
			if (!s_runnerBusy.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel)) {
				return false;
			}
			std::lock_guard lock(s_mutex);
			if (s_state.status == Status::kRunning ||
				s_state.hasPendingLocalRequest) {
				s_runnerBusy.store(false, std::memory_order_release);
				return false;
			}
			Request request;
			request.requestId = MakeRequestId();
			request.requestedUtc = NowUtc();
			request.local = true;
			s_state.pendingLocalRequest = request;
			s_state.hasPendingLocalRequest = true;
			s_state.status = Status::kRunning;
			s_state.statusText = "Acceptance request queued for next Present";
			return true;
		} catch (...) {
			s_runnerBusy.store(false, std::memory_order_release);
			return false;
		}
	}

	StatusSnapshot GetStatusSnapshot() noexcept
	{
		StatusSnapshot snapshot;
		try {
			std::lock_guard lock(s_mutex);
			snapshot.status = s_state.status;
			snapshot.deliveryPending =
				s_state.verdictArmed || s_state.resultQueued;
			snapshot.framesObserved = s_state.observation.frames;
			snapshot.elapsedMilliseconds =
				s_state.observation.elapsedMilliseconds;
			snapshot.timeoutMilliseconds =
				s_state.request.timeoutMilliseconds;
			auto copyText = [](auto& destination, std::string_view source) {
				const std::size_t count = (std::min)(
					destination.size() - 1u, source.size());
				std::memcpy(destination.data(), source.data(), count);
				destination[count] = '\0';
			};
			const std::string_view requestId = s_state.hasPendingLocalRequest
				? std::string_view(s_state.pendingLocalRequest.requestId)
				: std::string_view(s_state.request.requestId);
			copyText(snapshot.requestId, requestId);
			copyText(snapshot.sessionId, s_protocolIdentity.sessionId);
			copyText(snapshot.statusText, s_state.statusText);
		} catch (...) {
			snapshot.status = Status::kFail;
			constexpr std::string_view text = "Acceptance status unavailable";
			std::memcpy(snapshot.statusText.data(), text.data(), text.size());
			snapshot.statusText[text.size()] = '\0';
		}
		return snapshot;
	}

	bool IsRunning() noexcept
	{
		return s_runnerBusy.load(std::memory_order_acquire);
	}
}
