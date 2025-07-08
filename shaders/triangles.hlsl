
#include <donut/shaders/binding_helpers.hlsli>

// Relative paths allow work for intellisense
#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"
#include "../libleb/hlsl/LongestEdgeBisection.hlsl"


void main_vs(
	uint i_vertexId : SV_VertexID,
	uint i_instanceId : SV_InstanceID,
	out float4 o_pos : SV_Position
)
{
    cbt_Node node = cbt_DecodeNode(i_instanceId);
    float3 xPos = float3(0, 0, 1);
    float3 yPos = float3(1, 0, 0);

    float3x2 posMatrix = leb_DecodeAttributeArray(node, float3x2(xPos, yPos));

    float2 pos = float2(posMatrix[0][i_vertexId], posMatrix[1][i_vertexId]);

    o_pos = float4((2.0f * pos - 1.0f), 0.0f, 1.0f);
}

void main_ps(
	in float4 i_pos : SV_Position,
	out float4 o_color : SV_Target0
)
{
	o_color = float4(0, 0, 0, 1);
}
