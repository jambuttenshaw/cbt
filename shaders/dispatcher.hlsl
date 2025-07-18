
#include <donut/shaders/binding_helpers.hlsli>
#include "cbt_shared.h"

#define CBT_HEAP_BUFFER_BINDING REGISTER_SRV(0, CBT_REGISTER_SPACE)
#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"

struct IndirectArgs
{
    struct
    {
        uint groupsX;
        uint groupsY;
        uint groupsZ;
    } cbtDispatch;

    struct
    {
        uint vertexCount;
        uint instanceCount;
        uint startVertexLocation;
        uint startInstanceLocation;
    } lebDispatch;
};
RWStructuredBuffer<IndirectArgs> RWIndirectArgs : REGISTER_UAV(0, INDIRECT_ARGS_REGISTER_SPACE);

[numthreads(1, 1, 1)]
void cbt_dispatcher_cs()
{
    uint nodeCount = cbt_NodeCount();
    RWIndirectArgs[0].cbtDispatch.groupsX = max(nodeCount >> 8, 1);
}

[numthreads(1, 1, 1)]
void leb_dispatcher_cs()
{
    RWIndirectArgs[0].lebDispatch.instanceCount = cbt_NodeCount();
}
