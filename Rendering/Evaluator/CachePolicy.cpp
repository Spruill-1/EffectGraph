#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "InputPins.h"

using namespace ShaderLab::Graph;

// D2D output caching, and dropping what the evaluator keeps when a node, its
// shader or the device changes.
namespace ShaderLab::Rendering::Evaluator
{
    void EvaluatorCore::UpdateEffectCachePolicy(ID2D1Effect* effect, NodeRuntime& runtime, bool contentDirty)
    {
        if (!effect)
            return;

        bool& enabled = runtime.instance.cacheEnabled;
        if (!m_frameOptions.effectOutputCaching)
        {
            // Kill switch: release any held intermediate and stop caching.
            if (enabled)
            {
                effect->SetValue(D2D1_PROPERTY_CACHED, FALSE);
                enabled = false;
            }
            return;
        }

        if (contentDirty)
        {
            // Content changed through a channel D2D can't see. Drop the cache
            // and STAY uncached while dirty: an uncached effect re-executes on
            // every pull, and -- the hysteresis part -- the CACHED property is
            // not touched again next frame. Any per-frame property transition
            // on a consumer makes D2D drop its PRODUCERS' cached intermediates
            // as collateral, so a node dirty every frame must generate no
            // property traffic for its upstream caches to survive. Caching
            // comes back one frame after the node goes clean.
            if (enabled)
            {
                effect->SetValue(D2D1_PROPERTY_CACHED, FALSE);
                enabled = false;
                ++m_cacheInvalidations;
            }
            return;
        }

        if (!enabled)
        {
            effect->SetValue(D2D1_PROPERTY_CACHED, TRUE);
            enabled = true;
        }
    }

    size_t EvaluatorCore::CachedEffectCount() const
    {
        return static_cast<size_t>(std::count_if(m_nodes.begin(), m_nodes.end(),
            [](const auto& entry) { return entry.second.instance.cacheEnabled; }));
    }

    void EvaluatorCore::ReleaseCache()
    {
        for (auto& [id, runtime] : m_nodes)
            runtime.ResetAll();
        // Entries hold owning image refs to outputs being released here.
        m_computeQueue.Clear();
        m_analysisDummyBitmap = nullptr;
        m_dummySourceBitmap = nullptr;
        m_unwiredPreRender = {};
        m_histogramTarget = nullptr;
        m_analysisTarget = nullptr;
        m_analysisCpuBitmap = nullptr;
    }

    void EvaluatorCore::ReleaseCache(GraphModel& graph)
    {
        ReleaseCache();
        // cachedOutput is a non-owning pointer into the caches just cleared.
        for (auto& node : graph.MutableNodes())
        {
            node.cachedOutput = nullptr;
            node.dirty = true;
            node.variantsCompiling = 0;
        }
    }

    void EvaluatorCore::InvalidateNode(uint32_t nodeId)
    {
        // The caller must also clear the node's cachedOutput, which may point
        // into what is dropped here; the graph-aware overload does.
        if (auto* runtime = FindRuntime(nodeId))
            runtime->ResetEffect();
    }

    void EvaluatorCore::InvalidateNode(GraphModel& graph, uint32_t nodeId)
    {
        InvalidateNode(nodeId);
        if (auto* node = graph.FindNode(nodeId))
        {
            node->cachedOutput = nullptr;
            node->dirty = true;
            node->variantsCompiling = 0;
        }
    }

    void EvaluatorCore::UpdateNodeShader(uint32_t nodeId, const EffectNode& node)
    {
        if (!node.customEffect.has_value() || !node.customEffect->isCompiled())
            return;
        const auto& def = node.customEffect.value();
        auto* runtime = FindRuntime(nodeId);
        if (!runtime)
            return;   // first compile: the next Evaluate creates the effect

        // A compute node gets a fresh bridge, which installs the new bytecode.
        if (def.shaderType == CustomShaderType::D3D11ComputeShader)
        {
            runtime->ResetEffect();
            return;
        }
        auto& instance = runtime->instance;
        if (!instance.HasCustomImpl())
            return;

        // A different input count is a structural change: recreate.
        if (instance.effect && instance.effect->GetInputCount() != DeclaredInputCount(def))
        {
            runtime->ResetEffect();
            return;
        }

        // Otherwise swap the bytecode in place.
        if (node.type == NodeType::PixelShader && instance.pixelImpl)
        {
            instance.pixelImpl->SetShaderGuid(GraphEvaluator::ShaderGuidFor(def.shaderGuid, def.compiledBytecode));
            instance.pixelImpl->LoadShaderBytecode(def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            runtime->variant.loadedPixelVariant = IdOfBytecode(def.compiledBytecode);
        }
        else if (node.type == NodeType::ComputeShader && instance.computeImpl)
        {
            instance.computeImpl->SetShaderGuid(GraphEvaluator::ShaderGuidFor(def.shaderGuid, def.compiledBytecode));
            instance.computeImpl->LoadShaderBytecode(def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            instance.computeImpl->SetThreadGroupSize(def.threadGroupX, def.threadGroupY);
        }

        // The effect survives the swap, so its cached intermediate is stale.
        // The next Evaluate re-enables caching after the fresh render.
        if (instance.effect && instance.cacheEnabled)
        {
            instance.effect->SetValue(D2D1_PROPERTY_CACHED, FALSE);
            instance.cacheEnabled = false;
            ++m_cacheInvalidations;
        }
    }

    void EvaluatorCore::SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids)
    {
        // A node that left the set reads back at once when it is re-added.
        for (auto& [id, runtime] : m_nodes)
            if (!ids.contains(id)) runtime.lastHintReadback.reset();
        m_cpuAnalysisInterest = std::move(ids);
    }

    void EvaluatorCore::ClearCpuAnalysisInterest()
    {
        m_cpuAnalysisInterest.clear();
        for (auto& [id, runtime] : m_nodes)
            runtime.lastHintReadback.reset();
    }
}
