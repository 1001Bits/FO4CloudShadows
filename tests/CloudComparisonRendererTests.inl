// Included inside the renderer fixture namespace. These tests use analytic
// camera rays and a world-space cloud edge, not capture output as their oracle.
void RunComparisonScreenTests(GPU& gpu, const std::filesystem::path& directory, bool vr)
{
    gpu.context->ClearState();
    constexpr UINT width = 16, height = 8, cubeSize = 64;
    const auto compile = [&](const char* entry) {
        auto code = Compile(directory / "FO4CloudShadowScreenCS.hlsl", entry, "cs_5_0", vr);
        ComPtr<ID3D11ComputeShader> shader;
        Check(gpu.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
            nullptr, &shader), "comparison shader");
        return shader;
    };
    auto preview = compile("mainSkyPreview");
    std::array<Float4,44> plugin{};
    plugin[4] = {width, height, 1.0f/width, 1.0f/height};
    plugin[8] = {1,1,0,0}; plugin[9] = {1,-2,-2,2};
    plugin[10] = {10000, 6371000.0f * 70.0f, 1, 0};
    plugin[26] = {0,0,0,1}; plugin[43] = {0,0,1,1};
    std::array<Float4,64> frame{};
    std::array<Float4,48> call{};
    call[0] = {1.0f/width, 1.0f/height,1.0f/width,1.0f/height};
    call[vr ? 45 : 27] = {1,1,0,0};
    const Float4 stereo{1,0,0,0};
    auto b0 = gpu.Constants(plugin.data(), sizeof(plugin));
    auto b1 = gpu.Constants(frame.data(), sizeof(frame));
    auto b2 = gpu.Constants(call.data(), sizeof(call));
    auto b3 = gpu.Constants(stereo.data(), sizeof(stereo));
    auto sampler = gpu.Sampler();
    ID3D11Buffer* constants[]{b0.Get(), b1.Get(), b2.Get(), b3.Get()};
    ID3D11SamplerState* samplers[]{sampler.Get(), sampler.Get()};
    gpu.context->CSSetConstantBuffers(0,4,constants);
    gpu.context->CSSetSamplers(0,2,samplers);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> previewOutput, depth, cube;
    ComPtr<ID3D11UnorderedAccessView> previewUav;
    ComPtr<ID3D11ShaderResourceView> depthSrv, cubeSrv;
    Check(gpu.device->CreateTexture2D(&desc,nullptr,&previewOutput), "preview output");
    Check(gpu.device->CreateUnorderedAccessView(previewOutput.Get(),nullptr,&previewUav), "preview UAV");
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Check(gpu.device->CreateTexture2D(&desc,nullptr,&depth), "comparison depth");
    Check(gpu.device->CreateShaderResourceView(depth.Get(),nullptr,&depthSrv), "comparison depth SRV");
    std::array<float, width*height> depths{};
    depths.fill(1.0f); depths[1] = 0.5f;
    gpu.context->UpdateSubresource(depth.Get(),0,nullptr,depths.data(),width*sizeof(float),0);
    desc.Width = desc.Height = cubeSize; desc.ArraySize = 6;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    Check(gpu.device->CreateTexture2D(&desc,nullptr,&cube), "asymmetric directional cube");
    Check(gpu.device->CreateShaderResourceView(cube.Get(),nullptr,&cubeSrv), "directional cube SRV");
    const auto opacity = [](std::array<float,3> ray) {
        const float length = std::sqrt(ray[0]*ray[0]+ray[1]*ray[1]+ray[2]*ray[2]);
        return 0.5f + (0.18f*ray[0]+0.1f*ray[1]+0.07f*ray[2])/length;
    };
    for (UINT face=0; face<6; ++face) {
        std::array<float,cubeSize*cubeSize> texels{};
        for (UINT y=0; y<cubeSize; ++y) for (UINT x=0; x<cubeSize; ++x) {
            const float u=(x+0.5f)*2/cubeSize-1, v=(y+0.5f)*2/cubeSize-1;
            const std::array<std::array<float,3>,6> rays{{
                {1,-v,-u}, {-1,-v,u}, {u,1,v}, {u,-1,-v}, {u,-v,1}, {-u,-v,-1}}};
            texels[y*cubeSize+x]=opacity(rays[face]);
        }
        gpu.context->UpdateSubresource(cube.Get(),face,nullptr,texels.data(),cubeSize*sizeof(float),0);
    }
    const auto dispatch = [&](ID3D11ComputeShader* shader) {
        gpu.context->UpdateSubresource(b0.Get(),0,nullptr,plugin.data(),0,0);
        gpu.context->UpdateSubresource(b1.Get(),0,nullptr,frame.data(),0,0);
        ID3D11ShaderResourceView* inputs[]{depthSrv.Get(), cubeSrv.Get()};
        auto* output = previewUav.Get();
        gpu.context->CSSetShaderResources(0,2,inputs);
        gpu.context->CSSetShader(shader,nullptr,0);
        gpu.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        gpu.context->Dispatch(width/8,height/8,1);
        output = nullptr;
        gpu.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        return gpu.Read(previewOutput.Get());
    };
    const std::array<std::array<float,3>,6> forwards{{{0,0,1},{1,0,0},{0,1,0},{0,0,-1},{-1,0,0},{0,-1,0}}};
    const std::array<std::array<float,3>,6> rights{{{1,0,0},{0,0,-1},{1,0,0},{-1,0,0},{0,0,1},{1,0,0}}};
    for (UINT orientation=0; orientation<6; ++orientation) {
        const auto f=forwards[orientation], r=rights[orientation];
        const std::array<float,3> u{f[1]*r[2]-f[2]*r[1],f[2]*r[0]-f[0]*r[2],f[0]*r[1]-f[1]*r[0]};
        frame={};
        for (UINT axis=0; axis<3; ++axis) frame[12+axis]={r[axis],u[axis],f[axis],0};
        frame[15][3]=1;
        for (UINT eye=0; eye<(vr?2u:1u); ++eye) {
            const UINT base=vr ? 32+4*eye : 20;
            const float shift=vr ? (eye ? -0.11f : 0.07f) : 0.09f;
            frame[base]={1.1f,0,0,shift};
            frame[base+1]={0,0.8f,0,-0.03f};
            frame[base+2]={0,0,0,1}; frame[base+3]={0,0,-0.99f,1};
            if (vr) {
                // VR: c32+4e is the eye's inverse projection (above) and
                // c12+4e its ViewProj, chosen so their product is the
                // world->view rotation with rows r/u/f.
                const UINT m=12+4*eye;
                for (UINT axis=0; axis<3; ++axis) {
                    frame[m][axis]=(r[axis]-shift*f[axis])/1.1f;
                    frame[m+1][axis]=(u[axis]+0.03f*f[axis])/0.8f;
                    frame[m+2][axis]=f[axis]/0.99f;
                    frame[m+3][axis]=f[axis];
                }
                frame[m][3]=frame[m+1][3]=frame[m+3][3]=0;
                frame[m+2][3]=-1.0f/0.99f;
            }
        }
        const auto raw=dispatch(preview.Get());
        for (UINT y=0; y<height; ++y) for (UINT x=0; x<width; ++x) {
            const UINT eye=vr && x>=width/2 ? 1u : 0u;
            const float nx=vr ? ((x%(width/2)+0.5f)*4/width-1) : (x+0.5f)*2/width-1;
            const float ny=1-(y+0.5f)*2/height;
            const float px=nx*1.1f+(vr ? (eye ? -0.11f : 0.07f) : 0.09f), py=ny*0.8f-0.03f;
            std::array<float,3> ray{};
            for (UINT axis=0; axis<3; ++axis) ray[axis]=r[axis]*px+u[axis]*py+f[axis];
            const auto index=(y*width+x)*4;
            Require(std::abs(raw[index]-opacity(ray))<0.006f && raw[index]==raw[index+1] &&
                raw[index]==raw[index+2] && raw[index+3]==1,
                "raw cubemap agrees with analytic camera rays across six directions and asymmetric VR eyes");
        }
        frame[35]={123456,654321,321,0};
        if (vr) { // c35 belongs to the eye-0 projection, not the VR camera.
            frame[35]={0,0,-0.99f,1}; frame[59]=frame[60]={123456,654321,321,0};
        }
        plugin[26]={99999,-7777,4321,1}; plugin[43]={1,0,0,1}; plugin[10][0]=90000;
        Require(dispatch(preview.Get())==raw,
            "raw sky preview is independent of player translation, field anchor, height and sun");
        plugin[9][3]=1;
        const auto overlay=dispatch(preview.Get());
        for (UINT i=0; i<width*height; ++i)
            Require(std::abs(overlay[i*4+3]-(i==1 ? 0 : raw[i*4]*0.65f))<0.0001f,
                "overlay tints sky only and leaves foreground geometry visible");
        plugin[9][3]=2;
    }

    gpu.context->ClearState(); gpu.CheckMessages();
    std::cout << (vr ? "VR" : "Flat") << " sky alignment contracts passed\n";
}
