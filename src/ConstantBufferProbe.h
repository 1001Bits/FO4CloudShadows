// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace FO4CS
{
    // Developer diagnostic: copies a bound constant buffer about once a second
    // and returns selected registers of its bound window when the copy has
    // completed. Never flushes or waits on the render thread.
    class ConstantBufferProbe
    {
    public:
        using Register = std::array<float, 4>;
        using Sample = std::vector<std::pair<UINT, Register>>;

        // True when `sample` holds a completed readback.
        bool Poll(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
            UINT firstConstant, std::initializer_list<UINT> registers,
            Sample& sample) noexcept
        {
            sample.clear();
            if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
                return false;
            if (pending_) {
                BOOL complete = FALSE;
                const auto status = context->GetData(query_.Get(), &complete,
                    sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (status == S_FALSE || (status == S_OK && !complete))
                    return false;
                pending_ = false;
                if (status != S_OK)
                    return false;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(staging_.Get(), 0, D3D11_MAP_READ,
                        D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) || !mapped.pData)
                    return false;
                D3D11_BUFFER_DESC description{};
                staging_->GetDesc(&description);
                for (const UINT index : registers_) {
                    const std::uint64_t offset =
                        (static_cast<std::uint64_t>(first_) + index) * 16u;
                    if (offset + 16u > description.ByteWidth)
                        continue;
                    Register value{};
                    std::memcpy(value.data(),
                        static_cast<const std::byte*>(mapped.pData) + offset,
                        sizeof(value));
                    sample.emplace_back(index, value);
                }
                context->Unmap(staging_.Get(), 0);
                return !sample.empty();
            }
            if (!buffer || GetTickCount64() < nextCopy_)
                return false;
            nextCopy_ = GetTickCount64() + 1000;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            D3D11_BUFFER_DESC source{};
            buffer->GetDesc(&source);
            D3D11_BUFFER_DESC previous{};
            if (staging_)
                staging_->GetDesc(&previous);
            if (device_ != device || !staging_ || previous.ByteWidth != source.ByteWidth) {
                device_ = device;
                staging_.Reset();
                query_.Reset();
                D3D11_BUFFER_DESC description{};
                description.ByteWidth = source.ByteWidth;
                description.Usage = D3D11_USAGE_STAGING;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                const D3D11_QUERY_DESC event{ D3D11_QUERY_EVENT, 0 };
                if (FAILED(device->CreateBuffer(&description, nullptr, &staging_)) ||
                    FAILED(device->CreateQuery(&event, &query_))) {
                    staging_.Reset();
                    query_.Reset();
                    return false;
                }
            }
            context->CopyResource(staging_.Get(), buffer);
            context->End(query_.Get());
            first_ = firstConstant;
            registers_.assign(registers.begin(), registers.end());
            pending_ = true;
            return false;
        }

        static std::string Describe(const Sample& sample)
        {
            std::string text;
            char buffer[96];
            for (const auto& [index, value] : sample) {
                std::snprintf(buffer, sizeof(buffer), " c%u=(%.4f,%.4f,%.4f,%.4f)",
                    index, value[0], value[1], value[2], value[3]);
                text += buffer;
            }
            return text;
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging_;
        Microsoft::WRL::ComPtr<ID3D11Query> query_;
        std::vector<UINT> registers_;
        ULONGLONG nextCopy_{};
        UINT first_{};
        bool pending_{};
    };
}
