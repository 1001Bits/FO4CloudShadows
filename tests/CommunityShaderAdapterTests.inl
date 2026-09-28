// SPDX-License-Identifier: GPL-3.0-only
// Independent fixtures exercise CS's named constant/resource ABI without
// embedding its shaders. Optional corpus mode validates real compiled variants.
namespace
{
    ComPtr<ID3DBlob> CommunityFixture(const char* extra = "")
    {
        std::string source = extra;
        source += R"hlsl(
cbuffer PerFrame_CB12 : register(b12) {
 float4 pad[20]; float4 FarReproj_row0; float4 FarReproj_row1; float4 FarReproj_row2; float4 FarReproj_row3;
 float4 NearReproj_row0; float4 NearReproj_row1; float4 NearReproj_row2; float4 NearReproj_row3;
};
cbuffer PerCall_CB2 : register(b2) {
 float4 ScreenSize; float4 SunDirection; float4 SunColor_HDR; float4 Ambient;
#ifdef LOCAL_LIGHT
 float4 LightAttenuation;
#endif
};
Texture2D<float4> g_tGbufferAlbedo : register(t0);
Texture2D<float4> g_tGbufferNormal : register(t1);
Texture2D<float4> g_tGbufferMaterial : register(t2);
Texture2D<float4> g_tMainDepth : register(t3);
SamplerState g_sMainDepth : register(s3);
Texture2D<float> ContactShadow : register(t24);
#ifdef CONFLICT
Texture2D<float> Conflict : register(t47);
#endif
struct O { float4 diffuse : SV_Target0; float4 specular : SV_Target1; };
O main(float4 pos : SV_Position) {
 float4 g = g_tGbufferAlbedo.Load(int3(0,0,0)) + g_tGbufferNormal.Load(int3(0,0,0)) +
            g_tGbufferMaterial.Load(int3(0,0,0)) + g_tMainDepth.Sample(g_sMainDepth, pos.xy * ScreenSize.xy);
 float frameTerm = dot(FarReproj_row0 + FarReproj_row3 + NearReproj_row0 + NearReproj_row3, g);
 float contact = ContactShadow.Load(int3(0,0,0));
 float3 direct = SunColor_HDR.xyz * g.xyz * contact;
 if (SunDirection.x > 0) direct += abs(-SunColor_HDR.yzx) * 0.37 * frameTerm;
 else direct += SunColor_HDR.zxy * 0.23;
 O o;
 o.diffuse = float4((direct + Ambient.xyz) / 3, SunColor_HDR.w);
 o.specular = float4(SunColor_HDR.xyz * (g.w * contact + 0.43 * frameTerm) + Ambient.zyx, 1);
#ifdef CONFLICT
 o.specular.xyz *= Conflict.Load(int3(0,0,0));
#endif
 return o;
})hlsl";
        ComPtr<ID3DBlob> blob, errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
            "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        return blob;
    }

    void RunCommunityCompositionTests(TestState& tests, ID3D11Device* device,
        ID3D11DeviceContext* context, const SIE::DXBCPatch& payload, uint32_t factorRegister)
    {
        auto source = CommunityFixture();
        tests.Require(source != nullptr, "community composition fixture compiles");
        if (!source) return;
        tests.Require(SIE::DXBCPatcher::IsCommunitySunCandidate(source->GetBufferPointer(), source->GetBufferSize()),
            "community sunlight ABI is recognized");
        auto unshadowed = CommunityFixture("#define SunDirection LightVector\n#define SunColor_HDR LightColor_HDR\n");
        tests.Require(unshadowed && SIE::DXBCPatcher::IsCommunitySunCandidate(unshadowed->GetBufferPointer(), unshadowed->GetBufferSize()),
            "community unshadowed directional light is recognized");
        auto local = CommunityFixture("#define SunDirection LightVector\n#define SunColor_HDR LightColor_HDR\n#define LOCAL_LIGHT\n");
        tests.Require(local && !SIE::DXBCPatcher::IsCommunitySunCandidate(local->GetBufferPointer(), local->GetBufferSize()),
            "community point lights remain unmodified");
        auto unknown = CommunityFixture("#define SunColor_HDR UnknownColour\n");
        tests.Require(unknown && !SIE::DXBCPatcher::IsCommunitySunCandidate(unknown->GetBufferPointer(), unknown->GetBufferSize()),
            "unknown light layouts are rejected");
        auto conflict = CommunityFixture("#define CONFLICT\n");
        tests.Require(conflict && !SIE::DXBCPatcher::IsCommunitySunCandidate(conflict->GetBufferPointer(), conflict->GetBufferSize()),
            "community adapter rejects occupied t47");
        ComPtr<ID3DBlob> stripped;
        D3DStripShader(source->GetBufferPointer(), source->GetBufferSize(), D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped);
        tests.Require(stripped && !SIE::DXBCPatcher::IsCommunitySunCandidate(stripped->GetBufferPointer(), stripped->GetBufferSize()),
            "community adapter requires the audited named ABI");
        ComPtr<ID3DBlob> patched;
        patched.Attach(SIE::DXBCPatcher::PatchCommunitySunShader(source.Get(), payload, factorRegister));
        tests.Require(patched != nullptr, "community sunlight fixture patches");
        if (!patched) return;
        tests.Require(!SIE::DXBCPatcher::IsCommunitySunCandidate(patched->GetBufferPointer(), patched->GetBufferSize()),
            "community adapter rejects a second patch");
        ComPtr<ID3D11PixelShader> originalPS, patchedPS;
        tests.Require(SUCCEEDED(device->CreatePixelShader(source->GetBufferPointer(), source->GetBufferSize(), nullptr, &originalPS)) &&
            SUCCEEDED(device->CreatePixelShader(patched->GetBufferPointer(), patched->GetBufferSize(), nullptr, &patchedPS)),
            "both community composition shaders create on WARP");
        if (!originalPS || !patchedPS) return;
        {
            SIE::DFLightPatcher runtime;
            const auto hash = SIE::DFLightPatcher::HashDXBC(source->GetBufferPointer(), source->GetBufferSize());
            runtime.RecordPSHash(originalPS.Get(), hash);
            runtime.StoreBytecode(hash, source->GetBufferPointer(), source->GetBufferSize());
            runtime.Initialize(device);
            runtime.WaitForClassification();
            ComPtr<ID3D11PixelShader> replacement;
            replacement.Attach(runtime.AcquireDescriptor(0x01220202, originalPS.Get()));
            tests.Require(replacement != nullptr, "live patcher selects CS directional replacement");
            replacement.Reset();
            replacement.Attach(runtime.AcquireDescriptor(4, originalPS.Get()));
            tests.Require(!replacement, "directional adapter cannot replace a local-light descriptor");
            runtime.Release();
            runtime.Initialize(device);
            runtime.WaitForClassification();
            replacement.Attach(runtime.AcquireDescriptor(0x01220202, originalPS.Get()));
            tests.Require(replacement != nullptr, "CS adapter survives renderer recreation");
        }
        const char* vs = "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
        ComPtr<ID3DBlob> vsCode;
        ComPtr<ID3D11VertexShader> vertex;
        if (FAILED(D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, nullptr)) ||
            FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vertex))) {
            tests.Require(false, "community vertex fixture creates"); return;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        std::array<ComPtr<ID3D11Texture2D>, 2> outputs;
        std::array<ComPtr<ID3D11RenderTargetView>, 2> rtvs;
        for (size_t i = 0; i < 2; ++i) {
            if (FAILED(device->CreateTexture2D(&td, nullptr, &outputs[i])) ||
                FAILED(device->CreateRenderTargetView(outputs[i].Get(), nullptr, &rtvs[i]))) {
                tests.Require(false, "community targets create"); return;
            }
        }
        ComPtr<ID3D11Texture2D> tex, mask, readback;
        ComPtr<ID3D11ShaderResourceView> texView, maskView;
        const std::array<float,4> texel{.2f,.3f,.4f,.8f};
        D3D11_SUBRESOURCE_DATA input{texel.data(),sizeof(texel),0};
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, &input, &tex)) ||
            FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &texView))) {
            tests.Require(false, "community inputs create"); return;
        }
        td.Format = DXGI_FORMAT_R32_FLOAT;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &mask)) ||
            FAILED(device->CreateShaderResourceView(mask.Get(), nullptr, &maskView))) {
            tests.Require(false, "community mask creates"); return;
        }
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &readback))) {
            tests.Require(false, "community readback creates"); return;
        }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 28 * 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> perFrame, perCall;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &perFrame))) {
            tests.Require(false, "community frame buffer creates"); return;
        }
        bd.ByteWidth = 4 * 16;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &perCall))) {
            tests.Require(false, "community light buffer creates"); return;
        }
        std::array<float,28*4> frame{}; frame.fill(.1f);
        context->UpdateSubresource(perFrame.Get(),0,nullptr,frame.data(),0,0);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        if (FAILED(device->CreateSamplerState(&sd,&sampler)) || FAILED(device->CreateRasterizerState(&rd,&raster))) {
            tests.Require(false, "community sampler/rasterizer creates"); return;
        }
        context->ClearState();
        context->VSSetShader(vertex.Get(),nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetState(raster.Get());
        const D3D11_VIEWPORT viewport{0,0,1,1,0,1};context->RSSetViewports(1,&viewport);
        ID3D11RenderTargetView* targets[]{rtvs[0].Get(),rtvs[1].Get()};context->OMSetRenderTargets(2,targets,nullptr);
        ID3D11ShaderResourceView* textures[]{texView.Get(),texView.Get(),texView.Get(),texView.Get()};
        context->PSSetShaderResources(0,4,textures);
        context->PSSetShaderResources(24,1,texView.GetAddressOf());
        context->PSSetShaderResources(47,1,maskView.GetAddressOf());
        context->PSSetSamplers(3,1,sampler.GetAddressOf());
        context->PSSetConstantBuffers(2,1,perCall.GetAddressOf());
        context->PSSetConstantBuffers(12,1,perFrame.GetAddressOf());
        const auto render = [&](ID3D11PixelShader* shader, float lightScale, float branch) {
            std::array<float,16> light{1,1,1,1, branch,0,0,0,
                .6f*lightScale,.8f*lightScale,1.1f*lightScale,.73f, .13f,.17f,.21f,0};
            context->UpdateSubresource(perCall.Get(),0,nullptr,light.data(),0,0);
            context->PSSetShader(shader,nullptr,0);context->Draw(3,0);
            std::array<float,8> result{};
            for (size_t i=0;i<2;++i) {
                context->CopyResource(readback.Get(),outputs[i].Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped))) {
                    tests.Require(false,"community pixels read back");return result;
                }
                memcpy(result.data()+4*i,mapped.pData,16);context->Unmap(readback.Get(),0);
            }
            return result;
        };
        for (float branch : {-1.f,1.f}) {
            const auto ambient=render(originalPS.Get(),0,branch);
            const auto sun=render(originalPS.Get(),1,branch);
            tests.Require(ambient[0] > .01f && ambient[4] > .01f && sun[0] > ambient[0]+.01f,
                "composition fixture exercises nonzero direct and ambient light");
            for (float factor : {0.f,.25f,.5f,1.f}) {
                context->UpdateSubresource(mask.Get(),0,nullptr,&factor,4,0);
                const auto expected=render(originalPS.Get(),factor,branch);
                const auto actual=render(patchedPS.Get(),1,branch);
                for (size_t i=0;i<8;++i)
                    tests.Require(std::isfinite(actual[i]) && std::abs(actual[i]-expected[i])<.00001f,
                        "CS adapter preserves ambient, feature factors, swizzles, modifiers and alpha");
            }
        }
        context->ClearState();
    }

    void RunCommunityCorpusTests(TestState& tests, ID3D11Device* device,
        const std::filesystem::path& path, const SIE::DXBCPatch& payload, uint32_t factorRegister)
    {
        unsigned total=0, matched=0;
        for (const auto& file : std::filesystem::directory_iterator(path)) {
            if (file.path().extension()!=L".dxbc") continue;
            ++total;const auto bytes=ReadFile(file.path());
            if (!SIE::DXBCPatcher::IsCommunitySunCandidate(bytes.data(),bytes.size())) continue;
            ++matched;auto source=MakeBlob(bytes);
            ComPtr<ID3DBlob> result;
            result.Attach(SIE::DXBCPatcher::PatchCommunitySunShader(source.Get(),payload,factorRegister));
            ComPtr<ID3D11PixelShader> shader;
            const bool valid=result && SUCCEEDED(device->CreatePixelShader(result->GetBufferPointer(),result->GetBufferSize(),nullptr,&shader));
            if (!valid) std::cerr << file.path().string() << '\n';
            tests.Require(valid,"every recognized CS corpus shader patches and creates on WARP");
        }
        tests.Require(total>0 && matched>0,"community corpus contains validated directional lighting");
        std::cout << "Community corpus: total=" << total << " matched=" << matched << '\n';
    }
}
