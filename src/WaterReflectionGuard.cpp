#include "WaterReflectionGuard.h"

#include "PCH.h"
#include "RuntimeAPI.h"

#include <atomic>
#include <mutex>

namespace
{
    using FO4CS::AddressKind;
    using FO4CS::F4SECompat::RuntimeTarget;
    using FO4CS::RuntimeAPI;
    using FO4CS::RuntimeRVA;

    // Flat IDs are exact Address Library entries; the VR RVA comes from the
    // 1.2.72 PDB/binary correspondence and is selected only after RuntimeAPI
    // has proved that the loaded image is Fallout4VR.exe 1.2.72.
    constexpr std::uint64_t kUpdateLegacyID = 907876;
    constexpr std::uint64_t kUpdateAEID = 2213919;
    constexpr std::uintptr_t kUpdateVRRva = 0x07CA020;

    using UpdateFn = std::uint8_t (*)(void* reflections);

    UpdateFn g_originalUpdate = nullptr;
    std::mutex g_installMutex;
    std::atomic<bool> g_installAttempted{ false };
    std::atomic<bool> g_installed{ false };
    thread_local std::uint32_t g_reflectionUpdateDepth = 0;

    [[nodiscard]] std::uintptr_t ResolveUpdate() noexcept
    {
        auto& runtime = RuntimeAPI::GetSingleton();
        switch (runtime.Target()) {
        case RuntimeTarget::kLegacy:
            return runtime.ResolveID(kUpdateLegacyID, AddressKind::kExecutable);
        case RuntimeTarget::kAE:
            return runtime.ResolveID(kUpdateAEID, AddressKind::kExecutable);
        case RuntimeTarget::kVR:
            return runtime.ResolveRVA(
                RuntimeRVA{ 0, 0, kUpdateVRRva }, AddressKind::kExecutable);
        default:
            return 0;
        }
    }

    [[nodiscard]] bool IsUnpatchedFunctionEntry(std::uintptr_t address) noexcept
    {
        auto& runtime = RuntimeAPI::GetSingleton();
        if (!runtime.ValidateMemory(address, 8, AddressKind::kExecutable))
            return false;
        const auto* code = reinterpret_cast<const std::uint8_t*>(address);
        // Fail closed on common near/absolute jump encodings. A second owner
        // on an already-detoured prologue has unknowable ordering/teardown.
        return code[0] != 0xE9 && code[0] != 0xEB &&
            !(code[0] == 0xFF && code[1] == 0x25) &&
            code[0] != 0xCC && code[0] != 0x00;
    }

    struct ReflectionUpdateScope
    {
        ReflectionUpdateScope() noexcept { ++g_reflectionUpdateDepth; }
        ~ReflectionUpdateScope() { --g_reflectionUpdateDepth; }
        ReflectionUpdateScope(const ReflectionUpdateScope&) = delete;
        ReflectionUpdateScope& operator=(const ReflectionUpdateScope&) = delete;
    };

    std::uint8_t HookWaterReflectionUpdate(void* reflections)
    {
        if (!g_originalUpdate)
            return 0;
        // Observation only: Fallout receives the exact object and runs its
        // normal scheduler; this scope changes TLS bookkeeping alone.
        ReflectionUpdateScope scope;
        return g_originalUpdate(reflections);
    }
}

namespace FO4CS::WaterReflectionGuard
{
    bool Install() noexcept
    {
        std::scoped_lock lock(g_installMutex);
        if (g_installed.load(std::memory_order_acquire))
            return true;
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel))
            return false;

        const auto updateAddress = ResolveUpdate();
        if (updateAddress == 0 || !IsUnpatchedFunctionEntry(updateAddress)) {
            SPDLOG_ERROR(
                "[CloudShadows] Water-reflection guard unavailable or already "
                "owned (update={:#x}); reflection passes rely on the render-"
                "target checks alone",
                updateAddress);
            return false;
        }

        g_originalUpdate = reinterpret_cast<UpdateFn>(updateAddress);
        // Installed once from the F4SE load thread before any render thread
        // can execute this prologue; only the calling thread is enlisted, so
        // Detours never allocates while other threads are suspended.
        LONG error = DetourTransactionBegin();
        if (error == NO_ERROR)
            error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR) {
            error = DetourAttach(
                reinterpret_cast<PVOID*>(&g_originalUpdate),
                reinterpret_cast<PVOID>(&HookWaterReflectionUpdate));
        }
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            g_originalUpdate = nullptr;
            SPDLOG_ERROR(
                "[CloudShadows] Water-reflection guard detour failed: {}", error);
            return false;
        }
        error = DetourTransactionCommit();
        if (error != NO_ERROR) {
            g_originalUpdate = nullptr;
            SPDLOG_ERROR(
                "[CloudShadows] Water-reflection guard commit failed: {}", error);
            return false;
        }
        g_installed.store(true, std::memory_order_release);
        SPDLOG_INFO(
            "[CloudShadows] Water-reflection guard installed (update={:#x})",
            updateAddress);
        return true;
    }

    bool IsReflectionUpdateActive() noexcept
    {
        return g_reflectionUpdateDepth != 0;
    }
}
