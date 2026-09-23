#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Versioned, allocation-free protocol used to place the standalone Cloud
// Shadows controls inside a compatible host menu.  The protocol is delivered
// through F4SE messaging, so it does not add a public DLL export and never
// shares ImGui objects or C++ library types across module boundaries.
namespace FO4CloudShadowsMenuBridge
{
	inline constexpr std::uint32_t kHandshakeMessage = 0x424D5343u;  // "CSMB"
	inline constexpr std::uint32_t kHandshakeMagic = 0x31534346u;    // "FCS1"
	inline constexpr std::uint32_t kAbiVersion = 2u;

	enum HostCapability : std::uint32_t
	{
		kHostOwnsMenu = 1u << 0u,
		kHostForwardsBeginTechnique = 1u << 1u,
		kHostActivationPending = 1u << 2u,
	};

	inline constexpr std::uint32_t kRequiredHostCapabilities =
		kHostOwnsMenu | kHostForwardsBeginTechnique;

	enum ApiCapability : std::uint32_t
	{
		kApiSettings = 1u << 0u,
		kApiDiagnostics = 1u << 1u,
		kApiBeginTechniqueObserver = 1u << 2u,
	};

	inline constexpr std::uint32_t kApiCapabilities =
		kApiSettings | kApiDiagnostics | kApiBeginTechniqueObserver;

	// DiagnosticsV1::reserved is an ABI-preserving hard-unavailable bitmask.
	// A set bit means the named interception boundary is not active; counters
	// must not be interpreted as evidence that the effect is operational.
	enum HookUnavailable : std::uint32_t
	{
		kCreateDeviceHookUnavailable = 1u << 0u,
		kPresentHookUnavailable = 1u << 1u,
		kCreateVertexShaderHookUnavailable = 1u << 2u,
		kCreatePixelShaderHookUnavailable = 1u << 3u,
		kDrawHookUnavailable = 1u << 4u,
		kBeginTechniqueHookUnavailable = 1u << 5u,
	};

	struct SettingsV1
	{
		std::uint32_t structSize;
		std::uint32_t abiVersion;
		std::uint32_t enabled;
		std::uint32_t reserved;
		float opacity;
		float cloudHeight;
		float layerHeightStep;
		float worldTileSize;
		float layerScaleMultiplier;
		float verticalOpticalDepth;
		float sunAngularRadius;
		float maxOpticalSlant;
		float baseMipBias;
		float sunFadeStart;
		float sunFadeEnd;
		float debugMode;
		// Legacy ABI slot. Capture always composites every authenticated draw.
		float maxLayers;
	};

	struct alignas(8) DiagnosticsV1
	{
		std::uint32_t structSize;
		std::uint32_t abiVersion;
		std::uint32_t worldCloudReady;
		std::uint32_t shadowMaskValid;
		std::uint32_t activeLayers;
		std::uint32_t skyDrawsSeen;
		std::uint32_t cloudDrawCount;
		std::uint32_t lastSkyTechnique;
		std::uint32_t tileCaptures;
		std::uint32_t replacementDraws;
		std::uint32_t captureOverflow;
		std::uint32_t reserved;
		std::uint64_t pendingEpoch;
		std::uint64_t committedEpoch;
		float anchorX;
		float anchorY;
		float anchorZ;
		float reservedFloat;
	};

	// Requests are incremental, one-way lifetime claims. The return value is the
	// cumulative accepted capability mask; late BeginTechnique forwarding is
	// rejected once the standalone detour has been installed.
	//
	// Settings mutation callbacks are safe to call from another thread: the
	// implementation queues an immutable snapshot for the render thread. Load
	// and save remain render-thread operations and fail/no-op off that thread.
	using SetHostActiveFn = std::uint32_t (*)(std::uint32_t hostCapabilities) noexcept;
	using GetSettingsFn = bool (*)(SettingsV1* output, std::uint32_t outputSize) noexcept;
	using ApplySettingsFn = bool (*)(const SettingsV1* input, std::uint32_t inputSize) noexcept;
	using RestoreDefaultsFn = void (*)() noexcept;
	using LoadSettingsFn = bool (*)() noexcept;
	using SaveSettingsFn = void (*)() noexcept;
	using GetDiagnosticsFn = bool (*)(DiagnosticsV1* output, std::uint32_t outputSize) noexcept;
	using ObserveBeginTechniqueFn = void (*)(
		void* shader,
		std::uint32_t vertexDescriptor,
		std::uint32_t hullDescriptor,
		std::uint32_t domainDescriptor,
		std::uint32_t pixelDescriptor,
		void* outputStruct,
		std::uint32_t succeeded) noexcept;

	struct ApiV1
	{
		std::uint32_t structSize;
		std::uint32_t abiVersion;
		std::uint32_t pluginVersionMajor;
		std::uint32_t pluginVersionMinor;
		std::uint32_t pluginVersionPatch;
		std::uint32_t capabilities;
		SetHostActiveFn setHostActive;
		GetSettingsFn getSettings;
		ApplySettingsFn applySettings;
		RestoreDefaultsFn restoreDefaults;
		LoadSettingsFn loadSettings;
		SaveSettingsFn saveSettings;
		GetDiagnosticsFn getDiagnostics;
		ObserveBeginTechniqueFn observeBeginTechnique;
	};

	struct HandshakeV1
	{
		std::uint32_t structSize;
		std::uint32_t magic;
		std::uint32_t requestedAbiVersion;
		std::uint32_t hostCapabilities;
		const ApiV1* api;
	};

	static_assert(sizeof(SettingsV1) == 68u);
	static_assert(sizeof(DiagnosticsV1) == 80u);
	static_assert(sizeof(ApiV1) == 88u);
	static_assert(sizeof(HandshakeV1) == 24u);
	static_assert(std::is_standard_layout_v<SettingsV1> &&
		std::is_trivially_copyable_v<SettingsV1>);
	static_assert(std::is_standard_layout_v<DiagnosticsV1> &&
		std::is_trivially_copyable_v<DiagnosticsV1>);
	static_assert(std::is_standard_layout_v<ApiV1> &&
		std::is_trivially_copyable_v<ApiV1>);
}
