// Included inside the renderer fixture namespace. Verify the real menu helper
// using synthetic native-format faces, including every pixel's orientation.
void RunCubemapPreviewTests(GPU& gpu)
{
    gpu.context->ClearState();
    constexpr UINT size = 8, mips = 4;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = mips; desc.ArraySize = 6; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    std::array<ComPtr<ID3D11Texture2D>, 2> cubes;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> views;
    const auto fill = [&](UINT cube, float offset) {
        for (UINT face = 0; face < 6; ++face) {
            for (UINT mip = 0; mip < mips; ++mip) {
                std::array<uint16_t, size * size> values{};
                const UINT width = size >> mip;
                for (UINT y = 0; y < width; ++y) for (UINT x = 0; x < width; ++x)
                    values[y * width + x] = DirectX::PackedVector::XMConvertFloatToHalf(mip == 0
                        ? offset + static_cast<float>(face * 64 + y * 8 + x) / 400.0f
                        : 1.0f); // Upper mips intentionally disagree with the source pixels.
                gpu.context->UpdateSubresource(cubes[cube].Get(), D3D11CalcSubresource(mip, face, mips),
                    nullptr, values.data(), width * sizeof(uint16_t), 0);
            }
        }
    };
    for (UINT i = 0; i < 2; ++i) {
        Check(gpu.device->CreateTexture2D(&desc, nullptr, &cubes[i]), "preview input cube");
        Check(gpu.device->CreateShaderResourceView(cubes[i].Get(), nullptr, &views[i]), "preview cube SRV");
        fill(i, i == 0 ? -0.1f : 0.2f);
    }

    desc.MiscFlags = 0; desc.ArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> originalOutput;
    ComPtr<ID3D11UnorderedAccessView> originalUav;
    ComPtr<ID3D11ShaderResourceView> invalidCube;
    Check(gpu.device->CreateTexture2D(&desc, nullptr, &originalOutput), "original output");
    Check(gpu.device->CreateUnorderedAccessView(originalOutput.Get(), nullptr, &originalUav), "original UAV");
    Check(gpu.device->CreateShaderResourceView(originalOutput.Get(), nullptr, &invalidCube), "non-cube input");
    constexpr char originalCode[] = "RWTexture2D<float4> Out : register(u0); "
        "[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { Out[id.xy] = 1; }";
    ComPtr<ID3DBlob> blob;
    Check(D3DCompile(originalCode, sizeof(originalCode) - 1, nullptr, nullptr, nullptr,
        "main", "cs_5_0", 0, 0, &blob, nullptr), "original CS code");
    ComPtr<ID3D11ComputeShader> originalShader;
    Check(gpu.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, &originalShader), "original CS");
    const Float4 constants{1,2,3,4};
    auto buffer = gpu.Constants(constants.data(), sizeof(constants));
    auto sampler = gpu.Sampler();
    const D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    ComPtr<ID3D11Predicate> predicate;
    Check(gpu.device->CreatePredicate(&predicateDesc, &predicate), "preview predicate");
    gpu.context->Begin(predicate.Get()); gpu.context->End(predicate.Get());
    gpu.context->SetPredication(predicate.Get(), TRUE);
    gpu.context->CSSetShader(originalShader.Get(), nullptr, 0);
    gpu.context->CSSetShaderResources(0, 1, views[1].GetAddressOf());
    gpu.context->CSSetUnorderedAccessViews(0, 1, originalUav.GetAddressOf(), nullptr);
    gpu.context->CSSetConstantBuffers(0, 1, buffer.GetAddressOf());
    gpu.context->CSSetSamplers(0, 1, sampler.GetAddressOf());
    const auto checkState = [&] {
        ComPtr<ID3D11ComputeShader> shader;
        ComPtr<ID3D11ShaderResourceView> source;
        ComPtr<ID3D11UnorderedAccessView> output;
        ComPtr<ID3D11Buffer> cb;
        ComPtr<ID3D11SamplerState> samp;
        ComPtr<ID3D11Predicate> pred;
        BOOL value{};
        gpu.context->CSGetShader(&shader, nullptr, nullptr);
        gpu.context->CSGetShaderResources(0, 1, &source);
        gpu.context->CSGetUnorderedAccessViews(0, 1, &output);
        gpu.context->CSGetConstantBuffers(0, 1, &cb);
        gpu.context->CSGetSamplers(0, 1, &samp);
        gpu.context->GetPredication(&pred, &value);
        Require(shader == originalShader && source == views[1] && output == originalUav &&
            cb == buffer && samp == sampler && pred == predicate && value == TRUE,
            "menu preview preserves native CS bindings and predication");
    };
    FO4CS::CloudCubePreview preview;
    ComPtr<ID3D11ShaderResourceView> firstAtlas;
    const auto verify = [&](UINT cube) {
        // Test readbacks are intentionally synchronous. The helper itself
        // performs only a GPU dispatch and never reads back the source.
        gpu.context->SetPredication(nullptr, FALSE);
        const auto source = gpu.Read(cubes[cube].Get());
        gpu.context->SetPredication(predicate.Get(), TRUE);
        auto* atlas = preview.Update(gpu.context.Get(), views[cube].Get());
        Require(atlas && preview.FaceSize() == size, "published cube produces a menu atlas");
        checkState();
        if (!firstAtlas) firstAtlas = atlas;
        Require(firstAtlas.Get() == atlas, "ping-pong input reuses the menu atlas");
        ComPtr<ID3D11Resource> image;
        atlas->GetResource(&image);
        gpu.context->SetPredication(nullptr, FALSE);
        const auto pixels = gpu.Read(image.Get());
        Require(source == gpu.Read(cubes[cube].Get()), "menu conversion never changes captured cloud pixels");
        gpu.context->SetPredication(predicate.Get(), TRUE);
        Require(pixels.size() == size * size * 6 * 4, "atlas contains exactly six RGBA faces");
        for (UINT face = 0; face < 6; ++face) for (UINT y = 0; y < size; ++y) for (UINT x = 0; x < size; ++x) {
            const auto input = source[face * size * size + y * size + x];
            const UINT index = (((face / 3) * size + y) * size * 3 + (face % 3) * size + x) * 4;
            Require(std::abs(pixels[index] - std::clamp(input, 0.0f, 1.0f)) <= 0.5f / 255.0f + 0.00001f &&
                pixels[index] == pixels[index + 1] && pixels[index] == pixels[index + 2] && pixels[index + 3] == 1,
                "atlas preserves mip-zero face order/orientation and raw opacity without contrast or gamma");
        }
    };
    verify(0); verify(1); verify(0);
    gpu.context->SetPredication(nullptr, FALSE);
    fill(0, 0.1f);
    gpu.context->SetPredication(predicate.Get(), TRUE);
    verify(0); // The same source object contains a new capture, not cached pixels.
    Require(!preview.Update(gpu.context.Get(), nullptr) &&
        !preview.Update(gpu.context.Get(), invalidCube.Get()) &&
        !preview.Update(nullptr, views[0].Get()), "missing/invalid input never exposes a stale atlas");
    checkState();
    preview.Reset(); firstAtlas.Reset();
    Require(preview.FaceSize() == 0, "device teardown releases the menu preview");
    verify(1); // Reinitializes after teardown.
    preview.Reset();
    gpu.context->ClearState();
    gpu.CheckMessages();
    std::cout << "Cubemap menu preview: six raw faces, live updates and native state restoration passed\n";
}
