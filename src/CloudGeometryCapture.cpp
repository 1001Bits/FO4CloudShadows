// SPDX-License-Identifier: GPL-3.0-only
#include "CpuStageProfiler.h"
#include "CloudGeometryCapture.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <spdlog/spdlog.h>

namespace FO4CS::CloudGeometryCapture
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        namespace Motion = CloudMotionResolver;

        // The reviewed stock VS uses World for the camera-relative POSITION0
        // output, TexCoordOff for UV animation, and BlendColor0.w*(color.w+1e-6)
        // for opacity. OG SetupGeometry (RVA 0x287B130) subtracts the render
        // camera from World translation before upload. It is therefore the
        // dome's camera-relative offset, which must stay in its sky direction.
        // Preserve mesh UV/alpha before animation: baking a zero initial blend
        // permanently loses a layer that becomes visible in a later frame.
        constexpr char kShader[] = R"(
cbuffer Face : register(b0) { float4 Right; float4 Up; float4 Forward;
#if SUN_MASK
float4 Center;
#endif
};
cbuffer Sky : register(b2) {
#if VR
    float4 prefix[8];
    row_major float3x4 World;
    float4 BlendColor[3];
    float2 TexCoordOff;
#else
    float4 prefix[4];
    row_major float3x4 World;
    float4 BlendColor[3];
    float2 TexCoordOff;
#endif
};
struct Input { float3 p : POSITION0; float2 uv : TEXCOORD0; float4 c : COLOR0; };
// SV_ClipDistance is rejected by Proton/Wine's vkd3d d3dcompiler (E5013), so
// the horizon clip is carried as an interpolator and applied with clip() in
// the pixel shader. Per-pixel results match the clip-distance path.
struct Output { float4 p : SV_Position; float2 uv : TEXCOORD0; float alpha : TEXCOORD1;
    float horizon : TEXCOORD2; };
Output VS(Input i, uint instanceIndex : SV_InstanceID) {
    Output o;
    float3 direction = mul(World, float4(i.p, 1));
#if SUN_MASK
    float lengthSquared = dot(direction, direction);
    float3 ray = direction * rsqrt(max(lengthSquared, 1.0e-12));
    float height = Up.w;
    float radius = Forward.w;
    float deficit = height * (2.0 * radius + height);
    float b = radius * ray.z;
    float root = sqrt(b * b + deficit);
    float distanceToShell = b >= 0 ? deficit / max(b + root, 1.0e-7) : root - b;
    // Preserve projective UV interpolation as the native dome is mapped onto
    // the cloud shell. This becomes the exact ray/plane homography locally.
    float w = sqrt(max(lengthSquared, 1.0e-12)) / max(distanceToShell, 1.0e-7);
    float3 offset = direction - Center.xyz * w;
    o.p = float4(dot(Right.xyz, offset) / Right.w,
        dot(Up.xyz, offset) / Right.w, 0.5 * w, w);
    o.horizon = direction.z;
#else
    o.horizon = 1.0;
    float forward = dot(Forward.xyz, direction);
    o.p = float4(dot(Right.xyz, direction), dot(Up.xyz, direction), forward, forward);
#endif
    o.uv = i.uv;
    o.alpha = i.c.w + 0.000001;
#if OPACITY
    o.uv += TexCoordOff;
    o.alpha *= BlendColor[0].w;
#if VR
    // Native VR submits the same dome for both eyes. The directional cube
    // has one camera, so duplicate stereo instances must not double opacity.
    if (instanceIndex != 0) o.p = float4(0, 0, -1, 1);
#endif
#endif
    return o;
}
float4 PS(Output i) : SV_Target0 { clip(i.horizon); return float4(i.uv, i.alpha, 1); }
)";

        constexpr char kOpacityPS[] = R"(
cbuffer CloudTechniqueParameters : register(b2) { float4 Parameter; };
Texture2D<float4> Cloud0 : register(t0);
Texture2D<float4> Cloud1 : register(t1);
SamplerState Sampler0 : register(s0);
SamplerState Sampler1 : register(s1);
struct Input { float4 p : SV_Position; float2 uv : TEXCOORD0; float alpha : TEXCOORD1;
    float horizon : TEXCOORD2; };
// Portable finite test. Proton/Wine's vkd3d-based d3dcompiler lacks the
// isfinite intrinsic, and fxc may fold NaN comparisons; the exponent bit test
// is exact IEEE-754 on both compilers. A macro, not overloads: vkd3d cannot
// prioritize between compatible overloads (E5017).
#define IS_FINITE(x) ((asuint(x) & 0x7F800000u) != 0x7F800000u)
float4 PS(Input i) : SV_Target0 {
    clip(i.horizon);
    float alpha = Cloud0.Sample(Sampler0, i.uv).a;
#if TECHNIQUE == 6
    alpha = lerp(alpha, Cloud1.Sample(Sampler1, i.uv).a, Parameter.x);
#elif TECHNIQUE == 7
    alpha *= saturate(Parameter.x - 0.4) * (5.0 / 3.0);
#endif
    alpha = saturate(alpha * i.alpha);
    alpha = IS_FINITE(alpha) ? alpha : 0;
    return alpha.xxxx;
}
)";

        struct FaceBasis { std::array<float, 4> right, up, forward; };
        // D3D TextureCube coordinates; rasterizer Y points up, texture V down.
        constexpr std::array<FaceBasis, 6> kFaces{{
            {{0,0,-1,0}, {0,1,0,0}, {1,0,0,0}},
            {{0,0,1,0}, {0,1,0,0}, {-1,0,0,0}},
            {{1,0,0,0}, {0,0,-1,0}, {0,1,0,0}},
            {{1,0,0,0}, {0,0,1,0}, {0,-1,0,0}},
            {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}},
            {{-1,0,0,0}, {0,1,0,0}, {0,0,-1,0}}
        }};

        struct Resources
        {
            ComPtr<ID3D11Device> device;
            std::array<ComPtr<ID3D11VertexShader>, 2> vs;
            std::array<ComPtr<ID3D11VertexShader>, 2> opacityVS;
            std::array<ComPtr<ID3D11VertexShader>, 2> sunOpacityVS;
            bool sunVsFailed{};
            ComPtr<ID3D11Buffer> sunProjectionCB;
            SunMaskProjection sunProjection{};
            bool sunProjectionValid{};
            std::array<ComPtr<ID3D11PixelShader>, 3> opacityPS;
            ComPtr<ID3D11BlendState> opacityBlend;
            std::array<ComPtr<ID3D11RasterizerState>, 6> opacityRasterizers;
            ComPtr<ID3D11PixelShader> ps;
            std::array<ComPtr<ID3D11Buffer>, 6> faces;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth;
            ComPtr<ID3D11RasterizerState> rasterizer;
        };
        Resources s_resources;

        bool EnsureResourcesUncached(ID3D11Device* device);

        // A persistent compile failure must not re-run D3DCompile for every
        // authenticated cloud draw; retry at most every few seconds.
        bool EnsureResources(ID3D11Device* device)
        {
            static ID3D11Device* s_failedDevice = nullptr;
            static ULONGLONG s_failedTick = 0;
            if (s_resources.device.Get() == device && s_resources.ps)
                return true;
            if (s_failedDevice == device && GetTickCount64() - s_failedTick < 5000)
                return false;
            if (EnsureResourcesUncached(device)) {
                s_failedDevice = nullptr;
                return true;
            }
            s_failedDevice = device;
            s_failedTick = GetTickCount64();
            return false;
        }

        bool EnsureResourcesUncached(ID3D11Device* device)
        {
            if (s_resources.device.Get() == device && s_resources.ps)
                return true;
            Resources next;
            next.device = device;
            for (std::size_t layout = 0; layout < next.vs.size(); ++layout) {
                const D3D_SHADER_MACRO macros[]{ {"VR", layout ? "1" : "0"}, {} };
                ComPtr<ID3DBlob> code, errors;
                if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1,
                        "CloudGeometryCapture", macros, nullptr, "VS", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                    FAILED(device->CreateVertexShader(code->GetBufferPointer(),
                        code->GetBufferSize(), nullptr, &next.vs[layout]))) {
                    if (errors) SPDLOG_ERROR("Cloud mapping VS: {}",
                        static_cast<const char*>(errors->GetBufferPointer()));
                    return false;
                }
                const D3D_SHADER_MACRO opacityMacros[]{
                    {"VR", layout ? "1" : "0"}, {"OPACITY", "1"}, {} };
                if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1,
                        "CloudOpacityCapture", opacityMacros, nullptr, "VS", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                    FAILED(device->CreateVertexShader(code->GetBufferPointer(),
                        code->GetBufferSize(), nullptr, &next.opacityVS[layout]))) {
                    if (errors) SPDLOG_ERROR("Cloud opacity VS: {}",
                        static_cast<const char*>(errors->GetBufferPointer()));
                    return false;
                }
                const D3D_SHADER_MACRO sunMacros[]{
                    {"VR", layout ? "1" : "0"}, {"OPACITY", "1"},
                    {"SUN_MASK", "1"}, {} };
                // The sun-oriented capture is optional: its failure must never
                // disable the cubemap producer (observed on Proton 1.0.1/1.0.2).
                if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1,
                        "SunOpacityCapture", sunMacros, nullptr, "VS", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                    FAILED(device->CreateVertexShader(code->GetBufferPointer(),
                        code->GetBufferSize(), nullptr, &next.sunOpacityVS[layout]))) {
                    static bool loggedSunFailure = false;
                    if (!loggedSunFailure) {
                        loggedSunFailure = true;
                        SPDLOG_WARN("Sun opacity VS unavailable; the Sun 2D method falls back to the cubemap: {}",
                            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile/create failed");
                    }
                    next.sunOpacityVS[layout].Reset();
                    next.sunVsFailed = true;
                }
            }
            constexpr const char* techniques[]{ "5", "6", "7" };
            for (std::size_t index = 0; index < next.opacityPS.size(); ++index) {
                ComPtr<ID3DBlob> code, errors;
                const D3D_SHADER_MACRO macros[]{ {"TECHNIQUE", techniques[index]}, {} };
                if (FAILED(D3DCompile(kOpacityPS, sizeof(kOpacityPS) - 1,
                        "CloudOpacityCapture", macros, nullptr, "PS", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                    FAILED(device->CreatePixelShader(code->GetBufferPointer(),
                        code->GetBufferSize(), nullptr, &next.opacityPS[index]))) {
                    if (errors) SPDLOG_ERROR("Cloud opacity PS: {}",
                        static_cast<const char*>(errors->GetBufferPointer()));
                    return false;
                }
            }
            ComPtr<ID3DBlob> code, errors;
            const D3D_SHADER_MACRO macros[]{ {"VR", "0"}, {} };
            if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1,
                    "CloudGeometryCapture", macros, nullptr, "PS", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                FAILED(device->CreatePixelShader(code->GetBufferPointer(),
                    code->GetBufferSize(), nullptr, &next.ps)))
                return false;
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = sizeof(FaceBasis);
            buffer.Usage = D3D11_USAGE_IMMUTABLE;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            // Each basis is constant for the device lifetime. Rewriting one
            // in-use buffer six times per cloud draw creates needless upload
            // and driver resource-renaming work on the live rendering path.
            for (std::size_t face = 0; face < kFaces.size(); ++face) {
                D3D11_SUBRESOURCE_DATA initial{};
                initial.pSysMem = &kFaces[face];
                if (FAILED(device->CreateBuffer(&buffer, &initial, &next.faces[face])))
                    return false;
            }
            buffer.ByteWidth = sizeof(SunMaskProjection);
            buffer.Usage = D3D11_USAGE_DYNAMIC;
            buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&buffer, nullptr, &next.sunProjectionCB)))
                return false;
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            D3D11_DEPTH_STENCIL_DESC depth{};
            D3D11_RASTERIZER_DESC rasterizer{};
            rasterizer.FillMode = D3D11_FILL_SOLID;
            rasterizer.CullMode = D3D11_CULL_NONE;
            rasterizer.DepthClipEnable = TRUE;
            if (FAILED(device->CreateBlendState(&blend, &next.blend)) ||
                FAILED(device->CreateDepthStencilState(&depth, &next.depth)) ||
                FAILED(device->CreateRasterizerState(&rasterizer, &next.rasterizer)))
                return false;
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            if (FAILED(device->CreateBlendState(&blend, &next.opacityBlend)))
                return false;
            for (UINT index = 0; index < next.opacityRasterizers.size(); ++index) {
                rasterizer.CullMode = static_cast<D3D11_CULL_MODE>(index / 2 + 1);
                rasterizer.FrontCounterClockwise = (index & 1) != 0;
                if (FAILED(device->CreateRasterizerState(
                        &rasterizer, &next.opacityRasterizers[index])))
                    return false;
            }
            s_resources = std::move(next);
            SPDLOG_INFO("[CloudShadows] Main-Sky geometry mapping shaders ready (flat/VR)");
            return true;
        }

        class SavedState
        {
        public:
            explicit SavedState(ID3D11DeviceContext* context) : context_(context)
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureSave);
                context_->VSGetShader(&vs_, nullptr, nullptr);
                context_->PSGetShader(&ps_, nullptr, nullptr);
                (void)context_->QueryInterface(IID_PPV_ARGS(&context1_));
                if (context1_)
                    context1_->VSGetConstantBuffers1(0, 1, &cb_, &first_, &count_);
                else
                    context_->VSGetConstantBuffers(0, 1, &cb_);
                context_->OMGetRenderTargets(static_cast<UINT>(targets_.size()),
                    targets_.data(), &dsv_);
                context_->OMGetBlendState(&blend_, factor_.data(), &sampleMask_);
                context_->OMGetDepthStencilState(&depth_, &stencil_);
                context_->RSGetState(&rasterizer_);
                context_->RSGetViewports(&viewportCount_, viewports_.data());
                context_->GetPredication(&predicate_, &predicateValue_);
            }
            ~SavedState()
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureRestore);
                context_->VSSetShader(vs_.Get(), nullptr, 0);
                context_->PSSetShader(ps_.Get(), nullptr, 0);
                ID3D11Buffer* cb = cb_.Get();
                if (context1_)
                    context1_->VSSetConstantBuffers1(0, 1, &cb, &first_, &count_);
                else
                    context_->VSSetConstantBuffers(0, 1, &cb);
                context_->OMSetRenderTargets(static_cast<UINT>(targets_.size()),
                    targets_.data(), dsv_.Get());
                for (auto* target : targets_) {
                    if (target)
                        target->Release();
                }
                context_->OMSetBlendState(blend_.Get(), factor_.data(), sampleMask_);
                context_->OMSetDepthStencilState(depth_.Get(), stencil_);
                context_->RSSetState(rasterizer_.Get());
                context_->RSSetViewports(viewportCount_, viewports_.data());
                context_->SetPredication(predicate_.Get(), predicateValue_);
            }
            SavedState(const SavedState&) = delete;
            SavedState& operator=(const SavedState&) = delete;
        private:
            ID3D11DeviceContext* context_;
            ComPtr<ID3D11DeviceContext1> context1_;
            ComPtr<ID3D11VertexShader> vs_;
            ComPtr<ID3D11PixelShader> ps_;
            ComPtr<ID3D11Buffer> cb_;
            UINT first_{}, count_{};
            std::array<ID3D11RenderTargetView*, 8> targets_{};
            ComPtr<ID3D11DepthStencilView> dsv_;
            ComPtr<ID3D11BlendState> blend_;
            std::array<float, 4> factor_{};
            UINT sampleMask_{}, stencil_{};
            ComPtr<ID3D11DepthStencilState> depth_;
            ComPtr<ID3D11RasterizerState> rasterizer_;
            UINT viewportCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            std::array<D3D11_VIEWPORT,
                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
            ComPtr<ID3D11Predicate> predicate_;
            BOOL predicateValue_{};
        };

        bool SupportedPipeline(ID3D11DeviceContext* context)
        {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureValidation);
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->IAGetPrimitiveTopology(&topology);
            if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
                topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
                return false;
            ComPtr<ID3D11GeometryShader> gs;
            ComPtr<ID3D11HullShader> hs;
            ComPtr<ID3D11DomainShader> ds;
            context->GSGetShader(&gs, nullptr, nullptr);
            context->HSGetShader(&hs, nullptr, nullptr);
            context->DSGetShader(&ds, nullptr, nullptr);
            if (gs || hs || ds)
                return false;
            ComPtr<ID3D11VertexShader> vs;
            ComPtr<ID3D11PixelShader> ps;
            UINT vsClasses = 0, psClasses = 0;
            context->VSGetShader(&vs, nullptr, &vsClasses);
            context->PSGetShader(&ps, nullptr, &psClasses);
            if (!vs || !ps || vsClasses != 0 || psClasses != 0)
                return false;
            std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> outputs{};
            context->SOGetTargets(static_cast<UINT>(outputs.size()), outputs.data());
            bool supported = true;
            for (auto* output : outputs) {
                if (output) {
                    supported = false;
                    output->Release();
                }
            }
            // OMSetRenderTargets would otherwise silently disturb an owner's
            // pixel UAV bindings. Stock Sky has none.
            std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs{};
            context->OMGetRenderTargetsAndUnorderedAccessViews(
                0, nullptr, nullptr, 0, static_cast<UINT>(uavs.size()), uavs.data());
            for (auto* uav : uavs) {
                if (uav) {
                    supported = false;
                    uav->Release();
                }
            }
            return supported;
        }
    }

    bool CaptureLayer(ID3D11DeviceContext* context,
        Motion::SkyConstantLayout layout, std::uint64_t stableLayerId,
        Motion::CloudTechnique technique, std::uint32_t faceSize,
        SubmitDraw submit, void* user) noexcept
    {
        try {
            if (!context || !submit || !stableLayerId || !faceSize ||
                context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                (layout != Motion::SkyConstantLayout::kFlat &&
                 layout != Motion::SkyConstantLayout::kVr) ||
                !SupportedPipeline(context))
                return false;
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            if (!EnsureResources(device.Get()))
                return false;
            SavedState saved(context);
            context->VSSetShader(s_resources.vs[
                layout == Motion::SkyConstantLayout::kVr ? 1 : 0].Get(), nullptr, 0);
            context->PSSetShader(s_resources.ps.Get(), nullptr, 0);
            context->OMSetBlendState(s_resources.blend.Get(), nullptr, UINT(-1));
            context->OMSetDepthStencilState(s_resources.depth.Get(), 0);
            context->RSSetState(s_resources.rasterizer.Get());
            context->SetPredication(nullptr, FALSE);
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(faceSize),
                static_cast<float>(faceSize), 0, 1};
            context->RSSetViewports(1, &viewport);
            for (std::uint32_t face = 0; face < 6; ++face) {
                Motion::CaptureLayerFace layer{
                    stableLayerId, face, technique, true };
                Motion::CaptureFaceTarget destination;
                if (!Motion::AcquireLayerMappingTarget(context, layer, destination))
                    return false;
                auto* target = destination.mappingRtv.Get();
                context->OMSetRenderTargets(1, &target, nullptr);
                auto* faceCB = s_resources.faces[face].Get();
                context->VSSetConstantBuffers(0, 1, &faceCB);
                submit(user);
                if (!Motion::CompleteLayerMapping(layer, true))
                    return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    bool SunCaptureAvailable() noexcept
    {
        return !s_resources.sunVsFailed;
    }

    void ReleaseDeviceResources() noexcept { s_resources = {}; }

    static bool Accumulate(ID3D11DeviceContext* context,
        Motion::SkyConstantLayout layout, Motion::CloudTechnique technique,
        std::uint32_t faceSize,
        const std::array<ID3D11RenderTargetView*, 6>& targets,
        SubmitDraw submit, void* user, const SunMaskProjection* sun) noexcept
    {
        FO4CS::CpuProfile::Scope cpuTotal(FO4CS::CpuProfile::Stage::CaptureTotal);
        try {
            FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureValidation);
            const auto techniqueIndex = static_cast<std::uint32_t>(technique) - 5u;
            if (!context || !submit || !faceSize || techniqueIndex >= 3 ||
                context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                (layout != Motion::SkyConstantLayout::kFlat &&
                 layout != Motion::SkyConstantLayout::kVr) ||
                !SupportedPipeline(context))
                return false;
            const std::uint32_t faceCount = sun ? 1u : 6u;
            for (std::uint32_t face = 0; face < faceCount; ++face)
                if (!targets[face]) return false;
            if (sun && (sun->centerAndValid.w != 1.0f ||
                    sun->rightAndHalfWidth.w <= 0.0f))
                return false;
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            if (!EnsureResources(device.Get()))
                return false;
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureBuffer);
            if (sun && (!s_resources.sunProjectionValid ||
                    std::memcmp(sun, &s_resources.sunProjection, sizeof(*sun)) != 0)) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(s_resources.sunProjectionCB.Get(), 0,
                        D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
                    return false;
                std::memcpy(mapped.pData, sun, sizeof(*sun));
                context->Unmap(s_resources.sunProjectionCB.Get(), 0);
                s_resources.sunProjection = *sun;
                s_resources.sunProjectionValid = true;
            }
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureValidation);
            ComPtr<ID3D11RasterizerState> nativeRasterizer;
            context->RSGetState(&nativeRasterizer);
            D3D11_RASTERIZER_DESC rasterizer{};
            rasterizer.CullMode = D3D11_CULL_BACK;
            if (nativeRasterizer) nativeRasterizer->GetDesc(&rasterizer);
            if (rasterizer.CullMode < D3D11_CULL_NONE ||
                rasterizer.CullMode > D3D11_CULL_BACK)
                return false;
            // Native Ni-world -> view projection reverses handedness; the
            // D3D cube-face bases above do not. Preserve the same physical
            // front faces by reversing the screen-space winding convention.
            // Captured native triangles have positive projected area while
            // these same front-facing triangles have negative cube-face area.
            const auto rasterizerIndex = (rasterizer.CullMode - 1u) * 2u +
                (rasterizer.FrontCounterClockwise ? 0u : 1u);
            SavedState saved(context);
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureBind);
            if (sun && s_resources.sunVsFailed)
                return false;
            context->VSSetShader((sun ? s_resources.sunOpacityVS : s_resources.opacityVS)[
                layout == Motion::SkyConstantLayout::kVr ? 1 : 0].Get(), nullptr, 0);
            context->PSSetShader(s_resources.opacityPS[techniqueIndex].Get(), nullptr, 0);
            context->OMSetBlendState(s_resources.opacityBlend.Get(), nullptr, UINT(-1));
            context->OMSetDepthStencilState(s_resources.depth.Get(), 0);
            context->RSSetState(s_resources.opacityRasterizers[rasterizerIndex].Get());
            context->SetPredication(nullptr, FALSE);
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(faceSize),
                static_cast<float>(faceSize), 0, 1};
            context->RSSetViewports(1, &viewport);
            for (std::uint32_t face = 0; face < faceCount; ++face) {
                cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureBind);
                auto* target = targets[face];
                context->OMSetRenderTargets(1, &target, nullptr);
                auto* faceCB = sun ? s_resources.sunProjectionCB.Get() : s_resources.faces[face].Get();
                context->VSSetConstantBuffers(0, 1, &faceCB);
                cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureDraw);
                submit(user);
            }
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureValidation);
            return SUCCEEDED(device->GetDeviceRemovedReason());
        } catch (...) {
            return false;
        }
    }

    bool AccumulateOpacity(ID3D11DeviceContext* context,
        Motion::SkyConstantLayout layout, Motion::CloudTechnique technique,
        std::uint32_t faceSize, const std::array<ID3D11RenderTargetView*, 6>& targets,
        SubmitDraw submit, void* user) noexcept
    {
        return Accumulate(context, layout, technique, faceSize, targets, submit, user, nullptr);
    }

    bool AccumulateSunOpacity(ID3D11DeviceContext* context,
        Motion::SkyConstantLayout layout, Motion::CloudTechnique technique,
        ID3D11RenderTargetView* target, const SunMaskProjection& projection,
        SubmitDraw submit, void* user) noexcept
    {
        const std::array<ID3D11RenderTargetView*, 6> targets{ target };
        return Accumulate(context, layout, technique, SunMaskProjection::kResolution,
            targets, submit, user, &projection);
    }
}
