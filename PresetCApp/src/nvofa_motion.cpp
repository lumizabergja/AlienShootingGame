#include "nvofa_motion.h"
#include "diagnostics.h"
#include <sstream>

namespace {
const char *name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8";
    case DXGI_FORMAT_R16G16_SINT: return "R16G16_SINT";
    default: return "unsupported format";
    }
}
}

void NvOfMotion::require(NV_OF_STATUS result, const char *operation) {
    if (result != NV_OF_SUCCESS)
        throw std::runtime_error(std::string(operation) + " returned NV_OF_STATUS=" +
                                 std::to_string(int(result)));
}

DXGI_FORMAT NvOfMotion::selectFormat(NV_OF_BUFFER_USAGE usage,
                                     std::initializer_list<DXGI_FORMAT> desired) {
    uint32_t n = 0;
    require(api.nvOFGetSurfaceFormatCountD3D12(session, usage, NV_OF_MODE_OPTICALFLOW, &n),
            "NVOFA format count");
    if (n == 0 || n > 64) throw std::runtime_error("No NVOFA D3D12 formats");
    std::vector<DXGI_FORMAT> formats(n);
    require(api.nvOFGetSurfaceFormatD3D12(session, usage, NV_OF_MODE_OPTICALFLOW,
                                          formats.data()), "NVOFA formats");
    for (auto requested : desired)
        if (std::find(formats.begin(), formats.end(), requested) != formats.end())
            return requested;
    std::ostringstream s;
    s << "NVOFA requested format missing (usage=" << int(usage) << ", supported=";
    for (auto f : formats) s << unsigned(f) << ',';
    s << ')';
    throw std::runtime_error(s.str());
}

void NvOfMotion::registerResource(ID3D12Resource *res, NvOFGPUBufferHandle &h) {
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
    p.resource = res;
    p.hOFGpuBuffer = &h;
    p.inputFencePoint = {registered.Get(), registerValue};
    p.outputFencePoint = {registered.Get(), ++registerValue};
    require(api.nvOFRegisterResourceD3D12(session, &p), "NVOFA register D3D12 resource");
}

void NvOfMotion::initShader(ID3D12Device *d, MotionResolve &resolve) {
    const char *code = R"(
        Texture2D<int2> opticalFlow : register(t0);
        RWTexture2D<float2> motion : register(u0);
        cbuffer Shape : register(b0) { uint ofaWidth; uint ofaHeight; };
        float2 at(int2 p, int2 limit) {
            return float2(opticalFlow.Load(int3(clamp(p, int2(0,0), limit), 0)));
        }
        [numthreads(8, 8, 1)]
        void main(uint3 id : SV_DispatchThreadID) {
            uint w, h;
            motion.GetDimensions(w, h);
            if (id.x >= w || id.y >= h) return;
            uint gridW, gridH;
            opticalFlow.GetDimensions(gridW, gridH);
            int2 limit = int2(gridW, gridH) - 1;
            // NVOFA returns S10.5 motion in *reduced-input* pixel units.
            // Bilinear grid expansion prevents 10%-scale 4x4 blocks becoming
            // hard 40x40-pixel motion discontinuities on the full-resolution output.
            float2 p = (float2(id.xy) + 0.5f) *
                       (float2(ofaWidth, ofaHeight) / float2(w, h)) / 4.0f - 0.5f;
            int2 b = int2(floor(p));
            float2 t = frac(p);
            float2 a = lerp(at(b, limit), at(b + int2(1,0), limit), t.x);
            float2 c = lerp(at(b + int2(0,1), limit), at(b + int2(1,1), limit), t.x);
            float2 v = lerp(a, c, t.y);
            motion[id.xy] = v * (float2(w, h) / float2(ofaWidth, ofaHeight)) / 32.0f;
        }
    )";

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER roots[3]{};
    for (UINT i = 0; i < 2; ++i) {
        roots[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[i].DescriptorTable.NumDescriptorRanges = 1;
        roots[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        roots[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[2].Constants = {0, 0, 2};
    roots[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = roots;
    ComPtr<ID3DBlob> serialized, errors;
    auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &serialized, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? std::string((char *)errors->GetBufferPointer(),
                                      errors->GetBufferSize()) : "NVOFA root signature");
    check(d->CreateRootSignature(0, serialized->GetBufferPointer(),
                                 serialized->GetBufferSize(), IID_PPV_ARGS(&signature)),
          "NVOFA motion root signature");
    auto cs = compile(code, "main", "cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = signature.Get();
    ps.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    check(d->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)),
          "NVOFA 4x4 expand pipeline");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 * FrameCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)), "NVOFA descriptors");
    descriptorStep = d->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = outputFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(vectors[i].Get(), &srv, cpu);
        cpu.ptr += descriptorStep;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d->CreateUnorderedAccessView(resolve.motion[i].Get(), nullptr, &uav, cpu);
        cpu.ptr += descriptorStep;
        compute[i].init(d, D3D12_COMMAND_LIST_TYPE_DIRECT);
    }
}

void NvOfMotion::initScaler(ID3D12Device *d) {
    // Direct source-buffer -> tiny linear UAV -> NVOFA texture copy.
    // All operations stay in the same recorded GPU command list, with no CPU readback.
    const char *shader = R"(
        ByteAddressBuffer source : register(t0);
        RWByteAddressBuffer target : register(u0);
        cbuffer Shape : register(b0) {
            uint sourceWidth, sourceHeight, sourcePitch, targetWidth;
            uint targetHeight, targetPitch, swapRedBlue;
        };
        float4 pixel(int2 p) {
            p = clamp(p, int2(0,0), int2(sourceWidth,sourceHeight) - 1);
            uint c = source.Load(p.y * sourcePitch + p.x * 4);
            return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255);
        }
        [numthreads(8,8,1)]
        void main(uint3 id : SV_DispatchThreadID) {
            if (id.x >= targetWidth || id.y >= targetHeight) return;
            float2 p = (float2(id.xy) + .5f) *
                       (float2(sourceWidth,sourceHeight) / float2(targetWidth,targetHeight)) - .5f;
            int2 b = int2(floor(p));
            float2 t = frac(p);
            float4 a = lerp(pixel(b), pixel(b + int2(1,0)), t.x);
            float4 c = lerp(pixel(b + int2(0,1)), pixel(b + int2(1,1)), t.x);
            uint4 v = uint4(round(lerp(a, c, t.y)));
            if (swapRedBlue != 0) { uint r = v.x; v.x = v.z; v.z = r; }
            target.Store(id.y * targetPitch + id.x * 4,
                         v.x | (v.y << 8) | (v.z << 16) | (v.w << 24));
        }
    )";
    D3D12_ROOT_PARAMETER roots[3]{};
    roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    roots[0].Descriptor.ShaderRegister = 0;
    roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    roots[1].Descriptor.ShaderRegister = 0;
    roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[2].Constants = {0, 0, 7};
    roots[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = roots;
    ComPtr<ID3DBlob> serialized, errors;
    auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &serialized, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? std::string((char *)errors->GetBufferPointer(),
                                      errors->GetBufferSize()) : "NVOFA scaler root signature");
    check(d->CreateRootSignature(0, serialized->GetBufferPointer(),
                                 serialized->GetBufferSize(), IID_PPV_ARGS(&scaleSignature)),
          "NVOFA scaler root signature");
    auto cs = compile(shader, "main", "cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = scaleSignature.Get();
    ps.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    check(d->CreateComputePipelineState(&ps, IID_PPV_ARGS(&scalePipeline)),
          "NVOFA scaler pipeline");

    stagedPitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                  ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = UINT64(stagedPitch) * height;
    rd.Height = 1;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto &buffer : scaledBuffers)
        check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buffer)),
              "NVOFA scaler intermediate buffer");
}

void NvOfMotion::init(ID3D12Device *d, UINT w, UINT h, UINT percent, MotionResolve &resolve) {
    sourceWidth = w;
    sourceHeight = h;
    resolutionPercent = percent;
    module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::runtime_error("NVIDIA nvofapi64.dll was not available");
    auto create = reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(
        GetProcAddress(module, "NvOFAPICreateInstanceD3D12"));
    if (!create) throw std::runtime_error("NVOFA D3D12 export unavailable");
    require(create(NV_OF_API_VERSION, &api), "NVOFA API instance");
    require(api.nvCreateOpticalFlowD3D12(d, &session), "NVOFA device instance");

    auto minimum = [&](NV_OF_CAPS cap) -> UINT {
        uint32_t count = 0;
        require(api.nvOFGetCaps(session, cap, nullptr, &count), "NVOFA caps query");
        if (!count || count > 32) throw std::runtime_error("Invalid NVOFA capability count");
        std::vector<uint32_t> values(count);
        require(api.nvOFGetCaps(session, cap, values.data(), &count), "NVOFA caps values");
        return values[0];
    };
    auto align4 = [](UINT n) { return (n + 3u) & ~3u; };
    width = align4(std::max(minimum(NV_OF_CAPS_WIDTH_MIN), UINT((UINT64(w) * percent + 50) / 100)));
    height = align4(std::max(minimum(NV_OF_CAPS_HEIGHT_MIN), UINT((UINT64(h) * percent + 50) / 100)));
    if (width > w || height > h)
        throw std::runtime_error("NVOFA minimum input dimension exceeds captured dimensions");
    gridWidth = (width + 3) / 4;
    gridHeight = (height + 3) / 4;

    inputFormat = selectFormat(NV_OF_BUFFER_USAGE_INPUT,
                               {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM});
    outputFormat = selectFormat(NV_OF_BUFFER_USAGE_OUTPUT, {DXGI_FORMAT_R16G16_SINT});
    NV_OF_INIT_PARAMS p{};
    p.width = width;
    p.height = height;
    p.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    p.mode = NV_OF_MODE_OPTICALFLOW;
    p.perfLevel = NV_OF_PERF_LEVEL_FAST;
    p.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    p.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    require(api.nvOFInit(session, &p), "NVOFA 4x4 FAST init");

    check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copied)), "NVOFA copied fence");
    check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&processed)), "NVOFA complete fence");
    check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&registered)), "NVOFA register fence");
    for (UINT i = 0; i < 2; ++i) {
        images[i] = texture(d, width, height, inputFormat, D3D12_RESOURCE_FLAG_NONE);
        registerResource(images[i].Get(), imageHandles[i]);
    }
    for (UINT i = 0; i < FrameCount; ++i) {
        vectors[i] = texture(d, gridWidth, gridHeight, outputFormat,
                             D3D12_RESOURCE_FLAG_NONE);
        registerResource(vectors[i].Get(), vectorHandles[i]);
    }
    {
        Event e;
        waitFence(registered.Get(), registerValue, e.h);
    }
    initScaler(d);
    initShader(d, resolve);
    diagnostics::write(std::string("NVOFA ACTIVE: 4x4 FAST, requested_scale=") +
                       std::to_string(resolutionPercent) + "%, captured=" +
                       std::to_string(sourceWidth) + "x" + std::to_string(sourceHeight) +
                       ", actual_OFA=" + std::to_string(width) + "x" + std::to_string(height) +
                       ", input=" + name(inputFormat) +
                       " output=" + name(outputFormat) + " grid=" +
                       std::to_string(gridWidth) + "x" + std::to_string(gridHeight) +
                       " depth=flat motion=S10.5_to_pixels");
    diagnostics::write("NVOFA low-latency v2: redundant input-reuse GPU wait removed; "
                       "DLSS-G frame fence and OFA output fence still enforced");
    diagnostics::write("NVOFA downscaler: bilinear CUDA-buffer sampling; source BGRA channels "
                       "converted to the registered NVOFA texture format");
}

void NvOfMotion::upload(ID3D12GraphicsCommandList *l, ID3D12Resource *source,
                        UINT pitch, unsigned frame) {
    UINT index = frame & 1;
    auto *dst = images[index].Get();
    auto *staging = scaledBuffers[index].Get();
    l->SetComputeRootSignature(scaleSignature.Get());
    l->SetPipelineState(scalePipeline.Get());
    l->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(1, staging->GetGPUVirtualAddress());
    const UINT shape[] = {sourceWidth, sourceHeight, pitch, width, height, stagedPitch,
                          UINT(inputFormat == DXGI_FORMAT_R8G8B8A8_UNORM)};
    l->SetComputeRoot32BitConstants(2, 7, shape, 0);
    l->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    barrier(l, staging, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(l, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format = inputFormat;
    from.PlacedFootprint.Footprint.Width = width;
    from.PlacedFootprint.Footprint.Height = height;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = stagedPitch;
    l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    barrier(l, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    barrier(l, staging, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool NvOfMotion::execute(ID3D12CommandQueue *q, unsigned frame) {
    UINT index = frame & 1;
    const uint64_t ready = ++copyValue;
    check(q->Signal(copied.Get(), ready), "NVOFA new image fence");
    if (previous < 0) { previous = int(index); return false; }

    NV_OF_FENCE_POINT inputFence{copied.Get(), ready};
    NV_OF_FENCE_POINT outputFence{processed.Get(), ++processValue};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    in.inputFrame = imageHandles[index];
    in.referenceFrame = imageHandles[previous];
    in.numFencePoints = 1;
    in.fencePoint = &inputFence;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer = vectorHandles[frame % FrameCount];
    out.fencePoint = &outputFence;
    require(api.nvOFExecuteD3D12(session, &in, &out), "NVOFA execute");
    previous = int(index);
    check(q->Wait(processed.Get(), processValue), "NVOFA direct queue completion wait");
    return true;
}

void NvOfMotion::convertAndTag(ID3D12CommandQueue *q, unsigned context,
                               MotionResolve &resolve,
                               const std::function<void(ID3D12GraphicsCommandList *)> &tag) {
    auto i = context % FrameCount;
    auto &c = compute[i];
    c.begin();
    auto *l = c.l.Get();
    barrier(l, vectors[i].Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, resolve.motion[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(signature.Get());
    l->SetPipelineState(pipeline.Get());
    auto base = descriptors->GetGPUDescriptorHandleForHeapStart();
    auto srv = base; srv.ptr += UINT64(2 * i) * descriptorStep;
    auto uav = srv; uav.ptr += descriptorStep;
    l->SetComputeRootDescriptorTable(0, srv);
    l->SetComputeRootDescriptorTable(1, uav);
    const UINT shape[] = {width, height};
    l->SetComputeRoot32BitConstants(2, 2, shape, 0);
    const auto desc = resolve.motion[i]->GetDesc();
    l->Dispatch((UINT(desc.Width) + 7) / 8, (desc.Height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = resolve.motion[i].Get();
    l->ResourceBarrier(1, &uavBarrier);
    barrier(l, resolve.motion[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(l, vectors[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    tag(l);
    c.submit(q);
}

void NvOfMotion::tagWithoutFlow(ID3D12CommandQueue *q, unsigned context,
                                MotionResolve &resolve,
                                const std::function<void(ID3D12GraphicsCommandList *)> &tag) {
    const auto i = context % FrameCount;
    auto &c = compute[i];
    c.begin();
    auto *l = c.l.Get();
    auto *motion = resolve.motion[i].Get();
    barrier(l, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
    l->SetDescriptorHeaps(1, heaps);
    auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
    auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(2 * i + 1) * descriptorStep;
    cpu.ptr += SIZE_T(2 * i + 1) * descriptorStep;
    const float zero[4] = {};
    l->ClearUnorderedAccessViewFloat(gpu, cpu, motion, zero, 0, nullptr);
    barrier(l, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tag(c.l.Get());
    c.submit(q);
}

NvOfMotion::~NvOfMotion() {
    if (processed && processValue) {
        try { Event e; waitFence(processed.Get(), processValue, e.h); } catch (...) {}
    }
    if (registered && registerValue) {
        try { Event e; waitFence(registered.Get(), registerValue, e.h); } catch (...) {}
    }
    if (session && api.nvOFUnregisterResourceD3D12) {
        for (auto &h : imageHandles) if (h) {
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{h};
            api.nvOFUnregisterResourceD3D12(&p);
        }
        for (auto &h : vectorHandles) if (h) {
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{h};
            api.nvOFUnregisterResourceD3D12(&p);
        }
    }
    if (session && api.nvOFDestroy) api.nvOFDestroy(session);
    if (module) FreeLibrary(module);
}
