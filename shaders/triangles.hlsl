
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

	float3x2 posMatrix = float3x2(float2(0, 0), 
								  float2(0, 1),
								  float2(1, 0));
    posMatrix = leb_DecodeAttributeArray(node, posMatrix);

    float2 pos = float2(posMatrix[i_vertexId][0], posMatrix[i_vertexId][1]);

	o_pos = float4((2.0f * pos - 1.0f), 0, 1);
}

void main_ps(
	in float4 i_pos : SV_Position,
	out float4 o_color : SV_Target0
)
{
	o_color = float4(0, 0, 0, 1);
}
