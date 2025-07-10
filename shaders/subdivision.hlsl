#include <donut/shaders/binding_helpers.hlsli>

#define CBT_FLAG_WRITE

#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"
#include "../libleb/hlsl/LongestEdgeBisection.hlsl"

#ifndef FLAG_SPLIT
#define FLAG_SPLIT (1)
#endif

#ifndef FLAG_MERGE
#define FLAG_MERGE (!FLAG_SPLIT)
#endif

struct PushConstants
{
    float2 Target;
};
DECLARE_PUSH_CONSTANTS(PushConstants, g_Push, 0, 0);


float Wedge(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

bool IsInside(float3x2 faceVertices)
{
    float2 v1 = float2(faceVertices[0][0], faceVertices[0][1]);
    float2 v2 = float2(faceVertices[1][0], faceVertices[1][1]);
    float2 v3 = float2(faceVertices[2][0], faceVertices[2][1]);
    float w1 = Wedge(v2 - v1, g_Push.Target - v1);
    float w2 = Wedge(v3 - v2, g_Push.Target - v2);
    float w3 = Wedge(v1 - v3, g_Push.Target - v3);
    float3 w = float3(w1, w2, w3);

    return all(w >= 0.0f);
}

float3x2 DecodeFaceVertices(cbt_Node node)
{
    float3x2 faceVertices = float3x2(float2(0, 1),
								     float2(0, 0),
								     float2(1, 0));
    faceVertices = leb_DecodeAttributeArray(node, faceVertices);

    return faceVertices;
}

[numthreads(256, 1, 1)]
void main_cs(uint3 DTid : SV_DispatchThreadID)
{
    uint threadID = DTid.x;

    if (threadID < cbt_NodeCount())
    {
        cbt_Node node = cbt_DecodeNode(threadID);

#if FLAG_SPLIT
        float3x2 faceVertices = DecodeFaceVertices(node);

        if (IsInside(faceVertices)) {
            leb_SplitNode(node);
        }
#endif

#if FLAG_MERGE
        leb_DiamondParent diamondParent = leb_DecodeDiamondParent(node);

        float3x2 baseFaceVertices = DecodeFaceVertices(diamondParent.base);
        float3x2 topFaceVertices = DecodeFaceVertices(diamondParent.top);

        if (!IsInside(baseFaceVertices) && !IsInside(topFaceVertices)) {
            leb_MergeNode(node, diamondParent);
        }
#endif
    }
}
