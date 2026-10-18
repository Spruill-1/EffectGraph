#include "pch_engine.h"
#include "VariantSelector.h"
#include "../../Effects/ShaderVariants.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        // Memos are content-addressed, so dropping one only costs a lookup.
        constexpr size_t cMaxMemoEntries = 256;
    }

    BytecodeId IdOfBytecode(const std::vector<uint8_t>& b)
    {
        BytecodeId id{};
        if (b.size() >= 20 && b[0] == 'D' && b[1] == 'X' && b[2] == 'B' && b[3] == 'C')
        {
            std::memcpy(id.data(), b.data() + 4, id.size());
            return id;
        }
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
        std::memcpy(id.data(), &h, sizeof(h));
        const uint64_t sz = b.size();
        std::memcpy(id.data() + 8, &sz, sizeof(sz));
        return id;
    }

    Effects::BytecodeCacheResult CompileViaCache(Effects::BytecodeCompileRequest request, bool async,
                                                 bool urgent, Effects::BytecodeCompileKey* outKey)
    {
        if (outKey) *outKey = request.key;
        auto& cache = Effects::BytecodeCache::Instance();
        // Answers at once for a prebuilt source: its build, or a refusal.
        if (!request.allowPrebuiltCompile && cache.IsPrebuiltSource(request.key.sourceHash))
            return cache.GetOrCompile(std::move(request));
        if (async)
        {
            auto existing = cache.TryGet(request.key);
            if (existing.status == Effects::BytecodeStatus::Ready ||
                existing.status == Effects::BytecodeStatus::Failed)
                return existing;
            cache.RequestCompile(std::move(request), urgent);   // idempotent while Pending
            Effects::BytecodeCacheResult pending;
            pending.status = Effects::BytecodeStatus::Pending;
            return pending;
        }
        return cache.GetOrCompile(std::move(request));
    }

    BaselineStatus CompileBaseline(EffectNode& node, const std::string& target, bool async, bool compilePrebuilt,
                                   std::wstring& error)
    {
        auto& def = node.customEffect.value();
        const auto identity = Effects::CompileIdentityOf(node);
        // Option variants are queued later, once the GPU bindings are known.
        auto request = Effects::MakeVariantRequest(def, identity.effectId, identity.effectVersion, target, 0, 0);
        request.allowPrebuiltCompile = compilePrebuilt;
        auto cached = CompileViaCache(std::move(request), async, /*urgent*/ true);
        if (cached.status == Effects::BytecodeStatus::Pending)
        {
            node.compilePending = true;
            return BaselineStatus::Pending;
        }
        node.compilePending = false;
        if (cached.status != Effects::BytecodeStatus::Ready)
        {
            error = std::move(cached.errorMessage);
            return BaselineStatus::Failed;
        }
        def.compiledBytecode = std::move(cached.bytecode);
        Effects::PrecompileBindingShapes(Effects::BytecodeCache::Instance(),
            def, identity.effectId, identity.effectVersion, target);
        return BaselineStatus::Compiled;
    }

    std::shared_ptr<const std::vector<uint8_t>> VariantSelector::GetBuild(
        const CustomEffectDefinition& def, const std::wstring& effectId, uint32_t effectVersion,
        const std::string& target, uint32_t bits, uint64_t options, bool urgent,
        std::optional<Effects::BytecodeCompileKey>& pendingKey)
    {
        if (def.compiledBytecode.empty()) return nullptr;
        // Keyed by the BASELINE's identity: a build is a pure function of the
        // source the baseline was compiled from plus bits and options, so a
        // recompile can never be served an old build.
        const BuildKey key{ IdOfBytecode(def.compiledBytecode), bits, options };
        auto it = m_builds.find(key);
        if (it != m_builds.end()) return it->second;

        Effects::BytecodeCompileKey cacheKey;
        auto request = Effects::MakeVariantRequest(def, effectId, effectVersion, target, bits, options);
        request.allowPrebuiltCompile = m_compilePrebuilt;
        auto variant = CompileViaCache(std::move(request), m_asyncCompile, urgent, &cacheKey);
        if (variant.status == Effects::BytecodeStatus::Pending && !pendingKey)
            pendingKey = cacheKey;
        if (variant.status != Effects::BytecodeStatus::Ready || variant.bytecode.empty())
            return nullptr;   // not memoized: a background compile may finish later
        if (m_builds.size() > cMaxMemoEntries) m_builds.clear();
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(variant.bytecode));
        m_builds[key] = bytes;
        return bytes;
    }

    VariantChoice VariantSelector::Select(
        EffectNode& node, NodeVariantState& state, const std::wstring& effectId, uint32_t effectVersion,
        const std::string& target, uint32_t bits, uint64_t options)
    {
        const auto& def = node.customEffect.value();
        // The wanted build is urgent; fallbacks queue behind it. The node is
        // re-dirtied when the first pending one in this order finishes.
        std::optional<Effects::BytecodeCompileKey> pendingKey;
        VariantChoice choice{ GetBuild(def, effectId, effectVersion, target, bits, options,
                                       /*urgent*/ true, pendingKey),
                              bits, options };
        // Keep the GPU routing without the options.
        if (!choice.bytes && options != 0 && bits != 0)
        {
            choice.bytes = GetBuild(def, effectId, effectVersion, target, bits, 0, /*urgent*/ false, pendingKey);
            if (choice.bytes) choice.options = 0;
        }
        // Drop the GPU routing, keeping the options.
        if (!choice.bytes && bits != 0)
        {
            choice.bits = 0;
            if (options != 0)
                choice.bytes = GetBuild(def, effectId, effectVersion, target, 0, options, /*urgent*/ false, pendingKey);
        }
        if (pendingKey)
            state.wanted = *pendingKey;
        // Nothing specialised is ready: the generic baseline.
        if (!choice.bytes || choice.bytes->empty())
            choice.options = 0;
        return choice;
    }

    void VariantSelector::QueueOptionVariants(EffectNode& node, NodeVariantState& state,
                                              const std::string& target, uint32_t gpuBits)
    {
        // Interactive hosts only; headless compiles just what it renders.
        if (!m_asyncCompile || !node.customEffect.has_value()) return;
        const auto& def = node.customEffect.value();
        // Wait for the baseline so it never queues behind these.
        if (def.compiledBytecode.empty()) return;
        // The baseline's identity changes with any edit that changes the
        // compiled shader. Toggling a parameter's "specialize" flag leaves it
        // unchanged, so the specialised names are part of the check too.
        const BytecodeId baseline = IdOfBytecode(def.compiledBytecode);
        auto optionNames = Effects::SpecializedOptionNames(def);
        if (!state.optionPrecompile) state.optionPrecompile.emplace();
        auto& precompile = *state.optionPrecompile;
        if (precompile.baseline == baseline && precompile.gpuBits == gpuBits &&
            precompile.optionNames == optionNames &&
            (precompile.done || !precompile.keys.empty()))
            return;   // already queued for this baseline, binding shape and option set
        precompile = OptionPrecompile{};
        precompile.baseline    = baseline;
        precompile.gpuBits     = gpuBits;
        precompile.optionNames = optionNames;
        node.variantsCompiling = 0;

        auto optionKeys = Effects::AllOptionKeys(def);
        if (optionKeys.empty()) { precompile.done = true; return; }   // nothing specialised, or too many
        // The combination the node shows now goes first.
        const uint64_t currentKey = Effects::OptionKeyFor(def, node.properties);
        std::stable_partition(optionKeys.begin(), optionKeys.end(),
            [&](uint64_t optionKey) { return optionKey == currentKey; });

        const std::string canonical = Effects::CanonicalizeHlslSource(def.hlslSource);
        const auto identity = Effects::CompileIdentityOf(node);
        auto& cache = Effects::BytecodeCache::Instance();
        for (uint64_t optionKey : optionKeys)
        {
            auto request = Effects::MakeVariantRequest(def, canonical, identity.effectId,
                                                       identity.effectVersion, target, gpuBits, optionKey);
            precompile.keys.push_back(request.key);
            // TryGet first: it loads from the disk cache, where RequestCompile would recompile.
            if (cache.TryGet(request.key).status == Effects::BytecodeStatus::NotRequested)
                cache.RequestCompile(std::move(request), /*urgent*/ optionKey == currentKey);
        }
    }

    void VariantSelector::Poll(EffectNode& node, NodeVariantState& state)
    {
        auto& cache = Effects::BytecodeCache::Instance();
        // Ready, failed or evicted: evaluate the node again.
        if (state.wanted && cache.GetStatus(*state.wanted) != Effects::BytecodeStatus::Pending)
        {
            node.dirty = true;
            state.wanted.reset();
        }
        if (state.optionPrecompile && !state.optionPrecompile->done)
        {
            uint32_t pending = 0;
            for (const auto& key : state.optionPrecompile->keys)
                if (cache.GetStatus(key) == Effects::BytecodeStatus::Pending) ++pending;
            node.variantsCompiling = pending;
            if (pending == 0) state.optionPrecompile->done = true;
        }
    }

    std::shared_ptr<const ReflectedComputeCb> VariantSelector::ReflectCompute(const std::vector<uint8_t>& bytes)
    {
        const BytecodeId id = IdOfBytecode(bytes);
        auto it = m_computeReflection.find(id);
        if (it != m_computeReflection.end()) return it->second;
        auto reflected = std::make_shared<const ReflectedComputeCb>(ReflectComputeConstants(bytes));
        if (m_computeReflection.size() > cMaxMemoEntries) m_computeReflection.clear();
        m_computeReflection[id] = reflected;
        return reflected;
    }

    std::shared_ptr<const Effects::ShaderReflectionResult> VariantSelector::ReflectPixel(const std::vector<uint8_t>& bytes)
    {
        const BytecodeId id = IdOfBytecode(bytes);
        auto it = m_pixelReflection.find(id);
        if (it != m_pixelReflection.end()) return it->second;
        if (m_pixelReflection.size() > cMaxMemoEntries) m_pixelReflection.clear();
        auto reflected = std::make_shared<const Effects::ShaderReflectionResult>(Effects::ShaderCompiler::Reflect(bytes));
        m_pixelReflection[id] = reflected;
        return reflected;
    }
}
