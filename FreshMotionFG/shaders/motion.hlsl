cbuffer Params : register(b0) {
    uint2 imageSize; uint2 analysisSize;
    uint2 flowSize; uint firstFrame; uint outputWidth;
    uint outputHeight; float mouseX; float mouseY; float cameraGain;
    float tanHalfFovX; float confidenceCutoff; uint capturePitchBytes; float pad1;
};
#define outputSize float2(outputWidth,outputHeight)
#define mouseDelta float2(mouseX,mouseY)
Texture2D<float4> currentFrame : register(t0);
Texture2D<float4> previousFrame : register(t1);
Texture2D<float4> coarseFlow : register(t2);
Texture2D<float4> reverseFlow : register(t3);
RWTexture2D<float4> blockVectors : register(u0);
RWTexture2D<float2> motionVectors : register(u1);
RWTexture2D<float> depthSurface : register(u2);

float grey(float3 rgb) { return dot(rgb, float3(.2126,.7152,.0722)); }
int2 clampPixel(int2 p) { return clamp(p,int2(0,0),int2(imageSize)-1); }
float4 loadFrame(Texture2D<float4> src,int2 p) {
    int2 coord=clampPixel(p);
    return src.Load(int3(coord,0));
}
float luma(Texture2D<float4> img,int2 p) {
    return grey(loadFrame(img,p).rgb);
}
float2 pixelAtAnalysis(float2 p) { return (p+0.5)*float2(imageSize)/float2(analysisSize); }

// All displacement candidates come from two observed images.
float sad(int2 origin,float2 delta,bool dense) {
    float s=0;
    [unroll] for(int y=0;y<4;y++) {
      [unroll] for(int x=0;x<4;x++) {
        if(!dense && ((x+y)&1))continue;
        float2 a=pixelAtAnalysis(float2(origin+int2(x,y)));
        int2 currentPixel=int2(a);
        int2 previousPixel=int2(a+delta);
        s+=abs(luma(currentFrame,currentPixel)-luma(previousFrame,previousPixel));
      }
    }
    return s*(dense?1.0/16.0:1.0/8.0);
}

void estimatePair(uint3 tid) {
    if(tid.x>=flowSize.x||tid.y>=flowSize.y)return;
    int2 origin=int2(tid.xy)*4;
    float2 best=0;
    float bestCost=1e8;
    float baseline=sad(origin,0,true);
    if(firstFrame!=0){blockVectors[tid.xy]=0;return;}
    if(baseline<.009){blockVectors[tid.xy]=float4(0,0,1,0);return;}
    float2 unit=float2(imageSize)/float2(analysisSize);
    [loop] for(int oy=-8;oy<=8;oy+=4) {
      [loop] for(int ox=-8;ox<=8;ox+=4) {
        float2 candidate=float2(ox,oy)*unit;
        float score=sad(origin,candidate,false)+.00005*length(candidate);
        if(score<bestCost){bestCost=score;best=candidate;}
      }
    }
    float2 center=best;
    [loop] for(int y=-2;y<=2;y++) {
      [loop] for(int x=-2;x<=2;x++) {
        float2 candidate=center+float2(x,y)*unit;
        float score=sad(origin,candidate,true)+.00005*length(candidate);
        if(score<bestCost){bestCost=score;best=candidate;}
      }
    }
    float confidence=saturate((baseline-bestCost)/max(.01,baseline));
    if(bestCost>.24 || (confidence<confidenceCutoff && length(best)>3)) {
        best=0;confidence=0;
    }
    blockVectors[tid.xy]=float4(best,confidence,bestCost);
}
[numthreads(8,8,1)] void EstimateForward(uint3 tid:SV_DispatchThreadID){estimatePair(tid);}
[numthreads(8,8,1)] void EstimateReverse(uint3 tid:SV_DispatchThreadID){estimatePair(tid);}

[numthreads(8,8,1)]
void ExpandMotion(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=uint(outputSize.x)||tid.y>=uint(outputSize.y))return;
    float2 sourcePixel=(float2(tid.xy)+.5)*float2(imageSize)/outputSize;
    float2 blockUV=((sourcePixel+.5)*float2(analysisSize)/float2(imageSize))/4.0-.5;
    int2 base=int2(floor(blockUV));
    float2 fracPart=saturate(frac(blockUV));
    float2 average=0;
    float weight=0;
    float consistency=0;
    float3 pixelColor=loadFrame(currentFrame,int2(sourcePixel)).rgb;
    [unroll] for(int y=0;y<2;y++){
      [unroll] for(int x=0;x<2;x++){
        int2 q=clamp(base+int2(x,y),0,int2(flowSize)-1);
        float4 f=coarseFlow.Load(int3(q,0));
        float w=(x==0?1-fracPart.x:fracPart.x)*(y==0?1-fracPart.y:fracPart.y);
        float2 origin=(float2(q)*4+2)*float2(imageSize)/float2(analysisSize);
        float3 blockColor=loadFrame(currentFrame,int2(origin)).rgb;
        float colorDist=length(blockColor-pixelColor);
        w*=max(.025,f.z)/(1+12*colorDist);
        average+=f.xy*w;weight+=w;
      }
    }
    average/=max(.001,weight);
    int2 reverseBlock=int2((sourcePixel+average)*float2(analysisSize)/float2(imageSize)/4);
    reverseBlock=clamp(reverseBlock,0,int2(flowSize)-1);
    float4 backward=reverseFlow.Load(int3(reverseBlock,0));
    float disagreement=length(average+backward.xy);
    float tolerance=3.0+length(average)*.16;
    float reliability=saturate(1.0-disagreement/max(1,tolerance));
    reliability*=saturate(weight*4);
    // Pixels that remain identical despite substantial surrounding image
    // motion are often HUD elements. Preserve them instead of dragging them.
    float still=abs(luma(currentFrame,int2(sourcePixel))-
                    luma(previousFrame,int2(sourcePixel)));
    if(still<.006 && length(average)>3)reliability*=.15;
    if(firstFrame!=0)reliability=0;
    // R16G16_FLOAT, pixel units matching sl::Constants::mvecScale.
    motionVectors[tid.xy]=average*reliability*outputSize/float2(imageSize);
    depthSurface[tid.xy]=.5; // intentionally flat; inferred depth is not native depth
}

Texture2D<float4> scene : register(t0);
struct VOut{float4 pos:SV_Position;float2 uv:TEXCOORD0;};
VOut VSMain(uint id:SV_VertexID){
    VOut v;
    float2 uv=float2(id==2?2:0,id==1?2:0);
    v.pos=float4(uv.x*2-1,1-uv.y*2,0,1);v.uv=uv;
    return v;
}
float4 PSMain(VOut v):SV_Target {
    // The game PID capture may be 4:3 while the desktop presenter is 16:9.
    // Sampling in normalized coordinates deliberately stretches it to fill.
    float2 src=v.uv*float2(imageSize)-0.5;
    int2 first=int2(floor(src));
    float2 blend=saturate(frac(src));
    float4 a=lerp(loadFrame(scene,first),loadFrame(scene,first+int2(1,0)),blend.x);
    float4 b=lerp(loadFrame(scene,first+int2(0,1)),loadFrame(scene,first+int2(1,1)),blend.x);
    return lerp(a,b,blend.y);
}
