#include "pch_engine.h"
#include "WorkingSpaceSync.h"

#include "../Graph/GraphModel.h"
#include "DisplayProfile.h"

namespace ShaderLab::Rendering
{
    bool UpdateWorkingSpaceNodes(Graph::GraphModel& graph, const DisplayProfile& profile)
    {
        using namespace Graph;
        using winrt::Windows::Foundation::Numerics::float2;

        const auto& caps = profile.caps;
        const bool isSim = profile.isSimulated;

        // Per-field change epsilons. Displays with adaptive color / auto
        // brightness re-report luminance continuously (sub-nit sensor
        // jitter at several Hz); without a dead-band every drift dirties
        // the node and re-evaluates every binding consumer. 1 nit is invisible
        // while slider drags (multi-nit steps) still propagate instantly.
        // Flags/mode use 0 (exact); MinNits values sit near 0.0005 so it
        // gets a proportionally tiny band.
        struct ScalarField { const wchar_t* name; float value; float eps; };
        const ScalarField scalars[] = {
            { L"ActiveColorMode",  static_cast<float>(caps.activeColorMode), 0.0f },
            { L"HdrSupported",     caps.hdrSupported    ? 1.0f : 0.0f,       0.0f },
            { L"HdrUserEnabled",   caps.hdrUserEnabled  ? 1.0f : 0.0f,       0.0f },
            { L"WcgSupported",     caps.wcgSupported    ? 1.0f : 0.0f,       0.0f },
            { L"WcgUserEnabled",   caps.wcgUserEnabled  ? 1.0f : 0.0f,       0.0f },
            { L"IsSimulated",      isSim ? 1.0f : 0.0f,                      0.0f },
            { L"SdrWhiteNits",     caps.sdrWhiteLevelNits,                   1.0f },
            { L"PeakNits",         caps.maxLuminanceNits,                    1.0f },
            { L"MinNits",          caps.minLuminanceNits,                    0.0001f },
            { L"MaxFullFrameNits", caps.maxFullFrameLuminanceNits,           1.0f },
        };

        constexpr float kChromaEps = 0.0005f;
        struct VectorField { const wchar_t* name; float2 value; };
        const VectorField vectors[] = {
            { L"RedPrimary",   float2{ profile.primaryRed.x,   profile.primaryRed.y   } },
            { L"GreenPrimary", float2{ profile.primaryGreen.x, profile.primaryGreen.y } },
            { L"BluePrimary",  float2{ profile.primaryBlue.x,  profile.primaryBlue.y  } },
            { L"WhitePoint",   float2{ profile.whitePoint.x,   profile.whitePoint.y   } },
        };

        bool anyChanged = false;
        for (auto& node : graph.MutableNodes())
        {
            if (!node.customEffect.has_value()) continue;
            if (node.customEffect->nodeKind != Graph::NodeKind::WorkingSpace) continue;

            bool nodeChanged = false;

            for (const auto& f : scalars)
            {
                auto it = node.properties.find(f.name);
                if (it == node.properties.end())
                {
                    node.properties[f.name] = PropertyValue{ f.value };
                    nodeChanged = true;
                    continue;
                }
                if (auto* cur = std::get_if<float>(&it->second))
                {
                    if (std::abs(*cur - f.value) > f.eps)
                    {
                        *cur = f.value;
                        nodeChanged = true;
                    }
                }
                else
                {
                    it->second = PropertyValue{ f.value };
                    nodeChanged = true;
                }
            }

            for (const auto& f : vectors)
            {
                auto it = node.properties.find(f.name);
                if (it == node.properties.end())
                {
                    node.properties[f.name] = PropertyValue{ f.value };
                    nodeChanged = true;
                    continue;
                }
                if (auto* cur = std::get_if<float2>(&it->second))
                {
                    if (std::abs(cur->x - f.value.x) > kChromaEps ||
                        std::abs(cur->y - f.value.y) > kChromaEps)
                    {
                        *cur = f.value;
                        nodeChanged = true;
                    }
                }
                else
                {
                    it->second = PropertyValue{ f.value };
                    nodeChanged = true;
                }
            }

            if (nodeChanged)
            {
                node.dirty = true;
                anyChanged = true;
            }
        }

        return anyChanged;
    }
}
