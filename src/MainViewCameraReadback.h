// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FO4CS
{
    // Read the exact b12 camera consumed by a validated main DFLight draw.
    // GraphicsState::posAdjust can still describe a loading or secondary
    // camera here. The first completed copy establishes the world anchor;
    // later copies, a few per second, drive travel re-anchoring.
    class MainViewCameraReadback
    {
    public:
        using Point = std::array<float, 3>;

        [[nodiscard]] bool Pending() const noexcept { return pending_; }

        void Reset() noexcept
        {
            staging_.Reset();
            query_.Reset();
            device_.Reset();
            generation_ = 0;
            pending_ = false;
            nextAttempt_ = 0;
        }

        bool TryRead(ID3D11DeviceContext* context, ID3D11Buffer* perFrame,
            UINT firstConstant, bool vr, std::uint64_t generation,
            Point& camera) noexcept
        {
            camera = {};
            if (!context || !perFrame ||
                context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
                return false;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            if (device_ != device || generation_ != generation) {
                Reset();
                device_ = device;
                generation_ = generation;
            }

            if (pending_) {
                BOOL complete = FALSE;
                const auto status = context->GetData(query_.Get(), &complete,
                    sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (status == S_FALSE || (status == S_OK && !complete)) {
                    if (GetTickCount64() - submittedAt_ > 5000)
                        BackOff();
                    return false;
                }
                if (status != S_OK) {
                    BackOff();
                    return false;
                }
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const auto result = context->Map(staging_.Get(), 0,
                    D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
                    if (GetTickCount64() - submittedAt_ > 5000)
                        BackOff();
                    return false;
                }
                if (FAILED(result) || !mapped.pData) {
                    BackOff();
                    return false;
                }
                Point left{}, right{};
                const auto* bytes = static_cast<const std::byte*>(mapped.pData);
                std::memcpy(left.data(), bytes + cameraOffset_, sizeof(left));
                right = left;
                if (stereo_)
                    std::memcpy(right.data(), bytes + cameraOffset_ + 16, sizeof(right));
                context->Unmap(staging_.Get(), 0);
                pending_ = false;
                for (unsigned i = 0; i < 3; ++i) {
                    if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
                        return false;
                    camera[i] = left[i] * 0.5f + right[i] * 0.5f;
                    if (!std::isfinite(camera[i]))
                        return false;
                }
                return true;
            }

            if (GetTickCount64() < nextAttempt_)
                return false;
            D3D11_BUFFER_DESC source{};
            perFrame->GetDesc(&source);
            // Exact flat c35 / VR c59,c60 ABI, including a D3D11.1 window.
            const std::uint64_t offset =
                (static_cast<std::uint64_t>(firstConstant) + (vr ? 59u : 35u)) * 16u;
            if ((source.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0 ||
                offset + (vr ? 32u : 16u) > source.ByteWidth)
                return false;
            D3D11_BUFFER_DESC previous{};
            if (staging_)
                staging_->GetDesc(&previous);
            if (!staging_ || previous.ByteWidth != source.ByteWidth) {
                staging_.Reset();
                D3D11_BUFFER_DESC description{};
                description.ByteWidth = source.ByteWidth;
                description.Usage = D3D11_USAGE_STAGING;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(device->CreateBuffer(&description, nullptr, &staging_))) {
                    BackOff();
                    return false;
                }
            }
            if (!query_) {
                const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
                if (FAILED(device->CreateQuery(&description, &query_))) {
                    BackOff();
                    return false;
                }
            }
            // Full buffer copy: partial constant-buffer copies are not a
            // portable D3D11 operation. No binding/state changes, Flush or wait.
            context->CopyResource(staging_.Get(), perFrame);
            context->End(query_.Get());
            cameraOffset_ = static_cast<UINT>(offset);
            stereo_ = vr;
            submittedAt_ = GetTickCount64();
            pending_ = true;
            return false;
        }

    private:
        void BackOff() noexcept
        {
            pending_ = false;
            staging_.Reset();
            query_.Reset();
            nextAttempt_ = GetTickCount64() + 5000;
        }
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging_;
        Microsoft::WRL::ComPtr<ID3D11Query> query_;
        std::uint64_t generation_{};
        ULONGLONG submittedAt_{}, nextAttempt_{};
        UINT cameraOffset_{};
        bool pending_{}, stereo_{};
    };
}
