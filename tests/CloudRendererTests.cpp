// GPU contract tests use generated textures/constants, never game assets.
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
namespace Motion = FO4CS::CloudGeometryCapture;
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

        template<class Work>
        void Benchmark(const std::string& name, Work work, UINT repetitions)
        {
            // Two warmup rounds, then nine measured rounds.
            std::array<double,9> values{};
            for (UINT round=0; round<11; ++round) {
                const double value = Measure(work,repetitions);
                if (round>=2) values[round-2]=value;
            }
            std::sort(values.begin(),values.end());
            std::cout << name << " GPU-span-ms median=" << values[4]
                << " min=" << values.front() << " max=" << values.back() << " batches=9\n";
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

    };

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
        struct Draw { ID3D11DeviceContext* context; UINT calls{}; bool vr; } draw{gpu.context.Get(),0,vr};
        ComPtr<ID3D11Buffer> restoredCB;
        UINT restoredFirst{}, restoredCount{};
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
            const auto values = gpu.Read(opacity.Get());
            const size_t faceTexels = static_cast<size_t>(opacityFaceSize) * opacityFaceSize;
            for (size_t i = 0; i < values.size(); ++i) {
                const bool belowHorizonFace = i / faceTexels == 5;
                Require(std::abs(values[i] - (belowHorizonFace ? 0.0f : expected)) < 0.001f,
                    "all overlapping surfaces contribute exactly once, including VR stereo; "
                    "the never-sampled -Z face is not replayed");
            }
            const UINT callsBefore = draw.calls;
            Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,
                Motion::CloudTechnique::kClouds,opacityFaceSize,opacityTargets,submitOverlap,&draw),
                "repeat capture for draw accounting");
            Require(draw.calls - callsBefore == Geometry::kCapturedCubeFaceCount,
                "one replay per captured face");
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
            const auto values = gpu.Read(opacity.Get());
            const size_t faceTexels = static_cast<size_t>(opacityFaceSize) * opacityFaceSize;
            for (size_t i = 0; i < values.size(); ++i) {
                const float expected = i / faceTexels == 5 ? 0.0f : (winding ? 0.9375f : 0.0f);
                Require(std::abs(values[i] - expected) < 0.001f,
                    "native CCW cloud fronts survive cube projection; reversed fronts are culled");
            }
            ComPtr<ID3D11RasterizerState> restoredRasterizer;
            gpu.context->RSGetState(&restoredRasterizer);
            Require(restoredRasterizer.Get() == nativeCullState.Get(),
                "opacity capture restores the native rasterizer");
        }
        gpu.context->RSSetState(noCullState.Get());
        restoredCB.Reset();
        context1->VSGetConstantBuffers1(0,1,&restoredCB,&restoredFirst,&restoredCount);
        Require(restoredCB.Get() == sentinel && restoredFirst == first && restoredCount == count,
            "opacity capture restores the exact native constant buffer window");
        if (benchmark) {
            gpu.Benchmark("Capture layers=9 cube=256x256x5 surfaces=2", [&] {
                for (auto* target : opacityTargets)
                    gpu.context->ClearRenderTargetView(target,zero);
                for (UINT layer = 0; layer < 9; ++layer)
                    Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,
                        Motion::CloudTechnique::kClouds,opacityFaceSize,opacityTargets,
                        submitOverlap,&draw), "benchmark live opacity");
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
                if (frame%3 != 0) {
                    for (auto* target : opacityTargets)
                        gpu.context->ClearRenderTargetView(target,zero);
                    Require(Geometry::AccumulateOpacity(gpu.context.Get(),layout,technique,
                        opacityFaceSize,opacityTargets,submitOverlap,&draw), "roundtrip cube capture");
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
                        "visible sky is pixel-identical across OFF and cubemap capture frames");
                }
            }
            std::cout << "Visible sky roundtrip passed: 27 alternating OFF/cube frames, "
                << (vr ? "VR" : "flat") << ", cloud techniques 5/6/7\n";
        }
        const UINT callsBeforeReject = draw.calls;
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        auto reason = Geometry::RejectReason::kNone;
        Require(!Geometry::AccumulateOpacity(gpu.context.Get(), layout,
            Motion::CloudTechnique::kClouds, opacityFaceSize, opacityTargets,
            submitOverlap, &draw, &reason) && reason == Geometry::RejectReason::kTopology,
            "unsupported geometry remains fail neutral and reports why");
        Require(draw.calls == callsBeforeReject, "rejected capture never submits geometry");
        Geometry::ReleaseDeviceResources();
        gpu.context->ClearState();
        gpu.CheckMessages();
    }

    ComPtr<ID3DBlob> Compile(const std::filesystem::path& file,
        const char* entry, const char* profile, bool vr)
    {
        const D3D_SHADER_MACRO macros[]{ { "FO4CS_SHADER_VR", vr ? "1" : "0" }, {} };
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

        std::array<Float4, 44> plugin{};
        plugin[0][0] = plugin[1][1] = plugin[2][2] = plugin[3][3] = 1.0f;
        plugin[4] = { static_cast<float>(width), static_cast<float>(height),
            1.0f / width, 1.0f / height };
        plugin[8] = { 2.0f, 1.0f, 0, 0 };
        plugin[9] = { 1, -2, -2, 0 };
        plugin[10] = { benchmark ? 10000.0f : 35000.0f,
            benchmark ? 6371000.0f * 70.0f : 6371000.0f / 0.01428f, 1, 0 };
        plugin[26] = { 0, 0, 0, 1 };
        std::array<Float4, 64> frame{};
        for (UINT i = 0; i < 4; ++i)
            frame[12 + i][i] = 1.0f;
        // VR: identity ViewProj for the second eye too (c16-c19), so both
        // eyes' view rotations are identity with the identity projections.
        if (vr)
            for (UINT i = 0; i < 4; ++i)
                frame[16 + i][i] = 1.0f;
        for (UINT base = vr ? 32u : 20u; base < (vr ? 48u : 28u); base += 4)
            for (UINT i = 0; i < 4; ++i)
                frame[base + i][i] = 1.0f;
        std::array<Float4, 48> call{};
        call[0] = { 1.0f / width, 1.0f / height, 1.0f / width, 1.0f / height };
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
            gpu.Benchmark("Screen " + std::to_string(width) + "x" + std::to_string(height), [&] {
                auto* source=cubeView.Get(); gpu.context->CSSetShaderResources(1,1,&source);
                gpu.context->CSSetShader(shaders[0].Get(),nullptr,0);
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

        if (!vr) {
            // 1.11.240 DFLight binds 25 per-call registers, so b2 c27 holds
            // another technique's leftovers there: the CPU layout flag, not
            // the register contents, decides whether it scales the depth fetch.
            call[27] = { 0.5f, 0.5f, 0, 0 };
            gpu.context->UpdateSubresource(b2.Get(), 0, nullptr, call.data(), 0, 0);
            Require(dispatch(shaders[0].Get()) == production &&
                dispatch(shaders[1].Get()) == production,
                "the 1.11.240 layout ignores leftover b2 c27 contents");
            plugin[5][0] = 1;
            gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
            Require(dispatch(shaders[0].Get()) != production,
                "the OG layout applies the dynamic-resolution depth scale");
            plugin[5][0] = 0;
            call[27] = { 1, 1, 0, 0 };
            gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
            gpu.context->UpdateSubresource(b2.Get(), 0, nullptr, call.data(), 0, 0);
        }

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

        // A finite cloud casts a terrain pattern. Hold that cloud and sun
        // still while moving the camera: fixed world probes cannot change
        // and a player who leaves the patch becomes sunlit, on flat and VR.
        // A uniform cube would hide camera-following projection regressions.
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
        const auto screenBefore = dispatch(shaders[0].Get());
        Require(std::any_of(screenBefore.begin(), screenBefore.end(),
                    [](float value) { return value < 1.0f; }),
            "the player starts under the cloud patch");
        for (const Float4 camera : {Float4{120000,-80000,4000,0},
                Float4{-95000,110000,-3000,0}}) {
            if (vr) {
                frame[59] = frame[60] = camera;
                frame[59][0] -= 2;
                frame[60][0] += 2;
            } else
                frame[35] = camera;
            gpu.context->UpdateSubresource(b1.Get(), 0, nullptr, frame.data(), 0, 0);
            const auto moved = dispatch(shaders[0].Get());
            Require(moved == dispatch(shaders[1].Get()),
                "production and diagnostic use the same cloud origin");
            Require(dispatch(anchorProbe.Get()) == fixedTerrainBefore,
                "fixed world terrain must keep identical cloud coverage when only the camera moves");
            for (size_t i = 0; i < moved.size(); ++i)
                Require(moved[i] == 1.0f,
                    "a player travelling outside a stationary cloud patch becomes sunlit");
        }
        if (vr) {
            // Travel re-anchoring blends two projections of the same live
            // field. The current origin is far behind the player (clear sky
            // above the player from there); the previous origin is the
            // player's own position, directly under the stationary patch.
            const Float4 camera{ 120000, -80000, 4000, 0 };
            if (vr) {
                frame[59] = frame[60] = camera;
                frame[59][0] -= 2;
                frame[60][0] += 2;
            } else
                frame[35] = camera;
            gpu.context->UpdateSubresource(b1.Get(), 0, nullptr, frame.data(), 0, 0);
            plugin[6] = { camera[0], camera[1], camera[2], 1 };
            for (const float weight : { 0.0f, 0.5f, 1.0f }) {
                plugin[5] = { 0, weight, 0, 0 };
                gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
                const auto blended = dispatch(shaders[0].Get());
                Require(blended == dispatch(shaders[1].Get()),
                    "production and diagnostic blend a re-anchor identically");
                // Coverage lerps 0 -> 0.75; opacity 2 doubles it before the clamp.
                const float expected = (std::max)(0.0f, 1.0f - 0.75f * weight * 2.0f);
                for (size_t i = 0; i < blended.size(); ++i) {
                    const bool receiver = i != 0 && i != 2 && i != 3;
                    Require(std::abs(blended[i] - (receiver ? expected : 1.0f)) < 1.0e-4f,
                        "re-anchor crossfade weights the previous origin exactly");
                }
            }
            plugin[5] = {};
            plugin[6] = {};
            gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
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

            // Keep a terrain point fixed, move/roll the camera, and forward
            // project that point into a known depth texel. AE upscalers keep
            // the depth allocation at output size while reducing the rendered
            // viewport: b2 c0.xy addresses depth, c0.zw maps pixels to NDC.
            // OG deliberately uses c0.xy for NDC even when c0.zw differs.
            // Forward projection supplies the oracle, not our reconstruction.
            for (const bool legacy : {true, false}) {
                for (const UINT renderExtent : {8u, 6u, 4u}) {
                    if (legacy && renderExtent != 8u) continue;
                    plugin[5][0] = legacy ? 1.0f : 0.0f;
                    call[0] = {1.0f / 8, 1.0f / 8,
                        legacy ? 1.0f / 6 : 1.0f / renderExtent,
                        legacy ? 1.0f / 6 : 1.0f / renderExtent};
                    call[27] = legacy ? Float4{1,1,0,0} : Float4{0.25f,0.25f,0,0};
                    plugin[43] = {0,0,1,1}; // Fixed world sun, independent of the view.
                    gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
                    for (const bool nearReceiver : {false, true}) {
                        const XMVECTOR terrain = XMVectorAdd(origin, nearReceiver ?
                            XMVectorSet(0.5f, -0.25f, 16, 0) :
                            XMVectorSet(128, -64, 2048, 0));
                        XMFLOAT3 expectedTerrain;
                        XMStoreFloat3(&expectedTerrain, terrain);
                        UINT pose = 0;
                        for (const Float4 angles : {Float4{0,0,0,0},
                                Float4{0.12f,0.25f,-0.18f,1},
                                Float4{-0.08f,-0.2f,0.15f,-1}, Float4{0,0,0,0}}) {
                            const float motion = nearReceiver ? 0.5f : 32.0f;
                            const XMVECTOR camera = XMVectorAdd(origin,
                                XMVectorSet(angles[3] * motion, -angles[3] * motion * 0.5f,
                                    angles[3] * motion * 0.25f, 0));
                            XMFLOAT4 cameraPosition;
                            XMStoreFloat4(&cameraPosition, camera);
                            std::memcpy(frame[35].data(), &cameraPosition, sizeof(cameraPosition));
                            const XMMATRIX inverseView = XMMatrixRotationRollPitchYaw(
                                angles[0], angles[1], angles[2]);
                            writeMatrix(12, inverseView);
                            const XMVECTOR viewPoint = XMVector4Transform(
                                XMVectorSetW(XMVectorSubtract(terrain, camera), 1),
                                XMMatrixInverse(nullptr, inverseView));
                            XMMATRIX projection = nearReceiver ? nearProjection : farProjection;
                            const UINT x = 1 + (pose++ % 2), y = 1;
                            const XMVECTOR ndc = XMVectorSet(
                                (x + 0.5f) * 2 / renderExtent - 1,
                                1 - (y + 0.5f) * 2 / renderExtent, 0, 0);
                            // Off-centre projection puts this same world point
                            // on an exact pixel centre, avoiding sample rounding.
                            const XMVECTOR firstClip = XMVector4Transform(viewPoint, projection);
                            projection.r[2] = XMVectorAdd(projection.r[2], XMVectorSelect(
                                XMVectorZero(), XMVectorSubtract(ndc,
                                    XMVectorDivide(firstClip, XMVectorSplatW(firstClip))),
                                XMVectorSelectControl(1,1,0,0)));
                            const XMVECTOR clip = XMVector4Transform(viewPoint, projection);
                            const float nativeDepth = XMVectorGetZ(clip) / XMVectorGetW(clip);
                            writeMatrix(nearReceiver ? 24 : 20, XMMatrixInverse(nullptr, projection));
                            std::fill(depths.begin(), depths.end(), 1.0f);
                            depths[y * 8 + x] = nearReceiver ? nativeDepth * 0.01f :
                                (nativeDepth + 0.01f) / 1.01f;
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
                            const UINT index = (y * 8 + x) * 4;
                            Require(std::abs(observed[index] - expectedTerrain.x) < 0.2f &&
                                std::abs(observed[index + 1] - expectedTerrain.y) < 0.2f &&
                                std::abs(observed[index + 2] - expectedTerrain.z) < 0.2f &&
                                std::abs(observed[index + 3] - depths[y * 8 + x]) < 1.0e-6f,
                                "fixed terrain stays fixed during camera translation/roll at native and reduced AE resolution");
                        }
                    }
                }
            }
        }
        if (vr) {
            using namespace DirectX;
            // VR DFLight lights in view space: c12+4e is each eye's ViewProj
            // (eye-relative world -> clip) and c32+4e its inverse projection
            // (layout confirmed by a headset readback, 24 Sep 2026). Receivers
            // and the engine sun must come back in world space for any head
            // rotation; view-space receivers made shadows turn with the head.
            auto code = Compile(shaderDirectory.parent_path().parent_path() /
                "tests/CloudProjectionProbeCS.hlsl", "probe", "cs_5_0", true);
            ComPtr<ID3D11ComputeShader> probe;
            Check(gpu.device->CreateComputeShader(code->GetBufferPointer(),
                code->GetBufferSize(), nullptr, &probe), "VR projection observer");
            D3D11_TEXTURE2D_DESC probeDesc{};
            probeDesc.Width = 32;
            probeDesc.Height = 9;
            probeDesc.ArraySize = probeDesc.MipLevels = probeDesc.SampleDesc.Count = 1;
            probeDesc.Format = DXGI_FORMAT_R32_FLOAT;
            probeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> probeTexture;
            ComPtr<ID3D11UnorderedAccessView> probeTarget;
            Check(gpu.device->CreateTexture2D(&probeDesc, nullptr, &probeTexture),
                "VR projection observer texture");
            Check(gpu.device->CreateUnorderedAccessView(probeTexture.Get(), nullptr,
                &probeTarget), "VR projection observer target");
            const auto writeMatrix = [&](UINT base, FXMMATRIX matrix) {
                XMFLOAT4X4 packed;
                XMStoreFloat4x4(&packed, XMMatrixTranspose(matrix));
                std::memcpy(frame.data() + base, &packed, sizeof(packed));
            };
            const std::array<XMVECTOR, 2> eyes{
                XMVectorSet(-24048, -29155, 16340, 0), XMVectorSet(-24042, -29155, 16340, 0) };
            const std::array<XMMATRIX, 2> projections{
                XMMatrixPerspectiveOffCenterLH(-7, 5, -7, 9, 5, 100000),
                XMMatrixPerspectiveOffCenterLH(-5, 7, -7, 9, 5, 100000) };
            const XMVECTOR sunWorld = XMVector3Normalize(XMVectorSet(0.8f, 0.15f, 0.5f, 0));
            plugin[43] = {}; // Use the engine's per-eye (view-space) sun.
            gpu.context->UpdateSubresource(b0.Get(), 0, nullptr, plugin.data(), 0, 0);
            for (const Float4 angles : { Float4{0,0,0,0}, Float4{0.4f,1.2f,0.3f,0},
                    Float4{-0.7f,-1.8f,0.2f,0}, Float4{1.0f,2.6f,-0.4f,0} }) {
                const XMMATRIX inverseView = XMMatrixRotationRollPitchYaw(
                    angles[0], angles[1], angles[2]);
                const XMMATRIX view = XMMatrixInverse(nullptr, inverseView);
                for (UINT eye = 0; eye < 2; ++eye) {
                    writeMatrix(12 + 4 * eye, view * projections[eye]);
                    writeMatrix(32 + 4 * eye, XMMatrixInverse(nullptr, projections[eye]));
                    writeMatrix(40 + 4 * eye, XMMatrixInverse(nullptr, projections[eye]));
                    XMFLOAT4 eyePosition;
                    XMStoreFloat4(&eyePosition, eyes[eye]);
                    frame[59 + eye] = { eyePosition.x, eyePosition.y, eyePosition.z, 0 };
                    XMFLOAT4 sunView;
                    XMStoreFloat4(&sunView, XMVector3TransformNormal(sunWorld, view));
                    call[1 + eye] = { sunView.x, sunView.y, sunView.z, 0 };
                }
                std::array<XMFLOAT3, 64> expected{};
                for (UINT y = 0; y < 8; ++y) {
                    for (UINT x = 0; x < 8; ++x) {
                        const UINT eye = x >= 4 ? 1u : 0u;
                        const XMMATRIX& projection = projections[eye];
                        const XMVECTOR clip = XMVector4Transform(
                            XMVectorSet(0, 0, 2048.0f + 64.0f * x, 1), projection);
                        const float nativeDepth = XMVectorGetZ(clip) / XMVectorGetW(clip);
                        depths[y * 8 + x] = (nativeDepth + 0.01f) / 1.01f;
                        float ndcX = (x + 0.5f) * 0.25f - 1;
                        ndcX = (ndcX + (eye ? -0.5f : 0.5f)) * 2.0f;
                        const XMVECTOR ndc = XMVectorSet(ndcX, 1 - (y + 0.5f) * 0.25f, nativeDepth, 1);
                        XMVECTOR reconstructed = XMVector4Transform(ndc,
                            XMMatrixInverse(nullptr, projection));
                        reconstructed = XMVectorDivide(reconstructed, XMVectorSplatW(reconstructed));
                        XMStoreFloat3(&expected[y * 8 + x], XMVectorAdd(eyes[eye],
                            XMVector3TransformNormal(reconstructed, inverseView)));
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
                    const size_t index = static_cast<size_t>(pixel / 8) * 32 + (pixel % 8) * 4;
                    Require(std::abs(observed[index] - receiver.x) < 0.5f &&
                        std::abs(observed[index + 1] - receiver.y) < 0.5f &&
                        std::abs(observed[index + 2] - receiver.z) < 0.5f,
                        "VR receivers are world positions for every head rotation, in both eyes");
                }
                XMFLOAT3 expectedSun;
                XMStoreFloat3(&expectedSun, sunWorld);
                Require(std::abs(observed[256] - expectedSun.x) < 1.0e-4f &&
                    std::abs(observed[257] - expectedSun.y) < 1.0e-4f &&
                    std::abs(observed[258] - expectedSun.z) < 1.0e-4f && observed[259] == 1,
                    "the VR engine sun is rotated from view space to world space");
            }
        }
        gpu.context->ClearState();
        gpu.CheckMessages();
    }

    void RunGodrayCloudTests(GPU& gpu)
    {
        namespace Godray = FO4CS::GodrayCloudShader;
        gpu.context->ClearState();
        for (bool screen : { false, true }) {
            const auto source = Godray::PayloadSource(screen);
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
        const auto source = std::string(Godray::CommonSource()) + R"hlsl(
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
        description.ArraySize = 6;
        description.Format = DXGI_FORMAT_R32_FLOAT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
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
        cloud.geometryAndStrength = { 10000, 6371000.0f * 70.0f, 1, 1 };
        cloud.captureOriginAndBlend = { 0, 0, 0, 1 };
        cloud.expectedEyeAndTolerance = { 0, 0, 0, 16 };
        cloud.visibleSunDirectionAndValidity = { -0.5f, 0, 1, 1 };
        auto cloudBuffer = gpu.Constants(&cloud, sizeof(cloud));
        auto sampler = gpu.Sampler();
        const auto fill = [&](float opacity, bool edge = false) {
            for (UINT face = 0; face < 6u; ++face) {
                std::array<float, 64> texels{};
                for (UINT y = 0; y < 8; ++y)
                    for (UINT x = 0; x < 8; ++x)
                        texels[y * 8 + x] = edge ? (face == 4 && x < 4 ? 1.0f : 0.0f) : opacity;
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
        if (argc == 3 && !hardware)
            RunBenchmarks(argv[1]);
        std::cout << "Cloud renderer GPU contracts passed (flat/VR motion and masks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
