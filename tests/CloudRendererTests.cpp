// GPU contract tests use generated textures/constants, never game assets.
#include "CloudMotionResolver.h"
#include "CloudGeometryCapture.h"
#include "CloudCubePreview.h"
#include "GodrayCloudShader.h"
#include "MainViewCameraReadback.h"

#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace Motion = FO4CS::CloudMotionResolver;
using Float4 = std::array<float, 4>;

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void Check(HRESULT result, const char* message)
    {
        Require(SUCCEEDED(result), message);
    }

    struct GPU
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11InfoQueue> messages;

        explicit GPU(bool hardware = false)
        {
            D3D_FEATURE_LEVEL level{};
            HRESULT result = D3D11CreateDevice(nullptr,
                hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
                nullptr, hardware ? 0u : D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                D3D11_SDK_VERSION, &device, &level, &context);
            if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                    nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                    &device, &level, &context);
                std::cout << "D3D debug layer unavailable; GPU assertions still run\n";
            }
            Check(result, "create renderer test device");
            (void)device.As(&messages);
            if (hardware) {
                ComPtr<IDXGIDevice> dxgi;
                ComPtr<IDXGIAdapter> adapter;
                Check(device.As(&dxgi), "query benchmark adapter");
                Check(dxgi->GetAdapter(&adapter), "get benchmark adapter");
                DXGI_ADAPTER_DESC desc{};
                Check(adapter->GetDesc(&desc), "benchmark adapter description");
                std::wcout << L"Hardware renderer adapter: " << desc.Description << L'\n';
            }
        }

        template<class Work>
        double Measure(Work work, UINT repetitions)
        {
            ComPtr<ID3D11Query> disjoint, start, end;
            D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            Check(device->CreateQuery(&desc, &disjoint), "create disjoint query");
            desc.Query = D3D11_QUERY_TIMESTAMP;
            Check(device->CreateQuery(&desc, &start), "create start timestamp");
            Check(device->CreateQuery(&desc, &end), "create end timestamp");
            work();
            context->Begin(disjoint.Get());
            context->End(start.Get());
            for (UINT i = 0; i < repetitions; ++i)
                work();
            context->End(end.Get());
            context->End(disjoint.Get());
            context->Flush(); // Offline benchmark only; never linked into the DLL.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            const auto read = [&](ID3D11Query* query, void* data, UINT size) {
                HRESULT result;
                while ((result = context->GetData(query, data, size,
                            D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
                    Require(std::chrono::steady_clock::now() < deadline, "GPU query timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                Check(result, "read GPU timestamp");
            };
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
            UINT64 first{}, last{};
            read(disjoint.Get(), &frequency, sizeof(frequency));
            read(start.Get(), &first, sizeof(first));
            read(end.Get(), &last, sizeof(last));
            Require(!frequency.Disjoint && frequency.Frequency != 0, "stable GPU timestamps");
            return static_cast<double>(last - first) * 1000.0 /
                static_cast<double>(frequency.Frequency) / repetitions;
        }

        void CheckMessages()
        {
            if (!messages)
                return;
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                SIZE_T size{};
                Check(messages->GetMessage(i, nullptr, &size), "query debug message");
                std::vector<std::byte> bytes(size);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                Check(messages->GetMessage(i, message, &size), "read debug message");
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                    std::cerr << message->pDescription << '\n';
                    throw std::runtime_error("D3D debug-layer warning/error");
                }
            }
            messages->ClearStoredMessages();
        }

        template<class First, class Second>
        void Compare(const std::string& name, First first, Second second, UINT repetitions)
        {
            std::array<double,9> cube{}, sun{};
            // Two warmup pairs, then nine measured pairs. Reverse submission
            // order every round to expose order/clock effects in a short test.
            for (UINT round=0; round<11; ++round) {
                double a{}, b{};
                if (round%2) { b=Measure(second,repetitions); a=Measure(first,repetitions); }
                else { a=Measure(first,repetitions); b=Measure(second,repetitions); }
                if (round>=2) { cube[round-2]=a; sun[round-2]=b; }
            }
            const auto report = [&](const char* method, std::array<double,9> values) {
                std::sort(values.begin(),values.end());
                std::cout << name << " method=" << method << " GPU-span-ms median=" << values[4]
                    << " min=" << values.front() << " max=" << values.back() << " batches=9\n";
            };
            report("Cubemap",cube); report("Sun2D",sun);
        }

        ComPtr<ID3D11Buffer> Constants(const void* data, UINT size)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = size;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{};
            initial.pSysMem = data;
            ComPtr<ID3D11Buffer> buffer;
            Check(device->CreateBuffer(&desc, &initial, &buffer), "create constants");
            return buffer;
        }

        ComPtr<ID3D11SamplerState> Sampler()
        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            ComPtr<ID3D11SamplerState> sampler;
            Check(device->CreateSamplerState(&desc, &sampler), "create sampler");
            return sampler;
        }

        std::vector<float> Read(ID3D11Resource* resource)
        {
            ComPtr<ID3D11Texture2D> source;
            Check(resource->QueryInterface(IID_PPV_ARGS(&source)), "read texture type");
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            const auto format = desc.Format;
            Require(format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT ||
                format == DXGI_FORMAT_R32G32B32A32_FLOAT || format == DXGI_FORMAT_R8G8B8A8_UNORM,
                "readback supports float and RGBA8 preview formats");
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            Check(device->CreateTexture2D(&desc, nullptr, &staging), "create readback");
            context->CopyResource(staging.Get(), source.Get());
            std::vector<float> values;
            for (UINT face = 0; face < desc.ArraySize; ++face) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const UINT subresource = D3D11CalcSubresource(0, face, desc.MipLevels);
                Check(context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0,
                    &mapped), "map test readback");
                for (UINT y = 0; y < desc.Height; ++y) {
                    const auto* row = static_cast<const std::byte*>(mapped.pData) +
                        y * mapped.RowPitch;
                    const UINT components = format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                        format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4u : 1u;
                    for (UINT x = 0; x < desc.Width * components; ++x) {
                        values.push_back(format == DXGI_FORMAT_R16_FLOAT
                            ? DirectX::PackedVector::XMConvertHalfToFloat(
                                reinterpret_cast<const uint16_t*>(row)[x])
                            : format == DXGI_FORMAT_R8G8B8A8_UNORM
                                ? static_cast<float>(reinterpret_cast<const uint8_t*>(row)[x]) / 255.0f
                                : reinterpret_cast<const float*>(row)[x]);
                    }
                }
                context->Unmap(staging.Get(), subresource);
            }
            return values;
        }
    };

    void RunMainViewCameraReadbackTests(GPU& gpu)
    {
        using Readback = FO4CS::MainViewCameraReadback;
        std::array<Float4, 96> constants{};
        // Recorded regression: the CPU camera was at z=17000 while the draw
        // reconstructed the real player camera at z=1995.33. A decoy in an
        // earlier constant-buffer window must not establish the world anchor.
        constants[35] = {-24915, -30415, 17000, 0};
        constants[16 + 35] = {-24913.4f, -30412.13f, 1995.33f, 0};
        constants[16 + 59] = {-24915.4f, -30412.13f, 1995.33f, 0};
        constants[16 + 60] = {-24911.4f, -30412.13f, 1995.33f, 0};
        auto buffer = gpu.Constants(constants.data(), sizeof(constants));
        Readback readback;
        Readback::Point observed{};
        const auto awaitResult = [&](bool vr, std::uint64_t generation) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!readback.TryRead(gpu.context.Get(), buffer.Get(), 16, vr, generation, observed)) {
                // A standalone test has no Present to submit commands. This
                // flush/wait exists only in the test, never in the reader.
                gpu.context->Flush();
                Require(std::chrono::steady_clock::now() < deadline, "main-view camera readback timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        Require(!readback.TryRead(gpu.context.Get(), buffer.Get(), 16, false, 0, observed),
            "the initial GPU copy is asynchronous");
        // Mutating the engine's source after submission must not change the
        // captured draw camera; the staging copy owns that draw's contents.
        constants[16 + 35] = {100, 200, 300, 0};
        gpu.context->UpdateSubresource(buffer.Get(), 0, nullptr, constants.data(), 0, 0);
        awaitResult(false, 0); // No reset has occurred in a newly initialized world.
        Require(std::abs(observed[0] + 24913.4f) < 0.01f &&
            std::abs(observed[2] - 1995.33f) < 0.01f,
            "anchor uses the actual draw camera and D3D11.1 window, not a stale camera");
        readback.Reset();
        awaitResult(true, 2);
        Require(std::abs(observed[0] + 24913.4f) < 0.01f &&
            std::abs(observed[2] - 1995.33f) < 0.01f,
            "VR anchor is the mean of the two authenticated GPU eyes");
        Require(!readback.TryRead(gpu.context.Get(), buffer.Get(), 16, true, 2, observed),
            "queue an old-world camera");
        awaitResult(false, 3);
        Require(observed == Readback::Point{100,200,300},
            "a world change discards in-flight camera evidence from the old world");
        readback.Reset();
        Require(!readback.TryRead(gpu.context.Get(), buffer.Get(), UINT_MAX, false, 3, observed),
            "overflowing constant offsets fail before a GPU copy");
        constants[16 + 35][2] = std::numeric_limits<float>::quiet_NaN();
        gpu.context->UpdateSubresource(buffer.Get(), 0, nullptr, constants.data(), 0, 0);
        for (unsigned i = 0; i < 10; ++i) {
            Require(!readback.TryRead(gpu.context.Get(), buffer.Get(), 16, false, 3, observed),
                "non-finite GPU cameras never establish an anchor");
            gpu.context->Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        readback.Reset();
        gpu.CheckMessages();
    }

    void ExpectUniform(GPU& gpu, ID3D11ShaderResourceView* view, float expected)
    {
        gpu.CheckMessages();
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        const auto values = gpu.Read(resource.Get());
        for (float value : values) {
            if (!std::isfinite(value) || std::abs(value - expected) >= 0.001f)
                std::cerr << "Expected " << expected << ", observed " << value << '\n';
            Require(std::isfinite(value) && std::abs(value - expected) < 0.001f,
                "resolved opacity matches independent expected value on all faces");
        }
    }

    struct MotionFixture
    {
        GPU& gpu;
        Motion::SkyConstantLayout layout;
        std::array<Float4, 32> vertex{};
        std::array<Float4, 16> pixel{};
        ComPtr<ID3D11Buffer> vs;
        ComPtr<ID3D11Buffer> ps;
        ComPtr<ID3D11ShaderResourceView> texture;
        ComPtr<ID3D11SamplerState> sampler;
        uint64_t serial{};
        uint32_t faceSize = 8;

        MotionFixture(GPU& device, Motion::SkyConstantLayout constantLayout) :
            gpu(device), layout(constantLayout)
        {
            Motion::ReleaseDeviceResources();
            vs = gpu.Constants(vertex.data(), sizeof(vertex));
            ps = gpu.Constants(pixel.data(), sizeof(pixel));
            SetLive(0.0f, 0.5f, 0.0f);
            std::array<Float4, 16> texels{};
            for (size_t i = 0; i < texels.size(); ++i)
                texels[i] = { 1.0f, 1.0f, 1.0f, i % 4 < 2 ? 0.25f : 0.75f };
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = 4;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = texels.data();
            data.SysMemPitch = 4 * sizeof(Float4);
            ComPtr<ID3D11Texture2D> source;
            Check(gpu.device->CreateTexture2D(&desc, &data, &source), "cloud source");
            Check(gpu.device->CreateShaderResourceView(source.Get(), nullptr, &texture),
                "cloud source view");
            sampler = gpu.Sampler();
            ID3D11ShaderResourceView* sources[]{ texture.Get(), texture.Get() };
            ID3D11SamplerState* samplers[]{ sampler.Get(), sampler.Get() };
            gpu.context->PSSetShaderResources(0, 2, sources);
            gpu.context->PSSetSamplers(0, 2, samplers);
        }

        ~MotionFixture()
        {
            gpu.context->ClearState();
            Motion::ReleaseDeviceResources();
        }

        void SetLive(float offset, float blend, float parameter)
        {
            const bool vr = layout == Motion::SkyConstantLayout::kVr;
            vertex[vr ? 14 : 10] = { offset, 0, 0, 0 };
            vertex[vr ? 11 : 7][3] = blend;
            pixel[0][0] = parameter;
            gpu.context->UpdateSubresource(vs.Get(), 0, nullptr, vertex.data(), 0, 0);
            gpu.context->UpdateSubresource(ps.Get(), 0, nullptr, pixel.data(), 0, 0);
            ID3D11Buffer* rawVS = vs.Get();
            ID3D11Buffer* rawPS = ps.Get();
            gpu.context->VSSetConstantBuffers(2, 1, &rawVS);
            gpu.context->PSSetConstantBuffers(2, 1, &rawPS);
        }

        void Capture(uint64_t epoch, uint32_t layerCount = 2)
        {
            Require(Motion::BeginCaptureGeneration(gpu.device.Get(),
                { epoch, 1, faceSize, layout }), "begin mapping generation");
            for (uint32_t face = 0; face < 6; ++face) {
                Require(Motion::BeginCaptureCubeFace(face), "begin mapping face");
                for (uint32_t layer = 1; layer <= layerCount; ++layer) {
                    Motion::CaptureLayerFace key{ layer, face, Motion::CloudTechnique::kClouds };
                    Motion::CaptureFaceTarget target;
                    Require(Motion::AcquireLayerMappingTarget(gpu.context.Get(), key, target),
                        "acquire mapping target");
                    const bool vr = layout == Motion::SkyConstantLayout::kVr;
                    const Float4 mapping{ 0.125f + vertex[vr ? 14 : 10][0],
                        0.125f, vertex[vr ? 11 : 7][3], 1.0f };
                    gpu.context->ClearRenderTargetView(target.mappingRtv.Get(), mapping.data());
                    Require(Motion::CompleteLayerMapping(key, true), "complete mapping layer");
                }
                Require(Motion::CompleteCaptureCubeFace(face, true), "complete mapping face");
            }
            Require(Motion::PublishCaptureGeneration(), "publish complete mapping");
        }

        void Begin(uint64_t epoch)
        {
            Require(Motion::BeginFrame(gpu.context.Get(), { epoch, 1, ++serial }),
                "begin live frame");
        }

        void Accumulate(uint64_t id, Motion::CloudTechnique technique)
        {
            Require(Motion::AccumulateLiveLayer(gpu.context.Get(), { id, technique, true }),
                "resolve live cloud layer");
        }

        Motion::ResolvedSnapshot Complete()
        {
            Require(Motion::CompleteFrame(gpu.context.Get(), true), "publish live frame");
            Motion::ResolvedSnapshot result;
            Require(Motion::AcquireResolvedSnapshot(result), "acquire resolved frame");
            return result;
        }
    };

    void RunMotionTests(GPU& gpu, Motion::SkyConstantLayout layout)
    {
        MotionFixture fixture(gpu, layout);
        fixture.Capture(1);
        fixture.Begin(1);
        fixture.Accumulate(1, Motion::CloudTechnique::kClouds);
        auto first = fixture.Complete();
        ExpectUniform(gpu, first.opacityCube.Get(), 0.125f);

        fixture.SetLive(0.5f, 0.5f, 0.0f);
        fixture.Begin(1);
        fixture.Accumulate(1, Motion::CloudTechnique::kClouds);
        // The old field remains immutable until publication.
        ExpectUniform(gpu, first.opacityCube.Get(), 0.125f);
        auto moved = fixture.Complete();
        ExpectUniform(gpu, moved.opacityCube.Get(), 0.375f);

        fixture.SetLive(0.5f, 0.25f, 0.7f);
        fixture.Begin(1);
        fixture.Accumulate(1, Motion::CloudTechnique::kCloudsFade);
        auto faded = fixture.Complete();
        ExpectUniform(gpu, faded.opacityCube.Get(), 0.09375f);

        fixture.SetLive(0.0f, 0.5f, 0.25f);
        fixture.Begin(1);
        fixture.Accumulate(1, Motion::CloudTechnique::kCloudsLerp);
        fixture.Accumulate(2, Motion::CloudTechnique::kClouds);
        auto combined = fixture.Complete();
        ExpectUniform(gpu, combined.opacityCube.Get(), 0.234375f);

        fixture.Begin(1);
        auto clear = fixture.Complete();
        ExpectUniform(gpu, clear.opacityCube.Get(), 0.0f);
        Require(clear.layerCount == 0, "empty sky reports no live layers");

        fixture.Begin(1);
        fixture.Accumulate(1, Motion::CloudTechnique::kClouds);
        Require(!Motion::AccumulateLiveLayer(gpu.context.Get(),
            { 1, Motion::CloudTechnique::kClouds, true }), "duplicate layer rejected");
        Require(!Motion::CompleteFrame(gpu.context.Get(), true), "poisoned frame rejected");
        Motion::ResolvedSnapshot invalid;
        Require(!Motion::AcquireResolvedSnapshot(invalid), "failed frame withdraws field");
        fixture.Begin(1);
        fixture.Accumulate(2, Motion::CloudTechnique::kClouds);
        auto recovered = fixture.Complete();
        ExpectUniform(gpu, recovered.opacityCube.Get(), 0.125f);

        fixture.Capture(2);
        const auto warm = Motion::GetDiagnostics();
        fixture.Capture(3);
        const auto reused = Motion::GetDiagnostics();
        Require(reused.mappingResourceCreates == warm.mappingResourceCreates &&
            reused.mappingResourceReuses >= warm.mappingResourceReuses + 2,
            "natural refresh reuses warmed mapping textures");
        Require(reused.mappingBytes == 4u * 8u * 8u * 6u * 16u,
            "mapping storage remains bounded to two sets");
        fixture.Begin(3);
        fixture.Accumulate(1, Motion::CloudTechnique::kClouds);
        auto refreshed = fixture.Complete();
        ExpectUniform(gpu, refreshed.opacityCube.Get(), 0.125f);

        Require(Motion::BeginCaptureGeneration(gpu.device.Get(), { 4, 1, 8, layout }),
            "begin rejected refresh");
        Require(!Motion::PublishCaptureGeneration(), "incomplete cube rejected");
        Motion::AbortCaptureGeneration();
        Require(Motion::AcquireResolvedSnapshot(invalid), "bad refresh retains current field");
        fixture.SetLive(4096.0f, 0.5f, 0.0f);
        fixture.Capture(5);
        fixture.SetLive(4096.5f, 0.5f, 0.0f);
        // Verify D3D11.1 windows survive a resolver which temporarily binds
        // this same buffer through a different range.
        ComPtr<ID3D11DeviceContext1> context1;
        Check(gpu.context.As(&context1), "D3D11.1 context available");
        ID3D11Buffer* sentinel = fixture.vs.Get();
        const UINT firstConstant = 16, constantCount = 16;
        context1->CSSetConstantBuffers1(1, 1, &sentinel, &firstConstant, &constantCount);
        fixture.Begin(5);
        fixture.Accumulate(1, Motion::CloudTechnique::kClouds);
        ComPtr<ID3D11Buffer> restored;
        UINT restoredFirst{}, restoredCount{};
        context1->CSGetConstantBuffers1(1, 1, &restored, &restoredFirst, &restoredCount);
        Require(restored.Get() == sentinel && restoredFirst == firstConstant &&
            restoredCount == constantCount, "compute constant-buffer window restored");
        auto longRunning = fixture.Complete();
        ExpectUniform(gpu, longRunning.opacityCube.Get(), 0.375f);
        Motion::Invalidate();
        Require(!Motion::AcquireResolvedSnapshot(invalid), "world reset invalidates field");
        gpu.CheckMessages();
    }

    void RunGeometryCaptureTests(GPU& gpu, Motion::SkyConstantLayout layout,
        float initialBlend = 0.5f, bool benchmark = false)
    {
        namespace Geometry = FO4CS::CloudGeometryCapture;
        MotionFixture fixture(gpu, layout);
        const bool vr = layout == Motion::SkyConstantLayout::kVr;
        const UINT world = vr ? 8u : 4u;
        // World is already camera-relative. Rotate the dome 90 degrees about
        // Z and offset it from the camera; its visible directions include both.
        fixture.vertex[world] = {0, -1, 0, 0.2f};
        fixture.vertex[world + 1] = {1, 0, 0, -0.15f};
        fixture.vertex[world + 2] = {0, 0, 1, 0.3f};
        // A layer can be transparent during loading, then become visible
        // without changing geometry. Its raw mesh alpha must survive bootstrap.
        fixture.SetLive(0.25f, initialBlend, 0);
        constexpr char shader[] = R"(
struct Input { float3 p : POSITION0; float2 uv : TEXCOORD0; float4 c : COLOR0; };
struct Output { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };
Output VS(Input i) { Output o; o.p=float4(i.p,1); o.uv=i.uv; o.c=i.c; return o; }
float4 PS(Output i) : SV_Target { return float4(i.uv,i.c.w,1); }
)";
        const auto compile = [&](const char* entry, const char* profile) {
            ComPtr<ID3DBlob> code, errors;
            Check(D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr,
                entry, profile, D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors),
                "compile geometry test shader");
            return code;
        };
        const auto vertexCode = compile("VS", "vs_5_0");
        const auto pixelCode = compile("PS", "ps_5_0");
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        Check(gpu.device->CreateVertexShader(vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), nullptr, &vs), "test geometry VS");
        Check(gpu.device->CreatePixelShader(pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(), nullptr, &ps), "test geometry PS");
        const D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}
        };
        ComPtr<ID3D11InputLayout> inputLayout;
        Check(gpu.device->CreateInputLayout(elements, 3,
            vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), &inputLayout),
            "test geometry input layout");
        struct Vertex { std::array<float,3> p; std::array<float,2> uv; Float4 color; };
        std::array<Vertex, 36> vertices{};
        // Independent cube mesh: X faces span Y/Z, Y faces X/Z, Z faces X/Y.
        constexpr std::array<std::array<float,2>,6> corners{{
            {-1,-1}, {-1,1}, {1,-1}, {1,-1}, {-1,1}, {1,1}
        }};
        for (UINT face = 0; face < 6; ++face) {
            const UINT axis = face / 2;
            for (UINT index = 0; index < 6; ++index) {
                auto& vertex = vertices[face * 6 + index];
                vertex.p[axis] = face % 2 ? -1.0f : 1.0f;
                vertex.p[(axis + 1) % 3] = corners[index][0];
                vertex.p[(axis + 2) % 3] = corners[index][1];
                vertex.uv = {face % 2 ? 0.625f : 0.125f, 0.5f};
                vertex.color = {1,1,1,1};
            }
        }
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(vertices);
        bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = vertices.data();
        ComPtr<ID3D11Buffer> vb;
        Check(gpu.device->CreateBuffer(&bufferDesc, &initial, &vb), "test dome geometry");
        ID3D11Buffer* rawVB = vb.Get();
        const UINT stride = sizeof(Vertex), offset = 0;
        gpu.context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);
        gpu.context->IASetInputLayout(inputLayout.Get());
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->VSSetShader(vs.Get(), nullptr, 0);
        gpu.context->PSSetShader(ps.Get(), nullptr, 0);
        ComPtr<ID3D11DeviceContext1> context1;
        Check(gpu.context.As(&context1), "geometry D3D11.1 context");
        ID3D11Buffer* sentinel = fixture.vs.Get();
        const UINT first = 16, count = 16;
        context1->VSSetConstantBuffers1(0,1,&sentinel,&first,&count);
        D3D11_TEXTURE2D_DESC targetDesc{};
        targetDesc.Width = targetDesc.Height = 4;
        targetDesc.ArraySize = targetDesc.MipLevels = targetDesc.SampleDesc.Count = 1;
        targetDesc.Format = DXGI_FORMAT_R32_FLOAT;
        targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> primary;
        ComPtr<ID3D11RenderTargetView> primaryView;
        Check(gpu.device->CreateTexture2D(&targetDesc, nullptr, &primary), "primary sentinel");
        Check(gpu.device->CreateRenderTargetView(primary.Get(), nullptr, &primaryView),
            "primary sentinel RTV");
        const float clear[]{0.625f,0,0,0};
        gpu.context->ClearRenderTargetView(primaryView.Get(), clear);
        auto* rawTarget = primaryView.Get();
        gpu.context->OMSetRenderTargets(1,&rawTarget,nullptr);
        const D3D11_VIEWPORT viewport{0,0,4,4,0,1};
        gpu.context->RSSetViewports(1,&viewport);
        Require(Motion::BeginCaptureGeneration(gpu.device.Get(), {1,1,8,layout}),
            "bootstrap without any native reflection event");
        for (UINT face = 0; face < 6; ++face)
            Require(Motion::BeginCaptureCubeFace(face), "bootstrap face begin");
        struct Draw { ID3D11DeviceContext* context; UINT calls{}; bool vr; } draw{gpu.context.Get(),0,vr};
        const auto submit = [](void* user) {
            auto& request = *static_cast<Draw*>(user);
            ++request.calls;
            if (request.vr)
                request.context->DrawInstanced(36,2,0,0);
            else
                request.context->Draw(36,0);
        };
        Require(Geometry::CaptureLayer(gpu.context.Get(), layout, 1,
            Motion::CloudTechnique::kClouds, 8, submit, &draw), "capture native dome geometry");
        Require(draw.calls == 6, "bootstrap is six bounded draws");
        for (float value : gpu.Read(primary.Get()))
            Require(value == 0.625f, "bootstrap leaves visible sky target untouched");
        ComPtr<ID3D11VertexShader> restoredVS;
        ComPtr<ID3D11PixelShader> restoredPS;
        ComPtr<ID3D11Buffer> restoredCB;
        UINT restoredFirst{}, restoredCount{};
        gpu.context->VSGetShader(&restoredVS,nullptr,nullptr);
        gpu.context->PSGetShader(&restoredPS,nullptr,nullptr);
        context1->VSGetConstantBuffers1(0,1,&restoredCB,&restoredFirst,&restoredCount);
        Require(restoredVS.Get() == vs.Get() && restoredPS.Get() == ps.Get() &&
            restoredCB.Get() == sentinel && restoredFirst == first && restoredCount == count,
            "bootstrap restores native shaders and constant-buffer window");
        for (UINT face = 0; face < 6; ++face)
            Require(Motion::CompleteCaptureCubeFace(face,true), "bootstrap face complete");
        Require(Motion::PublishCaptureGeneration(), "bootstrap mapping publishes");
        Require(Motion::HasCapturedLayer(1) && !Motion::HasCapturedLayer(2),
            "new weather geometry requests a new mapping");
        for (UINT frame = 0; frame < 2; ++frame) {
            fixture.SetLive(frame ? 0.5f : 0, 0.5f, 0);
            fixture.Begin(1);
            fixture.Accumulate(1, Motion::CloudTechnique::kClouds);
            auto resolved = fixture.Complete();
            ComPtr<ID3D11Resource> cube;
            resolved.opacityCube->GetResource(&cube);
            const auto values = gpu.Read(cube.Get());
            for (size_t i = 0; i < values.size(); ++i) {
                const float u = (static_cast<float>(i % 8) + 0.5f) * 0.25f - 1;
                const float v = (static_cast<float>((i / 8) % 8) + 0.5f) * 0.25f - 1;
                // Independently ray-trace the translated unit box. The exit
                // face determines its constant UV, without using capture VS
                // matrices, rasterization, or an expected captured texture.
                const std::array<std::array<float, 3>, 6> rays{{
                    {1,-v,-u}, {-1,-v,u}, {u,1,v}, {u,-1,-v}, {u,-v,1}, {-u,-v,-1}
                }};
                const auto& ray = rays[i / 64];
                const std::array<float, 3> localRay{ray[1], -ray[0], ray[2]};
                const std::array<float, 3> localCamera{0.15f, 0.2f, -0.3f};
                float firstExit = std::numeric_limits<float>::max();
                bool high = false;
                for (UINT axis = 0; axis < 3; ++axis) {
                    if (std::abs(localRay[axis]) < 1.0e-6f)
                        continue;
                    const bool negative = localRay[axis] < 0;
                    const float distance = ((negative ? -1.0f : 1.0f) -
                        localCamera[axis]) / localRay[axis];
                    if (distance < firstExit) {
                        firstExit = distance;
                        high = negative;
                    }
                }
                const float expected = high != (frame != 0) ? 0.375f : 0.125f;
                Require(std::abs(values[i] - expected) < 0.001f,
                    "camera-relative dome translation, rotation and live cloud speed agree");
            }
        }
        Require(draw.calls == 6, "animated frames do not replay geometry");
        // Real FO4 cloud meshes contain overlapping surfaces within a single
        // draw. Both .25-alpha surfaces must contribute (.25 + .25*.75),
        // including in VR where native geometry is submitted once per eye.
        std::array<Vertex, 72> overlapping{};
        for (UINT i = 0; i < overlapping.size(); ++i) {
            UINT sourceIndex = i % static_cast<UINT>(vertices.size());
            // Make all six faces point inward, as the captured native cloud
            // shell does. The earlier no-cull fixture concealed a handedness
            // mismatch between native Ni-world and the cube-face projection.
            if ((sourceIndex / 6) % 2 != 0 && sourceIndex % 3 != 0)
                sourceIndex += sourceIndex % 3 == 1 ? 1u : UINT(-1);
            overlapping[i] = vertices[sourceIndex];
            for (auto& coordinate : overlapping[i].p)
                coordinate *= i < vertices.size() ? 1.0f : 1.2f;
            overlapping[i].uv = {0.125f, 0.5f};
        }
        bufferDesc.ByteWidth = sizeof(overlapping);
        initial.pSysMem = overlapping.data();
        ComPtr<ID3D11Buffer> overlapVB;
        Check(gpu.device->CreateBuffer(&bufferDesc, &initial, &overlapVB),
            "overlapping cloud surfaces");
        rawVB = overlapVB.Get();
        gpu.context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);
        D3D11_RASTERIZER_DESC noCull{};
        noCull.FillMode = D3D11_FILL_SOLID;
        noCull.CullMode = D3D11_CULL_NONE;
        noCull.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> noCullState;
        Check(gpu.device->CreateRasterizerState(&noCull,&noCullState), "overlap rasterizer");
        gpu.context->RSSetState(noCullState.Get());
        const UINT opacityFaceSize = benchmark ? 256u : 8u;
        targetDesc.Width = targetDesc.Height = opacityFaceSize;
        targetDesc.ArraySize = 6;
        targetDesc.Format = DXGI_FORMAT_R16_FLOAT;
        targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        targetDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        ComPtr<ID3D11Texture2D> opacity;
        Check(gpu.device->CreateTexture2D(&targetDesc,nullptr,&opacity), "overlap cube");
        std::array<ComPtr<ID3D11RenderTargetView>,6> opacityViews;
        std::array<ID3D11RenderTargetView*,6> opacityTargets{};
        const float zero[4]{};
        for (UINT face = 0; face < 6; ++face) {
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R16_FLOAT;
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = face;
            view.Texture2DArray.ArraySize = 1;
            Check(gpu.device->CreateRenderTargetView(opacity.Get(),&view,
                &opacityViews[face]), "overlap face");
            opacityTargets[face] = opacityViews[face].Get();
        }
        const auto submitOverlap = [](void* user) {
            auto& request = *static_cast<Draw*>(user);
            ++request.calls;
            if (request.vr) request.context->DrawInstanced(72,2,0,0);
            else request.context->Draw(72,0);
        };
        // Include a transparent first frame and a later changed World matrix:
        // direct opacity must use this frame's geometry, UV, and blend.
        for (UINT frame = 0; frame < 3; ++frame) {
            fixture.vertex[world] = {1,0,0,frame == 2 ? 0.1f : 0.0f};
            fixture.vertex[world+1] = {0,1,0,0};
            fixture.vertex[world+2] = {0,0,1,0};
            fixture.SetLive(frame == 2 ? 0.5f : 0.0f, frame ? 1.0f : 0.0f, 0);
            for (auto* target : opacityTargets)
                gpu.context->ClearRenderTargetView(target,zero);
            Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,
                Motion::CloudTechnique::kClouds,opacityFaceSize,opacityTargets,submitOverlap,&draw),
                "capture overlapping live opacity");
            const float expected = frame == 0 ? 0.0f : frame == 1 ? 0.4375f : 0.9375f;
            for (float value : gpu.Read(opacity.Get()))
                Require(std::abs(value-expected)<0.001f,
                    "all overlapping surfaces contribute exactly once, including VR stereo");
            for (float value : gpu.Read(primary.Get()))
                Require(value == 0.625f,"opacity capture leaves the visible target untouched");
        }
        // Native stock Sky uses back-face culling and CCW screen fronts.
        // Its view projection reverses winding relative to the cube bases.
        for (UINT winding = 0; winding < 2; ++winding) {
            D3D11_RASTERIZER_DESC nativeCull = noCull;
            nativeCull.CullMode = D3D11_CULL_BACK;
            nativeCull.FrontCounterClockwise = winding != 0;
            ComPtr<ID3D11RasterizerState> nativeCullState;
            Check(gpu.device->CreateRasterizerState(&nativeCull,&nativeCullState),
                "native sky culling convention");
            gpu.context->RSSetState(nativeCullState.Get());
            for (auto* target : opacityTargets)
                gpu.context->ClearRenderTargetView(target,zero);
            Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,
                Motion::CloudTechnique::kClouds,opacityFaceSize,opacityTargets,submitOverlap,&draw),
                "capture native front-facing overlapping cloud surfaces");
            for (float value : gpu.Read(opacity.Get()))
                Require(std::abs(value - (winding ? 0.9375f : 0.0f)) < 0.001f,
                    "native CCW cloud fronts survive cube projection; reversed fronts are culled");
            ComPtr<ID3D11RasterizerState> restoredRasterizer;
            gpu.context->RSGetState(&restoredRasterizer);
            Require(restoredRasterizer.Get() == nativeCullState.Get(),
                "opacity capture restores the native rasterizer");
        }
        gpu.context->RSSetState(noCullState.Get());
        // The alternate producer must rasterize directly into one 2D target,
        // retain all overlapping surfaces, and never submit the second VR eye.
        targetDesc.Width = targetDesc.Height = FO4CS::SunMaskProjection::kResolution;
        targetDesc.ArraySize = 1;
        targetDesc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> sunOpacity;
        ComPtr<ID3D11RenderTargetView> sunTarget;
        Check(gpu.device->CreateTexture2D(&targetDesc, nullptr, &sunOpacity), "sun opacity target");
        Check(gpu.device->CreateRenderTargetView(sunOpacity.Get(), nullptr, &sunTarget), "sun opacity RTV");
        FO4CS::SunMaskProjection sunProjection;
        for (const DirectX::XMFLOAT3 sun : { DirectX::XMFLOAT3{0,0,1}, DirectX::XMFLOAT3{0.6f,0,0.8f} }) {
            Require(FO4CS::SunMaskProjection::Build(sun, {0,0,0}, 10000, sunProjection), "sun basis");
            for (UINT frame = 0; frame < 3; ++frame) {
                fixture.vertex[world] = {1,0,0,0};
                fixture.SetLive(frame == 2 ? 0.5f : 0, frame ? 1.0f : 0, 0);
                gpu.context->ClearRenderTargetView(sunTarget.Get(), zero);
                const auto before = draw.calls;
                Require(Geometry::AccumulateSunOpacity(gpu.context.Get(), layout,
                    Motion::CloudTechnique::kClouds, sunTarget.Get(), sunProjection, submitOverlap, &draw),
                    "direct sun capture");
                Require(draw.calls == before + 1, "2D producer submits one draw, with no hidden cubemap");
                const auto values = gpu.Read(sunOpacity.Get());
                const float expected = frame == 0 ? 0.0f : frame == 1 ? 0.4375f : 0.9375f;
                for (UINT y = 248; y < 264; ++y)
                    for (UINT x = 248; x < 264; ++x)
                        Require(std::abs(values[y * 512 + x] - expected) < 0.001f,
                            "2D overlapping opacity, animated UV, fade and stereo multiplicity");
                for (float value : gpu.Read(primary.Get()))
                    Require(value == 0.625f, "2D capture leaves visible sky unchanged");
            }
        }
        // A textured edge across the roof gives the direct rasterizer an
        // independent spatial oracle. Check both a rotated native World and
        // a recentered sun map; constant-alpha domes cannot catch these errors.
        auto patterned = overlapping;
        for (UINT i = 0; i < patterned.size(); ++i)
            patterned[i].uv[0] = patterned[i].p[0] / (i < 36 ? 1.0f : 1.2f) * 0.5f + 0.5f;
        initial.pSysMem = patterned.data();
        ComPtr<ID3D11Buffer> patternedVB;
        Check(gpu.device->CreateBuffer(&bufferDesc, &initial, &patternedVB), "spatial cloud edge");
        rawVB = patternedVB.Get();
        gpu.context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);
        for (bool rotated : {false, true}) for (bool recentered : {false, true}) {
            fixture.vertex[world] = rotated ? Float4{0,-1,0,0} : Float4{1,0,0,0};
            fixture.vertex[world+1] = rotated ? Float4{1,0,0,0} : Float4{0,1,0,0};
            fixture.SetLive(0,1,0);
            Require(FO4CS::SunMaskProjection::Build({0,0,1},
                {recentered ? 5000.0f : 0.0f,0,0},10000,sunProjection), "spatial edge projection");
            gpu.context->ClearRenderTargetView(sunTarget.Get(),zero);
            Require(Geometry::AccumulateSunOpacity(gpu.context.Get(),layout,
                Motion::CloudTechnique::kClouds,sunTarget.Get(),sunProjection,submitOverlap,&draw),
                "spatial cloud raster");
            const auto values = gpu.Read(sunOpacity.Get());
            for (UINT y=248; y<264; ++y) for (UINT x=248; x<264; ++x) {
                // Sample the same world patch after shifting the map 32 texels.
                const UINT sampleX = recentered ? x-32 : x;
                const bool high = rotated ? y<256 : x>=256;
                Require(std::abs(values[y*512+sampleX]-(high ? 0.9375f : 0.4375f))<0.001f,
                    "native UV edge follows world rotation and stays fixed under map recentering");
            }
        }
        rawVB=overlapVB.Get();
        gpu.context->IASetVertexBuffers(0,1,&rawVB,&stride,&offset);
        fixture.vertex[world]={1,0,0,0}; fixture.vertex[world+1]={0,1,0,0};
        fixture.SetLive(0.5f,1,0);
        Require(FO4CS::SunMaskProjection::Build({0.6f,0,0.8f},{},10000,sunProjection), "restore oblique sun");
        for (UINT winding = 0; winding < 2; ++winding) {
            D3D11_RASTERIZER_DESC nativeCull = noCull;
            nativeCull.CullMode = D3D11_CULL_BACK;
            nativeCull.FrontCounterClockwise = winding != 0;
            ComPtr<ID3D11RasterizerState> state;
            Check(gpu.device->CreateRasterizerState(&nativeCull, &state), "2D native winding");
            gpu.context->RSSetState(state.Get());
            gpu.context->ClearRenderTargetView(sunTarget.Get(), zero);
            Require(Geometry::AccumulateSunOpacity(gpu.context.Get(), layout,
                Motion::CloudTechnique::kClouds, sunTarget.Get(), sunProjection, submitOverlap, &draw),
                "2D native culling");
            const auto values = gpu.Read(sunOpacity.Get());
            Require(std::abs(values[256 * 512 + 256] - (winding ? 0.9375f : 0.0f)) < 0.001f,
                "2D preserves native CCW fronts and culls reversed faces");
            ComPtr<ID3D11RasterizerState> restored;
            gpu.context->RSGetState(&restored);
            Require(restored == state, "2D restores rasterizer");
        }
        restoredCB.Reset();
        context1->VSGetConstantBuffers1(0,1,&restoredCB,&restoredFirst,&restoredCount);
        Require(restoredCB.Get() == sentinel && restoredFirst == first && restoredCount == count,
            "2D restores exact native constant buffer window");
        gpu.context->RSSetState(noCullState.Get());
        if (benchmark) {
            gpu.Compare("Capture layers=9 cube=256x256x6 sun=512x512 surfaces=2", [&] {
                for (auto* target : opacityTargets)
                    gpu.context->ClearRenderTargetView(target,zero);
                for (UINT layer = 0; layer < 9; ++layer)
                    Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,
                        Motion::CloudTechnique::kClouds,opacityFaceSize,opacityTargets,
                        submitOverlap,&draw), "benchmark live opacity");
            }, [&] {
                gpu.context->ClearRenderTargetView(sunTarget.Get(), zero);
                for (UINT layer = 0; layer < 9; ++layer)
                    Require(Geometry::AccumulateSunOpacity(gpu.context.Get(), layout,
                        Motion::CloudTechnique::kClouds, sunTarget.Get(), sunProjection,
                        submitOverlap, &draw), "benchmark direct sun opacity");
            },30);
        }
        if (!benchmark) {
            // Repeated native draws must produce the same pixels when private
            // captures are inserted between them. Merely checking that capture
            // leaves RT0 untouched cannot detect state leaked into the next draw.
            constexpr char visibleShader[] = R"(
cbuffer NativeVS : register(b0) { float4 Marker; };
Texture2D<float4> CloudTexture : register(t0);
SamplerState CloudSampler : register(s0);
struct Input { float3 p : POSITION0; float2 uv : TEXCOORD0; float4 c : COLOR0; };
struct Output { float4 p : SV_Position; float2 uv : TEXCOORD0; };
Output VS(Input i) {
    Output o; o.p=float4(i.p.xy*0.7+Marker.xy,0.5,1); o.uv=i.uv; return o;
}
float4 PS(Output i) : SV_Target {
    return float4(CloudTexture.Sample(CloudSampler,i.uv).a*0.5+i.uv.x*0.25,0,0,0.6);
}
)";
            const auto compileVisible = [&](const char* entry, const char* profile) {
                ComPtr<ID3DBlob> code, errors;
                Check(D3DCompile(visibleShader, sizeof(visibleShader)-1, nullptr, nullptr,
                    nullptr, entry, profile, D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors),
                    "compile visible sky roundtrip shader");
                return code;
            };
            auto visibleVSCode = compileVisible("VS", "vs_5_0");
            auto visiblePSCode = compileVisible("PS", "ps_5_0");
            ComPtr<ID3D11VertexShader> visibleVS;
            ComPtr<ID3D11PixelShader> visiblePS;
            Check(gpu.device->CreateVertexShader(visibleVSCode->GetBufferPointer(),
                visibleVSCode->GetBufferSize(), nullptr, &visibleVS), "visible sky roundtrip VS");
            Check(gpu.device->CreatePixelShader(visiblePSCode->GetBufferPointer(),
                visiblePSCode->GetBufferSize(), nullptr, &visiblePS), "visible sky roundtrip PS");
            std::array<Float4,32> marker{};
            marker[16] = {0.15f,-0.1f,0,0};
            auto markerCB = gpu.Constants(marker.data(), sizeof(marker));
            ID3D11Buffer* markerBinding = markerCB.Get();
            context1->VSSetConstantBuffers1(0,1,&markerBinding,&first,&count);
            gpu.context->VSSetShader(visibleVS.Get(),nullptr,0);
            gpu.context->PSSetShader(visiblePS.Get(),nullptr,0);
            gpu.context->OMSetRenderTargets(1,&rawTarget,nullptr);
            gpu.context->RSSetViewports(1,&viewport);
            D3D11_RASTERIZER_DESC visibleRasterizer = noCull;
            visibleRasterizer.ScissorEnable = TRUE;
            ComPtr<ID3D11RasterizerState> visibleRasterizerState;
            Check(gpu.device->CreateRasterizerState(&visibleRasterizer,&visibleRasterizerState),
                "visible sky scissor rasterizer");
            gpu.context->RSSetState(visibleRasterizerState.Get());
            const D3D11_RECT scissor{1,0,4,3};
            gpu.context->RSSetScissorRects(1,&scissor);
            D3D11_BLEND_DESC visibleBlend{};
            auto& blendTarget = visibleBlend.RenderTarget[0];
            blendTarget.BlendEnable = TRUE;
            blendTarget.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
            blendTarget.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
            blendTarget.BlendOp = D3D11_BLEND_OP_ADD;
            blendTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
            blendTarget.DestBlendAlpha = D3D11_BLEND_ZERO;
            blendTarget.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
            ComPtr<ID3D11BlendState> visibleBlendState;
            Check(gpu.device->CreateBlendState(&visibleBlend,&visibleBlendState),
                "visible sky non-default alpha blend");
            const float visibleFactor[]{0.35f,0.45f,0.55f,0.65f};
            gpu.context->OMSetBlendState(visibleBlendState.Get(),visibleFactor,UINT_MAX);
            D3D11_DEPTH_STENCIL_DESC visibleDepth{};
            visibleDepth.DepthEnable = FALSE;
            visibleDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            visibleDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
            ComPtr<ID3D11DepthStencilState> visibleDepthState;
            Check(gpu.device->CreateDepthStencilState(&visibleDepth,&visibleDepthState),
                "visible sky depth state");
            gpu.context->OMSetDepthStencilState(visibleDepthState.Get(),7);
            std::vector<float> reference;
            for (UINT frame=0; frame<27; ++frame) {
                gpu.context->ClearRenderTargetView(primaryView.Get(),clear);
                const auto technique = static_cast<Motion::CloudTechnique>(5u+(frame/3u)%3u);
                if (frame%3 == 1) {
                    for (auto* target : opacityTargets)
                        gpu.context->ClearRenderTargetView(target,zero);
                    Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,technique,
                        opacityFaceSize,opacityTargets,submitOverlap,&draw), "roundtrip cube capture");
                } else if (frame%3 == 2) {
                    gpu.context->ClearRenderTargetView(sunTarget.Get(),zero);
                    Require(Geometry::AccumulateSunOpacity(gpu.context.Get(),layout,technique,
                        sunTarget.Get(),sunProjection,submitOverlap,&draw), "roundtrip sun capture");
                }
                // Deliberately do not rebind native state here: Fallout can
                // elide those setters when its CPU-side cache has not changed.
                submitOverlap(&draw);
                const auto pixels = gpu.Read(primary.Get());
                if (frame == 0) {
                    reference = pixels;
                    Require(std::any_of(pixels.begin(),pixels.end(),[&](float v) { return v != clear[0]; }),
                        "visible sky roundtrip baseline contains actual geometry");
                    Require(std::any_of(pixels.begin(),pixels.end(),[&](float v) { return v == clear[0]; }),
                        "visible sky roundtrip baseline contains preserved background");
                } else {
                    Require(pixels == reference,
                        "visible sky is pixel-identical across OFF, cubemap and sun capture frames");
                }
            }
            std::cout << "Visible sky roundtrip passed: 27 alternating OFF/cube/sun frames, "
                << (vr ? "VR" : "flat") << ", cloud techniques 5/6/7\n";
        }
        const UINT callsBeforeReject = draw.calls;
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        Require(!Geometry::CaptureLayer(gpu.context.Get(), layout, 1,
            Motion::CloudTechnique::kClouds, 8, submit, &draw),
            "unsupported geometry remains fail neutral");
        Require(draw.calls == callsBeforeReject, "rejected capture never submits geometry");
        Geometry::ReleaseDeviceResources();
        gpu.context->ClearState();
        gpu.CheckMessages();
    }

    ComPtr<ID3DBlob> Compile(const std::filesystem::path& file,
        const char* entry, const char* profile, bool vr, bool sunMask = false)
    {
        const D3D_SHADER_MACRO macros[]{ { "FO4CS_SHADER_VR", vr ? "1" : "0" },
            { "FO4CS_SUN_MASK", sunMask ? "1" : "0" }, {} };
        ComPtr<ID3DBlob> blob, errors;
        const HRESULT result = D3DCompileFromFile(file.c_str(), macros, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, profile, D3DCOMPILE_ENABLE_STRICTNESS |
                D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &blob, &errors);
        if (FAILED(result) && errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        Check(result, "strict runtime shader compilation");
        return blob;
    }

#include "CloudComparisonRendererTests.inl"
#include "CloudCubePreviewTests.inl"

    void RunScreenTests(GPU& gpu, const std::filesystem::path& shaderDirectory, bool vr,
        UINT width = 8, UINT height = 8, bool benchmark = false)
    {
        gpu.context->ClearState();
        const std::array<const char*, 4> legacyShaders{
            "WorldCloudTileVS.hlsl", "WorldCloudTilePS.hlsl",
            "WorldCloudSkyVS.hlsl", "WorldCloudSkyPS.hlsl" };
        for (size_t i = 0; i < legacyShaders.size(); ++i)
            (void)Compile(shaderDirectory / legacyShaders[i], "main",
                i % 2 == 0 ? "vs_5_0" : "ps_5_0", vr);

        std::array<ComPtr<ID3D11ComputeShader>, 2> shaders;
        for (size_t i = 0; i < shaders.size(); ++i) {
            auto blob = Compile(shaderDirectory / "FO4CloudShadowScreenCS.hlsl",
                i == 0 ? "mainProduction" : "main", "cs_5_0", vr);
            Check(gpu.device->CreateComputeShader(blob->GetBufferPointer(),
                blob->GetBufferSize(), nullptr, &shaders[i]), "create mask shader");
            if (i == 0) {
                ComPtr<ID3D11ShaderReflection> reflection;
                Check(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(),
                    IID_PPV_ARGS(&reflection)), "reflect production shader");
                D3D11_SHADER_DESC desc{};
                Check(reflection->GetDesc(&desc), "production shader statistics");
                for (UINT binding = 0; binding < desc.BoundResources; ++binding) {
                    D3D11_SHADER_INPUT_BIND_DESC resource{};
                    Check(reflection->GetResourceBindingDesc(binding, &resource),
                        "production shader bindings");
                    Require(std::strcmp(resource.Name, "CloudTelemetry") != 0 &&
                        std::strcmp(resource.Name, "ReceiverValidity") != 0,
                        "production shader excludes diagnostic UAVs");
                }
                std::cout << (vr ? "VR" : "Flat") << " production instructions="
                    << desc.InstructionCount << '\n';
            }
        }

        std::vector<float> depths(static_cast<size_t>(width) * height, 0.5f);
        depths[0] = 1.0f;
        depths[1] = 0.005f;
        depths[2] = -0.1f;
        depths[3] = std::numeric_limits<float>::quiet_NaN();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = depths.data();
        initial.SysMemPitch = width * sizeof(float);
        ComPtr<ID3D11Texture2D> depth;
        Check(gpu.device->CreateTexture2D(&desc, &initial, &depth), "create test depth");
        ComPtr<ID3D11ShaderResourceView> depthView;
        Check(gpu.device->CreateShaderResourceView(depth.Get(), nullptr, &depthView), "depth view");
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> output;
        Check(gpu.device->CreateTexture2D(&desc, nullptr, &output), "create mask output");
        ComPtr<ID3D11UnorderedAccessView> outputView;
        Check(gpu.device->CreateUnorderedAccessView(output.Get(), nullptr, &outputView), "mask UAV");

        const UINT cloudSize=benchmark ? 256u : 8u;
        std::vector<float> cloud(cloudSize*cloudSize,0.25f);
        initial.pSysMem = cloud.data();
        initial.SysMemPitch = cloudSize * sizeof(float);
        std::array<D3D11_SUBRESOURCE_DATA, 6> faces;
        faces.fill(initial);
        desc.ArraySize = 6;
        desc.Width = desc.Height = cloudSize;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        ComPtr<ID3D11Texture2D> cube;
        Check(gpu.device->CreateTexture2D(&desc, faces.data(), &cube), "create test cloud cube");
        ComPtr<ID3D11ShaderResourceView> cubeView;
        Check(gpu.device->CreateShaderResourceView(cube.Get(), nullptr, &cubeView), "cube view");

        std::array<Float4, 48> plugin{};
        plugin[0][0] = plugin[1][1] = plugin[2][2] = plugin[3][3] = 1.0f;
        plugin[4] = { static_cast<float>(width), static_cast<float>(height),
            1.0f / width, 1.0f / height };
        plugin[8] = { 2.0f, 1.0f, 0, 0 };
        plugin[9] = { 1, -2, -2, 0 };
        plugin[10] = { benchmark ? 10000.0f : 35000.0f,
            benchmark ? FO4CS::SunMaskProjection::kPlanetRadius : 6371000.0f / 0.01428f, 1, 0 };
        plugin[26] = { 0, 0, 0, 1 };
        std::array<Float4, 64> frame{};
        for (UINT i = 0; i < 4; ++i)
            frame[12 + i][i] = 1.0f;
        for (UINT base = vr ? 32u : 20u; base < (vr ? 48u : 28u); base += 4)
            for (UINT i = 0; i < 4; ++i)
                frame[base + i][i] = 1.0f;
        std::array<Float4, 48> call{};
        call[0] = { 1.0f / width, 1.0f / height, 0, 0 };
        call[1] = { 0, 0, 1, 0 };
        call[2] = { 0, 0, 1, 0 };
        call[vr ? 45 : 27] = { 1, 1, 0, 0 };
        const Float4 stereo{ 1, 0, 0, 0 };
        auto b0 = gpu.Constants(plugin.data(), sizeof(plugin));
        auto b1 = gpu.Constants(frame.data(), sizeof(frame));
        auto b2 = gpu.Constants(call.data(), sizeof(call));
        auto b3 = gpu.Constants(stereo.data(), sizeof(stereo));
        auto sampler = gpu.Sampler();
        ID3D11Buffer* constants[]{ b0.Get(), b1.Get(), b2.Get(), b3.Get() };
        ID3D11ShaderResourceView* inputs[]{ depthView.Get(), cubeView.Get() };
        ID3D11SamplerState* samplers[]{ sampler.Get(), sampler.Get() };
        ID3D11UnorderedAccessView* target = outputView.Get();
        gpu.context->CSSetConstantBuffers(0, 4, constants);
        gpu.context->CSSetShaderResources(0, 2, inputs);
        gpu.context->CSSetSamplers(0, 2, samplers);

        if (benchmark) {
            gpu.context->CSSetShader(shaders[0].Get(), nullptr, 0);
            gpu.context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
            // Same output, depth, optics and work submission; only the captured
            // field's representation and corresponding lookup shader differ.
            desc.ArraySize=1; desc.MiscFlags=0; desc.Width=desc.Height=512;
            std::vector<float> sunTexels(512*512,0.25f);
            initial.pSysMem=sunTexels.data(); initial.SysMemPitch=512*sizeof(float);
            ComPtr<ID3D11Texture2D> sunTexture;
            ComPtr<ID3D11ShaderResourceView> sunView;
            Check(gpu.device->CreateTexture2D(&desc,&initial,&sunTexture), "benchmark sun map");
            Check(gpu.device->CreateShaderResourceView(sunTexture.Get(),nullptr,&sunView), "benchmark sun SRV");
            FO4CS::SunMaskProjection projection;
            Require(FO4CS::SunMaskProjection::Build({0,0,1},{},plugin[10][0],projection), "benchmark sun basis");
            std::memcpy(plugin.data()+44,&projection,sizeof(projection));
            gpu.context->UpdateSubresource(b0.Get(),0,nullptr,plugin.data(),0,0);
            auto sunCode=Compile(shaderDirectory / "FO4CloudShadowScreenCS.hlsl","mainProduction","cs_5_0",vr,true);
            ComPtr<ID3D11ComputeShader> sunShader;
            Check(gpu.device->CreateComputeShader(sunCode->GetBufferPointer(),sunCode->GetBufferSize(),nullptr,&sunShader),
                "benchmark sun shader");
            gpu.Compare("Screen " + std::to_string(width) + "x" + std::to_string(height), [&] {
                auto* source=cubeView.Get(); gpu.context->CSSetShaderResources(1,1,&source);
                gpu.context->CSSetShader(shaders[0].Get(),nullptr,0);
                gpu.context->Dispatch((width+7)/8,(height+7)/8,1);
            }, [&] {
                auto* source=sunView.Get(); gpu.context->CSSetShaderResources(1,1,&source);
                gpu.context->CSSetShader(sunShader.Get(),nullptr,0);
                gpu.context->Dispatch((width+7)/8,(height+7)/8,1);
            },60);
            gpu.context->ClearState();
            return;
        }

        const auto dispatch = [&](ID3D11ComputeShader* shader) {
            gpu.context->CSSetShader(shader, nullptr, 0);
            gpu.context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
            gpu.context->Dispatch(1, 1, 1);
            ID3D11UnorderedAccessView* nullTarget = nullptr;
            gpu.context->CSSetUnorderedAccessViews(0, 1, &nullTarget, nullptr);
            return gpu.Read(output.Get());
        };
        const auto production = dispatch(shaders[0].Get());
        const auto diagnostic = dispatch(shaders[1].Get());
        Require(production == diagnostic, "normal diagnostic output equals production");
        for (size_t i = 0; i < production.size(); ++i)
            Require(production[i] == (i == 0 || i == 2 || i == 3 ? 1.0f : 0.5f),
                "near/far and both stereo eyes preserve receiver/background contract");

        // Isolation belongs only to the right capture-analysis panel. A
        // remembered selector must never remove clouds from normal shadows.
        plugin[9] = { 1, 0.9f, 0.95f, 0 };
        plugin[7] = { 0, 0, -1, 0 };
        gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
        Require(dispatch(shaders[1].Get()) == production,
            "normal diagnostic shadows ignore capture selector");
        Require(dispatch(shaders[0].Get()) == production,
            "production shadows ignore capture selector");
        plugin[9][3] = 4;
        gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
        const auto outside = dispatch(shaders[1].Get());
        plugin[7] = { 0, 0, 1, 0 };
        gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
        const auto inside = dispatch(shaders[1].Get());
        for (size_t i = 4; i < inside.size(); ++i) {
            const bool selectedPanel = 3u * (i % width) >= 2u * width;
            Require(inside[i] == 0.25f &&
                outside[i] == (selectedPanel ? 0.0f : 0.25f),
                "only right capture panel applies cloud isolation");
        }

        call[1] = { 0, 0, -1, 0 };
        call[2] = { 0, 0, -1, 0 };
        gpu.context->UpdateSubresource(b2.Get(), 0, nullptr, call.data(), 0, 0);
        for (float value : dispatch(shaders[0].Get()))
            Require(value == 1.0f, "below-horizon sun is exactly neutral");

        // The visible disc and the engine shadow light are distinct inputs.
        // Cloud shadows follow the visible disc, including both VR eyes.
        plugin[43] = {0,0,1,1};
        gpu.context->UpdateSubresource(b0.Get(),0,nullptr,plugin.data(),0,0);
        Require(dispatch(shaders[0].Get()) == production,
            "visible sun overrides an offset engine shadow-light direction");
        call[1] = call[2] = {0,0,1,0};
        gpu.context->UpdateSubresource(b2.Get(),0,nullptr,call.data(),0,0);
        plugin[43][3] = -1;
        gpu.context->UpdateSubresource(b0.Get(),0,nullptr,plugin.data(),0,0);
        for (float value : dispatch(shaders[0].Get()))
            Require(value == 1.0f,"missing visible sun fails neutral despite a valid engine light");
        plugin[43] = {};
        gpu.context->UpdateSubresource(b0.Get(),0,nullptr,plugin.data(),0,0);

        // A finite cloud casts a fixed terrain pattern. Hold that cloud and
        // sun still while moving the camera: fixed world probes cannot change,
        // and a player who leaves the patch must become sunlit. A uniform cube
        // would hide the original camera-following defect.
        std::fill(cloud.begin(),cloud.end(),0.0f);
        for (UINT face = 0; face < 6; ++face)
            gpu.context->UpdateSubresource(cube.Get(), face, nullptr, cloud.data(),
                8 * sizeof(float), 0);
        std::fill(cloud.begin(),cloud.end(),0.75f);
        gpu.context->UpdateSubresource(cube.Get(), 4, nullptr, cloud.data(),
            8 * sizeof(float), 0);
        call[1] = call[2] = {0,0,1,0};
        plugin[9] = {1,-2,-2,0};
        plugin[10][0] = 10000;
        plugin[26] = {0,0,0,1}; // Confirmed gameplay anchor, not a loading view.
        gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
        gpu.context->UpdateSubresource(b2.Get(), 0, nullptr, call.data(), 0, 0);
        auto anchorCode = Compile(shaderDirectory.parent_path().parent_path() /
            "tests/CloudAnchorProbeCS.hlsl", "probe", "cs_5_0", vr);
        ComPtr<ID3D11ComputeShader> anchorProbe;
        Check(gpu.device->CreateComputeShader(anchorCode->GetBufferPointer(),
            anchorCode->GetBufferSize(), nullptr, &anchorProbe), "world anchor observer");
        const auto fixedTerrainBefore = dispatch(anchorProbe.Get());
        const auto [leastCoverage, mostCoverage] = std::minmax_element(
            fixedTerrainBefore.begin(), fixedTerrainBefore.end());
        Require(*mostCoverage - *leastCoverage > 0.5f,
            "world-anchor fixture must contain both cloud and clear terrain");
        for (const Float4 camera : {Float4{120000,-80000,4000,0},
                Float4{-95000,110000,-3000,0}}) {
            if (vr) {
                frame[59] = frame[60] = camera;
                frame[59][0] -= 2;
                frame[60][0] += 2;
            } else
                frame[35] = camera;
            gpu.context->UpdateSubresource(b1.Get(), 0, nullptr, frame.data(), 0, 0);
            Require(dispatch(anchorProbe.Get()) == fixedTerrainBefore,
                "fixed world terrain must keep identical cloud coverage when only the camera moves");
            const auto moved = dispatch(shaders[0].Get());
            Require(moved == dispatch(shaders[1].Get()),
                "production and diagnostic use the same fixed cloud origin");
            for (size_t i = 0; i < moved.size(); ++i)
                Require(moved[i] == 1.0f,
                    "a player travelling outside a stationary cloud patch becomes sunlit");
        }
        std::fill(cloud.begin(),cloud.end(),0.0f);
        gpu.context->UpdateSubresource(cube.Get(), 4, nullptr, cloud.data(),
            8 * sizeof(float), 0);
        for (float value : dispatch(shaders[0].Get()))
            Require(value == 1.0f, "clear sky remains exactly lit after camera travel");
        std::fill(cloud.begin(),cloud.end(),0.25f);
        for (UINT face = 0; face < 6; ++face)
            gpu.context->UpdateSubresource(cube.Get(), face, nullptr, cloud.data(),
                8 * sizeof(float), 0);
        frame[35] = frame[59] = frame[60] = {};
        if (vr)
            frame[35][3] = 1;  // Restore eye-zero inverse-reprojection row.
        gpu.context->UpdateSubresource(b1.Get(), 0, nullptr, frame.data(), 0, 0);

        if (!vr) {
            using namespace DirectX;
            // Encode receivers and sunlight as Fallout does: view-space sun,
            // split near/far projection, transposed inverse view, and c35
            // camera origin. Deliberately leave a DIFFERENT scene camera in
            // plugin b0; only the camera paired with the actual draw is valid.
            auto code = Compile(shaderDirectory.parent_path().parent_path() /
                "tests/CloudProjectionProbeCS.hlsl", "probe", "cs_5_0", false);
            ComPtr<ID3D11ComputeShader> probe;
            Check(gpu.device->CreateComputeShader(code->GetBufferPointer(),
                code->GetBufferSize(), nullptr, &probe), "projection observer");
            D3D11_TEXTURE2D_DESC probeDesc{};
            probeDesc.Width = 32;
            probeDesc.Height = 9;
            probeDesc.ArraySize = probeDesc.MipLevels = probeDesc.SampleDesc.Count = 1;
            probeDesc.Format = DXGI_FORMAT_R32_FLOAT;
            probeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> probeTexture;
            ComPtr<ID3D11UnorderedAccessView> probeTarget;
            Check(gpu.device->CreateTexture2D(&probeDesc, nullptr, &probeTexture),
                "projection observer texture");
            Check(gpu.device->CreateUnorderedAccessView(probeTexture.Get(), nullptr,
                &probeTarget), "projection observer target");
            const XMVECTOR origin = XMVectorSet(-24045, -29155, 16340, 0);
            const XMVECTOR sunWorld = XMVector3Normalize(XMVectorSet(0.8f, 0.15f, 0.5f, 0));
            const XMMATRIX farProjection = XMMatrixPerspectiveOffCenterLH(
                -6, 10, -7, 9, 8, 10000);
            const XMMATRIX nearProjection = XMMatrixPerspectiveOffCenterLH(
                -0.06f, 0.1f, -0.07f, 0.09f, 0.08f, 128);
            const auto writeMatrix = [&](UINT base, FXMMATRIX matrix) {
                XMFLOAT4X4 packed;
                XMStoreFloat4x4(&packed, XMMatrixTranspose(matrix));
                std::memcpy(frame.data() + base, &packed, sizeof(packed));
            };
            writeMatrix(20, XMMatrixInverse(nullptr, farProjection));
            writeMatrix(24, XMMatrixInverse(nullptr, nearProjection));
            frame[35] = {-24045, -29155, 16340, 0};
            plugin[0][3] = 70000;
            plugin[1][3] = -50000;
            plugin[2][3] = 100000;
            plugin[26] = {-24045, -29155, 16340, 1};
            plugin[9] = {1, -2, -2, 0};
            gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
            for (const Float4 angles : { Float4{0,0,0,0}, Float4{0.4f,1.2f,0.3f,0},
                    Float4{-0.7f,-1.8f,0.2f,0}, Float4{1.0f,2.6f,-0.4f,0} }) {
                const XMMATRIX inverseView = XMMatrixRotationRollPitchYaw(
                    angles[0], angles[1], angles[2]);
                const XMMATRIX view = XMMatrixInverse(nullptr, inverseView);
                writeMatrix(12, inverseView);
                XMFLOAT4 sunView;
                XMStoreFloat4(&sunView, XMVector4Transform(sunWorld, view));
                std::memcpy(call[1].data(), &sunView, sizeof(sunView));
                std::array<XMFLOAT3, 64> expected{};
                for (UINT y = 0; y < 8; ++y) {
                    for (UINT x = 0; x < 8; ++x) {
                        const bool nearReceiver = ((x + y) % 2) != 0;
                        const XMMATRIX projection = nearReceiver ? nearProjection : farProjection;
                        XMVECTOR clip = XMVector4Transform(
                            XMVectorSet(0, 0, nearReceiver ? 16.0f : 2048.0f, 1), projection);
                        const float nativeDepth = XMVectorGetZ(clip) / XMVectorGetW(clip);
                        depths[y * 8 + x] = nearReceiver ? nativeDepth * 0.01f :
                            (nativeDepth + 0.01f) / 1.01f;
                        const XMVECTOR ndc = XMVectorSet((x + 0.5f) * 0.25f - 1,
                            1 - (y + 0.5f) * 0.25f, nativeDepth, 1);
                        XMVECTOR reconstructed = XMVector4Transform(ndc,
                            XMMatrixInverse(nullptr, projection));
                        reconstructed = XMVectorDivide(reconstructed, XMVectorSplatW(reconstructed));
                        XMStoreFloat3(&expected[y * 8 + x], XMVectorAdd(origin,
                            XMVector4Transform(reconstructed, inverseView)));
                    }
                }
                gpu.context->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(),
                    8 * sizeof(float), 0);
                gpu.context->UpdateSubresource(b1.Get(), 0, nullptr, frame.data(), 0, 0);
                gpu.context->UpdateSubresource(b2.Get(), 0, nullptr, call.data(), 0, 0);
                gpu.context->CSSetShader(probe.Get(), nullptr, 0);
                ID3D11UnorderedAccessView* probeUav = probeTarget.Get();
                gpu.context->CSSetUnorderedAccessViews(3, 1, &probeUav, nullptr);
                gpu.context->Dispatch(1, 1, 1);
                probeUav = nullptr;
                gpu.context->CSSetUnorderedAccessViews(3, 1, &probeUav, nullptr);
                const auto observed = gpu.Read(probeTexture.Get());
                for (UINT pixel = 0; pixel < 64; ++pixel) {
                    const auto& receiver = expected[pixel];
                    Require(std::abs(observed[pixel * 4] - receiver.x) < 0.2f &&
                        std::abs(observed[pixel * 4 + 1] - receiver.y) < 0.2f &&
                        std::abs(observed[pixel * 4 + 2] - receiver.z) < 0.2f,
                        "near/far world reconstruction follows the bound camera");
                }
                XMFLOAT3 expectedSun;
                XMStoreFloat3(&expectedSun, sunWorld);
                Require(std::abs(observed[256] - expectedSun.x) < 1.0e-5f &&
                    std::abs(observed[257] - expectedSun.y) < 1.0e-5f &&
                    std::abs(observed[258] - expectedSun.z) < 1.0e-5f && observed[259] == 1,
                    "world sunlight is invariant to the bound camera rotation");
                for (float value : dispatch(shaders[0].Get()))
                    Require(value == 0.5f,
                        "cloud covering sunlight shadows receivers despite a stale scene camera");
            }
        }
        gpu.context->ClearState();
        gpu.CheckMessages();
    }

    void RunGodrayCloudTests(GPU& gpu, bool sunMask = false)
    {
        namespace Godray = FO4CS::GodrayCloudShader;
        gpu.context->ClearState();
        for (bool screen : { false, true }) {
            const auto source = Godray::PayloadSource(screen, sunMask);
            ComPtr<ID3DBlob> code, errors;
            const auto result = D3DCompile(source.data(), source.size(), nullptr,
                nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS |
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            Check(result, "compile production godray payload");
            ComPtr<ID3D11PixelShader> shader;
            Check(gpu.device->CreatePixelShader(code->GetBufferPointer(),
                code->GetBufferSize(), nullptr, &shader), "create production godray payload");
        }
        const auto source = std::string(sunMask ? "#define FO4CS_SUN_MASK 1\n" : "") + Godray::CommonSource() + R"hlsl(
RWTexture2D<float> Result : register(u0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID)
{
    Result[id.xy] = IntegratedFactor(PassData[14].xyz, PassData[15].xyz).x;
}
)hlsl";
        ComPtr<ID3DBlob> code, errors;
        const auto compiled = D3DCompile(source.data(), source.size(), nullptr,
            nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS |
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        Check(compiled, "compile shared godray lookup probe");
        ComPtr<ID3D11ComputeShader> shader;
        Check(gpu.device->CreateComputeShader(code->GetBufferPointer(),
            code->GetBufferSize(), nullptr, &shader), "create godray lookup probe");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = description.Height = 8;
        description.MipLevels = description.SampleDesc.Count = 1;
        description.ArraySize = sunMask ? 1 : 6;
        description.Format = DXGI_FORMAT_R32_FLOAT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = sunMask ? 0 : D3D11_RESOURCE_MISC_TEXTURECUBE;
        ComPtr<ID3D11Texture2D> cube;
        Check(gpu.device->CreateTexture2D(&description, nullptr, &cube), "create godray fixture cube");
        ComPtr<ID3D11ShaderResourceView> view;
        Check(gpu.device->CreateShaderResourceView(cube.Get(), nullptr, &view), "godray fixture view");
        description.Width = description.Height = description.ArraySize = 1;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> output;
        ComPtr<ID3D11UnorderedAccessView> outputView;
        Check(gpu.device->CreateTexture2D(&description, nullptr, &output), "godray fixture output");
        Check(gpu.device->CreateUnorderedAccessView(output.Get(), nullptr, &outputView), "godray fixture UAV");
        std::array<Float4, 20> pass{};
        pass[15] = { 0, 0, 100, 0 };
        std::array<Float4, 41> nativeVolume{};
        nativeVolume[38] = { -0.5f, 0, -1, 0 };
        auto passBuffer = gpu.Constants(pass.data(), sizeof(pass));
        auto nativeBuffer = gpu.Constants(nativeVolume.data(), sizeof(nativeVolume));
        Godray::Constants cloud{};
        if (sunMask)
            Require(FO4CS::SunMaskProjection::Build({0,0,1}, {}, 10000, cloud.sunProjection), "godray sun projection");
        cloud.geometryAndStrength = { 10000, 6371000.0f * 70.0f, 1, 1 };
        cloud.captureOriginAndBlend = { 0, 0, 0, 1 };
        cloud.expectedEyeAndTolerance = { 0, 0, 0, 16 };
        cloud.visibleSunDirectionAndValidity = { -0.5f, 0, 1, 1 };
        auto cloudBuffer = gpu.Constants(&cloud, sizeof(cloud));
        auto sampler = gpu.Sampler();
        const auto fill = [&](float opacity, bool edge = false) {
            for (UINT face = 0; face < (sunMask ? 1u : 6u); ++face) {
                std::array<float, 64> texels{};
                for (UINT y = 0; y < 8; ++y)
                    for (UINT x = 0; x < 8; ++x)
                        texels[y * 8 + x] = edge ? ((sunMask || face == 4) && x < 4 ? 1.0f : 0.0f) : opacity;
                gpu.context->UpdateSubresource(cube.Get(), face, nullptr,
                    texels.data(), 8 * sizeof(float), 0);
            }
        };
        const auto expect = [&](float expected, const char* message) {
            gpu.context->UpdateSubresource(cloudBuffer.Get(), 0, nullptr, &cloud, 0, 0);
            ID3D11Buffer* native[]{ passBuffer.Get(), nativeBuffer.Get() };
            ID3D11Buffer* cb = cloudBuffer.Get();
            ID3D11ShaderResourceView* srv = view.Get();
            ID3D11SamplerState* state = sampler.Get();
            ID3D11UnorderedAccessView* uav = outputView.Get();
            gpu.context->CSSetShader(shader.Get(), nullptr, 0);
            gpu.context->CSSetConstantBuffers(0, 2, native);
            gpu.context->CSSetConstantBuffers(13, 1, &cb);
            gpu.context->CSSetShaderResources(47, 1, &srv);
            gpu.context->CSSetSamplers(15, 1, &state);
            gpu.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            gpu.context->Dispatch(1, 1, 1);
            gpu.context->ClearState();
            const auto result = gpu.Read(output.Get()).front();
            Require(std::isfinite(result) && std::abs(result - expected) < 0.001f, message);
        };
        fill(0); expect(1, "clear sky leaves native godray intensity unchanged");
        fill(1); expect(0, "opaque cloud extinguishes directional sunlight along the view segment");
        fill(0.25f); expect(0.75f, "partial opacity retains the expected transmittance");
        fill(0, true); expect(0, "visible sun behind cloud is occluded despite a different native light direction");
        pass[14][0] = pass[15][0] = 20000;
        cloud.expectedEyeAndTolerance.x = 20000;
        gpu.context->UpdateSubresource(passBuffer.Get(), 0, nullptr, pass.data(), 0, 0);
        expect(1, "godray receivers leaving a fixed cloud patch become sunlit");
        pass[14][0] = pass[15][0] = 0;
        cloud.expectedEyeAndTolerance.x = 0;
        gpu.context->UpdateSubresource(passBuffer.Get(), 0, nullptr, pass.data(), 0, 0);
        expect(0, "returning to the same world cloud patch restores godray occlusion");
        cloud.visibleSunDirectionAndValidity.x = 0.5f;
        expect(1, "visible sun moving across a cloud edge brightens the godray lookup");
        fill(1);
        cloud.visibleSunDirectionAndValidity.w = -1;
        expect(1, "invalid visible sun is neutral");
        cloud.visibleSunDirectionAndValidity = { 0, 0, 0, 1 };
        expect(1, "zero sun direction is neutral");
        cloud.visibleSunDirectionAndValidity = { 0, 0, -1, 1 };
        expect(1, "below-horizon sun is neutral");
        cloud.visibleSunDirectionAndValidity = { std::numeric_limits<float>::quiet_NaN(), 0, 1, 1 };
        expect(1, "non-finite sun direction is neutral");
        cloud.visibleSunDirectionAndValidity = { 0, 0, 1, 1 };
        cloud.geometryAndStrength.w = 0;
        expect(1, "disabled cloud attenuation is neutral");
        cloud.geometryAndStrength.w = 1;
        cloud.expectedEyeAndTolerance.x = 1000;
        expect(1, "unattested eye coordinates leave native godrays unchanged");
        gpu.CheckMessages();
    }

    void RunBenchmarks(const std::filesystem::path& directory)
    {
        GPU gpu(true);
        std::cout << "Synthetic warm-cache timings; excludes native capture and game lighting.\n";
        RunGeometryCaptureTests(gpu, Motion::SkyConstantLayout::kFlat,0.5f,true);
        for (uint32_t layers : { 1u, 4u, 8u, 16u }) {
            MotionFixture fixture(gpu, Motion::SkyConstantLayout::kFlat);
            fixture.faceSize = 256;
            fixture.Capture(1, layers);
            const double milliseconds = gpu.Measure([&] {
                fixture.Begin(1);
                for (uint32_t id = 1; id <= layers; ++id)
                    fixture.Accumulate(id, Motion::CloudTechnique::kClouds);
                (void)fixture.Complete();
            }, 30);
            std::cout << "Motion 256x256x6 layers=" << layers << " GPU span ms="
                << milliseconds << '\n';
        }
        for (auto extent : { std::array<UINT, 2>{ 1920, 1080 },
                 std::array<UINT, 2>{ 2560, 1440 }, std::array<UINT, 2>{ 3840, 2160 } })
            RunScreenTests(gpu, directory, false, extent[0], extent[1], true);
    }
}

int main(int argc, char** argv)
{
    try {
        const bool hardware = argc == 3 && std::string(argv[2]) == "--hardware";
        Require(argc == 2 || (argc == 3 && (hardware || std::string(argv[2]) == "--benchmark")),
            "usage: CloudRendererTests <shader directory> [--benchmark|--hardware]");
        GPU gpu(hardware);
        RunMainViewCameraReadbackTests(gpu);
        RunMotionTests(gpu, Motion::SkyConstantLayout::kFlat);
        RunMotionTests(gpu, Motion::SkyConstantLayout::kVr);
        RunGeometryCaptureTests(gpu, Motion::SkyConstantLayout::kFlat);
        RunGeometryCaptureTests(gpu, Motion::SkyConstantLayout::kVr);
        RunGeometryCaptureTests(gpu, Motion::SkyConstantLayout::kFlat, 0.0f);
        RunGeometryCaptureTests(gpu, Motion::SkyConstantLayout::kVr, 0.0f);
        RunScreenTests(gpu, argv[1], false);
        RunScreenTests(gpu, argv[1], true);
        RunComparisonScreenTests(gpu, argv[1], false);
        RunComparisonScreenTests(gpu, argv[1], true);
        RunCubemapPreviewTests(gpu);
        RunGodrayCloudTests(gpu);
        RunGodrayCloudTests(gpu, true);
        if (argc == 3 && !hardware)
            RunBenchmarks(argv[1]);
        std::cout << "Cloud renderer GPU contracts passed (flat/VR motion and masks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
