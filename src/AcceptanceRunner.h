#pragma once

#include <array>
#include <cstdint>

namespace CloudShadows::AcceptanceRunner
{
	enum class Status : std::uint32_t
	{
		kIdle = 0,
		kRunning,
		kPass,
		kFail,
		kInconclusiveWeather
	};

	struct StatusSnapshot
	{
		Status status{ Status::kIdle };
		bool deliveryPending{ false };
		std::uint8_t reserved[3]{};
		std::uint64_t framesObserved{ 0 };
		std::uint64_t elapsedMilliseconds{ 0 };
		std::uint64_t timeoutMilliseconds{ 0 };
		std::array<char, 40> requestId{};
		std::array<char, 40> sessionId{};
		std::array<char, 192> statusText{};
	};

	struct PresentEvidence
	{
		std::uint64_t actualDfLightSwapHits{ 0 };
		std::uint64_t lastVanillaShaderHash{ 0 };
		std::uint32_t lastPixelDescriptor{ 0 };
		std::uint32_t reserved{ 0 };
	};

	// Called once from the authoritative swap-chain Present hook after the
	// completed-frame latch has been finalized and before the real Present.
	// This function never performs protocol file I/O, sleeps, flushes the GPU,
	// launches a process, or injects input.
	void TickAtPresent(const PresentEvidence& evidence) noexcept;

	// Called after the corresponding real Present returns. A file-backed verdict
	// is not handed to the protocol worker until a successful Present confirms
	// that the observed frame was accepted by the swap chain.
	void CompletePresent(bool succeeded) noexcept;

	// Starts the same passive observation used by a request file, without
	// creating or controlling a Fallout process. Intended for the F11 menu.
	[[nodiscard]] bool StartLocalRequest() noexcept;

	// Returns a short POD copy under the runner mutex. The Present-side tick uses
	// try-lock and therefore never waits for an overlapping UI snapshot.
	[[nodiscard]] StatusSnapshot GetStatusSnapshot() noexcept;

	// Lock-free render-path gate for diagnostic GPU telemetry. Normal gameplay
	// must not pay for acceptance-only sampling when no request is active.
	[[nodiscard]] bool IsRunning() noexcept;
}
