#include "CommonResources.hlsli"
#include "Constants.hlsli"

#define FFT_WORK_GROUP_DIM 256

struct FFTConstants
{
	uint inputIdx;
	uint outputIdx;

	uint seqCount;
	uint subseqCount;
};
ConstantBuffer<FFTConstants> FFTPassCB : register(b1);

float2 MultiplyComplex(float2 a, float2 b)
{
	return float2(a[0] * b[0] - a[1] * b[1], a[1] * b[0] + a[0] * b[1]);
}
float4 ButterflyOperation(float2 a, float2 b, float2 twiddle)
{
	float2 twiddleB = MultiplyComplex(twiddle, b);
	float4 result = float4(a + twiddleB, a - twiddleB);
	return result;
}

struct CSInput
{
	uint3 GroupId : SV_GroupID;
	uint3 GroupThreadId : SV_GroupThreadID;
	uint3 DispatchThreadId : SV_DispatchThreadID;
	uint GroupIndex : SV_GroupIndex;
};

[numthreads(FFT_WORK_GROUP_DIM, 1, 1)]
void FFT_HorizontalCS(CSInput input)
{
	uint2 pixelCoord = uint2(input.GroupThreadId.x, input.GroupId.x);

	int threadCount = int(FFTPassCB.seqCount * 0.5f);
	int threadIdx = pixelCoord.x;

	int inIdx = threadIdx & (FFTPassCB.subseqCount - 1);
	int outIdx = ((threadIdx - inIdx) << 1) + inIdx;

	float angle = -M_PI * (float(inIdx) / float(FFTPassCB.subseqCount));
	float2 twiddle = float2(cos(angle), sin(angle));

	Texture2D inputTexture = ResourceDescriptorHeap[FFTPassCB.inputIdx];
	float4 a = inputTexture.Load(uint3(pixelCoord, 0));
	float4 b = inputTexture.Load(uint3(pixelCoord.x + threadCount, pixelCoord.y, 0));

	float4 result0 = ButterflyOperation(a.xy, b.xy, twiddle);
	float4 result1 = ButterflyOperation(a.zw, b.zw, twiddle);

	RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[FFTPassCB.outputIdx];
	outputTexture[int2(outIdx, pixelCoord.y)] = float4(result0.xy, result1.xy);
	outputTexture[int2(outIdx + FFTPassCB.subseqCount, pixelCoord.y)] = float4(result0.zw, result1.zw);
}

[numthreads(FFT_WORK_GROUP_DIM, 1, 1)]
void FFT_VerticalCS(CSInput input)
{
	uint2 pixelCoord = uint2(input.GroupId.x, input.GroupThreadId.x);

	int threadCount = int(FFTPassCB.seqCount * 0.5f);
	int threadIdx = pixelCoord.y;

	int inIdx = threadIdx & (FFTPassCB.subseqCount - 1);
	int outIdx = ((threadIdx - inIdx) << 1) + inIdx;

	float angle = -M_PI * (float(inIdx) / float(FFTPassCB.subseqCount));
	float2 twiddle = float2(cos(angle), sin(angle));

	Texture2D inputTexture = ResourceDescriptorHeap[FFTPassCB.inputIdx];
	float4 a = inputTexture.Load(uint3(pixelCoord, 0));
	float4 b = inputTexture.Load(uint3(pixelCoord.x, pixelCoord.y + threadCount, 0));

	float4 result0 = ButterflyOperation(a.xy, b.xy, twiddle);
	float4 result1 = ButterflyOperation(a.zw, b.zw, twiddle);

	RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[FFTPassCB.outputIdx];
	outputTexture[int2(pixelCoord.x, outIdx)] = float4(result0.xy, result1.xy);
	outputTexture[int2(pixelCoord.x, outIdx + FFTPassCB.subseqCount)] = float4(result0.zw, result1.zw);
}