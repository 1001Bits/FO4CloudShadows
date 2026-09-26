// Private-corpus verification; no game/mod shader bytes are distributed.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
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

// The live path authenticates stock Sky cloud shaders by exact FNV-1a hash,
// including three FO4VR Deferred replacements that differ from stock only in
// equivalent CB12 motion-vector indexing. With an extracted DXBC corpus (file
// or directory arguments) this proves those replacements render identically.
// Without one it proves the comparator itself on WARP.
namespace
{
    std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            return {};
        const std::streamoff length = input.tellg();
        if (length <= 0 || length > (1 << 24))
            return {};
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
        return input ? bytes : std::vector<uint8_t>{};
    }

    uint64_t HashDXBC(const std::vector<uint8_t>& bytes)
    {
        uint64_t hash = 0xCBF29CE484222325ULL;
        for (const uint8_t byte : bytes) {
            hash ^= byte;
            hash *= 0x100000001B3ULL;
        }
        return hash;
    }

    void AddCorpusPath(const std::filesystem::path& path,
        std::vector<std::filesystem::path>& files)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && !error) {
            files.push_back(path);
            return;
        }
        if (!std::filesystem::is_directory(path, error) || error)
            return;
        for (std::filesystem::recursive_directory_iterator iterator(path, error), end;
             iterator != end && !error; iterator.increment(error)) {
            if (iterator->is_regular_file(error) && !error &&
                iterator->path().extension() == ".dxbc")
                files.push_back(iterator->path());
        }
    }

    std::vector<uint8_t> CompileSyntheticCloudShader(bool altered)
    {
        const char* source = R"hlsl(
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
cbuffer Material : register(b2) { float4 M; };
Texture2D<float4> Cloud0 : register(t0);
SamplerState Sampler0 : register(s0);
struct O { float4 color : SV_Target0; float4 motion : SV_Target1; };
O main(V i) {
    O o;
    o.color = float4(i.color.rgb * M.y, i.color.a * Cloud0.Sample(Sampler0, i.uv).a);
#if ALTERED
    o.color.r += 0.25;
#endif
    o.motion = float4(i.current.xy - i.previous.xy, 0, 1);
    return o;
}
)hlsl";
        const D3D_SHADER_MACRO macros[]{ { "ALTERED", altered ? "1" : "0" }, {} };
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(source, std::char_traits<char>::length(source), nullptr,
                macros, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                &code, &errors)))
            throw std::runtime_error("synthetic cloud shader failed to compile");
        const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
        return { bytes, bytes + code->GetBufferSize() };
    }
}

int main(int argc, char** argv)
{
    try {
        ComPtr<ID3D11Device> device;
        constexpr D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                &level, 1, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            throw std::runtime_error("WARP D3D11 device is unavailable");

        const auto stock = CompileSyntheticCloudShader(false);
        const auto altered = CompileSyntheticCloudShader(true);
        if (!VerifySkyOutputEquivalence(device.Get(), stock, stock))
            throw std::runtime_error("comparator rejects identical cloud shaders");
        if (VerifySkyOutputEquivalence(device.Get(), stock, altered))
            throw std::runtime_error("comparator accepts a shader with different colour output");

        std::vector<std::filesystem::path> corpus;
        for (int index = 1; index < argc; ++index)
            AddCorpusPath(argv[index], corpus);
        std::unordered_map<uint64_t, std::vector<uint8_t>> shaders;
        for (const auto& path : corpus) {
            auto bytes = ReadBytes(path);
            if (!bytes.empty())
                shaders.emplace(HashDXBC(bytes), std::move(bytes));
        }
        // {stock VR cloud PS, accepted FO4VR Deferred replacement}.
        const std::array<std::array<uint64_t, 2>, 3> compatibilityPairs{{
            { 0x0219856733164CB0ULL, 0x6D2B9D973AF2EC0DULL },
            { 0x7C98FE0C571E1916ULL, 0x9D6DBBA74E764A0CULL },
            { 0x9672C2620203FD20ULL, 0x585F89F70474752CULL }
        }};
        unsigned verified = 0;
        for (const auto& pair : compatibilityPairs) {
            if (!shaders.contains(pair[1]))
                continue;
            if (!shaders.contains(pair[0]) ||
                !VerifySkyOutputEquivalence(device.Get(), shaders.at(pair[0]), shaders.at(pair[1])))
                throw std::runtime_error("VR replacement changes stock colour, alpha or motion output");
            ++verified;
        }
        std::cout << "Sky cloud compatibility tests passed (comparator self-test; "
            << verified << " corpus replacement pair(s) verified)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
