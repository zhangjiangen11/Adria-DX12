#pragma once

namespace adria
{

	enum SkyParams : Uint16
	{
		SkyParam_A = 0,
		SkyParam_B,
		SkyParam_C,
		SkyParam_D,
		SkyParam_E,
		SkyParam_F,
		SkyParam_G,
		SkyParam_I,
		SkyParam_H,
		SkyParam_Z,
		SkyParam_Count
	};

	using SkyParameters = std::array<Vector3, SkyParam_Count>;

	SkyParameters CalculateSkyParameters(Float turbidity, Float albedo, Vector3 const& sun_direction);
}