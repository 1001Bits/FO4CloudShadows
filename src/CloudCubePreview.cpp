// SPDX-License-Identifier: GPL-3.0-only
#include "CloudCubePreview.h"

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>
#include <utility>

namespace FO4CS
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        constexpr char kShader[] = R"(
Texture2DArray<float> Faces : register(t0);
RWTexture2D<float4> Atlas : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height, slices;
    Faces.GetDimensions(width, height, slices);
    if (any(id.xy >= uint2(width * 3, height * 2))) return;
    uint face = id.x / width + (id.y / height) * 3;
    float opacity = saturate(Faces.Load(int4(id.x % width, id.y % height, face, 0)));
    Atlas[id.xy] = float4(opacity.xxx, 1);
}
)";

        struct ComputeState
        {
            ID3D11DeviceContext* context;
            ComPtr<ID3D11ComputeShader> shader;
            std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
            UINT instanceCount = static_cast<UINT>(instances.size());
            ComPtr<ID3D11ShaderResourceView> source;
            ComPtr<ID3D11UnorderedAccessView> output;
            ComPtr<ID3D11Predicate> predicate;
            BOOL predicateValue{};

            explicit ComputeState(ID3D11DeviceContext* ctx) noexcept : context(ctx)
            {
                context->CSGetShader(&shader, instances.data(), &instanceCount);
                context->CSGetShaderResources(0, 1, &source);
                context->CSGetUnorderedAccessViews(0, 1, &output);
                context->GetPredication(&predicate, &predicateValue);
            }
            ~ComputeState()
            {
                ID3D11ShaderResourceView* noSource = nullptr;
                ID3D11UnorderedAccessView* noOutput = nullptr;
                context->CSSetShaderResources(0, 1, &noSource);
                context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
                context->CSSetShader(shader.Get(), instances.data(), instanceCount);
                context->CSSetShaderResources(0, 1, source.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, output.GetAddressOf(), nullptr);
                context->SetPredication(predicate.Get(), predicateValue);
                for (UINT i = 0; i < instanceCount; ++i)
                    if (instances[i]) instances[i]->Release();
            }
        };
    }

    void CloudCubePreview::Reset() noexcept
    {
        sources_ = {};
        atlasOutput_.Reset(); atlasView_.Reset(); atlas_.Reset();
        shader_.Reset(); device_.Reset();
        faceSize_ = nextSource_ = 0;
        compileAttempted_ = false;
    }

    ID3D11ShaderResourceView* CloudCubePreview::Update(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* committedCube) noexcept
    {
        if (!context || !committedCube) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        committedCube->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBE ||
            viewDesc.TextureCube.MostDetailedMip != 0 ||
            (viewDesc.Format != DXGI_FORMAT_R16_FLOAT && viewDesc.Format != DXGI_FORMAT_R32_FLOAT))
            return nullptr;
        ComPtr<ID3D11Resource> resource;
        committedCube->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.ArraySize != 6 || desc.Width != desc.Height || !desc.Width ||
            desc.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 3 || desc.SampleDesc.Count != 1)
            return nullptr;
        ComPtr<ID3D11Device> device, sourceDevice;
        context->GetDevice(&device);
        texture->GetDevice(&sourceDevice);
        if (device != sourceDevice) return nullptr;
        if (device_ != device) { Reset(); device_ = device; }

        if (!compileAttempted_) {
            compileAttempted_ = true;
            ComPtr<ID3DBlob> code, errors;
            const HRESULT compiled = D3DCompile(kShader, sizeof(kShader) - 1,
                "CloudCubePreview", nullptr, nullptr, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(compiled) || FAILED(device->CreateComputeShader(
                    code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_))) {
                SPDLOG_ERROR("[CloudShadows][Menu] Cubemap preview shader unavailable: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader creation failed");
                return nullptr;
            }
        }
        if (!shader_) return nullptr;

        // The producer alternates two immutable published resources. Retain at
        // most those two array views, without creating an SRV every menu frame.
        ID3D11ShaderResourceView* faces = nullptr;
        for (const auto& source : sources_)
            if (source.texture == texture) faces = source.arrayView.Get();
        if (!faces) {
            Source replacement;
            D3D11_SHADER_RESOURCE_VIEW_DESC arrayDesc{};
            arrayDesc.Format = viewDesc.Format;
            arrayDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            arrayDesc.Texture2DArray.MipLevels = 1;
            arrayDesc.Texture2DArray.ArraySize = 6;
            if (FAILED(device->CreateShaderResourceView(texture.Get(), &arrayDesc,
                    &replacement.arrayView))) return nullptr;
            replacement.texture = texture;
            faces = replacement.arrayView.Get();
            sources_[nextSource_] = std::move(replacement);
            nextSource_ = (nextSource_ + 1) % static_cast<UINT>(sources_.size());
        }

        if (!atlas_ || faceSize_ != desc.Width) {
            D3D11_TEXTURE2D_DESC atlasDesc{};
            atlasDesc.Width = desc.Width * 3; atlasDesc.Height = desc.Height * 2;
            atlasDesc.MipLevels = atlasDesc.ArraySize = atlasDesc.SampleDesc.Count = 1;
            atlasDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            atlasDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            Ptr<ID3D11Texture2D> atlas;
            Ptr<ID3D11ShaderResourceView> srv;
            Ptr<ID3D11UnorderedAccessView> uav;
            if (FAILED(device->CreateTexture2D(&atlasDesc, nullptr, &atlas)) ||
                FAILED(device->CreateShaderResourceView(atlas.Get(), nullptr, &srv)) ||
                FAILED(device->CreateUnorderedAccessView(atlas.Get(), nullptr, &uav))) return nullptr;
            atlas_ = std::move(atlas); atlasView_ = std::move(srv); atlasOutput_ = std::move(uav);
            faceSize_ = desc.Width;
        }
        const ComputeState saved(context);
        context->SetPredication(nullptr, FALSE);
        context->CSSetShader(shader_.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &faces);
        context->CSSetUnorderedAccessViews(0, 1, atlasOutput_.GetAddressOf(), nullptr);
        context->Dispatch((desc.Width * 3 + 7) / 8, (desc.Height * 2 + 7) / 8, 1);
        return atlasView_.Get();
    }
}
