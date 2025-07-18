#include <donut/shaders/binding_helpers.hlsli>
#include "cbt_shared.h"

struct PushConstants
{
	float2 Target;
};
DECLARE_PUSH_CONSTANTS(PushConstants, g_Push, 0, CONSTANTS_REGISTER_SPACE);

static const float2 g_offsets[] = {
	float2(-1, -1),
	float2(-1, 1),
	float2(1, -1),
	float2(1, 1)
};

void main_vs(
	uint i_vertexId : SV_VertexID,
	out float4 o_pos : SV_Position,
	out float2 o_uv : TEXCOORD0
)
{
	o_uv = g_offsets[i_vertexId];
	o_pos = float4(g_Push.Target + (0.025f * o_uv), 0, 1);
}

void main_ps(
	in float4 i_pos : SV_Position,
	in float2 i_uv : TEXCOORD0,
	out float4 o_color : SV_Target0
)
{
	float r = length(i_uv);
	if (r > 1.0f || r < 0.7f)
	{
		discard;
	}
	o_color = float4(1, 0, 0, 1);
}
