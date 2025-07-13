#include <donut/shaders/binding_helpers.hlsli>
#include "cbt_shared.h"

#define CBT_FLAG_WRITE

#define CBT_HEAP_BUFFER_BINDING REGISTER_UAV(0, CBT_REGISTER_SPACE)
#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"

struct PushConstants
{
    uint PassID;
};
DECLARE_PUSH_CONSTANTS(PushConstants, g_Push, 0, CONSTANTS_REGISTER_SPACE);


// For improved performance, multiple passes (5) can be performed in a single kernel
// This avoids having situations where threads are operating on 2-6 bits a time, which results in high memory contention
[numthreads(256, 1, 1)]
void sum_reduction_prepass_cs(uint3 DTid : SV_DispatchThreadID)
{
    uint cnt = (1u << g_Push.PassID);
    uint threadID = DTid.x << 5;

    if (threadID < cnt)
    {
        uint nodeID = threadID + cnt;
        uint alignedBitOffset = cbt__NodeBitID(cbt_CreateNode(nodeID, g_Push.PassID));
        uint bitField = u_CbtBuffer[alignedBitOffset >> 5u];
        uint bitData = 0u;

        // 2-bits
        bitField = (bitField & 0x55555555u) + ((bitField >> 1u) & 0x55555555u);
        bitData = bitField;
        u_CbtBuffer[(alignedBitOffset - cnt) >> 5u] = bitData;

        // 3-bits
        bitField = (bitField & 0x33333333u) + ((bitField >> 2u) & 0x33333333u);
        bitData = ((bitField >> 0u) & (7u << 0u))
                | ((bitField >> 1u) & (7u << 3u))
                | ((bitField >> 2u) & (7u << 6u))
                | ((bitField >> 3u) & (7u << 9u))
                | ((bitField >> 4u) & (7u << 12u))
                | ((bitField >> 5u) & (7u << 15u))
                | ((bitField >> 6u) & (7u << 18u))
                | ((bitField >> 7u) & (7u << 21u));
        cbt__HeapWriteExplicit(cbt_CreateNode(nodeID >> 2u, g_Push.PassID - 2), 24, bitData);

        // 4-bits
        bitField = (bitField & 0x0F0F0F0Fu) + ((bitField >> 4u) & 0x0F0F0F0Fu);
        bitData = ((bitField >> 0u) & (15u << 0u))
                | ((bitField >> 4u) & (15u << 4u))
                | ((bitField >> 8u) & (15u << 8u))
                | ((bitField >> 12u) & (15u << 12u));
        cbt__HeapWriteExplicit(cbt_CreateNode(nodeID >> 3u, g_Push.PassID - 3), 16, bitData);

        // 5-bits
        bitField = (bitField & 0x00FF00FFu) + ((bitField >> 8u) & 0x00FF00FFu);
        bitData = ((bitField >> 0u) & (31u << 0u))
                | ((bitField >> 11u) & (31u << 5u));
        cbt__HeapWriteExplicit(cbt_CreateNode(nodeID >> 4u, g_Push.PassID - 4), 10, bitData);

        // 6-bits
        bitField = (bitField & 0x0000FFFFu) + ((bitField >> 16u) & 0x0000FFFFu);
        bitData = bitField;
        cbt__HeapWriteExplicit(cbt_CreateNode(nodeID >> 5u, g_Push.PassID - 5), 6, bitData);
    }
}


[numthreads(256, 1, 1)]
void sum_reduction_cs(uint3 DTid : SV_DispatchThreadID)
{
    uint cnt = (1u << g_Push.PassID);
    uint threadID = DTid.x;

    if (threadID < cnt)
    {
        uint nodeID = threadID + cnt;
        uint x0 = cbt_HeapRead(cbt_CreateNode(nodeID << 1u, g_Push.PassID + 1));
        uint x1 = cbt_HeapRead(cbt_CreateNode(nodeID << 1u | 1u, g_Push.PassID + 1));

        cbt__HeapWrite(cbt_CreateNode(nodeID, g_Push.PassID), x0 + x1);
    }
}
