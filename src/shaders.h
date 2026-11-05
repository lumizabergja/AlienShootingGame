#pragma once
inline constexpr char kShaders[]=R"HLSL(
cbuffer Params : register(b0) {uint Width,Height,Pitch,Current;uint Reset,OutWidth,OutHeight,Padding;};
ByteAddressBuffer Raw : register(t0);
Texture2D<float4> Color0 : register(t1);
Texture2D<float4> Color1 : register(t2);
Texture2D<int2> Flow : register(t3);
Texture2D<uint> Cost : register(t4);
SamplerState LinearClamp : register(s0);
struct Vertex {float4 position:SV_Position;float2 uv:TEXCOORD0;};
Vertex VS(uint id:SV_VertexID){Vertex v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 CurrentAt(int2 p){p=clamp(p,int2(0,0),int2(Width-1,Height-1));return Current==0?Color0.Load(int3(p,0)):Color1.Load(int3(p,0));}
float4 PSUnpack(Vertex v):SV_Target{uint2 p=min(uint2(v.position.xy),uint2(Width-1,Height-1));uint b=Raw.Load(p.y*Pitch+4*p.x);return float4((b>>16)&255,(b>>8)&255,b&255,255)/255.0;}
float4 PSPresent(Vertex v):SV_Target{return Current==0?Color0.SampleLevel(LinearClamp,v.uv,0):Color1.SampleLevel(LinearClamp,v.uv,0);}
// NvOFA estimates CURRENT -> PREVIOUS; S10.5 signed vectors are pixel displacements.
// Cost is inverse confidence. Never multiply vectors by confidence: that shrinks
// otherwise valid displacement and causes judder. Confidence selects/weights samples.
float2 PSMotion(Vertex v):SV_Target {
 if(Reset)return 0;
 int2 pixel=int2(v.position.xy);float2 q=(v.position.xy-2.0)/4.0;
 int2 base=int2(floor(q));float2 fracq=frac(q);int2 grid=int2((Width+3)/4,(Height+3)/4);
 float3 center=CurrentAt(pixel).rgb;float2 candidates[4];float weights[4];float total=0,best=-1;int chosen=0;
 [unroll]for(int i=0;i<4;++i){int2 off=int2(i&1,i>>1);int2 c=clamp(base+off,int2(0,0),grid-1);float2 flow=float2(Flow.Load(int3(c,0)))/32.0;
  float cost=float(Cost.Load(int3(c,0)));float conf=exp2(-cost/32.0);
  float3 local=CurrentAt(c*4+2).rgb;float edge=exp2(-20*dot(abs(local-center),float3(.299,.587,.114)));
  float2 f=lerp(1-fracq,fracq,float2(off));float weight=max(.001,f.x*f.y)*edge*conf;
  bool valid=all(abs(flow)<float2(Width,Height))&&cost<240;
  weight=valid?weight:0;candidates[i]=flow;weights[i]=weight;total+=weight;if(weight>best){best=weight;chosen=i;}
 }
 if(total<1e-6)return 0;
 float2 anchor=candidates[chosen],sum=0;float norm=0;
 [unroll]for(int j=0;j<4;++j){float coherent=dot(candidates[j]-anchor,candidates[j]-anchor)<=9?1:0;float w=weights[j]*coherent;sum+=candidates[j]*w;norm+=w;}
 return norm>0?sum/norm:anchor;
}
)HLSL";
