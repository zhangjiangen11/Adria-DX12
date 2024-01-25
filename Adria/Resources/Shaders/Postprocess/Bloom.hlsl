#include "CommonResources.hlsli"

#define BLOCK_SIZE 8

#ifndef FIRST_PASS
#define FIRST_PASS 0
#endif

struct CSInput
{
	uint3 GroupId : SV_GroupID;
	uint3 GroupThreadId : SV_GroupThreadID;
	uint3 DispatchThreadId : SV_DispatchThreadID;
	uint GroupIndex : SV_GroupIndex;
};

struct BloomDownsampleConstants
{
	float2 dimsInv;
	uint   sourceIdx;
	uint   targetIdx;
};
ConstantBuffer<BloomDownsampleConstants> PassCB : register(b1);

float Luminance(float3 color)
{
	return dot(color, float3(0.2126729, 0.7151522, 0.0721750));
}

float3 ComputePartialAverage(float3 v0, float3 v1, float3 v2, float3 v3)
{
#if FIRST_PASS //Karis Average
	float w0 = rcp(1.0 + Luminance(v0));
	float w1 = rcp(1.0 + Luminance(v1));
	float w2 = rcp(1.0 + Luminance(v2));
	float w3 = rcp(1.0 + Luminance(v3));
#else
	float w0 = 1.0;
	float w1 = 1.0;
	float w2 = 1.0;
	float w3 = 1.0;
#endif
	return (v0 * w0 + v1 * w1 + v2 * w2 + v3 * w3) / (w0 + w1 + w2 + w3);
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void BloomDownsample(CSInput input)
{
	Texture2D sourceTx = ResourceDescriptorHeap[PassCB.sourceIdx];
	RWTexture2D<float4> targetTx = ResourceDescriptorHeap[PassCB.targetIdx];

	float2 uv = ((float2) input.DispatchThreadId.xy + 0.5f) * 1.0f * PassCB.dimsInv;

	float3 outColor = 0.0f;
	float3 M0 = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(-1.0f, 1.0f)).xyz;
	float3 M1 = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(1.0f, 1.0f)).xyz;
	float3 M2 = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(-1.0f, -1.0f)).xyz;
	float3 M3 = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(1.0f, -1.0f)).xyz;

	float3 TL = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(-2.0f, 2.0f)).xyz;
	float3 T  = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(0.0f, 2.0f)).xyz;
	float3 TR = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(2.0f, 2.0f)).xyz;
	float3 L  = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(-2.0f, 0.0f)).xyz;
	float3 C  = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(0.0f, 0.0f)).xyz;
	float3 R  = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(2.0f, 0.0f)).xyz;
	float3 BL = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(-2.0f, -2.0f)).xyz;
	float3 B  = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(0.0f, -2.0f)).xyz;
	float3 BR = sourceTx.SampleLevel(LinearClampSampler, uv, 0, int2(2.0f, -2.0f)).xyz;

	outColor += ComputePartialAverage(M0, M1, M2, M3) * 0.5f;
	outColor += ComputePartialAverage(TL, T, C, L) * 0.125f;
	outColor += ComputePartialAverage(TR, T, C, R) * 0.125f;
	outColor += ComputePartialAverage(BL, B, C, L) * 0.125f;
	outColor += ComputePartialAverage(BR, B, C, R) * 0.125f;

	targetTx[input.DispatchThreadId.xy] = float4(outColor, 1);
}

struct BloomUpsampleConstants
{
	float2 dimsInv;
	uint   lowInputIdx;
	uint   highInputIdx;
	uint   outputIdx;
	float  radius;
};
ConstantBuffer<BloomUpsampleConstants> PassCB2 : register(b1);

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void BloomUpsample(CSInput input)
{
	Texture2D inputLowTx = ResourceDescriptorHeap[PassCB2.lowInputIdx];
	Texture2D inputHighTx = ResourceDescriptorHeap[PassCB2.highInputIdx];
	RWTexture2D<float3> outputTx = ResourceDescriptorHeap[PassCB2.outputIdx];

	float2 uv = ((float2) input.DispatchThreadId.xy + 0.5f) * 1.0f * PassCB2.dimsInv;

	float3 outColor = 0.0f;
	outColor += 0.0625f * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(-1.0f, 1.0f)).xyz;
	outColor += 0.125f  * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(0.0f, 1.0f)).xyz;
	outColor += 0.0625f * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(1.0f, 1.0f)).xyz;
	outColor += 0.125f  * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(-1.0f, 0.0f)).xyz;
	outColor += 0.25f   * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(0.0f, 0.0f)).xyz;
	outColor += 0.125f  * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(1.0f, 0.0f)).xyz;
	outColor += 0.0625f * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(-1.0f, -1.0f)).xyz;
	outColor += 0.125f  * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(0.0f, -1.0f)).xyz;
	outColor += 0.0625f * inputLowTx.SampleLevel(LinearBorderSampler, uv, 0, int2(1.0f, -1.0f)).xyz;

	outputTx[input.DispatchThreadId.xy] = lerp(inputHighTx[input.DispatchThreadId.xy].xyz, outColor, PassCB2.radius);
}