#pragma once
#include "GfxShader.h"
#include "Utilities/Delegate.h"

namespace adria
{
	struct GfxInputLayout;

	struct GfxShaderCompileOutput
	{
		GfxShader shader;
		std::vector<std::string> includes;
		Uint64 shader_hash[2];
	};
	using GfxShaderCompileInput = GfxShaderDesc;

	namespace GfxShaderCompiler
	{
		void Initialize();
		void Destroy();
		Bool CompileShader(GfxShaderCompileInput const& input, GfxShaderCompileOutput& output);
		void ReadBlobFromFile(std::string const& filename, GfxShaderBlob& blob);
		void FillInputLayoutDesc(GfxShader const& vertex_shader, GfxInputLayout& input_layout);
	}
}