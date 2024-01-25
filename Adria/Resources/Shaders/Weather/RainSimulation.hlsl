#include "CommonResources.hlsli"
#include "Random.hlsli"

#define BLOCK_SIZE 256

struct Constants
{
	uint   rainDataIdx;
	uint   depthIdx;
};
ConstantBuffer<Constants> PassCB : register(b1);

struct RainData
{
	float3 Pos;
	float3 Vel;
	float State;
};

struct CSInput
{
	uint3 GroupId : SV_GroupID;
	uint3 GroupThreadId : SV_GroupThreadID;
	uint3 DispatchThreadId : SV_DispatchThreadID;
	uint  GroupIndex : SV_GroupIndex;
};

[numthreads(BLOCK_SIZE, 1, 1)]
void RainSimulationCS(CSInput input)
{
	uint3 dispatchThreadId = input.DispatchThreadId;
	
	RWStructuredBuffer<RainData> rainDataBuffer = ResourceDescriptorHeap[PassCB.rainDataIdx];
	uint GroupIdx = dispatchThreadId.x;

	RainData rainDrop = rainDataBuffer[GroupIdx];
	rainDrop.Pos += rainDrop.Vel * FrameCB.deltaTime; 
	
	float3 boundsCenter = FrameCB.cameraPosition.xyz;
	const float3 boundsExtents = float3(50, 50, 50); 
	
	float2 offsetAmount = (rainDrop.Pos.xz - boundsCenter.xz) / boundsExtents.xz;
	rainDrop.Pos.xz -= boundsExtents.xz * ceil(0.5 * offsetAmount - 0.5);
	
	if(abs(rainDrop.Pos.y - boundsCenter.y) > boundsExtents.y)
	{
		uint randSeed = InitRand(GroupIdx, 47, 16);
		float4 random01 = float4(NextRand(randSeed), NextRand(randSeed), NextRand(randSeed), NextRand(randSeed));
		float4 random_11 = (random01 * 2.0) - 1.0;
	
		rainDrop.Pos.xz = boundsCenter.xz + boundsExtents.xz * random_11.xy;
		rainDrop.Pos.y  = boundsCenter.y + boundsExtents.y;
		rainDrop.Vel.y = -10.0f;
	}
	
	rainDrop.State = 1.0f;
	rainDataBuffer[GroupIdx] = rainDrop;
}