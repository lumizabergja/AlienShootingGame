
cbuffer Settings : register(b0) {
    uint width; uint height; uint pitchCurrent; uint pitchPrevious;
    uint motionWidth; uint motionHeight; uint blockWidth; uint blockHeight;
    uint firstFrame; uint outputWidth; uint outputHeight; uint reserved2;
};
ByteAddressBuffer frameCurrent : register(t0);
ByteAddressBuffer framePrevious : register(t1);
Texture2D<float2> blockFlow : register(t2);
RWTexture2D<float2> outBlockFlow : register(u0);
RWTexture2D<float2> outMotion : register(u1);
RWTexture2D<float> outDepth : register(u2);
RWTexture2D<float4> outColor : register(u3);

uint packed(ByteAddressBuffer frame, uint pitch, int2 p) {
    p = clamp(p, int2(0,0), int2(int(width)-1, int(height)-1));
    return frame.Load(uint(p.y)*pitch + uint(p.x)*4);
}
float3 rgb(uint q) { return float3((q>>16)&255, (q>>8)&255, q&255)/255.0; }
float luminance(uint q) { return dot(rgb(q),float3(0.2126,0.7152,0.0722)); }
int2 toNative(int2 p) {
    return int2((float2(p)+0.5)*float2(width,height)/float2(motionWidth,motionHeight));
}
float comparePyramid(int2 origin, int2 displacement, int scale, bool dense) {
    float error=0.0;
    [loop] for(int y=0;y<4;y++) {
      [loop] for(int x=0;x<4;x++) {
        if(!dense && ((x+y)&1)) continue;
        int2 q=origin+(int2(x,y)-int2(1,1))*scale;
        int2 c=toNative(q);
        int2 p=toNative(q+displacement);
        float currentL=luminance(packed(frameCurrent,pitchCurrent,c));
        float previousL=luminance(packed(framePrevious,pitchPrevious,p));
        error+=abs(currentL-previousL);
      }
    }
    return error;
}
[numthreads(8,8,1)]
void Flow4x4(uint3 dt:SV_DispatchThreadID) {
    if(dt.x>=blockWidth || dt.y>=blockHeight)return;
    int2 origin=int2(dt.xy)*4;
    if(firstFrame!=0) {outBlockFlow[dt.xy]=float2(0,0);return;}
    // Stable blocks should never wander to an unrelated matching patch.
    float baseline=comparePyramid(origin,int2(0,0),1,true);
    if(baseline<0.045) {outBlockFlow[dt.xy]=float2(0,0);return;}
    int2 best=int2(0,0);
    float bestScore=1e20;
    // Pyramid level 2: 4x spatial spacing, wide search up to +/- 32 grid pixels.
    [loop] for(int y=-8;y<=8;y+=2) {
      [loop] for(int x=-8;x<=8;x+=2) {
        int2 d=int2(x,y)*4;
        float value=comparePyramid(origin,d,4,false)+0.0025*length(float2(d));
        if(value<bestScore){bestScore=value;best=d;}
      }
    }
    // Pyramid level 1: refine +/- 4 pixels in steps of 2.
    int2 anchor=best;bestScore=1e20;
    [loop] for(int y=-2;y<=2;y++) {
      [loop] for(int x=-2;x<=2;x++) {
        int2 d=anchor+int2(x,y)*2;
        float value=comparePyramid(origin,d,2,true)+0.003*length(float2(d));
        if(value<bestScore){bestScore=value;best=d;}
      }
    }
    // Pyramid level 0: native 4x4 matching, 1-pixel refinement.
    anchor=best;bestScore=1e20;
    [loop] for(int y=-2;y<=2;y++) {
      [loop] for(int x=-2;x<=2;x++) {
        int2 d=anchor+int2(x,y);
        float value=comparePyramid(origin,d,1,true)+0.003*length(float2(d));
        if(value<bestScore){bestScore=value;best=d;}
      }
    }
    if(bestScore>1.6 || bestScore>baseline*1.1+0.07)best=int2(0,0);
    outBlockFlow[dt.xy]=float2(best)*float2(width,height)/float2(motionWidth,motionHeight);
}
[numthreads(8,8,1)]
void ExpandAndDecode(uint3 dt:SV_DispatchThreadID) {
    if(dt.x>=outputWidth || dt.y>=outputHeight) return;
    int2 imagePos=int2((float2(dt.xy)+0.5)*float2(width,height)/float2(outputWidth,outputHeight));
    uint q=packed(frameCurrent,pitchCurrent,imagePos);
    float3 c=rgb(q);
    outColor[dt.xy]=float4(c,1);
    // Conservative flat depth: true game depth is unavailable to external capture.
    outDepth[dt.xy]=0.5;
    float2 grid=((float2(dt.xy)+0.5)*float2(motionWidth,motionHeight)/float2(outputWidth,outputHeight))/4.0-0.5;
    int2 base=int2(floor(grid));
    float2 fraction=saturate(frac(grid));
    float2 mv=0;float weightSum=0;
    [unroll] for(int y=0;y<2;y++) {
      [unroll] for(int x=0;x<2;x++) {
        int2 p=clamp(base+int2(x,y),int2(0,0),int2(blockWidth-1,blockHeight-1));
        float2 flow=blockFlow.Load(int3(p,0));
        float bilinear=(x==0?(1-fraction.x):fraction.x)*(y==0?(1-fraction.y):fraction.y);
        int2 sample=toNative(p*4+2);
        float edge=abs(luminance(packed(frameCurrent,pitchCurrent,sample))-luminance(q));
        float w=bilinear/(1.0+edge*20.0);
        mv+=flow*w; weightSum+=w;
      }
    }
    if(weightSum>0)mv/=weightSum;
    if(firstFrame!=0)mv=float2(0,0);
    outMotion[dt.xy]=mv*float2(outputWidth,outputHeight)/float2(width,height);
}

struct V2P {float4 pos:SV_Position;float2 uv:TEXCOORD0;};
V2P FullscreenVS(uint id:SV_VertexID){
    V2P v;
    float2 uv=float2((id==2)?2.0:0.0,(id==1)?2.0:0.0);
    v.pos=float4(uv.x*2.0-1.0,1.0-uv.y*2.0,0.0,1.0);
    v.uv=uv;
    return v;
}
Texture2D<float4> presentationColor:register(t0);
SamplerState linearClamp:register(s0);
float4 FullscreenPS(V2P p):SV_Target {
    return presentationColor.Sample(linearClamp,p.uv);
}
