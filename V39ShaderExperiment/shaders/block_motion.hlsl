// Independent image-to-image optical flow, written for the V39 NVOFA proxy experiment.
// The driver-facing output uses the NVOFA 4x4 grid and 1/32-pixel signed units.
// This shader is intentionally conservative: an uncertain match returns zero motion.

Texture2D<float4> currentFrame  : register(t0);
Texture2D<float4> previousFrame : register(t1);
RWTexture2D<int2>  flowOutput   : register(u0);
RWTexture2D<uint>  costOutput   : register(u1);

cbuffer MotionOptions : register(b0)
{
    uint sourceWidth;
    uint sourceHeight;
    uint blocksWidth;
    uint blocksHeight;
};

float Luma(int2 p, bool current)
{
    p = clamp(p, int2(0, 0), int2(sourceWidth - 1, sourceHeight - 1));
    float3 rgb = current ? currentFrame.Load(int3(p, 0)).rgb
                         : previousFrame.Load(int3(p, 0)).rgb;
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float MatchCost(int2 center, int2 delta)
{
    // Eight samples spread across one 4x4 block, plus its center.
    // This keeps the cost smaller than a complete 16-sample SAD.
    static const int2 offsets[9] = {
        int2(-1,-1), int2(0,-1), int2(1,-1),
        int2(-1, 0), int2(0, 0), int2(1, 0),
        int2(-1, 1), int2(0, 1), int2(1, 1)
    };
    float total = 0.0f;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        int2 p = center + offsets[k] * 2;
        float a = Luma(p, true);
        float b = Luma(p + delta, false);
        total += abs(a - b);
    }
    return total * (1.0f / 9.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= blocksWidth || tid.y >= blocksHeight) return;

    int2 center = int2(tid.xy * 4 + 2);
    float bestCost = 1.0e10f;
    float runnerUp = 1.0e10f;
    int2 bestVector = int2(0, 0);

    // Coarse-to-fine matching in a single pass, avoiding extra full-screen
    // pyramid allocations and additional GPU synchronization.
    [loop] for (int y = -12; y <= 12; y += 3)
    {
        [loop] for (int x = -12; x <= 12; x += 3)
        {
            float c = MatchCost(center, int2(x, y));
            if (c < bestCost) { runnerUp = bestCost; bestCost = c; bestVector = int2(x, y); }
            else if (c < runnerUp) { runnerUp = c; }
        }
    }
    int2 coarse = bestVector;
    [loop] for (int fy = -2; fy <= 2; ++fy)
    {
        [loop] for (int fx = -2; fx <= 2; ++fx)
        {
            int2 v = coarse + int2(fx, fy);
            float c = MatchCost(center, v);
            if (c < bestCost) { runnerUp = bestCost; bestCost = c; bestVector = v; }
            else if (c < runnerUp && any(v != bestVector)) { runnerUp = c; }
        }
    }

    float staticCost = MatchCost(center, int2(0, 0));
    float improvement = saturate((staticCost - bestCost) / max(0.03f, staticCost));
    float uniqueness = saturate((runnerUp - bestCost) / max(0.02f, runnerUp));
    float confidence = improvement * uniqueness;

    // Leave static or ambiguous blocks alone to avoid wavy scenery and HUD.
    if (bestCost > 0.22f || confidence < 0.08f)
    {
        bestVector = int2(0, 0);
        confidence = 0.0f;
    }

    flowOutput[tid.xy] = bestVector * 32;
    costOutput[tid.xy] = (uint)round(255.0f * (1.0f - confidence));
}
