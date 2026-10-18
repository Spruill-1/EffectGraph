#include "pch_engine.h"
#include "HlslScaffold.h"
#include "ShaderVariants.h"
#include "../Engine/Utf8.h"

namespace ShaderLab::Effects
{
    namespace
    {
        bool IsSpecialisedOption(const Graph::CustomEffectDefinition& def, const std::wstring& name)
        {
            const std::string utf8Name = WideToUtf8(name);
            for (const auto& option : SpecializedOptionNames(def))
                if (option == utf8Name) return true;
            return false;
        }

        // One cbuffer member. A specialised option goes through SHADERLAB_OPTION,
        // which keeps its slot, and so every later member's offset, in each variant.
        std::wstring CbufferMember(const Graph::CustomEffectDefinition& def,
                                   const Graph::ParameterDefinition& p)
        {
            if (IsSpecialisedOption(def, p.name))
                return L"    SHADERLAB_OPTION(" + p.typeName + L", " + p.name + L")  // one variant per option\n";
            return L"    " + p.typeName + L" " + p.name + L";\n";
        }

        // Emitted after the cbuffer: each specialised option's value, a constant
        // in its variants and nothing in the generic build.
        std::wstring OptionValues(const Graph::CustomEffectDefinition& def)
        {
            std::wstring out;
            for (const auto& p : def.parameters)
                if (IsSpecialisedOption(def, p.name))
                    out += L"SHADERLAB_OPTION_VALUE(" + p.typeName + L", " + p.name + L")\n";
            return out.empty() ? out : out + L"\n";
        }
    }

    std::wstring GenerateEffectScaffold(const Graph::CustomEffectDefinition& def)
    {
        // Both engine headers are optional: an author may delete either
        // include and paste the code in instead. Specialised options need
        // the params header (SHADERLAB_OPTION).
        std::wstring hlsl;
        hlsl += L"#include \"shaderlab_colormath.hlsli\"   // shared color math (optional)\n";
        hlsl += L"#include \"shaderlab_params.hlsli\"      // parameter macros (optional)\n\n";

        // A pixel shader's constant buffer holds the parameters alone; the
        // compute shapes below declare theirs after the values the host injects.
        if (def.shaderType == Graph::CustomShaderType::PixelShader && !def.parameters.empty())
        {
            hlsl += L"cbuffer Constants : register(b0)\n{\n";
            for (const auto& p : def.parameters)
            {
                hlsl += CbufferMember(def, p);
            }
            hlsl += L"};\n\n";
            hlsl += OptionValues(def);
        }

        if (def.shaderType == Graph::CustomShaderType::PixelShader)
        {
            // Texture inputs.
            for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            {
                hlsl += std::format(L"Texture2D {} : register(t{});\n", def.inputNames[i], i);
            }
            if (!def.inputNames.empty())
            {
                hlsl += L"SamplerState InputSampler : register(s0);\n";
                hlsl += L"\n// SCENE_POSITION is mandatory. D2D pixel shaders take\n";
                hlsl += L"// (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), ONE TEXCOORD PER INPUT.\n";
                hlsl += L"// Omit the middle parameter and the binding shifts, so what you label\n";
                hlsl += L"// TEXCOORD0 receives the SCENE coordinate instead of input 0 texel\n";
                hlsl += L"// coords -- correct-looking until the source is smaller than the D2D\n";
                hlsl += L"// intermediate, then displaced by -(intermediate - content)/2.\n";
                hlsl += L"// uv0 is NORMALIZED: sample with Sample(); pixel coords come from\n";
                hlsl += L"// scenePos.\n\n";
            }

            // Entry point: SV_POSITION, SCENE_POSITION, then one TEXCOORD per
            // input. The inputs share the output's rect, not a coordinate, so
            // each keeps its own; with no inputs the signature ends at
            // SCENE_POSITION, as the built-in generators' does.
            hlsl += L"float4 main(\n";
            hlsl += L"    float4 pos      : SV_POSITION,\n";
            hlsl += L"    float4 scenePos : SCENE_POSITION";
            for (uint32_t ti = 0; ti < def.inputNames.size(); ++ti)
                hlsl += std::format(L",\n    float4 uv{0}      : TEXCOORD{0}", ti);
            hlsl += L") : SV_TARGET\n";
            hlsl += L"{\n";

            if (def.inputNames.size() >= 1)
            {
                for (uint32_t i = 0; i < def.inputNames.size(); ++i)
                {
                    hlsl += std::format(L"    float4 color{0} = {1}.Sample(InputSampler, uv{0}.xy);\n",
                        i, def.inputNames[i]);
                }

                hlsl += L"\n";
                if (def.inputNames.size() == 1)
                {
                    hlsl += L"    // Your code here\n\n";
                    hlsl += L"    return color0;\n";
                }
                else
                {
                    hlsl += L"    // Your code here -- blend, combine, or process the inputs\n\n";
                    hlsl += L"    return color0;\n";
                }
            }
            else
            {
                hlsl += L"    // Your code here (no inputs)\n";
                hlsl += L"    return float4(0, 0, 0, 1);\n";
            }
            hlsl += L"}\n";
        }
        else if (def.shaderType == Graph::CustomShaderType::D3D11ComputeShader)
        {
            // D3D11 compute shader — dispatched outside D2D tiling.
            // Single dispatch(1,1,1) with stride pattern over the full image.
            hlsl += L"// D3D11 Compute Shader — runs outside D2D's tiling system.\n";
            hlsl += L"// Single dispatch(1,1,1) — one thread group strides over the full image.\n";
            hlsl += L"// Width and Height are auto-injected at cbuffer offset 0.\n\n";

            // Texture input
            if (!def.inputNames.empty())
                hlsl += std::format(L"Texture2D<float4> {} : register(t0);\n", def.inputNames[0]);
            else
                hlsl += L"Texture2D<float4> Source : register(t0);\n";

            // Output structured buffer
            hlsl += L"RWStructuredBuffer<float4> Result : register(u0);\n\n";

            // Cbuffer with Width/Height first
            hlsl += L"cbuffer Constants : register(b0)\n{\n";
            hlsl += L"    uint Width;   // Auto-injected: source image width\n";
            hlsl += L"    uint Height;  // Auto-injected: source image height\n";
            for (const auto& p : def.parameters)
                hlsl += CbufferMember(def, p);
            hlsl += L"};\n\n";
            hlsl += OptionValues(def);

            // Analysis field layout comments
            uint32_t pixOff = 0;
            for (const auto& fd : def.analysisFields)
            {
                std::wstring typeTag;
                switch (fd.type)
                {
                case Graph::AnalysisFieldType::Float:      typeTag = L"float"; break;
                case Graph::AnalysisFieldType::Float2:     typeTag = L"float2"; break;
                case Graph::AnalysisFieldType::Float3:     typeTag = L"float3"; break;
                case Graph::AnalysisFieldType::Float4:     typeTag = L"float4"; break;
                case Graph::AnalysisFieldType::FloatArray:  typeTag = L"float[]"; break;
                case Graph::AnalysisFieldType::Float2Array: typeTag = L"float2[]"; break;
                case Graph::AnalysisFieldType::Float3Array: typeTag = L"float3[]"; break;
                case Graph::AnalysisFieldType::Float4Array: typeTag = L"float4[]"; break;
                }
                hlsl += std::format(L"// Result[{}]: {} {} ({})\n",
                    pixOff, typeTag, fd.name,
                    Graph::AnalysisFieldIsArray(fd.type)
                        ? std::format(L"{} elements", fd.pixelCount())
                        : L"1 element");
                pixOff += fd.pixelCount();
            }
            if (pixOff > 0) hlsl += L"\n";

            // Groupshared + main
            std::wstring srcName = def.inputNames.empty() ? L"Source" : def.inputNames[0];
            hlsl += L"// Groupshared accumulators for parallel reduction.\n";
            hlsl += L"groupshared float4 gs_sum[32 * 32];\n\n";

            hlsl += L"[numthreads(32, 32, 1)]\n";
            hlsl += L"void main(uint3 GTid : SV_GroupThreadID)\n";
            hlsl += L"{\n";
            hlsl += L"    uint tid = GTid.y * 32 + GTid.x;\n";
            hlsl += L"    float4 acc = float4(0, 0, 0, 0);\n\n";

            hlsl += L"    // Stride over entire image — each thread covers many pixels.\n";
            hlsl += L"    for (uint y = GTid.y; y < Height; y += 32)\n";
            hlsl += L"    {\n";
            hlsl += L"        for (uint x = GTid.x; x < Width; x += 32)\n";
            hlsl += L"        {\n";
            hlsl += std::format(L"            float4 pixel = {}.Load(int3(x, y, 0));\n", srcName);
            hlsl += L"            // TODO: accumulate your statistics here\n";
            hlsl += L"            acc += pixel;\n";
            hlsl += L"        }\n";
            hlsl += L"    }\n\n";

            hlsl += L"    // Parallel reduction in groupshared memory.\n";
            hlsl += L"    gs_sum[tid] = acc;\n";
            hlsl += L"    GroupMemoryBarrierWithGroupSync();\n\n";

            hlsl += L"    for (uint s = (32 * 32) / 2; s > 0; s >>= 1)\n";
            hlsl += L"    {\n";
            hlsl += L"        if (tid < s)\n";
            hlsl += L"            gs_sum[tid] += gs_sum[tid + s];\n";
            hlsl += L"        GroupMemoryBarrierWithGroupSync();\n";
            hlsl += L"    }\n\n";

            hlsl += L"    // Thread 0 writes final results.\n";
            hlsl += L"    if (tid == 0)\n";
            hlsl += L"    {\n";
            hlsl += L"        float totalPixels = float(Width * Height);\n";
            hlsl += L"        float4 mean = gs_sum[0] / max(totalPixels, 1.0);\n";

            if (!def.analysisFields.empty())
            {
                pixOff = 0;
                for (const auto& fd : def.analysisFields)
                {
                    hlsl += std::format(L"        Result[{}] = mean; // {} — replace with your value\n",
                        pixOff, fd.name);
                    pixOff += fd.pixelCount();
                }
            }
            else
            {
                hlsl += L"        Result[0] = mean;\n";
            }
            hlsl += L"    }\n";
            hlsl += L"}\n";
        }
        else
        {
            for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            {
                hlsl += std::format(L"Texture2D {} : register(t{});\n", def.inputNames[i], i);
            }
            hlsl += L"RWTexture2D<float4> Output : register(u0);\n";
            if (!def.inputNames.empty())
                hlsl += L"SamplerState Sampler0 : register(s0);\n";
            hlsl += L"\n";

            if (def.analysisOutputType == Graph::AnalysisOutputType::Typed)
            {
                // Analysis compute shader: reads entire input, writes summary stats
                // to the output row. Each field occupies a known pixel offset.
                hlsl += L"// Analysis compute shader pattern:\n";
                hlsl += L"// The output texture is used as a data buffer.\n";
                hlsl += L"// Write results to Output[int2(pixelOffset, 0)].\n";
                hlsl += L"// The host reads typed fields back after evaluation.\n\n";

                hlsl += L"cbuffer Constants : register(b0)\n{\n";
                hlsl += L"    int2  _TileOffset;  // Auto-injected: tile origin in full image\n";
                for (const auto& p : def.parameters)
                    hlsl += CbufferMember(def, p);
                hlsl += L"};\n\n";
                hlsl += OptionValues(def);

                // Generate field pixel offset comments.
                uint32_t pixOff = 0;
                for (const auto& fd : def.analysisFields)
                {
                    std::wstring typeTag;
                    switch (fd.type)
                    {
                    case Graph::AnalysisFieldType::Float:       typeTag = L"float"; break;
                    case Graph::AnalysisFieldType::Float2:      typeTag = L"float2"; break;
                    case Graph::AnalysisFieldType::Float3:      typeTag = L"float3"; break;
                    case Graph::AnalysisFieldType::Float4:      typeTag = L"float4"; break;
                    case Graph::AnalysisFieldType::FloatArray:   typeTag = L"float[]"; break;
                    case Graph::AnalysisFieldType::Float2Array:  typeTag = L"float2[]"; break;
                    case Graph::AnalysisFieldType::Float3Array:  typeTag = L"float3[]"; break;
                    case Graph::AnalysisFieldType::Float4Array:  typeTag = L"float4[]"; break;
                    }
                    hlsl += std::format(L"// pixel {}: {} {} ({})\n",
                        pixOff, typeTag, fd.name,
                        Graph::AnalysisFieldIsArray(fd.type)
                            ? std::format(L"{} pixels", fd.pixelCount())
                            : L"1 pixel");
                    pixOff += fd.pixelCount();
                }
                hlsl += L"\n";

                hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                    def.threadGroupX, def.threadGroupY, def.threadGroupZ);
                hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
                hlsl += L"{\n";

                if (!def.inputNames.empty())
                {
                    hlsl += L"    // Get full source dimensions and compute global position.\n";
                    hlsl += L"    uint srcW, srcH;\n";
                    hlsl += std::format(L"    {}.GetDimensions(srcW, srcH);\n", def.inputNames[0]);
                    hlsl += L"    uint2 globalPos = DTid.xy + uint2(_TileOffset);\n";
                    hlsl += L"    if (globalPos.x >= srcW || globalPos.y >= srcH) return;\n\n";
                    hlsl += L"    // Sample input using normalized UVs (Load() not available in D2D compute).\n";
                    hlsl += L"    float2 uv = (float2(globalPos) + 0.5) / float2(srcW, srcH);\n";
                    hlsl += std::format(L"    float4 pixel = {}.SampleLevel(Sampler0, uv, 0);\n\n", def.inputNames[0]);
                }

                hlsl += L"    // TODO: Accumulate statistics across pixels.\n";
                hlsl += L"    // For whole-image statistics, use groupshared memory and atomic ops,\n";
                hlsl += L"    // or output per-pixel data and reduce on the CPU.\n\n";

                hlsl += L"    // Write results to known pixel locations.\n";
                pixOff = 0;
                for (const auto& fd : def.analysisFields)
                {
                    hlsl += std::format(L"    // Output[int2({}, 0)] = ...; // {}\n", pixOff, fd.name);
                    pixOff += fd.pixelCount();
                }
                if (def.analysisFields.empty())
                    hlsl += L"    // Output[int2(0, 0)] = float4(result, 0, 0, 0);\n";

                hlsl += L"}\n";
            }
            else
            {
                // Image-processing compute shader (passthrough scaffold).
                hlsl += L"// Image-processing compute shader:\n";
                hlsl += L"// D2D evaluates compute effects in tiles. _TileOffset is auto-injected\n";
                hlsl += L"// per tile so the shader sees correct global coordinates.\n";
                hlsl += L"// Use Source.GetDimensions() for the full image size.\n";
                hlsl += L"// Use SampleLevel() with normalized UVs (Load() not available in D2D compute).\n\n";

                hlsl += L"cbuffer Constants : register(b0)\n{\n";
                hlsl += L"    int2  _TileOffset;  // Auto-injected: tile origin in full image\n";
                for (const auto& p : def.parameters)
                    hlsl += CbufferMember(def, p);
                hlsl += L"};\n\n";
                hlsl += OptionValues(def);

                hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                    def.threadGroupX, def.threadGroupY, def.threadGroupZ);
                hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
                hlsl += L"{\n";
                if (!def.inputNames.empty())
                {
                    hlsl += L"    // Get full source dimensions and compute global position.\n";
                    hlsl += L"    uint srcW, srcH;\n";
                    hlsl += std::format(L"    {}.GetDimensions(srcW, srcH);\n", def.inputNames[0]);
                    hlsl += L"    uint2 globalPos = DTid.xy + uint2(_TileOffset);\n";
                    hlsl += L"    if (globalPos.x >= srcW || globalPos.y >= srcH) return;\n\n";
                    hlsl += L"    // Sample input using normalized UVs.\n";
                    hlsl += L"    float2 uv = (float2(globalPos) + 0.5) / float2(srcW, srcH);\n";
                    hlsl += std::format(L"    float4 color = {}.SampleLevel(Sampler0, uv, 0);\n", def.inputNames[0]);
                    hlsl += L"\n    // Your code here\n\n";
                    hlsl += L"    Output[DTid.xy] = color;\n";
                }
                else
                {
                    hlsl += L"    // Your code here\n";
                    hlsl += L"    Output[DTid.xy] = float4(0, 0, 0, 1);\n";
                }
                hlsl += L"}\n";
            }
        }

        return hlsl;
    }
}
