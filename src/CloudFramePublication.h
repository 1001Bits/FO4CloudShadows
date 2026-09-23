// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <utility>

namespace FO4CS
{
    // Used by WorldClouds under its resource mutex. Only Present can swap the
    // immutable read cube with the cube receiving the current main-Sky draws.
    class CloudFramePublication
    {
    public:
        enum class PresentResult { Withdrawn, Published, Retained };
        [[nodiscard]] uint32_t ReadIndex() const noexcept { return read_; }
        [[nodiscard]] uint32_t WriteIndex() const noexcept { return write_; }
        [[nodiscard]] bool Active() const noexcept { return active_; }
        [[nodiscard]] bool Authenticated() const noexcept { return authenticated_; }
        [[nodiscard]] uint64_t Serial() const noexcept { return serial_; }
        [[nodiscard]] uint64_t PublishedEpoch() const noexcept { return published_; }
        [[nodiscard]] uint64_t PendingEpoch() const noexcept { return active_ ? serial_ : 0; }

        void Begin() noexcept
        {
            if (active_) return;
            if (++serial_ == 0) ++serial_;
            active_ = authenticated_ = true;
        }

        void Reject() noexcept
        {
            authenticated_ = false;
            published_ = 0;
        }

        void CancelCapture() noexcept { active_ = authenticated_ = false; }

        void Withdraw(bool preserveCapture = false) noexcept
        {
            published_ = 0;
            if (!preserveCapture) CancelCapture();
        }

        void Reset(bool restartSerial = false) noexcept
        {
            Withdraw();
            read_ = 0;
            write_ = 1;
            if (restartSerial) serial_ = 0;
        }

        [[nodiscard]] bool Present(bool worldReady) noexcept
        {
            if (!worldReady || !active_ || !authenticated_) {
                Withdraw();
                return false;
            }
            CancelCapture();
            published_ = serial_;
            std::swap(read_, write_);
            return true;
        }

        [[nodiscard]] PresentResult CompletePresent(
            bool worldReady, bool companionWindow) noexcept
        {
            // VR's companion can present repeatedly without a new rendered
            // Sky. An unchanged valid world keeps its completed field. A new
            // (including cloudless) Sky, rejected capture, reset, or world
            // transition still goes through the normal publication contract.
            if (companionWindow && worldReady && !active_ && published_ != 0)
                return PresentResult::Retained;
            return Present(worldReady) ? PresentResult::Published :
                                         PresentResult::Withdrawn;
        }

    private:
        uint64_t serial_{ 0 };
        uint64_t published_{ 0 };
        uint32_t read_{ 0 };
        uint32_t write_{ 1 };
        bool active_{ false };
        bool authenticated_{ false };
    };
}
