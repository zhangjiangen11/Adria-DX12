#pragma once

namespace adria
{
	enum class NoiseType : Uint8
	{
		OpenSimplex2,
		OpenSimplex2S,
		Cellular,
		Perlin,
		ValueCubic,
		Value
	};
	enum class FractalType : Uint8
	{
		None,
		FBM,
		Ridged,
		PingPong
	};
	struct HeightmapDesc
	{
		Uint32 width;
		Uint32 depth;
		Uint32 max_height;
		FractalType fractal_type = FractalType::None;
		NoiseType noise_type = NoiseType::Perlin;
		Int32 seed = 1337;
		Float persistence = 0.5f;
		Float lacunarity = 2.0f;
		Int32 octaves = 3;
		Float noise_scale = 10;
	};
	
	class Heightmap
	{
	public:	
		explicit Heightmap(HeightmapDesc const& desc);
		explicit Heightmap(std::string_view heightmap_path);

		Float HeightAt(Uint64 x, Uint64 z);
		Uint64 Width() const;
		Uint64 Depth() const;

	private:
		std::vector<std::vector<Float>> heightmap;
	};
}