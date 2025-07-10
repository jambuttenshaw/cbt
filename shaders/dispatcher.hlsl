
#include <donut/shaders/binding_helpers.hlsli>

#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"

struct IndirectArgs
{
    uint groupsX;
    uint groupsY;
    uint groupsZ;

    uint vertexCount;
    uint instanceCount;
    uint startVertexLocation;
    uint startInstanceLocation;
};
RWStructuredBuffer<IndirectArgs> RWIndirectArgs : REGISTER_UAV(0, 0);

[numthreads(1, 1, 1)]
void leb_dispatcher_cs()
{
	RWIndirectArgs[0].instanceCount = cbt_NodeCount();
}
