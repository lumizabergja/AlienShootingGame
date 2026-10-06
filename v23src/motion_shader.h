#pragma once
static const char* kMotionShader=R"HLSL(
Texture2D<float4> source : register(t0);
Texture2D<float4> current : register(t1);
Texture2D<float4> previous : register(t2);
RWTexture2D<float4> downsampled : register(u0);
RWTexture2D<float2> motion : register(u1);
RWTexture2D<float> depth : register(u2);
SamplerState linearClamp : register(s0);
cbuffer Settings : register(b0) { uint width; uint height; uint resetHistory; };
[numthreads(8,8,1)]
void Resample(uint3 p:SV_DispatchThreadID) {
 if(p.x>=width||p.y>=height) return;
 downsampled[p.xy]=float4(source.SampleLevel(linearClamp,(float2(p.xy)+0.5)/float2(width,height),0).rgb,1);
}
float Luma(float3 c) { return dot(c,float3(0.2126,0.7152,0.0722)); }
float PatchError(int2 center,int2 displacement) {
 float score=0;
 [unroll] for(int y=-2;y<=2;y+=2) [unroll] for(int x=-2;x<=2;x+=2) {
  int2 a=clamp(center+int2(x,y),int2(0,0),int2(width-1,height-1));
  int2 b=clamp(a+displacement,int2(0,0),int2(width-1,height-1));
  score+=abs(Luma(current.Load(int3(a,0)).rgb)-Luma(previous.Load(int3(b,0)).rgb));
 }
 return score+0.0001*dot(float2(displacement),float2(displacement));
}
// One GPU thread estimates an 8x8 block, using coarse-to-fine patch search.
// These are previous-minus-current image displacements, NOT engine vectors.
[numthreads(8,8,1)]
void Motion(uint3 group:SV_DispatchThreadID) {
 uint2 origin=group.xy*8;
 if(origin.x>=width||origin.y>=height) return;
 int2 center=int2(origin)+int2(4,4),best=int2(0,0);
 if(!resetHistory) {
  float cost=PatchError(center,best);
  [unroll] for(int step=8;step>=1;step/=2) {
   int2 base=best;
   [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++) {
    int2 trial=base+int2(x,y)*step;
    float candidate=PatchError(center,trial);
    if(candidate<cost) { cost=candidate;best=trial; }
   }
  }
  // Reject a poor match instead of inserting a large spurious displacement.
  if(cost>1.0) best=int2(0,0);
 }
 [unroll] for(uint y=0;y<8;y++) [unroll] for(uint x=0;x<8;x++) {
  uint2 p=origin+uint2(x,y);
  if(p.x<width&&p.y<height) {
   motion[p]=float2(best);depth[p]=0.5;
  }
 }
}
)HLSL";
