#pragma once

#include "pch_engine.h"
#include "../../Graph/EffectNode.h"
#include "../../Effects/BytecodeCache.h"
#include "../../Effects/ShaderCompiler.h"
#include "ComputeDispatch.h"

// Which compiled build of a shader a node runs: the generic baseline, or a
// variant specialised for its GPU-served bindings and option values. Owns the
// content-addressed memos (variant bytecode, reflection) shared by every node;
// a node's own variant state lives in its NodeVariantState.
namespace ShaderLab::Rendering::Evaluator
{
    // A bytecode's identity: the DXBC checksum (bytes 4..19, written by the
    // compiler), or a hash for anything that is not DXBC. A recompile
    // produces a new identity, so memos keyed by it never go stale.
    using BytecodeId = std::array<uint8_t, 16>;
    BytecodeId IdOfBytecode(const std::vector<uint8_t>& bytes);

    // Fetch-or-compile one build through the BytecodeCache. Async takes what
    // the cache has, or queues the compile and reports Pending.
    // A prebuilt source (an effect blob) is only looked up unless
    // request.allowPrebuiltCompile is set, in either mode.
    Effects::BytecodeCacheResult CompileViaCache(Effects::BytecodeCompileRequest request, bool async,
                                                 bool urgent = false, Effects::BytecodeCompileKey* outKey = nullptr);

    enum class BaselineStatus { Compiled, Pending, Failed };

    // Compiles the node's generic build (urgent: without it the node has no
    // output) into its definition, then queues its GPU-binding shapes so a
    // binding wired later finds its shader ready. Pending sets
    // node.compilePending; Failed leaves the compiler's message in `error`.
    BaselineStatus CompileBaseline(Graph::EffectNode& node, const std::string& target, bool async,
                                   bool compilePrebuilt, std::wstring& error);

    // Eager compile of every option combination for one node: what it was
    // queued for, and the keys, for the status count.
    struct OptionPrecompile
    {
        BytecodeId baseline{};                   // the baseline it was queued for
        uint32_t gpuBits{ 0 };                   // the GPU-binding shape it was built for
        std::vector<std::string> optionNames;    // the specialised options at the time
        std::vector<Effects::BytecodeCompileKey> keys;
        bool done{ false };
    };

    // One node's variant state.
    struct NodeVariantState
    {
        // Identity of the pixel-shader bytecode last loaded. A different
        // wanted bytecode forces a reload under its own GUID.
        std::optional<BytecodeId> loadedPixelVariant;
        // GPU-binding bits of the build the node last ran (2 bits per
        // gpu-bindable parameter). A fallback build clears them, and its
        // bindings then need the CPU readback.
        uint32_t appliedGpuBits{ 0 };
        std::optional<OptionPrecompile> optionPrecompile;
        // Set while the node renders a fallback and its wanted build compiles.
        std::optional<Effects::BytecodeCompileKey> wanted;
    };

    // The build chosen for (bits, options). bits and options come back as the
    // build actually chosen; bytes is nullptr for the generic baseline.
    struct VariantChoice
    {
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        uint32_t bits{ 0 };
        uint64_t options{ 0 };
    };

    class VariantSelector
    {
    public:
        // Compile on the cache's worker threads (GraphEvaluator::SetAsyncCompile).
        void SetAsyncCompile(bool on) { m_asyncCompile = on; }
        bool AsyncCompile() const { return m_asyncCompile; }
        // EvaluatorOptions::compilePrebuiltEffects, set each frame.
        void SetCompilePrebuilt(bool on) { m_compilePrebuilt = on; }
        bool CompilePrebuilt() const { return m_compilePrebuilt; }

        // The best ready build for (bits, options), falling back to drop the
        // options, then the GPU routing. A build still compiling is recorded
        // in state.wanted, and Poll re-dirties the node when it finishes.
        VariantChoice Select(Graph::EffectNode& node, NodeVariantState& state,
                             const std::wstring& effectId, uint32_t effectVersion,
                             const std::string& target, uint32_t bits, uint64_t options);

        // Queues every option combination of the node's shader for a GPU
        // binding shape (interactive hosts only), the one it shows now first.
        void QueueOptionVariants(Graph::EffectNode& node, NodeVariantState& state,
                                 const std::string& target, uint32_t gpuBits);

        // Once per frame: re-dirties the node when its wanted build finished
        // and publishes its variantsCompiling count.
        static void Poll(Graph::EffectNode& node, NodeVariantState& state);

        // Reflection, memoized per bytecode identity.
        std::shared_ptr<const ReflectedComputeCb> ReflectCompute(const std::vector<uint8_t>& bytes);
        std::shared_ptr<const Effects::ShaderReflectionResult> ReflectPixel(const std::vector<uint8_t>& bytes);

    private:
        // The build for (def, bits, options), compiling through the cache on
        // the first request. nullptr if not ready; a pending compile's key
        // goes to pendingKey if that is unset.
        std::shared_ptr<const std::vector<uint8_t>> GetBuild(
            const Graph::CustomEffectDefinition& def, const std::wstring& effectId,
            uint32_t effectVersion, const std::string& target, uint32_t bits,
            uint64_t options, bool urgent, std::optional<Effects::BytecodeCompileKey>& pendingKey);

        struct BuildKey
        {
            BytecodeId baseline{};
            uint32_t   bits{ 0 };      // GPU-binding modes
            uint64_t   options{ 0 };   // option key (ShaderVariants.h)
            bool operator<(const BuildKey& o) const
            {
                if (baseline != o.baseline) return baseline < o.baseline;
                if (bits != o.bits) return bits < o.bits;
                return options < o.options;
            }
        };

        bool m_asyncCompile{ false };
        bool m_compilePrebuilt{ false };
        std::map<BuildKey, std::shared_ptr<const std::vector<uint8_t>>> m_builds;
        std::map<BytecodeId, std::shared_ptr<const ReflectedComputeCb>> m_computeReflection;
        std::map<BytecodeId, std::shared_ptr<const Effects::ShaderReflectionResult>> m_pixelReflection;
    };
}
