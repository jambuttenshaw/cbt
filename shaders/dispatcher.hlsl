
#include <donut/shaders/binding_helpers.hlsli>

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
RWStructuredBuffer<IndirectArgs> RWIndirectArgs : REGISTER_UAV(0, 0);

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
