#pragma once
#include "common.h"
// Raw BGRA capture is consumed directly, without an intermediate color texture.
class RawColor {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
public:
    void init(ID3D12Device *device) {
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants={0,0,4}; rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=rp;
        ComPtr<ID3DBlob> blob,err;
        check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err),"Raw root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Raw root");
        const char *shader=R"(
ByteAddressBuffer src:register(t0);
cbuffer Shape:register(b0){uint w;uint h;uint pitch;uint nativeSize;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V vs(uint i:SV_VertexID){V o;o.uv=float2((i<<1)&2,i&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float4 pixel(int2 p){p=clamp(p,int2(0,0),int2(w-1,h-1));uint c=src.Load(p.y*pitch+p.x*4);return float4((c>>16)&255,(c>>8)&255,c&255,255)/255.0;}
float4 ps(V i):SV_Target{
 if(nativeSize)return pixel(int2(i.p.xy));
 float2 p=i.uv*float2(w,h)-.5;int2 b=int2(floor(p));float2 t=frac(p);
 return lerp(lerp(pixel(b),pixel(b+int2(1,0)),t.x),lerp(pixel(b+int2(0,1)),pixel(b+1),t.x),t.y);
})";
        auto vs=compile(shader,"vs","vs_5_0"),ps=compile(shader,"ps","ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto &blend = pd.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.FrontFace.StencilFunc = pd.DepthStencilState.BackFace.StencilFunc =
        D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.FrontFace.StencilFailOp = pd.DepthStencilState.FrontFace.StencilDepthFailOp =
        pd.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = 1;
    check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "Scale pipeline");
    }
    void drawRegion(ID3D12GraphicsCommandList *l, ID3D12Resource *buffer,
                    D3D12_CPU_DESCRIPTOR_HANDLE target, UINT w, UINT h, UINT pitch,
                    UINT dw, UINT dh, LONG left, LONG top, LONG right, LONG bottom) {
        l->SetPipelineState(pso.Get());l->SetGraphicsRootSignature(root.Get());
        l->SetGraphicsRootShaderResourceView(0,buffer->GetGPUVirtualAddress());
        UINT shape[]={w,h,pitch,UINT(w==dw && h==dh)};l->SetGraphicsRoot32BitConstants(1,4,shape,0);
        D3D12_VIEWPORT vp{0,0,float(dw),float(dh),0,1};
        D3D12_RECT rect{left,top,right,bottom};
        l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&rect);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->OMSetRenderTargets(1,&target,FALSE,nullptr);l->DrawInstanced(3,1,0,0);
    }
    void draw(ID3D12GraphicsCommandList *l, ID3D12Resource *buffer,
              D3D12_CPU_DESCRIPTOR_HANDLE target, UINT w, UINT h, UINT pitch, UINT dw, UINT dh) {
        drawRegion(l, buffer, target, w, h, pitch, dw, dh, 0, 0, LONG(dw), LONG(dh));
    }
};
