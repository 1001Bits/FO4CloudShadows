// Private-corpus verification; no game/mod shader bytes are distributed.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

bool VerifySkyOutputEquivalence(ID3D11Device* device,
    const std::vector<uint8_t>& stock, const std::vector<uint8_t>& replacement)
{
    try {
        const auto check = [](HRESULT hr) {
            if (FAILED(hr)) throw std::runtime_error("D3D call failed in Sky comparison");
        };
        using Float4 = std::array<float, 4>;
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        context->ClearState();
        const char* source = R"hlsl(
cbuffer Params : register(b0) { float4 Parameters; };
struct V {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR0;
    float4 current : POSITION0;
    float4 previous : POSITION1;
    nointerpolation uint eye : POSITION2;
    float cull : SV_CullDistance0;
    float clip : SV_ClipDistance0;
};
V main(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(uv * float2(2,-2) + float2(-1,1), 0.5, 1);
    o.uv = uv; o.uv1 = uv + float2(0.25,0.75);
    o.color = float4(0.4,0.7,0.9,Parameters.x * (0.3 + uv.x * 0.7));
    o.current = float4(o.pos.xy * 0.5, 0.5, 1);
    o.previous = o.current + float4(0.15,-0.2,0.1,0);
    o.eye = (uint)Parameters.y; o.cull = 1; o.clip = 1;
    return o;
}
)hlsl";
        ComPtr<ID3DBlob> code, errors;
        check(D3DCompile(source, std::char_traits<char>::length(source), nullptr,
            nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors));
        ComPtr<ID3D11VertexShader> vertex;
        check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex));
        ComPtr<ID3D11PixelShader> first, second;
        check(device->CreatePixelShader(stock.data(), stock.size(), nullptr, &first));
        check(device->CreatePixelShader(replacement.data(), replacement.size(), nullptr, &second));
        const auto buffer = [&](const void* data, UINT size) {
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth = size;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = data;
            ComPtr<ID3D11Buffer> result;
            check(device->CreateBuffer(&desc, &initial, &result));
            return result;
        };
        Float4 parameters{}, material{};
        std::array<Float4, 71> camera{};
        for (UINT row = 0; row < camera.size(); ++row)
            camera[row] = { 0.1f, 0.2f, 0.3f, 1 + row * 0.003f };
        auto vertexConstants = buffer(parameters.data(), sizeof(parameters));
        auto pixelConstants = buffer(material.data(), sizeof(material));
        auto cameraConstants = buffer(camera.data(), sizeof(camera));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 2;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::array<ComPtr<ID3D11ShaderResourceView>, 2> inputs;
        for (UINT i = 0; i < 2; ++i) {
            const float scale = i == 0 ? 0.3f : 0.9f;
            std::array<Float4, 4> pixels{};
            for (UINT j = 0; j < 4; ++j)
                pixels[j] = { scale, 0.2f + j * 0.1f, 0.8f, scale * j / 3.0f };
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = pixels.data(); data.SysMemPitch = 2 * sizeof(Float4);
            ComPtr<ID3D11Texture2D> texture;
            check(device->CreateTexture2D(&desc, &data, &texture));
            check(device->CreateShaderResourceView(texture.Get(), nullptr, &inputs[i]));
        }
        desc.Width = desc.Height = 8;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        std::array<ComPtr<ID3D11Texture2D>, 2> targets;
        std::array<ComPtr<ID3D11RenderTargetView>, 2> targetViews;
        for (UINT i = 0; i < 2; ++i) {
            check(device->CreateTexture2D(&desc, nullptr, &targets[i]));
            check(device->CreateRenderTargetView(targets[i].Get(), nullptr, &targetViews[i]));
        }
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        check(device->CreateSamplerState(&sample, &sampler));
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rasterizer;
        check(device->CreateRasterizerState(&raster, &rasterizer));
        const D3D11_VIEWPORT viewport{ 0,0,8,8,0,1 };
        const auto draw = [&](ID3D11PixelShader* pixel) {
            ID3D11RenderTargetView* rt[]{ targetViews[0].Get(), targetViews[1].Get() };
            const float sentinel[]{ -50,-50,-50,-50 };
            for (auto view : rt) context->ClearRenderTargetView(view, sentinel);
            context->OMSetRenderTargets(2, rt, nullptr);
            context->RSSetState(rasterizer.Get());
            context->RSSetViewports(1, &viewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vertex.Get(), nullptr, 0);
            context->PSSetShader(pixel, nullptr, 0);
            ID3D11Buffer* cb = vertexConstants.Get(); context->VSSetConstantBuffers(0, 1, &cb);
            cb = pixelConstants.Get(); context->PSSetConstantBuffers(2, 1, &cb);
            cb = cameraConstants.Get(); context->PSSetConstantBuffers(12, 1, &cb);
            ID3D11ShaderResourceView* srvs[]{ inputs[0].Get(), inputs[1].Get() };
            context->PSSetShaderResources(0, 2, srvs);
            ID3D11SamplerState* samplers[]{ sampler.Get(), sampler.Get() };
            context->PSSetSamplers(0, 2, samplers);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            std::vector<float> values;
            for (const auto& target : targets) {
                context->CopyResource(staging.Get(), target.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                for (UINT y = 0; y < 8; ++y) {
                    auto row = reinterpret_cast<const float*>(
                        static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                    values.insert(values.end(), row, row + 32);
                }
                context->Unmap(staging.Get(), 0);
            }
            return values;
        };
        for (float eye : { 0.0f, 1.0f })
            for (float alpha : { 0.0f, 0.35f, 1.0f })
                for (float blend : { 0.0f, 0.5f, 1.0f })
                    for (float brightness : { -1.0f, 2.0f }) {
                        parameters = { alpha, eye, 0, 0 }; material = { blend, brightness, 0, 0 };
                        context->UpdateSubresource(vertexConstants.Get(), 0, nullptr, parameters.data(), 0, 0);
                        context->UpdateSubresource(pixelConstants.Get(), 0, nullptr, material.data(), 0, 0);
                        const auto before = draw(first.Get()), after = draw(second.Get());
                        for (size_t i = 0; i < before.size(); ++i)
                            if (!std::isfinite(before[i]) || !std::isfinite(after[i]) ||
                                before[i] == -50 || std::abs(before[i] - after[i]) > 0.00001f)
                                throw std::runtime_error("Sky replacement changed color/alpha/motion output");
                    }
        context->ClearState();
        return true;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return false;
    }
}
