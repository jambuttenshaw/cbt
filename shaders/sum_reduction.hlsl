#include <donut/shaders/binding_helpers.hlsli>

#define CBT_FLAG_WRITE

#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"

struct PushConstants
{
    uint PassID;
};
DECLARE_PUSH_CONSTANTS(PushConstants, g_Push, 0, 0);


[numthreads(256, 1, 1)]
void main_cs(uint3 DTid : SV_DispatchThreadID)
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
