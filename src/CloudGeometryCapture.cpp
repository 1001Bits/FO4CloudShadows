// SPDX-License-Identifier: GPL-3.0-only
#include "CpuStageProfiler.h"
#include "CloudGeometryCapture.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <spdlog/spdlog.h>

namespace FO4CS::CloudGeometryCapture
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // The reviewed stock VS uses World for the camera-relative POSITION0
        // output, TexCoordOff for UV animation, and BlendColor0.w*(color.w+1e-6)
        // for opacity. OG SetupGeometry (RVA 0x287B130) subtracts the render
        // camera from World translation before upload. It is therefore the
        // dome's camera-relative offset, which must stay in its sky direction.
        // Preserve mesh UV/alpha before animation: baking a zero initial blend
        // permanently loses a layer that becomes visible in a later frame.
        constexpr char kShader[] = R"(
cbuffer Face : register(b0) { float4 Right; float4 Up; float4 Forward; };
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
struct Output { float4 p : SV_Position; float2 uv : TEXCOORD0; float alpha : TEXCOORD1; };
Output VS(Input i, uint instanceIndex : SV_InstanceID) {
    Output o;
    float3 direction = mul(World, float4(i.p, 1));
    float forward = dot(Forward.xyz, direction);
    o.p = float4(dot(Right.xyz, direction), dot(Up.xyz, direction), forward, forward);
    o.uv = i.uv + TexCoordOff;
    o.alpha = (i.c.w + 0.000001) * BlendColor[0].w;
#if VR
    // Native VR submits the same dome for both eyes. The directional cube
    // has one camera, so duplicate stereo instances must not double opacity.
    if (instanceIndex != 0) o.p = float4(0, 0, -1, 1);
#endif
    return o;
}
)";

        constexpr char kOpacityPS[] = R"(
cbuffer CloudTechniqueParameters : register(b2) { float4 Parameter; };
Texture2D<float4> Cloud0 : register(t0);
Texture2D<float4> Cloud1 : register(t1);
SamplerState Sampler0 : register(s0);
SamplerState Sampler1 : register(s1);
struct Input { float4 p : SV_Position; float2 uv : TEXCOORD0; float alpha : TEXCOORD1; };
// Portable finite test. Proton/Wine's vkd3d-based d3dcompiler lacks the
// isfinite intrinsic, and fxc may fold NaN comparisons; the exponent bit test
// is exact IEEE-754 on both compilers. A macro, not overloads: vkd3d cannot
// prioritize between compatible overloads (E5017).
#define IS_FINITE(x) ((asuint(x) & 0x7F800000u) != 0x7F800000u)
float4 PS(Input i) : SV_Target0 {
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
        static_assert(kCapturedCubeFaceCount <= kFaces.size());

        // Compiles are deterministic for a device: a failure is retried with
        // exponential backoff and logged once per failure streak, never on
        // every authenticated cloud draw.
        struct CompileLatch
        {
            ULONGLONG retryAfter{};
            std::uint32_t failures{};

            [[nodiscard]] bool Blocked() const noexcept
            {
                return failures != 0 && GetTickCount64() < retryAfter;
            }

            bool Fail() noexcept
            {
                ++failures;
                retryAfter = GetTickCount64() +
                    (std::min)(300000ull, 5000ull << (std::min)(failures, 6u));
                return failures == 1;
            }
        };

        struct Resources
        {
            ComPtr<ID3D11Device> device;
            bool baseReady{};
            CompileLatch baseLatch;
            std::array<ComPtr<ID3D11VertexShader>, 2> opacityVS;
            std::array<CompileLatch, 2> opacityLatch;
            std::array<ComPtr<ID3D11PixelShader>, 3> opacityPS;
            ComPtr<ID3D11BlendState> opacityBlend;
            std::array<ComPtr<ID3D11RasterizerState>, 6> opacityRasterizers;
            std::array<ComPtr<ID3D11Buffer>, 6> faces;
            ComPtr<ID3D11DepthStencilState> depth;
        };
        // Never destroyed: D3D objects must not be released from a DLL
        // static destructor under the loader lock at process exit.
        Resources& s_resources = *new Resources();

        [[nodiscard]] bool CompileVertexShader(ID3D11Device* device,
            const D3D_SHADER_MACRO* macros, const char* name,
            ComPtr<ID3D11VertexShader>& shader, CompileLatch& latch)
        {
            if (shader)
                return true;
            if (latch.Blocked())
                return false;
            ComPtr<ID3DBlob> code, errors;
            if (SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, name, macros,
                    nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors)) &&
                SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),
                    code->GetBufferSize(), nullptr, &shader))) {
                latch = {};
                return true;
            }
            shader.Reset();
            if (latch.Fail()) {
                SPDLOG_ERROR("[CloudShadows] {} failed: {}", name, errors
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "compile/create failed");
            }
            return false;
        }

        // Pixel shaders, face bases and fixed-function states shared by both
        // layouts.
        [[nodiscard]] bool EnsureBaseResources(ID3D11Device* device)
        {
            if (s_resources.device.Get() != device) {
                s_resources = {};
                s_resources.device = device;
            }
            if (s_resources.baseReady)
                return true;
            if (s_resources.baseLatch.Blocked())
                return false;
            Resources next;
            next.device = device;
            const auto fail = [&](const char* what, ID3DBlob* errors) {
                if (s_resources.baseLatch.Fail()) {
                    SPDLOG_ERROR("[CloudShadows] Cloud capture {} failed: {}", what,
                        errors ? static_cast<const char*>(errors->GetBufferPointer())
                               : "creation failed");
                }
                return false;
            };
            constexpr const char* techniques[]{ "5", "6", "7" };
            for (std::size_t index = 0; index < next.opacityPS.size(); ++index) {
                ComPtr<ID3DBlob> code, errors;
                const D3D_SHADER_MACRO macros[]{ {"TECHNIQUE", techniques[index]}, {} };
                if (FAILED(D3DCompile(kOpacityPS, sizeof(kOpacityPS) - 1,
                        "CloudOpacityCapture", macros, nullptr, "PS", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                    FAILED(device->CreatePixelShader(code->GetBufferPointer(),
                        code->GetBufferSize(), nullptr, &next.opacityPS[index])))
                    return fail("opacity PS", errors.Get());
            }
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
                    return fail("face basis", nullptr);
            }
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            D3D11_DEPTH_STENCIL_DESC depth{};
            if (FAILED(device->CreateBlendState(&blend, &next.opacityBlend)) ||
                FAILED(device->CreateDepthStencilState(&depth, &next.depth)))
                return fail("blend/depth state", nullptr);
            D3D11_RASTERIZER_DESC rasterizer{};
            rasterizer.FillMode = D3D11_FILL_SOLID;
            rasterizer.DepthClipEnable = TRUE;
            for (UINT index = 0; index < next.opacityRasterizers.size(); ++index) {
                rasterizer.CullMode = static_cast<D3D11_CULL_MODE>(index / 2 + 1);
                rasterizer.FrontCounterClockwise = (index & 1) != 0;
                if (FAILED(device->CreateRasterizerState(
                        &rasterizer, &next.opacityRasterizers[index])))
                    return fail("rasterizer state", nullptr);
            }
            next.baseReady = true;
            s_resources = std::move(next);
            SPDLOG_INFO("[CloudShadows] Cloud opacity capture resources ready");
            return true;
        }

        [[nodiscard]] ID3D11VertexShader* EnsureVertexShader(
            ID3D11Device* device, SkyConstantLayout layout)
        {
            if (!EnsureBaseResources(device))
                return nullptr;
            const std::size_t index = layout == SkyConstantLayout::kVr ? 1 : 0;
            const D3D_SHADER_MACRO macros[]{ {"VR", index ? "1" : "0"}, {} };
            return CompileVertexShader(device, macros, "Cloud opacity capture VS",
                       s_resources.opacityVS[index], s_resources.opacityLatch[index])
                ? s_resources.opacityVS[index].Get() : nullptr;
        }

        class SavedState
        {
        public:
            explicit SavedState(ID3D11DeviceContext* context) : context_(context)
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureSave);
                context_->VSGetShader(&vs_, nullptr, &vsClasses_);
                context_->PSGetShader(&ps_, nullptr, &psClasses_);
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

            // The stock pair was fetched once here; validation reuses it.
            [[nodiscard]] bool HasStaticShaders() const noexcept
            {
                return vs_ && ps_ && vsClasses_ == 0 && psClasses_ == 0;
            }
            [[nodiscard]] ID3D11RasterizerState* Rasterizer() const noexcept
            {
                return rasterizer_.Get();
            }
        private:
            ID3D11DeviceContext* context_;
            ComPtr<ID3D11DeviceContext1> context1_;
            ComPtr<ID3D11VertexShader> vs_;
            ComPtr<ID3D11PixelShader> ps_;
            UINT vsClasses_{}, psClasses_{};
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

        // Stages the replay cannot preserve. VS/PS were already fetched by
        // SavedState, so only the remaining pipeline slots are queried here.
        [[nodiscard]] RejectReason CheckPipeline(
            ID3D11DeviceContext* context, const SavedState& saved)
        {
            FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureValidation);
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->IAGetPrimitiveTopology(&topology);
            if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
                topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
                return RejectReason::kTopology;
            ComPtr<ID3D11GeometryShader> gs;
            ComPtr<ID3D11HullShader> hs;
            ComPtr<ID3D11DomainShader> ds;
            context->GSGetShader(&gs, nullptr, nullptr);
            context->HSGetShader(&hs, nullptr, nullptr);
            context->DSGetShader(&ds, nullptr, nullptr);
            if (gs || hs || ds)
                return RejectReason::kExtraShaderStage;
            if (!saved.HasStaticShaders())
                return RejectReason::kClassLinkage;
            std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> outputs{};
            context->SOGetTargets(static_cast<UINT>(outputs.size()), outputs.data());
            bool streamOutput = false;
            for (auto* output : outputs) {
                if (output) {
                    streamOutput = true;
                    output->Release();
                }
            }
            if (streamOutput)
                return RejectReason::kStreamOutput;
            // OMSetRenderTargets would otherwise silently disturb an owner's
            // pixel UAV bindings. Stock Sky has none.
            std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs{};
            context->OMGetRenderTargetsAndUnorderedAccessViews(
                0, nullptr, nullptr, 0, static_cast<UINT>(uavs.size()), uavs.data());
            bool pixelUav = false;
            for (auto* uav : uavs) {
                if (uav) {
                    pixelUav = true;
                    uav->Release();
                }
            }
            return pixelUav ? RejectReason::kPixelUav : RejectReason::kNone;
        }
    }

    void ReleaseDeviceResources() noexcept { s_resources = {}; }

    bool AccumulateOpacity(ID3D11DeviceContext* context,
        SkyConstantLayout layout, CloudTechnique technique,
        std::uint32_t faceSize, const std::array<ID3D11RenderTargetView*, 6>& targets,
        SubmitDraw submit, void* user, RejectReason* reason) noexcept
    {
        FO4CS::CpuProfile::Scope cpuTotal(FO4CS::CpuProfile::Stage::CaptureTotal);
        RejectReason unused{};
        auto& result = reason ? *reason : unused;
        result = RejectReason::kNone;
        try {
            FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CaptureValidation);
            const auto techniqueIndex = static_cast<std::uint32_t>(technique) - 5u;
            if (!context || !submit || !faceSize || techniqueIndex >= 3 ||
                context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                (layout != SkyConstantLayout::kFlat &&
                 layout != SkyConstantLayout::kVr)) {
                result = RejectReason::kInvalidRequest;
                return false;
            }
            for (std::uint32_t face = 0; face < kCapturedCubeFaceCount; ++face) {
                if (!targets[face]) {
                    result = RejectReason::kInvalidRequest;
                    return false;
                }
            }
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            auto* vertexShader = EnsureVertexShader(device.Get(), layout);
            if (!vertexShader) {
                result = RejectReason::kResources;
                return false;
            }
            SavedState saved(context);
            result = CheckPipeline(context, saved);
            if (result != RejectReason::kNone)
                return false;
            D3D11_RASTERIZER_DESC rasterizer{};
            rasterizer.CullMode = D3D11_CULL_BACK;
            if (auto* nativeRasterizer = saved.Rasterizer())
                nativeRasterizer->GetDesc(&rasterizer);
            if (rasterizer.CullMode < D3D11_CULL_NONE ||
                rasterizer.CullMode > D3D11_CULL_BACK) {
                result = RejectReason::kRasterizer;
                return false;
            }
            // Native Ni-world -> view projection reverses handedness; the
            // D3D cube-face bases above do not. Preserve the same physical
            // front faces by reversing the screen-space winding convention.
            // Captured native triangles have positive projected area while
            // these same front-facing triangles have negative cube-face area.
            const auto rasterizerIndex = (rasterizer.CullMode - 1u) * 2u +
                (rasterizer.FrontCounterClockwise ? 0u : 1u);
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureBind);
            context->VSSetShader(vertexShader, nullptr, 0);
            context->PSSetShader(s_resources.opacityPS[techniqueIndex].Get(), nullptr, 0);
            context->OMSetBlendState(s_resources.opacityBlend.Get(), nullptr, UINT(-1));
            context->OMSetDepthStencilState(s_resources.depth.Get(), 0);
            context->RSSetState(s_resources.opacityRasterizers[rasterizerIndex].Get());
            context->SetPredication(nullptr, FALSE);
            const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(faceSize),
                static_cast<float>(faceSize), 0, 1};
            context->RSSetViewports(1, &viewport);
            for (std::uint32_t face = 0; face < kCapturedCubeFaceCount; ++face) {
                cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureBind);
                auto* target = targets[face];
                context->OMSetRenderTargets(1, &target, nullptr);
                auto* faceCB = s_resources.faces[face].Get();
                context->VSSetConstantBuffers(0, 1, &faceCB);
                cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureDraw);
                submit(user);
            }
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureValidation);
            if (FAILED(device->GetDeviceRemovedReason())) {
                result = RejectReason::kDeviceRemoved;
                return false;
            }
            return true;
        } catch (...) {
            result = RejectReason::kException;
            return false;
        }
    }
}
