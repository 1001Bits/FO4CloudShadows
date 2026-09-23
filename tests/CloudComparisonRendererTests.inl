// Included inside the renderer fixture namespace. These tests use analytic
// camera rays and a world-space cloud edge, not capture output as their oracle.
void RunComparisonScreenTests(GPU& gpu, const std::filesystem::path& directory, bool vr)
{
    gpu.context->ClearState();
    constexpr UINT width = 16, height = 8, cubeSize = 64, mapSize = 512;
    const auto compile = [&](const char* entry, bool sun) {
        auto code = Compile(directory / "FO4CloudShadowScreenCS.hlsl", entry, "cs_5_0", vr, sun);
        ComPtr<ID3D11ComputeShader> shader;
        Check(gpu.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
            nullptr, &shader), "comparison shader");
        return shader;
    };
    auto preview = compile("mainSkyPreview", false);
    auto production = compile("mainProduction", true);
    auto diagnostic = compile("main", true);
    std::array<Float4,48> plugin{};
    plugin[4] = {width, height, 1.0f/width, 1.0f/height};
    plugin[8] = {1,1,0,0}; plugin[9] = {1,-2,-2,2};
    plugin[10] = {10000, FO4CS::SunMaskProjection::kPlanetRadius, 1, 0};
    plugin[26] = {0,0,0,1}; plugin[43] = {0,0,1,1};
    std::array<Float4,64> frame{};
    std::array<Float4,48> call{};
    call[0] = {1.0f/width, 1.0f/height,0,0};
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
    ComPtr<ID3D11Texture2D> previewOutput, maskOutput, depth, cube, map;
    ComPtr<ID3D11UnorderedAccessView> previewUav, maskUav;
    ComPtr<ID3D11ShaderResourceView> depthSrv, cubeSrv, mapSrv;
    Check(gpu.device->CreateTexture2D(&desc,nullptr,&previewOutput), "preview output");
    Check(gpu.device->CreateUnorderedAccessView(previewOutput.Get(),nullptr,&previewUav), "preview UAV");
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    Check(gpu.device->CreateTexture2D(&desc,nullptr,&maskOutput), "sun mask output");
    Check(gpu.device->CreateUnorderedAccessView(maskOutput.Get(),nullptr,&maskUav), "sun mask UAV");
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
    const auto dispatch = [&](ID3D11ComputeShader* shader, bool sky) {
        gpu.context->UpdateSubresource(b0.Get(),0,nullptr,plugin.data(),0,0);
        gpu.context->UpdateSubresource(b1.Get(),0,nullptr,frame.data(),0,0);
        ID3D11ShaderResourceView* inputs[]{depthSrv.Get(), sky ? cubeSrv.Get() : mapSrv.Get()};
        auto* output = sky ? previewUav.Get() : maskUav.Get();
        gpu.context->CSSetShaderResources(0,2,inputs);
        gpu.context->CSSetShader(shader,nullptr,0);
        gpu.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        gpu.context->Dispatch(width/8,height/8,1);
        output = nullptr;
        gpu.context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        return gpu.Read(sky ? previewOutput.Get() : maskOutput.Get());
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
            if (vr) for (UINT axis=0; axis<3; ++axis)
                frame[base+axis]={r[axis]*1.1f,u[axis]*0.8f,0,r[axis]*shift-u[axis]*0.03f+f[axis]};
        }
        const auto raw=dispatch(preview.Get(),true);
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
        Require(dispatch(preview.Get(),true)==raw,
            "raw sky preview is independent of player translation, field anchor, height and sun");
        plugin[9][3]=1;
        const auto overlay=dispatch(preview.Get(),true);
        for (UINT i=0; i<width*height; ++i)
            Require(std::abs(overlay[i*4+3]-(i==1 ? 0 : raw[i*4]*0.65f))<0.0001f,
                "overlay tints sky only and leaves foreground geometry visible");
        plugin[9][3]=2;
    }

    // A world-fixed edge at x=0 is encoded directly in the sun-plane map.
    desc.Width=desc.Height=mapSize; desc.ArraySize=1; desc.MiscFlags=0;
    Check(gpu.device->CreateTexture2D(&desc,nullptr,&map), "2D source");
    Check(gpu.device->CreateShaderResourceView(map.Get(),nullptr,&mapSrv), "2D source SRV");
    const auto fillMap = [&](float center) {
        std::vector<float> texels(mapSize*mapSize);
        for (UINT y=0; y<mapSize; ++y) for (UINT x=0; x<mapSize; ++x)
            texels[y*mapSize+x]=((x+0.5f)*80000/mapSize-40000+center)<0 ? 0.2f : 0.7f;
        gpu.context->UpdateSubresource(map.Get(),0,nullptr,texels.data(),mapSize*sizeof(float),0);
    };
    FO4CS::SunMaskProjection projection{};
    Require(!FO4CS::SunMaskProjection::Build({0,0,-1},{},10000,projection), "sun below horizon rejected");
    Require(!FO4CS::SunMaskProjection::Build({1.0e38f,0,1},{},10000,projection), "overflowing sun normalization rejected");
    Require(!FO4CS::SunMaskProjection::Build({0,0,1},{},1.0e38f,projection), "overflowing shell rejected");
    Require(FO4CS::SunMaskProjection::Build({0,0,1},{},10000,projection), "overhead sun map basis");
    std::memcpy(plugin.data()+44,&projection,sizeof(projection));
    plugin[9]={1,-2,-2,0}; plugin[10]={10000,FO4CS::SunMaskProjection::kPlanetRadius,1,0};
    plugin[26]={0,0,0,1}; plugin[43]={0,0,1,1};
    depths.fill(0.5f); depths[0]=1; depths[1]=0.005f;
    gpu.context->UpdateSubresource(depth.Get(),0,nullptr,depths.data(),width*sizeof(float),0);
    frame={};
    for (UINT i=0; i<4; ++i) frame[12+i][i]=1;
    for (UINT base=vr?32u:20u; base<(vr?48u:28u); base+=4) {
        frame[base]={20000,0,0,0}; frame[base+1]={0,20000,0,0};
        frame[base+2]={0,0,0,0}; frame[base+3]={0,0,0,1};
    }
    fillMap(0);
    const auto first=dispatch(production.Get(),false);
    Require(dispatch(diagnostic.Get(),false)==first, "2D diagnostic and production agree");
    for (UINT y=0; y<height; ++y) for (UINT x=0; x<width; ++x) {
        const UINT i=y*width+x, localX=vr ? x%(width/2) : x;
        const float expected=i==0 ? 1.0f : localX<(vr ? width/4 : width/2) ? 0.8f : 0.3f;
        Require(std::abs(first[i]-expected)<0.001f, "2D receiver samples the expected world cloud patch in both eyes");
    }
    // Keep terrain fixed while moving the camera by compensating its native
    // inverse projection translation, then recenter the finite capture window.
    if (vr) frame[59]=frame[60]={5000,0,0,0}; else frame[35]={5000,0,0,0};
    for (UINT base=vr?32u:20u; base<(vr?48u:28u); base+=4) frame[base][3]=-5000;
    Require(dispatch(production.Get(),false)==first, "camera movement does not drag the 2D shadow");
    Require(FO4CS::SunMaskProjection::Build({0,0,1},{5000,0,0},10000,projection), "recenter map");
    std::memcpy(plugin.data()+44,&projection,sizeof(projection)); fillMap(projection.centerAndValid.x);
    Require(dispatch(production.Get(),false)==first, "finite map recentering preserves world cloud positions");
    plugin[47][0]=100000;
    for (float value:dispatch(production.Get(),false)) Require(value==1, "outside finite map is neutral, never tiled");
    plugin[47][0]=0; plugin[47][3]=0;
    for (float value:dispatch(production.Get(),false)) Require(value==1, "invalid sun transform is neutral");
    gpu.context->ClearState(); gpu.CheckMessages();
    std::cout << (vr ? "VR" : "Flat") << " sky alignment and sun 2D projection contracts passed\n";
}
