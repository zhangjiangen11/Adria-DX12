#include <d3dcompiler.h>
#include "dxcapi.h"
#include "cereal/archives/binary.hpp"
#include "cereal/types/string.hpp"
#include "cereal/types/vector.hpp"
#include "GfxShaderCompiler.h"
#include "GfxDefines.h"
#include "GfxInputLayout.h"
#include "Core/Paths.h"
#include "Core/FatalAssert.h"
#include "Utilities/StringConversions.h"
#include "Utilities/PathHelpers.h"
#include "Utilities/Hash.h"
#include "Utilities/Ref.h"
#include "Utilities/DynamicLibrary.h"

namespace adria
{
	ADRIA_LOG_CHANNEL(ShaderCompiler);

	using DxcCreateInstanceT = decltype(DxcCreateInstance);
	extern DxcCreateInstanceT* PFN_DxcCreateInstance = nullptr;

	namespace
	{
		Ref<IDxcLibrary> library = nullptr;
		Ref<IDxcCompiler3> compiler = nullptr;
		Ref<IDxcUtils> utils = nullptr;
		Ref<IDxcIncludeHandler> include_handler = nullptr;
		DynamicLibrary dxcompiler;
	}

	class GfxIncludeHandler : public IDxcIncludeHandler
	{
	public:
		GfxIncludeHandler() {}

		HRESULT STDMETHODCALLTYPE LoadSource(_In_ LPCWSTR pFilename, _COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource) override
		{
			Ref<IDxcBlobEncoding> encoding;
			std::string include_file = NormalizePath(ToString(pFilename));
			if (!FileExists(include_file))
			{
				*ppIncludeSource = nullptr;
				return E_FAIL;
			}

			Bool already_included = false;
			for (std::string const& included_file : include_files)
			{
				if (include_file == included_file)
				{
					already_included = true;
					break;
				}
			}

			if (already_included)
			{
				static const Char nullStr[] = " ";
				utils->CreateBlob(nullStr, ARRAYSIZE(nullStr), CP_UTF8, encoding.GetAddressOf());
				*ppIncludeSource = encoding.Detach();
				return S_OK;
			}

			std::wstring winclude_file = ToWideString(include_file);
			HRESULT hr = utils->LoadFile(winclude_file.c_str(), nullptr, encoding.GetAddressOf());
			if (SUCCEEDED(hr))
			{
				include_files.push_back(include_file);
				*ppIncludeSource = encoding.Detach();
				return S_OK;
			}
			else *ppIncludeSource = nullptr;
			return E_FAIL;
		}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject) override
		{
			return include_handler->QueryInterface(riid, ppvObject);
		}

		ULONG STDMETHODCALLTYPE AddRef(void) override { return 1; }
		ULONG STDMETHODCALLTYPE Release(void) override { return 1; }

		std::vector<std::string> include_files;
	};

	class GfxShaderCompilerBlob : public IDxcBlob
	{
	public:
		GfxShaderCompilerBlob(void const* pShaderBytecode, Uint64 byteLength) : bytecode_size{ byteLength }
		{
			bytecode = const_cast<void*>(pShaderBytecode);
		}
		virtual ~GfxShaderCompilerBlob() { /*non owning blob -> empty destructor*/ }
		virtual LPVOID STDMETHODCALLTYPE GetBufferPointer(void) override { return bytecode; }
		virtual SIZE_T STDMETHODCALLTYPE GetBufferSize(void) override { return bytecode_size; }
		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppv) override
		{
			if (ppv == NULL)
			{
				return E_POINTER;
			}
			if (riid == __uuidof(IDxcBlob)) //uuid(guid_str)
			{
				*ppv = static_cast<IDxcBlob*>(this);
			}
			else if (riid == IID_IUnknown)
			{
				*ppv = static_cast<IUnknown*>(this);
			}
			else
			{
				*ppv = NULL;
				return E_NOINTERFACE;
			}
			reinterpret_cast<IUnknown*>(*ppv)->AddRef();
			return S_OK;
		}
		virtual ULONG STDMETHODCALLTYPE AddRef(void) override { return 1; }
		virtual ULONG STDMETHODCALLTYPE Release(void) override { return 1; }

	private:
		LPVOID bytecode = nullptr;
		SIZE_T bytecode_size = 0;
	};

	
	inline constexpr std::wstring GetTarget(GfxShaderStage stage, GfxShaderModel model)
	{
		std::wstring target = L"";
		switch (stage)
		{
		case GfxShaderStage::VS:
			target += L"vs_";
			break;
		case GfxShaderStage::PS:
			target += L"ps_";
			break;
		case GfxShaderStage::CS:
			target += L"cs_";
			break;
		case GfxShaderStage::GS:
			target += L"gs_";
			break;
		case GfxShaderStage::HS:
			target += L"hs_";
			break;
		case GfxShaderStage::DS:
			target += L"ds_";
			break;
		case GfxShaderStage::LIB:
			target += L"lib_";
			break;
		case GfxShaderStage::MS:
			target += L"ms_";
			break;
		case GfxShaderStage::AS:
			target += L"as_";
			break;
		default:
			ADRIA_ASSERT(false && "Invalid Shader Stage");
		}
		switch (model)
		{
		case SM_6_0:
			target += L"6_0";
			break;
		case SM_6_1:
			target += L"6_1";
			break;
		case SM_6_2:
			target += L"6_2";
			break;
		case SM_6_3:
			target += L"6_3";
			break;
		case SM_6_4:
			target += L"6_4";
			break;
		case SM_6_5:
			target += L"6_5";
			break;
		case SM_6_6:
			target += L"6_6";
			break;
		case SM_6_7:
			target += L"6_7";
			break;
		case SM_6_8:
			target += L"6_8";
			break;
		default:
			break;
		}
		return target;
	}

	namespace GfxShaderCompiler
	{
		static Bool CheckCache(Char const* cache_path, GfxShaderCompileInput const& input, GfxShaderCompileOutput& output)
		{
			std::string cache_binary(cache_path); cache_binary += ".bin";
			std::string cache_metadata(cache_path); cache_metadata += ".meta";

			if (!FileExists(cache_binary) || !FileExists(cache_metadata)) return false;
			if (GetFileLastWriteTime(cache_binary) < GetFileLastWriteTime(input.file)) return false;

			std::ifstream is(cache_metadata, std::ios::binary);
			cereal::BinaryInputArchive metadata_archive(is);

			metadata_archive(output.shader_hash);
			metadata_archive(output.includes);
			Uint64 binary_size = 0;
			metadata_archive(binary_size);

			//check if the one of the includes was modified after the cache binary was generated
			for (std::string const& include : output.includes)
			{
				if (GetFileLastWriteTime(cache_binary) < GetFileLastWriteTime(include)) 
					return false;
			}

			std::ifstream is2(cache_binary, std::ios::binary);
			cereal::BinaryInputArchive binary_archive(is2);

			std::unique_ptr<Char[]> binary_data(new Char[binary_size]);
			binary_archive.loadBinary(binary_data.get(), binary_size);
			output.shader.SetShaderData(binary_data.get(), binary_size);
			output.shader.SetDesc(input);
			return true;
		}
		static Bool SaveToCache(Char const* cache_path, GfxShaderCompileOutput const& output)
		{
			std::string cache_metadata(cache_path); cache_metadata += ".meta";
			std::ofstream os(cache_metadata, std::ios::binary);
			cereal::BinaryOutputArchive metadata_archive(os);
			metadata_archive(output.shader_hash);
			metadata_archive(output.includes);
			metadata_archive(output.shader.GetSize());

			std::string cache_binary(cache_path); cache_binary += ".bin";
			std::ofstream os2(cache_binary, std::ios::binary);
			cereal::BinaryOutputArchive binary_archive(os2);
			binary_archive.saveBinary(output.shader.GetData(), output.shader.GetSize());

			return true;
		}

		void Initialize()
		{
			dxcompiler.Open("dxcompiler.dll");
			ADRIA_FATAL_ASSERT(dxcompiler.IsOpen(), "Couldn't open dxcompiler.dll!");
			Bool const success = dxcompiler.GetSymbol("DxcCreateInstance", &PFN_DxcCreateInstance);
			ADRIA_FATAL_ASSERT(success && PFN_DxcCreateInstance != nullptr, "Couldn't get DxcCreateInstance symbol from dxcompiler.dll!");

			GFX_CHECK_HR(PFN_DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(library.GetAddressOf())));
			GFX_CHECK_HR(PFN_DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())));
			GFX_CHECK_HR(library->CreateIncludeHandler(include_handler.GetAddressOf()));
			GFX_CHECK_HR(PFN_DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.GetAddressOf())));

			std::filesystem::create_directory(paths::ShaderPDBDir);
		}
		void Destroy()
		{
			include_handler.Reset();
			compiler.Reset();
			library.Reset();
			utils.Reset();
		}
		Bool CompileShader(GfxShaderCompileInput const& input, GfxShaderCompileOutput& output)
		{
			std::string define_key;
			for (GfxShaderDefine const& define : input.defines)
			{
				define_key += define.name;
				define_key += define.value;
			}
			Uint64 define_hash = crc64(define_key.c_str(), define_key.size());
			std::string build_string = input.flags & GfxShaderCompilerFlag_Debug ? "debug" : "release";
			Char cache_path[256];
			sprintf_s(cache_path, "%s%s_%s_%llx_%s", paths::ShaderCacheDir.c_str(), GetFilenameWithoutExtension(input.file).c_str(),
												     input.entry_point.c_str(), define_hash, build_string.c_str());

			if (CheckCache(cache_path, input, output)) return true;
			ADRIA_LOG(INFO, "Shader '%s.%s' not found in cache. Compiling...", input.file.c_str(), input.entry_point.c_str());

			compile:
			Uint32 code_page = CP_UTF8;
			Ref<IDxcBlobEncoding> source_blob;

			std::wstring shader_source = ToWideString(input.file);
			HRESULT hr = library->CreateBlobFromFile(shader_source.data(), &code_page, source_blob.GetAddressOf());
			GFX_CHECK_HR(hr);

			std::wstring name = ToWideString(GetFilenameWithoutExtension(input.file));
			std::wstring dir  = ToWideString(paths::ShaderDir);
			std::wstring path = ToWideString(GetParentPath(input.file));

			std::wstring target = GetTarget(input.stage, input.model);
			std::wstring entry_point = ToWideString(input.entry_point);
			if (entry_point.empty()) entry_point = L"main";

			std::vector<Wchar const*> compile_args{};
			compile_args.push_back(name.c_str());
			if (input.flags & GfxShaderCompilerFlag_Debug)
			{
				compile_args.push_back(DXC_ARG_DEBUG);
			}

			if (input.flags & GfxShaderCompilerFlag_DisableOptimization)
			{
				compile_args.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
			}
			else
			{
				compile_args.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);
			}
			compile_args.push_back(L"-HV 2021");

			compile_args.push_back(L"-E");
			compile_args.push_back(entry_point.c_str());
			compile_args.push_back(L"-T");
			compile_args.push_back(target.c_str());

			compile_args.push_back(L"-I");
			compile_args.push_back(dir.c_str());
			compile_args.push_back(L"-I");
			compile_args.push_back(path.c_str());

			std::vector<std::wstring> defines;
			defines.reserve(input.defines.size());
			for (GfxShaderDefine const& define : input.defines)
			{
				std::wstring define_name = ToWideString(define.name);
				std::wstring define_value = ToWideString(define.value);
				compile_args.push_back(L"-D");
				if (define.value.empty()) defines.push_back(define_name + L"=1");
				else defines.push_back(define_name + L"=" + define_value);
				compile_args.push_back(defines.back().c_str());
			}

			DxcBuffer source_buffer{};
			source_buffer.Ptr = source_blob->GetBufferPointer();
			source_buffer.Size = source_blob->GetBufferSize();
			source_buffer.Encoding = DXC_CP_ACP;
			GfxIncludeHandler custom_include_handler{};

			Ref<IDxcResult> result;
			hr = compiler->Compile(
				&source_buffer,
				compile_args.data(), (Uint32)compile_args.size(),
				&custom_include_handler,
				IID_PPV_ARGS(result.GetAddressOf()));

			Ref<IDxcBlobUtf8> errors;
			if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr)))
			{
				if (errors && errors->GetStringLength() > 0)
				{
					Char const* err_msg = errors->GetStringPointer();
					ADRIA_LOG(ERROR, "%s", err_msg);
					std::string msg = "Click OK after you fixed the following errors: \n";
					msg += err_msg;
					Int32 result = MessageBoxA(NULL, msg.c_str(), NULL, MB_OKCANCEL);
					if (result == IDOK) goto compile;
					else if (result == IDCANCEL) return false;
				}
			}
			
			Ref<IDxcBlob> blob;
			GFX_CHECK_HR(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(blob.GetAddressOf()), nullptr));
			
			if (input.flags & GfxShaderCompilerFlag_Debug)
			{
				Ref<IDxcBlob> pdb_blob;
				Ref<IDxcBlobUtf16> pdb_path_utf16;
				if (SUCCEEDED(result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(pdb_blob.GetAddressOf()), pdb_path_utf16.GetAddressOf())))
				{
					Ref<IDxcBlobUtf8> pdb_path_utf8;
					if (SUCCEEDED(utils->GetBlobAsUtf8(pdb_path_utf16.Get(), pdb_path_utf8.GetAddressOf())))
					{
						Char pdb_path[256];
						sprintf_s(pdb_path, "%s%s", paths::ShaderPDBDir.c_str(), pdb_path_utf8->GetStringPointer());
						FILE* pdb_file = nullptr;
						fopen_s(&pdb_file, pdb_path, "wb");
						if (pdb_file)
						{
							fwrite(pdb_blob->GetBufferPointer(), pdb_blob->GetBufferSize(), 1, pdb_file);
							fclose(pdb_file);
						}
					}
				}
			}
			Ref<IDxcBlob> hash;
			if (SUCCEEDED(result->GetOutput(DXC_OUT_SHADER_HASH, IID_PPV_ARGS(hash.GetAddressOf()), nullptr)))
			{
				DxcShaderHash* hash_buf = (DxcShaderHash*)hash->GetBufferPointer();
				memcpy(output.shader_hash, hash_buf->HashDigest, sizeof(Uint64) * 2);
			}

			output.shader.SetDesc(input);
			output.shader.SetShaderData(blob->GetBufferPointer(), blob->GetBufferSize());
			output.includes = std::move(custom_include_handler.include_files);
			output.includes.push_back(input.file);
			SaveToCache(cache_path, output);
			return true;
		}
		void ReadBlobFromFile(std::string const& filename, GfxShaderBlob& blob)
		{
			std::wstring wide_filename = ToWideString(filename);
			Uint32 code_page = CP_UTF8;
			Ref<IDxcBlobEncoding> source_blob;
			HRESULT hr = library->CreateBlobFromFile(wide_filename.data(), &code_page, source_blob.GetAddressOf());
			GFX_CHECK_HR(hr);
			blob.resize(source_blob->GetBufferSize());
			memcpy(blob.data(), source_blob->GetBufferPointer(), source_blob->GetBufferSize());
		}
		void FillInputLayoutDesc(GfxShader const& vertex_shader, GfxInputLayout& input_layout)
		{
			ADRIA_ASSERT_MSG(PFN_DxcCreateInstance != nullptr, "Cannot do dxc reflection without PFN_DxcCreateInstance!");
			Ref<IDxcContainerReflection> reflection;
			HRESULT hr = PFN_DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(reflection.GetAddressOf()));
			GfxShaderCompilerBlob my_blob{ vertex_shader.GetData(), vertex_shader.GetSize() };
			GFX_CHECK_HR(hr);
			hr = reflection->Load(&my_blob);
			GFX_CHECK_HR(hr);
			Uint32 part_index;
#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d) (Uint)((Uchar)(a) | (Uchar)(b) << 8 | (Uchar)(c) << 16 | (Uchar)(d) << 24)
#endif
			GFX_CHECK_HR(reflection->FindFirstPartKind(MAKEFOURCC('D', 'X', 'I', 'L'), &part_index));
#undef MAKEFOURCC

			Ref<ID3D12ShaderReflection> vertex_shader_reflection;
			GFX_CHECK_HR(reflection->GetPartReflection(part_index, IID_PPV_ARGS(vertex_shader_reflection.GetAddressOf())));

			D3D12_SHADER_DESC shader_desc;
			GFX_CHECK_HR(vertex_shader_reflection->GetDesc(&shader_desc));

			D3D12_SIGNATURE_PARAMETER_DESC signature_param_desc{};
			input_layout.elements.clear();
			input_layout.elements.resize(shader_desc.InputParameters);
			for (Uint32 i = 0; i < shader_desc.InputParameters; i++)
			{
				vertex_shader_reflection->GetInputParameterDesc(i, &signature_param_desc);
				input_layout.elements[i].semantic_name = std::string(signature_param_desc.SemanticName);
				input_layout.elements[i].semantic_index = signature_param_desc.SemanticIndex;
				input_layout.elements[i].aligned_byte_offset = GfxInputLayout::APPEND_ALIGNED_ELEMENT;
				input_layout.elements[i].input_slot_class = GfxInputClassification::PerVertexData;
				input_layout.elements[i].input_slot = 0;

				if (input_layout.elements[i].semantic_name.starts_with("INSTANCE"))
				{
					input_layout.elements[i].input_slot_class = GfxInputClassification::PerInstanceData;
					input_layout.elements[i].input_slot = 1;
				}

				if (signature_param_desc.Mask == 1)
				{
					if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)	   input_layout.elements[i].format = GfxFormat::R32_UINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)  input_layout.elements[i].format = GfxFormat::R32_SINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) input_layout.elements[i].format = GfxFormat::R32_FLOAT;
				}
				else if (signature_param_desc.Mask <= 3)
				{
					if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)	   input_layout.elements[i].format = GfxFormat::R32G32_UINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)  input_layout.elements[i].format = GfxFormat::R32G32_SINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) input_layout.elements[i].format = GfxFormat::R32G32_FLOAT;
				}
				else if (signature_param_desc.Mask <= 7)
				{
					if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)	   input_layout.elements[i].format = GfxFormat::R32G32B32_UINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)  input_layout.elements[i].format = GfxFormat::R32G32B32_SINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) input_layout.elements[i].format = GfxFormat::R32G32B32_FLOAT;
				}
				else if (signature_param_desc.Mask <= 15)
				{
					if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)	   input_layout.elements[i].format = GfxFormat::R32G32B32A32_UINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)  input_layout.elements[i].format = GfxFormat::R32G32B32A32_SINT;
					else if (signature_param_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32) input_layout.elements[i].format = GfxFormat::R32G32B32A32_FLOAT;
				}
			}
		}

	}
}