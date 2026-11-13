// SPDX-License-Identifier: GPL-3.0-only
// Fixtures compile to the stock water layout without embedding any game shader.
// Optional corpus mode validates the real shaders.
namespace
{
    ComPtr<ID3DBlob> WaterFixture(const char* extra = "")
    {
        std::string source = extra;
        source += R"hlsl(
#ifndef ROW
#define ROW 2
#endif
cbuffer PerGeometry : register(b0) { float4 geometry[4]; };
cbuffer PerMaterial : register(b1) { float4 material[4]; };
#ifdef CONFLICT
Texture2D<float> Conflict : register(t47);
#endif
float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target0 {
#ifdef USES_POSITION
 uv.xyz += pos.xyz * 0.0001;
#endif
 float3 color = material[2].xyz * uv.z + material[2].zyx * uv.y * 0.5;
#ifdef CONFLICT
 color *= Conflict.Load(int3(0, 0, 0));
#endif
 float3 light = geometry[ROW].w * geometry[ROW + 1].xyz;
 float3 surface = light * pow(saturate(dot(uv.xyz, float3(-0.099, -0.099, 0.99))), material[0].w);
 float3 specular = light * pow(saturate(dot(uv.xyz, material[0].xyz)), material[3].y);
 float3 lighting = specular * material[1].w + surface;
#ifdef NO_LIGHTING
 lighting = 0;
#endif
 color = lighting + color;
 return float4(lerp(color, material[3].xyz, material[3].w * uv.w), 0);
})hlsl";
        ComPtr<ID3DBlob> blob, errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
            "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        return blob;
    }

    bool IsWaterCandidate(const ComPtr<ID3DBlob>& blob)
    {
        return blob && SIE::DXBCPatcher::IsWaterSunCandidate(blob->GetBufferPointer(), blob->GetBufferSize());
    }

    void RunWaterCompositionTests(TestState& tests, ID3D11Device* device,
        ID3D11DeviceContext* context, const SIE::DXBCPatch& payload, uint32_t factorRegister)
    {
        auto positioned = WaterFixture("#define USES_POSITION\n");
        auto bare = WaterFixture();
        auto og = WaterFixture("#define USES_POSITION\n#define ROW 1\n");
        tests.Require(IsWaterCandidate(positioned), "water light colour is recognized");
        tests.Require(IsWaterCandidate(bare), "water without a position read is recognized");
        tests.Require(IsWaterCandidate(og), "water light colour one row lower is recognized");
        tests.Require(!IsWaterCandidate(WaterFixture("#define ROW 0\n")), "another cb0 row is not the light colour");
        tests.Require(!IsWaterCandidate(WaterFixture("#define CONFLICT\n")), "water adapter rejects occupied t47");
        tests.Require(!IsWaterCandidate(WaterFixture("#define NO_LIGHTING\n")), "water without its lighting term is not the audited layout");
        tests.Require(!IsWaterCandidate(CommunityFixture()), "deferred lighting is not water");

        const char* vs =
            "struct O { float4 pos : SV_Position; float4 uv : TEXCOORD0; };"
            "O main(uint i : SV_VertexID) { O o; o.pos = float4(i == 2 ? 3 : -1, i == 1 ? 3 : -1, 0, 1); o.uv = float4(.25, .5, .75, 1); return o; }";
        ComPtr<ID3DBlob> vsCode, vsErrors;
        ComPtr<ID3D11VertexShader> vertex;
        if (FAILED(D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, &vsErrors)) ||
            FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vertex))) {
            tests.Require(false, "water vertex fixture creates");
            return;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> output, mask, readback;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> maskView;
        bool created = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &output)) &&
            SUCCEEDED(device->CreateRenderTargetView(output.Get(), nullptr, &rtv));
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        created = created && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &readback));
        td.Format = DXGI_FORMAT_R32_FLOAT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        created = created && SUCCEEDED(device->CreateTexture2D(&td, nullptr, &mask)) &&
            SUCCEEDED(device->CreateShaderResourceView(mask.Get(), nullptr, &maskView));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 4 * 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> geometry, material;
        created = created && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &geometry)) &&
            SUCCEEDED(device->CreateBuffer(&bd, nullptr, &material));
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        created = created && SUCCEEDED(device->CreateRasterizerState(&rd, &raster));
        if (!created) {
            tests.Require(false, "water composition resources create");
            return;
        }
        const auto render = [&](ID3D11PixelShader* shader, int row, float sunScale, float maskValue, float colorScale = 1) {
            const std::array<float, 16> materialData{ .3f,.7f,0,1, 0,0,0,.9f,
                .11f * colorScale,.13f * colorScale,.17f * colorScale,0, .41f,.43f,.47f,.25f };
            context->UpdateSubresource(material.Get(), 0, nullptr, materialData.data(), 0, 0);
            std::array<float, 16> rows{};
            rows[row * 4 + 3] = 1.5f * sunScale;     // sun strength
            rows[(row + 1) * 4 + 0] = .6f;           // sun colour
            rows[(row + 1) * 4 + 1] = .8f;
            rows[(row + 1) * 4 + 2] = 1.1f;
            context->ClearState();
            context->UpdateSubresource(geometry.Get(), 0, nullptr, rows.data(), 0, 0);
            context->UpdateSubresource(mask.Get(), 0, nullptr, &maskValue, 4, 0);
            context->VSSetShader(vertex.Get(), nullptr, 0);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->RSSetState(raster.Get());
            const D3D11_VIEWPORT viewport{ 0, 0, 1, 1, 0, 1 };
            context->RSSetViewports(1, &viewport);
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            context->PSSetConstantBuffers(0, 1, geometry.GetAddressOf());
            context->PSSetConstantBuffers(1, 1, material.GetAddressOf());
            context->PSSetShaderResources(47, 1, maskView.GetAddressOf());
            context->PSSetShader(shader, nullptr, 0);
            context->Draw(3, 0);
            std::array<float, 4> result{};
            context->CopyResource(readback.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                tests.Require(false, "water pixels read back");
                return result;
            }
            memcpy(result.data(), mapped.pData, sizeof(result));
            context->Unmap(readback.Get(), 0);
            return result;
        };
        const auto verify = [&](const ComPtr<ID3DBlob>& source, int row, const char* created_, const char* exact) {
            ComPtr<ID3DBlob> patched;
            patched.Attach(SIE::DXBCPatcher::PatchWaterSunShader(source.Get(), payload, factorRegister));
            ComPtr<ID3D11PixelShader> originalPS, patchedPS;
            const bool valid = patched &&
                SUCCEEDED(device->CreatePixelShader(source->GetBufferPointer(), source->GetBufferSize(), nullptr, &originalPS)) &&
                SUCCEEDED(device->CreatePixelShader(patched->GetBufferPointer(), patched->GetBufferSize(), nullptr, &patchedPS));
            tests.Require(valid, created_);
            if (!valid)
                return;
            tests.Require(!IsWaterCandidate(patched) || !SIE::DXBCPatcher::PatchWaterSunShader(patched.Get(), payload, factorRegister),
                "water adapter rejects a second patch");
            const auto full = render(originalPS.Get(), row, 1, 1);
            const auto dark = render(originalPS.Get(), row, 0, 1);
            tests.Require(full[0] > dark[0] + .01f && dark[0] > .01f,
                "water fixture exercises nonzero sun and refraction");
            for (float factor : { 0.f, .25f, .5f, 1.f }) {
                const float floor = SIE::DXBCPatcher::kWaterShadowFloor;
                const auto expected = render(originalPS.Get(), row, factor, 1, floor + (1 - floor) * factor);
                const auto actual = render(patchedPS.Get(), row, 1, factor);
                for (size_t i = 0; i < 4; ++i)
                    tests.Require(std::isfinite(actual[i]) && std::abs(actual[i] - expected[i]) < .00001f, exact);
            }
            context->ClearState();
        };
        if (positioned)
            verify(positioned, 2, "water fixture patches and creates on WARP",
                "water adapter scales the sun colour and darkens the water, not its fog");
        if (bare)
            verify(bare, 2, "water fixture without a position read patches and creates on WARP",
                "water adapter adds the position read it needs");
        if (og)
            verify(og, 1, "water fixture one row lower patches and creates on WARP",
                "water adapter scales the sun colour one row lower");
    }

    // A folder of whole game shaders: every candidate must patch and create. With
    // a `patched` subfolder present, the results are written there for review.
    void RunWaterCorpusTests(TestState& tests, ID3D11Device* device,
        const std::filesystem::path& path, const SIE::DXBCPatch& payload, uint32_t factorRegister)
    {
        unsigned total = 0, matched = 0;
        for (const auto& file : std::filesystem::directory_iterator(path)) {
            if (file.path().extension() != L".dxbc")
                continue;
            ++total;
            const auto bytes = ReadFile(file.path());
            if (!SIE::DXBCPatcher::IsWaterSunCandidate(bytes.data(), bytes.size()))
                continue;
            ++matched;
            auto source = MakeBlob(bytes);
            SIE::DXBCPatcher::SunShadowPatchInfo info;
            ComPtr<ID3DBlob> result;
            result.Attach(SIE::DXBCPatcher::PatchWaterSunShader(source.Get(), payload, factorRegister, &info));
            ComPtr<ID3D11PixelShader> shader;
            const bool valid = result && info.signatureVariant == 12 && SUCCEEDED(device->CreatePixelShader(
                result->GetBufferPointer(), result->GetBufferSize(), nullptr, &shader));
            if (!valid)
                std::cerr << file.path().string() << '\n';
            tests.Require(valid, "every recognized water corpus shader patches and creates on WARP");
            const auto patched = path / L"patched";
            if (valid && std::filesystem::is_directory(patched)) {
                std::ofstream out(patched / file.path().filename(), std::ios::binary);
                out.write(static_cast<const char*>(result->GetBufferPointer()),
                    static_cast<std::streamsize>(result->GetBufferSize()));
            }
        }
        tests.Require(total > 0 && matched > 0, "water corpus contains recognized shaders");
        std::cout << "Water corpus: total=" << total << " matched=" << matched << '\n';
    }
}
