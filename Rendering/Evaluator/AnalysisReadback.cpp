#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "../../Effects/IEngineComputeOutput.h"

using namespace ShaderLab::Graph;

// Getting analysis values out of the GPU: D2D analysis effects read back
// through a draw session of their own (so never inside the host's), D3D11
// compute nodes through their bridge, synchronously or a frame later.
namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        // Largest side of the temporary target an analysis effect is drawn into.
        constexpr uint32_t cMaxReadbackExtent = 4096;

        // A failed readback leaves the previous fields in place, so it has to
        // say so on the node.
        void ReportAnalysisReadbackFailure(EffectNode& node, HRESULT hr, const wchar_t* step)
        {
            node.runtimeError = std::format(L"analysis readback failed at {} (0x{:08X}); fields are stale",
                step, static_cast<uint32_t>(hr));
            std::fwprintf(stderr, L"[ShaderLab] node %u %s\n", node.id, node.runtimeError.c_str());
        }
    }

    void EvaluatorCore::ReadHistogramOutput(ID2D1DeviceContext5* dc, ID2D1Effect* effect, EffectNode& node)
    {
        if (!node.cachedOutput) return;

        // The histogram computes over its whole input when its output is
        // drawn, so draw all of it into a target that large.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(node.cachedOutput, &bounds);
        D2D1_RECT_L px{};
        if (FAILED(Effects::SnapComputeInputRect(bounds, px)))
        {
            node.runtimeError = L"histogram input is unbounded, empty or larger than 16384 px; crop it first";
            return;
        }
        const uint32_t w = (std::min)(static_cast<uint32_t>(px.right - px.left), cMaxReadbackExtent);
        const uint32_t h = (std::min)(static_cast<uint32_t>(px.bottom - px.top), cMaxReadbackExtent);

        if (!m_histogramTarget || m_histogramTargetW != w || m_histogramTargetH != h)
        {
            m_histogramTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_histogramTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_histogramTargetW = w;
            m_histogramTargetH = h;
        }

        dc->SetTarget(m_histogramTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(-static_cast<float>(px.left), -static_cast<float>(px.top)));
        const HRESULT histHr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(histHr))
        {
            node.runtimeError = L"histogram draw failed (nested draw session?)";
            std::fwprintf(stderr,
                L"[ShaderLab] ReadHistogramOutput node %u EndDraw failed hr=0x%08X -- histogram is STALE\n",
                node.id, static_cast<unsigned>(histHr));
            return;
        }

        uint32_t channelIdx = 0;
        auto chanIt = node.properties.find(L"ChannelSelect");
        if (chanIt != node.properties.end())
            if (auto* pv = std::get_if<uint32_t>(&chanIt->second))
                channelIdx = *pv;
        static const std::wstring channelNames[] = { L"Red", L"Green", L"Blue", L"Alpha" };

        const UINT32 dataSize = effect->GetValueSize(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT);
        if (dataSize == 0) return;

        std::vector<float> histData(dataSize / sizeof(float), 0.0f);
        const HRESULT hr = effect->GetValue(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT,
            reinterpret_cast<BYTE*>(histData.data()), dataSize);
        if (SUCCEEDED(hr))
        {
            node.analysisOutput.type = AnalysisOutputType::Histogram;
            node.analysisOutput.data = std::move(histData);
            node.analysisOutput.channelIndex = channelIdx;
            node.analysisOutput.label = (channelIdx < 4) ? channelNames[channelIdx] + L" Histogram" : L"Histogram";
        }
        else
        {
            node.analysisOutput.type = AnalysisOutputType::None;
            node.analysisOutput.data.clear();
        }
    }

    void EvaluatorCore::ReadCustomAnalysisOutput(ID2D1DeviceContext5* dc, EffectNode& node)
    {
        if (!node.cachedOutput || !node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();
        if (def.analysisFields.empty()) return;

        const uint32_t totalPixels = def.totalAnalysisPixels();
        if (totalPixels == 0) return;

        // Drawing the output makes D2D run the effect.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(node.cachedOutput, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, static_cast<float>(cMaxReadbackExtent)));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, static_cast<float>(cMaxReadbackExtent)));
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_analysisTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); ReportAnalysisReadbackFailure(node, hr, L"target bitmap"); return; }
            m_analysisTargetW = w;
            m_analysisTargetH = h;
        }

        dc->SetTarget(m_analysisTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(-bounds.left, -bounds.top));
        const HRESULT drawHr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(drawHr))
        {
            node.runtimeError = L"analysis draw failed (nested draw session?)";
            std::fwprintf(stderr,
                L"[ShaderLab] ReadCustomAnalysisOutput node %u EndDraw failed hr=0x%08X -- analysis fields are STALE\n",
                node.id, static_cast<unsigned>(drawHr));
            return;
        }

        // CPU-readable bitmap for the analysis row, kept across calls.
        const uint32_t readW = (std::max)(totalPixels, 1u);
        if (!m_analysisCpuBitmap || m_analysisCpuBitmapW != readW)
        {
            m_analysisCpuBitmap = nullptr;
            D2D1_BITMAP_PROPERTIES1 cpuProps = {};
            cpuProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            HRESULT chr = dc->CreateBitmap(D2D1::SizeU(readW, 1), nullptr, 0, cpuProps, m_analysisCpuBitmap.put());
            if (FAILED(chr)) { ReportAnalysisReadbackFailure(node, chr, L"readback bitmap"); return; }
            m_analysisCpuBitmapW = readW;
        }
        ID2D1Bitmap1* cpuBitmap = m_analysisCpuBitmap.get();

        D2D1_POINT_2U dest = { 0, 0 };
        D2D1_RECT_U srcRect = { 0, 0, readW, 1 };
        HRESULT hr = cpuBitmap->CopyFromBitmap(&dest, m_analysisTarget.get(), &srcRect);
        if (FAILED(hr)) { ReportAnalysisReadbackFailure(node, hr, L"CopyFromBitmap"); return; }

        D2D1_MAPPED_RECT mapped{};
        hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) { ReportAnalysisReadbackFailure(node, hr, L"Map"); return; }

        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields = UnpackAnalysisFields(def.analysisFields,
            reinterpret_cast<const float*>(mapped.bits), static_cast<size_t>(readW) * 4);

        cpuBitmap->Unmap();
    }

    void EvaluatorCore::UnpackAnalysisFloats(EffectNode& node, const std::vector<float>& analysisFloats)
    {
        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields = UnpackAnalysisFields(
            node.customEffect.value().analysisFields, analysisFloats.data(), analysisFloats.size());
    }

    void EvaluatorCore::MarkBindingConsumersDirty(GraphModel& graph, uint32_t producerId)
    {
        std::vector<uint32_t> staleQueue;
        for (const auto& consumer : graph.Nodes())
        {
            if (AnyBindingSource(consumer, [producerId](uint32_t sourceNodeId) { return sourceNodeId == producerId; }))
                staleQueue.push_back(consumer.id);
        }
        for (uint32_t consumerId : staleQueue)
        {
            if (auto* consumer = graph.FindNode(consumerId))
            {
                consumer->dirty = true;
                graph.MarkDownstreamDirty(consumerId);
            }
        }
    }

    bool EvaluatorCore::HasPendingReadbacks() const
    {
        return std::any_of(m_nodes.begin(), m_nodes.end(),
            [](const auto& entry) { return entry.second.asyncReadbackPending; });
    }

    bool EvaluatorCore::PollAsyncReadbacks(GraphModel& graph)
    {
        bool any = false;
        for (auto& [id, runtime] : m_nodes)
        {
            if (!runtime.asyncReadbackPending) continue;
            auto* node = graph.FindNode(id);
            auto* bridge = runtime.instance.bridgeImpl;
            if (!node || !node->customEffect.has_value() || !bridge)
            {
                runtime.asyncReadbackPending = false;
                continue;
            }
            std::vector<float> floats;
            if (bridge->PollAnalysisReadback(floats) && !floats.empty())
            {
                UnpackAnalysisFloats(*node, floats);
                MarkBindingConsumersDirty(graph, id);
                any = true;
            }
            runtime.asyncReadbackPending = bridge->HasPendingReadback();
        }
        return any;
    }

    ID2D1Bitmap1* EvaluatorCore::AnalysisBitmap(uint32_t producerNodeId) const
    {
        const auto* runtime = FindRuntime(producerNodeId);
        return runtime ? runtime->analysisTexture.bitmap.get() : nullptr;
    }

    ID2D1Bitmap1* EvaluatorCore::EnsureAnalysisBitmap(uint32_t producerNodeId, ID2D1DeviceContext5* dc)
    {
        if (!dc) return nullptr;
        auto* runtime = FindRuntime(producerNodeId);
        if (!runtime || !runtime->instance.bridgeImpl) return nullptr;

        winrt::com_ptr<Effects::IEngineComputeTexture> computeTexture;
        if (FAILED(runtime->instance.bridgeImpl->QueryInterface(
                __uuidof(Effects::IEngineComputeTexture), computeTexture.put_void())))
            return nullptr;

        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(computeTexture->GetAnalysisTexture(tex.put())) || !tex)
            return nullptr;   // not filled yet -- next frame

        // The bitmap is a VIEW of the texture the copy pass writes, so it is
        // reused while it still points at the same texture.
        auto& view = runtime->analysisTexture;
        if (view.bitmap && view.texture == tex.get())
            return view.bitmap.get();

        winrt::com_ptr<IDXGISurface> surface;
        if (FAILED(tex->QueryInterface(__uuidof(IDXGISurface), surface.put_void())))
            return nullptr;

        // FP32, matching the texture: FP16 would quantise a 620-nit peak to
        // half-nit steps, and the lane exists to avoid losing precision on
        // the way to the shader. IGNORE alpha: these are data, not colour,
        // and a premultiplied reading would let D2D scale .x by .w.
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_IGNORE));
        winrt::com_ptr<ID2D1Bitmap1> bmp;
        if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bmp.put())))
            return nullptr;

        view = { bmp, tex.get() };
        return bmp.get();
    }
}
