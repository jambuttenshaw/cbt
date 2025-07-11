
#include <donut/shaders/binding_helpers.hlsli>

// Relative paths allow work for intellisense
#include "../libcbt/hlsl/ConcurrentBinaryTree.hlsl"
#include "../libleb/hlsl/LongestEdgeBisection.hlsl"


void wireframe_vs(
	uint i_vertexId : SV_VertexID,
	uint i_instanceId : SV_InstanceID,
	out float4 o_pos : SV_Position
)
{
    cbt_Node node = cbt_DecodeNode(i_instanceId);

    float3x2 posMatrix = float3x2(float2(0, 1),
								  float2(0, 0),
								  float2(1, 0));
    posMatrix = leb_DecodeAttributeArray(node, posMatrix);

    float2 pos = float2(posMatrix[i_vertexId][0], posMatrix[i_vertexId][1]);

    o_pos = float4((2.0f * pos - 1.0f), 0, 1);
}

void wireframe_ps(
	in float4 i_pos : SV_Position,
	out float4 o_color : SV_Target0
)
{
    o_color = float4(0, 0, 0, 1);
}



void fill_vs(
	uint i_vertexId : SV_VertexID,
	uint i_instanceId : SV_InstanceID,
	out float4 o_pos : SV_Position,
	out nointerpolation float2 o_centre : POSITION
)
{
    cbt_Node node = cbt_DecodeNode(i_instanceId);

	float3x2 posMatrix = float3x2(float2(0, 1),
								  float2(0, 0),
								  float2(1, 0));
    posMatrix = leb_DecodeAttributeArray(node, posMatrix);

    float2 pos = float2(posMatrix[i_vertexId][0], posMatrix[i_vertexId][1]);

	o_pos = float4((2.0f * pos - 1.0f), 0, 1);
    o_centre = (float2(posMatrix[0][0], posMatrix[0][1]) 
			  + float2(posMatrix[1][0], posMatrix[1][1]) 
			  + float2(posMatrix[2][0], posMatrix[2][1])) / 3.0f;
}

float hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * .1031f);
    p3 += dot(p3, p3.yzx + 19.19f);
    return frac((p3.x + p3.y) * p3.z);
}

float3 HUEtoRGB(float H)
{
    float R = abs(H * 6 - 3) - 1;
    float G = 2 - abs(H * 6 - 2);
    float B = 2 - abs(H * 6 - 4);
    return saturate(float3(R, G, B));
}

void fill_ps(
	in float4 i_pos : SV_Position,
	in float2 i_centre : POSITION,
	out float4 o_color : SV_Target0
)
{
    o_color = float4(HUEtoRGB(saturate(hash(i_centre))), 1);
}
