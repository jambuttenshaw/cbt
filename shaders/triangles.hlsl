
#include <donut/shaders/binding_helpers.hlsli>

// Relative paths allow work for intellisense
#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"
#include "../libleb/hlsl/LongestEdgeBisection.hlsl"

struct PushConstants
{
	uint NodeCount;
	uint DisplayMode;
};
DECLARE_PUSH_CONSTANTS(PushConstants, g_Push, 0, 0);

void main_vs(
	uint i_vertexId : SV_VertexID,
	uint i_instanceId : SV_InstanceID,
	out float4 o_pos : SV_Position,
	out uint o_instanceId : InstanceID
)
{
	o_instanceId = i_instanceId;
    cbt_Node node = cbt_DecodeNode(i_instanceId);

	float3x2 posMatrix = float3x2(float2(0, 1),
								  float2(0, 0),
								  float2(1, 0));
    posMatrix = leb_DecodeAttributeArray(node, posMatrix);

    float2 pos = float2(posMatrix[i_vertexId][0], posMatrix[i_vertexId][1]);

	o_pos = float4((2.0f * pos - 1.0f), 0, 1);
}


float rand1(float n) { return frac(sin(n) * 43758.5453123); }

float3 HUEtoRGB(in float H)
{
    float R = abs(H * 6 - 3) - 1;
    float G = 2 - abs(H * 6 - 2);
    float B = 2 - abs(H * 6 - 4);
    return saturate(float3(R, G, B));
}

void main_ps(
	in float4 i_pos : SV_Position,
	in uint i_instanceId : InstanceID,
	out float4 o_color : SV_Target0
)
{
	float3 col = g_Push.DisplayMode ? 
		HUEtoRGB(rand1(((float) i_instanceId / (float) g_Push.NodeCount)))
		: 0.0f;
	o_color = float4(col, 1);
}
