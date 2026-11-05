#include "pch_engine.h"
#include "EffectGraphImpl.h"
#include "../EngineBootstrap.h"
#include "../../Graph/GraphEdits.h"
#include "../../Effects/BuiltInEffects.h"
#include "../../Effects/EffectBlob.h"
#include "../../Effects/EffectRegistry.h"
#include "../../Effects/NodeCatalog.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/SourceTextureUtil.h"
#include "../../Rendering/D2DHelpers.h"
#include "../../Rendering/SourceRuntime.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/WorkingSpaceSync.h"

namespace ShaderLab::Api
{
    namespace
    {
        bool IsFloatFormat(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        }

        // The formats each encoding is read from.
        bool FormatMatchesEncoding(DXGI_FORMAT format, EFFECTGRAPH_INPUT_ENCODING encoding)
        {
            switch (encoding)
            {
            case EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR: return IsFloatFormat(format);
            case EFFECTGRAPH_INPUT_ENCODING_SRGB:
                return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM;
            case EFFECTGRAPH_INPUT_ENCODING_HDR10: return format == DXGI_FORMAT_R10G10B10A2_UNORM;
            default: return false;
            }
        }

        // The shader cache directory is process-wide; the first context that
        // names one sets it.
        std::once_flag sCacheConfigured;
    }

    HRESULT CopyString(std::wstring_view text, PWSTR buffer, UINT32 bufferCount)
    {
        if (!buffer && bufferCount) return E_INVALIDARG;
        if (text.size() + 1 > bufferCount) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::copy(text.begin(), text.end(), buffer);
        buffer[text.size()] = L'\0';
        return S_OK;
    }

    Context::~Context()
    {
        // Node objects hold a reference to the context, so none is left here.
        m_graph.ClearCachedOutputs();
        m_evaluator.ReleaseCache();
    }

    HRESULT Context::Initialize(ID3D11Device* device, const EFFECTGRAPH_CONTEXT_DESC* desc)
    {
        if (!device) return E_INVALIDARG;
        if (!(device->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT))
            return EFFECTGRAPH_E_UNSUPPORTED_DEVICE;
        if (HRESULT hr = Rendering::CreateDeviceStackOn(device, m_devices); FAILED(hr)) return hr;
        if (HRESULT hr = m_devices.CreateDeviceContext(m_dc); FAILED(hr)) return hr;

        winrt::com_ptr<ID2D1Factory1> factory;
        m_devices.d2dFactory->QueryInterface(IID_PPV_ARGS(factory.put()));
        if (HRESULT hr = Engine::RegisterEngineD2DEffects(factory.get()); FAILED(hr)) return hr;

        // Compute results stay on the GPU unless something needs them on the
        // CPU: GetValues reads its node's buffer itself, and a binding the
        // GPU cannot serve still reads back.
        auto options = m_evaluator.Options();
        options.skipUnneededCpuReadback = true;

        if (desc)
        {
            m_deferredCommit = (desc->flags & EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT) != 0;
            options.deferredAnalysisCommit = m_deferredCommit;
            m_refuseFallbackBuilds = (desc->flags & EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK) != 0;
            if (desc->shaderCacheDirectory && *desc->shaderCacheDirectory)
            {
                std::wstring directory(desc->shaderCacheDirectory);
                std::call_once(sCacheConfigured, [&] { Engine::ConfigureBytecodeCache(std::move(directory)); });
            }
            if (desc->flags & EFFECTGRAPH_CONTEXT_FLAGS_ASYNC_COMPILE)
                m_evaluator.SetAsyncCompile(true);
            if (desc->flags & EFFECTGRAPH_CONTEXT_FLAGS_ALLOW_RUNTIME_COMPILE)
                options.compilePrebuiltEffects = true;
        }
        m_evaluator.SetOptions(options);
        return S_OK;
    }

    // ---- Node bookkeeping

    void Context::ForgetNodeObject(uint32_t nodeId, const NodeBase* object)
    {
        // After ReplaceGraph the id may belong to another node's object.
        if (auto it = m_nodeObjects.find(nodeId); it != m_nodeObjects.end() && it->second == object)
            m_nodeObjects.erase(it);
    }

    HRESULT Context::IdOf(IEffectGraphNode* node, uint32_t& nodeId) const
    {
        nodeId = 0;
        if (!node) return E_INVALIDARG;
        winrt::com_ptr<INodeIdentity> identity;
        if (FAILED(node->QueryInterface(__uuidof(INodeIdentity), identity.put_void())) || !identity)
            return EFFECTGRAPH_E_WRONG_CONTEXT;
        const NodeBase* object = identity->Object();
        if (object->Owner() != this) return EFFECTGRAPH_E_WRONG_CONTEXT;
        // Gone from the graph, or from a graph ReplaceGraph replaced.
        const auto it = m_nodeObjects.find(object->Id());
        if (!m_graph.FindNode(object->Id()) || it == m_nodeObjects.end() || it->second != object)
            return EFFECTGRAPH_E_NODE_REMOVED;
        nodeId = object->Id();
        return S_OK;
    }

    STDMETHODIMP_(uint32_t) Context::NodeId(IEffectGraphNode* node)
    {
        uint32_t nodeId = 0;
        return SUCCEEDED(IdOf(node, nodeId)) ? nodeId : 0;
    }

    STDMETHODIMP Context::NodeFor(uint32_t nodeId, IEffectGraphNode** node)
    {
        if (!node) return E_POINTER;
        *node = nullptr;
        if (!m_graph.FindNode(nodeId)) return S_FALSE;
        if (auto it = m_nodeObjects.find(nodeId); it != m_nodeObjects.end())
        {
            it->second->GetInterface(node);
            return S_OK;
        }
        NodeBase* object = nullptr;
        if (IsInput(nodeId))
        {
            auto created = winrt::make_self<InputNode>(this, nodeId);
            object = created.get();
            object->GetInterface(node);
        }
        else
        {
            auto created = winrt::make_self<Node>(this, nodeId);
            object = created.get();
            object->GetInterface(node);
        }
        m_nodeObjects[nodeId] = object;
        return S_OK;
    }

    HRESULT Context::AddNodeAndWrap(Graph::EffectNode node, IEffectGraphNode** out)
    {
        const uint32_t id = m_graph.AddNode(std::move(node));
        return NodeFor(id, out);
    }

    ID2D1Effect* Context::MetadataEffect(const GUID& clsid)
    {
        for (auto& [id, effect] : m_metadataEffects)
            if (id == clsid) return effect.get();
        winrt::com_ptr<ID2D1Effect> effect;
        if (FAILED(m_dc->CreateEffect(clsid, effect.put())) || !effect) return nullptr;
        m_metadataEffects.emplace_back(clsid, effect);
        return effect.get();
    }

    // ---- Creating nodes

    STDMETHODIMP Context::CreateEffect(PCWSTR name, IEffectGraphNode** node)
    {
        if (!name || !node) return E_INVALIDARG;
        *node = nullptr;
        auto created = Effects::CreateNodeByName(name, {}, m_graph);
        if (!created.node)
            return Effects::MissingBuiltInMessage(name).empty() ? EFFECTGRAPH_E_UNKNOWN_EFFECT
                                                                : EFFECTGRAPH_E_EFFECT_NOT_INCLUDED;
        const bool prepare = created.needsPrepare;
        HRESULT hr = AddNodeAndWrap(std::move(*created.node), node);
        if (SUCCEEDED(hr) && prepare)
        {
            if (auto* added = m_graph.FindNode(NodeId(*node)))
                m_sources.PrepareSourceNode(*added, m_dc.get(), m_devices.d3dDevice.get(), m_devices.d3dContext.get());
        }
        return hr;
    }

    STDMETHODIMP Context::CreateEffectById(REFCLSID effectId, IEffectGraphNode** node)
    {
        if (!node) return E_INVALIDARG;
        *node = nullptr;
        for (const auto& descriptor : Effects::ShaderLabEffects::Instance().All())
            if (Effects::EffectClsid(descriptor.effectId) == effectId)
                return CreateEffect(descriptor.name.c_str(), node);
        return Effects::MissingBuiltInMessage(effectId).empty() ? EFFECTGRAPH_E_UNKNOWN_EFFECT
                                                                : EFFECTGRAPH_E_EFFECT_NOT_INCLUDED;
    }

    STDMETHODIMP Context::CreateD2DEffect(REFCLSID effectId, IEffectGraphNode** node)
    {
        if (!node) return E_INVALIDARG;
        *node = nullptr;
        if (const auto* desc = Effects::EffectRegistry::Instance().FindByClsid(effectId))
            return AddNodeAndWrap(Effects::EffectRegistry::CreateNode(*desc), node);

        // A class ShaderLab does not list: its pins and name come from an instance.
        ID2D1Effect* effect = MetadataEffect(effectId);
        if (!effect) return EFFECTGRAPH_E_UNKNOWN_EFFECT;
        Graph::EffectNode created;
        created.type = Graph::NodeType::BuiltInEffect;
        created.effectClsid = effectId;
        wchar_t displayName[256]{};
        if (FAILED(effect->GetValue(D2D1_PROPERTY_DISPLAYNAME, reinterpret_cast<BYTE*>(displayName), sizeof(displayName))))
            wcscpy_s(displayName, L"D2D Effect");
        Graph::RenameNode(created, displayName);
        for (UINT32 i = 0; i < effect->GetInputCount(); ++i)
            created.inputPins.push_back({ std::format(L"Input {}", i), i });
        created.outputPins.push_back({ L"Output", 0 });
        return AddNodeAndWrap(std::move(created), node);
    }

    HRESULT Context::WrapTexture(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding, InputSource& source)
    {
        if (!texture) return E_INVALIDARG;
        winrt::com_ptr<ID3D11Device> owner;
        texture->GetDevice(owner.put());
        if (owner.get() != static_cast<ID3D11Device*>(m_devices.d3dDevice.get())) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 || !FormatMatchesEncoding(desc.Format, encoding))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;

        winrt::com_ptr<IDXGISurface> surface;
        if (HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(surface.put())); FAILED(hr)) return hr;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { desc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.dpiX = props.dpiY = 96.0f;
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put())))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;

        InputSource wrapped;
        wrapped.fromTexture = true;
        wrapped.texture.copy_from(texture);
        wrapped.bitmap = bitmap;
        if (encoding == EFFECTGRAPH_INPUT_ENCODING_HDR10)
        {
            // PQ / BT.2020 to scRGB at float precision, as ImageLoader decodes HDR10 files.
            winrt::com_ptr<ID2D1ColorContext1> pq;
            winrt::com_ptr<ID2D1ColorContext> scrgb;
            HRESULT hr = m_dc->CreateColorContextFromDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, pq.put());
            if (SUCCEEDED(hr)) hr = m_dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, scrgb.put());
            if (SUCCEEDED(hr)) hr = m_dc->CreateEffect(CLSID_D2D1ColorManagement, wrapped.decode.put());
            if (FAILED(hr)) return hr;
            wrapped.decode->SetInput(0, bitmap.get());
            wrapped.decode->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, pq.get());
            wrapped.decode->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, scrgb.get());
            wrapped.decode->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);
            wrapped.decode->GetOutput(wrapped.output.put());
        }
        else
        {
            std::wstring error;
            wrapped.output = Effects::AsLinearScRgb(m_dc.get(), bitmap.get(), desc.Format, wrapped.decode, error);
            if (!wrapped.output) return E_FAIL;
        }
        source = std::move(wrapped);
        return S_OK;
    }

    STDMETHODIMP Context::CreateTextureInput(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding,
                                             IEffectGraphNode** node)
    {
        if (!node) return E_INVALIDARG;
        *node = nullptr;
        InputSource source;
        if (HRESULT hr = WrapTexture(texture, encoding, source); FAILED(hr)) return hr;
        const uint32_t id = m_graph.AddNode(Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Texture Input"));
        m_inputs[id] = std::move(source);
        return NodeFor(id, node);
    }

    STDMETHODIMP Context::CreateImageInput(ID2D1Image* image, IEffectGraphNode** node)
    {
        if (!image || !node) return E_INVALIDARG;
        *node = nullptr;
        const uint32_t id = m_graph.AddNode(Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Image Input"));
        m_inputs[id].output.copy_from(image);
        return NodeFor(id, node);
    }

    HRESULT Context::SetInputTexture(uint32_t nodeId, ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding)
    {
        auto it = m_inputs.find(nodeId);
        if (it == m_inputs.end() || !it->second.fromTexture) return E_NOT_VALID_STATE;
        InputSource source;
        if (HRESULT hr = WrapTexture(texture, encoding, source); FAILED(hr)) return hr;
        it->second = std::move(source);
        Graph::MarkNodeChanged(m_graph, nodeId);
        return S_OK;
    }

    HRESULT Context::SetInputImage(uint32_t nodeId, ID2D1Image* image)
    {
        auto it = m_inputs.find(nodeId);
        if (it == m_inputs.end() || it->second.fromTexture) return E_NOT_VALID_STATE;
        if (!image) return E_INVALIDARG;
        it->second.output.copy_from(image);
        Graph::MarkNodeChanged(m_graph, nodeId);
        return S_OK;
    }

    STDMETHODIMP Context::RemoveNode(IEffectGraphNode* node)
    {
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        m_graph.RemoveNode(id);
        m_inputs.erase(id);
        m_uploadedValues.erase(id);
        m_incrementalTargets.erase(id);
        std::erase_if(m_ingestedMoves, [id](const IngestedMove& m) { return m.input == id; });
        m_evaluator.InvalidateNode(m_graph, id);
        m_sources.PruneOrphans(m_graph.Nodes());
        return S_OK;
    }

    STDMETHODIMP_(void) Context::ReplaceGraph(Graph::GraphModel&& graph)
    {
        m_uploadedValues.clear();
        m_incrementalTargets.clear();
        m_ingestedMoves.clear();
        m_collapsedMoves.clear();
        m_windowStart = m_lastIngestGeneration = 0;
        m_graph.ClearCachedOutputs();
        m_evaluator.ReleaseCache();
        m_inputs.clear();
        m_nodeObjects.clear();
        ++m_epoch;
        m_graph = std::move(graph);
        m_graph.MarkAllDirty();
        // Close the old graph's providers before a host deletes its media.
        m_sources.PruneOrphans(m_graph.Nodes());
        m_sources.ForgetVideoSettleTimeouts();
        Rendering::PrepareSources(m_graph, m_sources,
            { m_dc.get(), m_devices.d3dDevice.get(), m_devices.d3dContext.get() }, Rendering::SourceScope::All);
    }

    // ---- Evaluating

    void Context::SyncFrameInputs()
    {
        for (auto& [id, source] : m_inputs)
            if (auto* node = m_graph.FindNode(id)) node->cachedOutput = source.output.get();

        // Sources from a loaded graph: the dirty ones and any video, as
        // PrepareSources(Changed) would, but never an input node.
        for (auto& node : m_graph.MutableNodes())
        {
            if (node.type != Graph::NodeType::Source || m_inputs.contains(node.id)) continue;
            if (!node.IsDirty() && !m_sources.HasVideoProvider(node.id)) continue;
            // Preparing an unchanged file marks the source clean, which the
            // GUI wants on a redraw; here a source marked changed must still
            // re-render what reads it. Evaluating it clears the flag.
            const bool changed = node.IsDirty();
            m_sources.PrepareSourceNode(node, m_dc.get(), m_devices.d3dDevice.get(), m_devices.d3dContext.get());
            if (changed) Graph::MarkNodeChanged(node);
        }
        Rendering::UpdateWorkingSpaceNodes(m_graph, m_displayProfile);
        if (std::ranges::any_of(m_graph.Nodes(), [&](const Graph::EffectNode& n) { return m_sources.HasVideoProvider(n.id); }))
            Rendering::SettleVideoSources(m_graph, m_sources, m_evaluator, m_dc.get());
    }

    HRESULT Context::DeviceRemovedReason() const
    {
        return m_devices.d3dDevice ? m_devices.d3dDevice->GetDeviceRemovedReason() : S_OK;
    }

    std::vector<uint32_t> Context::UncachedRoots(uint32_t also) const
    {
        std::vector<uint32_t> roots;
        if (!m_evaluator.Options().dirtyRegions) return roots;
        for (const auto& [id, record] : m_incrementalTargets)
            if (record.uncached) roots.push_back(id);
        if (also && std::ranges::find(roots, also) == roots.end()) roots.push_back(also);
        return roots;
    }

    HRESULT Context::EvaluateFor(uint32_t nodeId, ID2D1Image* target, std::function<void(ID2D1DeviceContext5*)> draw,
                                 bool acknowledge, bool uncacheRoot)
    {
        if (HRESULT removed = DeviceRemovedReason(); FAILED(removed)) return removed;
        SyncFrameInputs();
        Rendering::FrameRequest request;
        request.demand = Rendering::FrameDemand::Roots;
        request.roots = { nodeId };
        request.target = target;
        request.draw = std::move(draw);
        if (acknowledge) request.acknowledgeRoots = { nodeId };
        request.uncachedRoots = UncachedRoots(uncacheRoot ? nodeId : 0);
        request.refuseFallbackBuilds = m_refuseFallbackBuilds;
        const auto result = Rendering::FrameExecutor::Run(m_graph, m_evaluator, m_dc.get(), request);
        // A device removed during the frame fails EndDraw with
        // D2DERR_RECREATE_TARGET and nodes with odd errors; say why instead.
        if (HRESULT removed = DeviceRemovedReason(); FAILED(removed)) return removed;
        if (FAILED(result.endDraw)) return result.endDraw;
        // NO_FALLBACK: nothing was drawn and nothing acknowledged. A build
        // still compiling may yet arrive; a missing one never will.
        if (result.fallbackRefused)
            return (result.fallbackBuilds || result.fallbacksInUse.missing) ? EFFECTGRAPH_E_FALLBACK_BUILD
                                                                            : EFFECTGRAPH_E_COMPILE_PENDING;

        const auto* node = m_graph.FindNode(nodeId);
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        if (node->compilePending) return EFFECTGRAPH_E_COMPILE_PENDING;
        if (!node->runtimeError.empty()) return EFFECTGRAPH_E_NODE_FAILED;
        if (!node->outputPins.empty() && !node->cachedOutput) return EFFECTGRAPH_E_NODE_FAILED;
        return S_OK;
    }

    STDMETHODIMP Context::Realize(IEffectGraphNode* node, ID2D1Image** image)
    {
        if (image) *image = nullptr;
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        if (HRESULT hr = EvaluateFor(id, nullptr, {}); FAILED(hr)) return hr;
        if (!image) return S_OK;
        auto* output = m_graph.FindNode(id)->cachedOutput;
        if (!output) return S_FALSE;
        output->AddRef();
        *image = output;
        return S_OK;
    }

    HRESULT Context::TargetBitmap(ID3D11Texture2D* target, winrt::com_ptr<ID2D1Bitmap1>& bitmap) const
    {
        if (!target) return E_INVALIDARG;
        winrt::com_ptr<ID3D11Device> owner;
        target->GetDevice(owner.put());
        if (owner.get() != static_cast<ID3D11Device*>(m_devices.d3dDevice.get())) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_RENDER_TARGET) || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;
        winrt::com_ptr<IDXGISurface> surface;
        if (HRESULT hr = target->QueryInterface(IID_PPV_ARGS(surface.put())); FAILED(hr)) return hr;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { desc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        props.dpiX = props.dpiY = 96.0f;
        bitmap = nullptr;
        if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put())))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;
        return S_OK;
    }

    STDMETHODIMP Context::RealizeTo(IEffectGraphNode* node, ID3D11Texture2D* target)
    {
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        if (!target) return E_INVALIDARG;
        if (m_graph.FindNode(id)->outputPins.empty()) return E_INVALIDARG;
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (HRESULT hr = TargetBitmap(target, bitmap); FAILED(hr)) return hr;
        // The target no longer holds any node's incremental realize.
        ForgetIncrementalTarget(target);

        // Drawn inside the frame's own session, after its compute work.
        auto& graph = m_graph;
        HRESULT hr = EvaluateFor(id, bitmap.get(), [&graph, id](ID2D1DeviceContext5* dc)
        {
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            if (const auto* rendered = graph.FindNode(id); rendered && rendered->cachedOutput)
                dc->DrawImage(rendered->cachedOutput, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                              D2D1_COMPOSITE_MODE_SOURCE_COPY);
        });
        // EndDraw submitted the draw to the device's immediate context, so
        // D3D11 work the caller issues next runs after it.
        return hr;
    }

    // ---- Dirty rects

    void Context::ForgetIncrementalTarget(ID3D11Texture2D* target, uint32_t keepFor)
    {
        std::erase_if(m_incrementalTargets, [&](const auto& entry)
        {
            return entry.first != keepFor && entry.second.texture.get() == target;
        });
    }

    HRESULT Context::InvalidateInputRects(uint32_t nodeId, const RECT* rects, UINT32 count)
    {
        auto it = m_inputs.find(nodeId);
        if (it == m_inputs.end()) return E_NOT_VALID_STATE;
        if (count == 0) return S_FALSE;
        const auto bounds = InputBounds(it->second);

        Graph::DirtyRegion region;
        const uint64_t generation = m_graph.RegionGeneration();
        for (UINT32 i = 0; i < count; ++i)
        {
            const D2D1_RECT_L rect{ rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
            region.Add(bounds ? Graph::DirtyRegion::Intersect(rect, *bounds) : rect, generation);
        }
        if (region.IsNone()) return S_FALSE;
        if (bounds && region.Covers(*bounds)) region = Graph::DirtyRegion::Whole();
        Graph::MarkNodeChanged(m_graph, nodeId, region);
        m_lastIngestGeneration = generation;
        return S_OK;
    }

    std::optional<D2D1_RECT_L> Context::InputBounds(const InputSource& source) const
    {
        if (source.texture)
        {
            D3D11_TEXTURE2D_DESC desc{};
            source.texture->GetDesc(&desc);
            return D2D1_RECT_L{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
        }
        if (source.output)
        {
            D2D1_RECT_F local{};
            constexpr float cFinite = 1.0e7f;
            if (SUCCEEDED(m_dc->GetImageLocalBounds(source.output.get(), &local)) &&
                local.left > -cFinite && local.top > -cFinite && local.right < cFinite && local.bottom < cFinite)
                return D2D1_RECT_L{ static_cast<LONG>(std::floor(local.left)), static_cast<LONG>(std::floor(local.top)),
                                    static_cast<LONG>(std::ceil(local.right)), static_cast<LONG>(std::ceil(local.bottom)) };
        }
        return std::nullopt;
    }

    HRESULT Context::MoveInputRects(uint32_t nodeId, const EFFECTGRAPH_MOVE_RECT* moves, UINT32 count)
    {
        auto it = m_inputs.find(nodeId);
        if (it == m_inputs.end()) return E_NOT_VALID_STATE;
        if (count == 0) return S_FALSE;
        const auto bounds = InputBounds(it->second);

        // Each destination clipped to what the input reads, its source
        // shifted with it. A move whose source then leaves the input is no
        // move: its destination is a dirty rect, and it is not reported.
        struct Clipped
        {
            D2D1_RECT_L dest;
            D2D1_POINT_2L source;
            bool asRect;
        };
        std::vector<Clipped> clipped;
        for (UINT32 i = 0; i < count; ++i)
        {
            D2D1_RECT_L dest{ moves[i].DestinationRect.left, moves[i].DestinationRect.top,
                              moves[i].DestinationRect.right, moves[i].DestinationRect.bottom };
            D2D1_POINT_2L source{ moves[i].SourcePoint.x, moves[i].SourcePoint.y };
            if (Graph::DirtyRegion::IsEmpty(dest)) continue;
            bool asRect = false;
            if (bounds)
            {
                const D2D1_RECT_L clippedDest = Graph::DirtyRegion::Intersect(dest, *bounds);
                source.x += clippedDest.left - dest.left;
                source.y += clippedDest.top - dest.top;
                dest = clippedDest;
                if (Graph::DirtyRegion::IsEmpty(dest)) continue;
                asRect = !Graph::DirtyRegion::Contains(*bounds, Graph::DirtyMove{ dest, source }.Source());
            }
            clipped.push_back({ dest, source, asRect });
        }
        if (clipped.empty()) return S_FALSE;

        // Rects and moves ingested in this generation came before these
        // moves: a new generation makes them older, so a move whose source
        // they overlap is drawn rather than copied from stale pixels.
        if (m_lastIngestGeneration == m_graph.RegionGeneration()) m_graph.AdvanceRegionGeneration();
        const uint64_t generation = m_graph.RegionGeneration();
        m_lastIngestGeneration = generation;

        // A second batch for this input before a realize: the first becomes
        // dirty rects at its destinations, wherever it has reached.
        // Both lists are in sequence order, so the new ones merge in.
        const size_t collapsedBefore = m_collapsedMoves.size();
        for (const auto& earlier : m_ingestedMoves)
            if (earlier.input == nodeId && earlier.generation >= m_windowStart &&
                !std::ranges::binary_search(m_collapsedMoves.begin(), m_collapsedMoves.begin() + collapsedBefore,
                                            earlier.sequence))
                m_collapsedMoves.push_back(earlier.sequence);
        std::inplace_merge(m_collapsedMoves.begin(), m_collapsedMoves.begin() + collapsedBefore, m_collapsedMoves.end());

        Graph::DirtyRegion region;
        for (const auto& [dest, source, asRect] : clipped)
        {
            if (asRect)
            {
                // Older than the batch's moves, so one reading it is drawn too.
                region.Add(dest, Graph::DirtyRegion::CollapsedGeneration(generation));
                continue;
            }
            const Graph::DirtyMove move{ dest, source, generation, m_graph.NextMoveSequence() };
            region.AddMove(move);
            m_ingestedMoves.push_back({ nodeId,
                                        EFFECTGRAPH_MOVE_RECT{ { source.x, source.y }, { dest.left, dest.top, dest.right, dest.bottom } },
                                        generation, move.sequence });
        }
        Graph::MarkNodeChanged(m_graph, nodeId, region);
        PruneIngestedMoves();
        return S_OK;
    }

    std::unordered_set<uint32_t> Context::ReadsOf(uint32_t nodeId) const
    {
        std::unordered_set<uint32_t> reads{ nodeId };
        std::vector<uint32_t> walk{ nodeId };
        while (!walk.empty())
        {
            const uint32_t at = walk.back();
            walk.pop_back();
            for (const auto* edge : m_graph.GetInputEdges(at))
                if (reads.insert(edge->sourceNodeId).second) walk.push_back(edge->sourceNodeId);
        }
        return reads;
    }

    void Context::PruneIngestedMoves()
    {
        // Moves since the last incremental realize stay, within the backlog:
        // a second batch on the same input collapses them, and a root
        // realized incrementally for the first time reports them.
        // A root not realized for cMaxMoveBacklog generations stops holding
        // what it has not reported: if a move on an input it reads is
        // dropped, it draws whole next; if none is, it gives up only its
        // claim on moves it does not read.
        const uint64_t generation = m_graph.RegionGeneration();
        const uint64_t oldestKept = generation > cMaxMoveBacklog ? generation - cMaxMoveBacklog : 0;
        uint64_t keepFrom = (std::min)(generation, (std::max)(m_windowStart, oldestKept));
        for (auto& [id, record] : m_incrementalTargets)
        {
            if (record.backlogLost) continue;
            if (record.reportedThrough + 1 < oldestKept)
            {
                const auto reads = ReadsOf(id);
                record.backlogLost = std::ranges::any_of(m_ingestedMoves, [&](const IngestedMove& m)
                {
                    return m.generation > record.reportedThrough && m.generation < oldestKept && reads.contains(m.input);
                });
                if (record.backlogLost) continue;
                record.reportedThrough = oldestKept - 1;
            }
            keepFrom = (std::min)(keepFrom, record.reportedThrough + 1);
        }
        std::erase_if(m_ingestedMoves, [keepFrom](const IngestedMove& m) { return m.generation < keepFrom; });
        // Both lists are in sequence order.
        std::erase_if(m_collapsedMoves, [this](uint64_t sequence)
        {
            return !std::ranges::binary_search(m_ingestedMoves, sequence, {}, &IngestedMove::sequence);
        });
        Testing::g_heldMoves = m_ingestedMoves.size();
        Testing::g_heldCollapsedMoves = m_collapsedMoves.size();
    }

    bool Context::ApplyMoves(ID3D11Texture2D* target, const std::vector<Graph::DirtyMove>& moves)
    {
        if (moves.empty()) return true;
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        UINT width = 0, height = 0;
        for (const auto& move : moves)
        {
            width = (std::max)(width, static_cast<UINT>(move.destination.right - move.destination.left));
            height = (std::max)(height, static_cast<UINT>(move.destination.bottom - move.destination.top));
        }
        D3D11_TEXTURE2D_DESC have{};
        if (m_moveScratch) m_moveScratch->GetDesc(&have);
        if (!m_moveScratch || have.Format != desc.Format || have.Width < width || have.Height < height)
        {
            const bool grow = m_moveScratch && have.Format == desc.Format;
            D3D11_TEXTURE2D_DESC scratch{};
            scratch.Width = (std::max)(width, grow ? have.Width : 0u);
            scratch.Height = (std::max)(height, grow ? have.Height : 0u);
            scratch.MipLevels = 1;
            scratch.ArraySize = 1;
            scratch.Format = desc.Format;
            scratch.SampleDesc.Count = 1;
            scratch.Usage = D3D11_USAGE_DEFAULT;
            m_moveScratch = nullptr;
            if (FAILED(m_devices.d3dDevice->CreateTexture2D(&scratch, nullptr, m_moveScratch.put()))) return false;
        }
        auto* context = m_devices.d3dContext.get();
        for (const auto& move : moves)
        {
            const D2D1_RECT_L source = move.Source();
            const D3D11_BOX from{ static_cast<UINT>(source.left), static_cast<UINT>(source.top), 0,
                                  static_cast<UINT>(source.right), static_cast<UINT>(source.bottom), 1 };
            const D3D11_BOX all{ 0, 0, 0, from.right - from.left, from.bottom - from.top, 1 };
            context->CopySubresourceRegion(m_moveScratch.get(), 0, 0, 0, 0, target, 0, &from);
            context->CopySubresourceRegion(target, 0, static_cast<UINT>(move.destination.left),
                                           static_cast<UINT>(move.destination.top), 0, m_moveScratch.get(), 0, &all);
            Testing::g_moveCopies += 2;
            if (m_moveScratch.get() == target) Testing::g_moveCopiesInPlace += 2;
        }
        return true;
    }

    namespace
    {
        // True when `rects`, together, cover all of `rect`.
        bool CoveredBy(const D2D1_RECT_L& rect, const std::vector<D2D1_RECT_L>& rects)
        {
            if (Graph::DirtyRegion::IsEmpty(rect)) return true;
            std::vector<LONG> xs{ rect.left, rect.right }, ys{ rect.top, rect.bottom };
            for (const auto& r : rects)
            {
                const auto c = Graph::DirtyRegion::Intersect(r, rect);
                if (Graph::DirtyRegion::IsEmpty(c)) continue;
                xs.push_back(c.left); xs.push_back(c.right);
                ys.push_back(c.top); ys.push_back(c.bottom);
            }
            std::ranges::sort(xs);
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            std::ranges::sort(ys);
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            for (size_t i = 0; i + 1 < xs.size(); ++i)
                for (size_t j = 0; j + 1 < ys.size(); ++j)
                {
                    const D2D1_RECT_L cell{ xs[i], ys[j], xs[i + 1], ys[j + 1] };
                    if (!std::ranges::any_of(rects, [&](const D2D1_RECT_L& r) { return Graph::DirtyRegion::Contains(r, cell); }))
                        return false;
                }
            return true;
        }
    }

    HRESULT Context::InvalidRectsOf(uint32_t nodeId, RECT* rects, UINT32 capacity, UINT32* count, BOOL* whole)
    {
        const bool regions = m_evaluator.Options().dirtyRegions;
        Graph::DirtyRegion region = m_graph.PendingDirtyRegion(nodeId, regions);
        region.Union(m_evaluator.PendingRegion(nodeId));
        if (!regions && !region.IsNone()) region = Graph::DirtyRegion::Whole();
        // A pending move changes its destination.
        region.CollapseMoves();
        if (region.IsWhole())
        {
            *whole = TRUE;
            return S_OK;
        }
        const auto held = region.Rects();
        *count = static_cast<UINT32>(held.size());
        if (held.size() > capacity) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        for (size_t i = 0; i < held.size(); ++i)
            rects[i] = { held[i].rect.left, held[i].rect.top, held[i].rect.right, held[i].rect.bottom };
        return S_OK;
    }

    STDMETHODIMP Context::RealizeToIncremental(IEffectGraphNode* node, ID3D11Texture2D* target, UINT32 flags,
                                               RECT* drawn, UINT32 drawnCapacity, UINT32* drawnCount,
                                               EFFECTGRAPH_MOVE_RECT* moved, UINT32 movedCapacity, UINT32* movedCount)
    {
        if (!drawnCount) return E_POINTER;
        *drawnCount = 0;
        if (movedCount) *movedCount = 0;
        // A move the host could not count would be copied and never forwarded.
        if (!drawn || drawnCapacity == 0 || (movedCapacity && (!moved || !movedCount))) return E_INVALIDARG;
        if (flags & ~static_cast<UINT32>(EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL)) return E_INVALIDARG;
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        if (!target) return E_INVALIDARG;
        if (m_graph.FindNode(id)->outputPins.empty()) return E_INVALIDARG;
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (HRESULT hr = TargetBitmap(target, bitmap); FAILED(hr)) return hr;

        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        const D2D1_RECT_L bounds{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
        const auto options = m_evaluator.Options();
        const auto known = m_incrementalTargets.find(id);
        // A root drawn incrementally is never cached by D2D: a cached root
        // reaches the target through an intermediate and rounds differently
        // from a clipped draw. It stays uncached on every frame after, a
        // RealizeTo's included, so its CACHED property does not flip. With
        // dirtyRegions off nothing changes from 1.0: the root is cached.
        const bool uncacheRoot = options.dirtyRegions &&
                                 ((flags & EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL) ||
                                  (known != m_incrementalTargets.end() && known->second.uncached));
        const bool sameTarget = known != m_incrementalTargets.end() && known->second.texture.get() == target &&
                                known->second.width == desc.Width && known->second.height == desc.Height &&
                                known->second.format == desc.Format && known->second.uncached == uncacheRoot;
        // A root whose unreported moves were pruned draws whole and reports none.
        const bool backlogLost = known != m_incrementalTargets.end() && known->second.backlogLost;
        const bool incremental = (flags & EFFECTGRAPH_REALIZE_FLAGS_INCREMENTAL) && options.dirtyRegions && sameTarget &&
                                 !backlogLost;

        // verifyDirtyRegions keeps the target as it was before the draw.
        winrt::com_ptr<ID3D11Texture2D> before;
        if (incremental && options.verifyDirtyRegions)
        {
            D3D11_TEXTURE2D_DESC copyDesc = desc;
            copyDesc.BindFlags = 0;
            copyDesc.MiscFlags = 0;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            if (SUCCEEDED(m_devices.d3dDevice->CreateTexture2D(&copyDesc, nullptr, before.put())))
                m_devices.d3dContext->CopyResource(before.get(), target);
        }

        // The moves to report: every one handed to an input this root reads
        // since its last successful realize (its first: this generation's).
        // Moves inside the target only: one that leaves it is drawn
        // (DirtyRegion::ClippedTo) and not reported; one wholly outside it
        // changes nothing the target holds.
        const uint64_t generation = m_graph.RegionGeneration();
        const uint64_t reportFrom = known != m_incrementalTargets.end() ? known->second.reportedThrough + 1 : m_windowStart;
        // A move pending during this root's last realize is a carry-over.
        const uint64_t carryThrough = known != m_incrementalTargets.end() ? known->second.attemptedThrough : generation;
        std::vector<EFFECTGRAPH_MOVE_RECT> forward;
        // Beside them, for the counters: each forwarded move's sequence, and
        // the part inside the target of each move that leaves it.
        std::vector<uint64_t> forwardSequences;
        std::vector<D2D1_RECT_L> clippedDestinations;
        if (!m_ingestedMoves.empty() && !backlogLost)
        {
            const auto reads = ReadsOf(id);
            for (const auto& ingested : m_ingestedMoves)
            {
                if (ingested.generation < reportFrom || !reads.contains(ingested.input)) continue;
                const RECT& d = ingested.move.DestinationRect;
                const D2D1_RECT_L dest{ d.left, d.top, d.right, d.bottom };
                const D2D1_RECT_L source = Graph::DirtyMove{ dest, { ingested.move.SourcePoint.x, ingested.move.SourcePoint.y } }.Source();
                if (Graph::DirtyRegion::Contains(bounds, dest) && Graph::DirtyRegion::Contains(bounds, source))
                {
                    forward.push_back(ingested.move);
                    forwardSequences.push_back(ingested.sequence);
                }
                else if (const auto inside = Graph::DirtyRegion::Intersect(bounds, dest); !Graph::DirtyRegion::IsEmpty(inside))
                {
                    clippedDestinations.push_back(inside);
                }
            }
        }
        // A move the host cannot be told of is drawn instead: the host
        // forwards only `drawn` then.
        const bool applyMoves = options.applyMoves && forward.size() <= movedCapacity;

        // Drawn inside the frame's own session, after its compute work: the
        // pending region is complete only once the frame's passes have run.
        // Each rect is clipped: under SOURCE_COPY, DrawImage clears to
        // transparent everything inside the clip that the image does not cover.
        std::vector<D2D1_RECT_L> drawnRects;
        std::vector<Graph::DirtyMove> appliedMoves;
        bool wholeDraw = false;
        bool cleared = false;
        auto& graph = m_graph;
        auto& evaluator = m_evaluator;
        HRESULT hr = EvaluateFor(id, bitmap.get(), [&](ID2D1DeviceContext5* dc)
        {
            const auto* rendered = graph.FindNode(id);
            ID2D1Image* image = rendered ? rendered->cachedOutput : nullptr;
            Graph::DirtyRegion region = incremental ? evaluator.PendingRegion(id).ClippedTo(bounds)
                                                    : Graph::DirtyRegion::Whole();
            // Moves first, in order; those that cannot be copied become rects.
            auto plan = Graph::PlanMoves(region, carryThrough, applyMoves && image, m_collapsedMoves);
            region = std::move(plan.rects);
            if (region.IsRects() && region.Covers(bounds)) region = Graph::DirtyRegion::Whole();
            if (region.IsWhole())
            {
                wholeDraw = true;
                drawnRects = { bounds };
                cleared = true;
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                if (image)
                    dc->DrawImage(image, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
                return;
            }
            for (const auto& rect : region.Rects())
                drawnRects.push_back(rect.rect);
            if (!plan.kept.empty())
            {
                // The copies reach the device before the rects: nothing is
                // queued in this session yet, and D2D submits it at EndDraw.
                dc->Flush();
                if (ApplyMoves(target, plan.kept))
                {
                    appliedMoves = std::move(plan.kept);
                }
                else
                {
                    // No scratch: every move is drawn at its destination.
                    for (const auto& move : plan.kept)
                        drawnRects.push_back(move.destination);
                }
            }
            if (drawnRects.size() > drawnCapacity)
            {
                D2D1_RECT_L box = drawnRects.front();
                for (const auto& rect : drawnRects)
                    box = { (std::min)(box.left, rect.left), (std::min)(box.top, rect.top),
                            (std::max)(box.right, rect.right), (std::max)(box.bottom, rect.bottom) };
                drawnRects = { box };
            }
            if (!image) return;
            if (Testing::g_unclipNextIncrementalDraw)
            {
                Testing::g_unclipNextIncrementalDraw = false;
                dc->DrawImage(image, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
                return;
            }
            for (const auto& rect : drawnRects)
            {
                const D2D1_RECT_F area{ static_cast<float>(rect.left), static_cast<float>(rect.top),
                                        static_cast<float>(rect.right), static_cast<float>(rect.bottom) };
                const D2D1_POINT_2F offset{ area.left, area.top };
                dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
                dc->DrawImage(image, &offset, &area, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                              D2D1_COMPOSITE_MODE_SOURCE_COPY);
                dc->PopAxisAlignedClip();
            }
        }, true, uncacheRoot);
        if (auto attempted = m_incrementalTargets.find(id); attempted != m_incrementalTargets.end())
            attempted->second.attemptedThrough = generation;
        m_graph.AdvanceRegionGeneration();
        m_windowStart = m_graph.RegionGeneration();
        if (FAILED(hr))
        {
            // A whole draw cleared the target before it failed: no node's
            // record of it holds any more, so the next realize draws whole.
            // Rects the failure left pending do not cover what was cleared.
            if (cleared) ForgetIncrementalTarget(target);
            // A root that keeps failing must not hold moves past the backlog.
            PruneIngestedMoves();
            return hr;
        }

        // What drew this root also drew what feeds it: their pending regions
        // go too, unless a node is realized incrementally on its own.
        std::vector<uint32_t> upstream{ id };
        std::unordered_set<uint32_t> seen{ id };
        while (!upstream.empty())
        {
            const uint32_t at = upstream.back();
            upstream.pop_back();
            for (const auto* edge : m_graph.GetInputEdges(at))
            {
                if (!seen.insert(edge->sourceNodeId).second || m_incrementalTargets.contains(edge->sourceNodeId)) continue;
                m_evaluator.AcknowledgeRegion(edge->sourceNodeId);
                upstream.push_back(edge->sourceNodeId);
            }
        }

        auto& record = m_incrementalTargets[id];
        record.texture.copy_from(target);
        record.width = desc.Width;
        record.height = desc.Height;
        record.format = desc.Format;
        record.uncached = uncacheRoot;
        record.reportedThrough = generation;
        record.attemptedThrough = generation;
        record.backlogLost = false;
        ForgetIncrementalTarget(target, id);
        PruneIngestedMoves();

        auto& stats = m_evaluator.MutableRegionStats();
        ++stats.realizes;
        if (!drawnRects.empty()) ++stats.draws;
        if (wholeDraw) ++stats.wholeDraws;
        stats.rectsDrawn += drawnRects.size();
        for (const auto& rect : drawnRects)
            stats.pixelsDrawn += static_cast<uint64_t>(rect.right - rect.left) * static_cast<uint64_t>(rect.bottom - rect.top);
        stats.pixelsTarget += static_cast<uint64_t>(desc.Width) * desc.Height;
        stats.movesApplied += appliedMoves.size();
        // A move reaching this root's target that was not copied counts as
        // drawn only where `drawn` covers its destination (inside the
        // target): one a node on the way collapsed, the plan collapsed, one
        // leaving the target, or a whole draw took. One neither copied nor
        // drawn counts as neither, so applied + collapsed falls short.
        for (size_t i = 0; i < forward.size(); ++i)
        {
            if (std::ranges::any_of(appliedMoves, [&](const Graph::DirtyMove& m) { return m.sequence == forwardSequences[i]; }))
                continue;
            const RECT& d = forward[i].DestinationRect;
            if (wholeDraw || CoveredBy({ d.left, d.top, d.right, d.bottom }, drawnRects)) ++stats.movesCollapsed;
        }
        for (const auto& inside : clippedDestinations)
            if (wholeDraw || CoveredBy(inside, drawnRects)) ++stats.movesCollapsed;
        for (const auto& move : appliedMoves)
            stats.movePixelsCopied += static_cast<uint64_t>(move.destination.right - move.destination.left) *
                                      static_cast<uint64_t>(move.destination.bottom - move.destination.top);
        if (wholeDraw) record.drift.clear();
        if (before && !wholeDraw)
            VerifyIncrementalDraw(id, target, before.get(), drawnRects, appliedMoves, record.drift);

        *drawnCount = static_cast<UINT32>(drawnRects.size());
        for (size_t i = 0; i < drawnRects.size(); ++i)
            drawn[i] = { drawnRects[i].left, drawnRects[i].top, drawnRects[i].right, drawnRects[i].bottom };
        // Every move this root's input was handed, applied or drawn, for the
        // host to forward before `drawn`; none when they do not all fit, and
        // then none was applied.
        if (forward.size() <= movedCapacity)
        {
            if (movedCount) *movedCount = static_cast<UINT32>(forward.size());
            std::ranges::copy(forward, moved);
            stats.movesReported += forward.size();
        }
        // A readback still to be collected: the host calls again to commit it.
        return m_deferredCommit && m_evaluator.HasPendingReadbacks() ? S_FALSE : S_OK;
    }

    STDMETHODIMP Context::PollReadbacks()
    {
        if (!m_deferredCommit) return E_NOT_VALID_STATE;
        if (HRESULT removed = DeviceRemovedReason(); FAILED(removed)) return removed;
        m_evaluator.PollReadbacks(m_graph);
        return m_evaluator.HasPendingReadbacks() ? S_FALSE : S_OK;
    }

    namespace
    {
        UINT BytesPerPixel(DXGI_FORMAT format)
        {
            switch (format)
            {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return 16;
            default:
                return 0;
            }
        }
    }

    void Context::VerifyIncrementalDraw(uint32_t nodeId, ID3D11Texture2D* target, ID3D11Texture2D* before,
                                        const std::vector<D2D1_RECT_L>& drawn, const std::vector<Graph::DirtyMove>& moves,
                                        std::vector<uint8_t>& drift)
    {
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        const UINT pixelBytes = BytesPerPixel(desc.Format);
        const auto* node = m_graph.FindNode(nodeId);
        if (!pixelBytes || !node || !node->cachedOutput) return;

        // A whole render of the same output, into a target of the same format.
        D3D11_TEXTURE2D_DESC wholeDesc = desc;
        wholeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        wholeDesc.MiscFlags = 0;
        wholeDesc.CPUAccessFlags = 0;
        wholeDesc.Usage = D3D11_USAGE_DEFAULT;
        winrt::com_ptr<ID3D11Texture2D> whole;
        winrt::com_ptr<ID2D1Bitmap1> wholeBitmap;
        if (FAILED(m_devices.d3dDevice->CreateTexture2D(&wholeDesc, nullptr, whole.put())) ||
            FAILED(TargetBitmap(whole.get(), wholeBitmap)))
            return;
        winrt::com_ptr<ID2D1Image> saved;
        m_dc->GetTarget(saved.put());
        m_dc->SetTarget(wholeBitmap.get());
        m_dc->BeginDraw();
        m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_dc->DrawImage(node->cachedOutput, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
        const HRESULT drewWhole = m_dc->EndDraw();
        m_dc->SetTarget(saved.get());
        if (FAILED(drewWhole)) return;

        // The three images on the CPU, rows packed.
        const auto read = [&](ID3D11Texture2D* texture, std::vector<uint8_t>& bytes)
        {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> staging;
            if (FAILED(m_devices.d3dDevice->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) return false;
            m_devices.d3dContext->CopyResource(staging.get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_devices.d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
            const size_t rowBytes = static_cast<size_t>(desc.Width) * pixelBytes;
            bytes.resize(rowBytes * desc.Height);
            for (UINT y = 0; y < desc.Height; ++y)
                std::memcpy(&bytes[y * rowBytes], static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
            m_devices.d3dContext->Unmap(staging.get(), 0);
            return true;
        };
        std::vector<uint8_t> after, previous, reference;
        if (!read(target, after) || !read(before, previous) || !read(whole.get(), reference)) return;

        // The applied moves, replayed in order onto the target as it was:
        // outside the rects drawn, the target must equal that. Their
        // destinations may drift from a whole render (a dithering effect)
        // until something draws them again.
        const size_t rowBytes = static_cast<size_t>(desc.Width) * pixelBytes;
        const size_t pixels = static_cast<size_t>(desc.Width) * desc.Height;
        if (drift.size() != pixels) drift.assign(pixels, 0);
        for (const auto& move : moves)
        {
            const D2D1_RECT_L source = move.Source();
            const size_t width = static_cast<size_t>(move.destination.right - move.destination.left) * pixelBytes;
            std::vector<uint8_t> block;
            for (LONG y = source.top; y < source.bottom; ++y)
                block.insert(block.end(), previous.begin() + y * rowBytes + source.left * pixelBytes,
                             previous.begin() + y * rowBytes + source.left * pixelBytes + width);
            for (LONG y = move.destination.top; y < move.destination.bottom; ++y)
            {
                std::memcpy(&previous[y * rowBytes + move.destination.left * pixelBytes],
                            &block[static_cast<size_t>(y - move.destination.top) * width], width);
                std::memset(&drift[static_cast<size_t>(y) * desc.Width + move.destination.left], 1,
                            static_cast<size_t>(move.destination.right - move.destination.left));
            }
        }
        for (const auto& rect : drawn)
            for (LONG y = rect.top; y < rect.bottom; ++y)
                std::memset(&drift[static_cast<size_t>(y) * desc.Width + rect.left], 0,
                            static_cast<size_t>(rect.right - rect.left));
        const bool eightBit = pixelBytes == 4 && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM;

        uint64_t outside = 0, notWhole = 0, drifted = 0;
        for (UINT y = 0; y < desc.Height; ++y)
            for (UINT x = 0; x < desc.Width; ++x)
            {
                const size_t at = (static_cast<size_t>(y) * desc.Width + x) * pixelBytes;
                if (std::memcmp(&after[at], &reference[at], pixelBytes) != 0)
                {
                    int distance = 0;
                    for (UINT c = 0; eightBit && c < pixelBytes; ++c)
                        distance = (std::max)(distance, std::abs(static_cast<int>(after[at + c]) - reference[at + c]));
                    if (drift[static_cast<size_t>(y) * desc.Width + x] && eightBit && distance <= Rendering::cMoveDitherCodes)
                        ++drifted;
                    else
                        ++notWhole;
                }
                const bool inside = std::ranges::any_of(drawn, [&](const D2D1_RECT_L& rect)
                {
                    return static_cast<LONG>(x) >= rect.left && static_cast<LONG>(x) < rect.right &&
                           static_cast<LONG>(y) >= rect.top && static_cast<LONG>(y) < rect.bottom;
                });
                if (!inside && std::memcmp(&after[at], &previous[at], pixelBytes) != 0) ++outside;
            }
        auto& stats = m_evaluator.MutableRegionStats();
        ++stats.verified;
        stats.verifyOutsideMismatches += outside;
        stats.verifyWholeMismatches += notWhole;
        stats.verifyMoveDrift += drifted;
        if (outside || notWhole)
            std::fprintf(stderr, "[EffectGraph] verifyDirtyRegions: node %u, %llu pixel(s) changed outside the %zu rect(s) "
                                 "drawn, %llu differ from a whole render\n",
                         nodeId, static_cast<unsigned long long>(outside), drawn.size(),
                         static_cast<unsigned long long>(notWhole));
    }

    STDMETHODIMP_(void) Context::GetD3DDevice(ID3D11Device** device)
    {
        if (!device) return;
        *device = m_devices.d3dDevice.get();
        (*device)->AddRef();
    }

    STDMETHODIMP_(void) Context::GetD2DDevice(ID2D1Device** device)
    {
        if (!device) return;
        m_devices.d2dDevice.as<ID2D1Device>().copy_to(device);
    }

    // ---- Analysis values

    namespace
    {
        bool IsComputeAnalysis(const Graph::EffectNode& node)
        {
            return node.customEffect.has_value() &&
                   node.customEffect->shaderType == Graph::CustomShaderType::D3D11ComputeShader &&
                   !node.customEffect->analysisFields.empty();
        }

        winrt::com_ptr<ID3D11Buffer> BufferOf(ID3D11ShaderResourceView* view)
        {
            winrt::com_ptr<ID3D11Resource> resource;
            view->GetResource(resource.put());
            return resource.as<ID3D11Buffer>();
        }
    }

    HRESULT Context::ReadValuesBuffer(ID3D11Buffer* buffer, const std::vector<Graph::AnalysisFieldDescriptor>& layout,
                                      std::vector<Graph::AnalysisFieldValue>& values)
    {
        // Copied to a staging buffer and mapped: this waits for the work that
        // writes it, which is what asking for CPU values means.
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        winrt::com_ptr<ID3D11Buffer> staging;
        if (HRESULT hr = m_devices.d3dDevice->CreateBuffer(&desc, nullptr, staging.put()); FAILED(hr)) return hr;
        m_devices.d3dContext->CopyResource(staging.get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (HRESULT hr = m_devices.d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped); FAILED(hr)) return hr;
        values = Graph::UnpackAnalysisFields(layout, static_cast<const float*>(mapped.pData), desc.ByteWidth / sizeof(float));
        m_devices.d3dContext->Unmap(staging.get(), 0);
        return S_OK;
    }

    HRESULT Context::ValuesOf(uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values)
    {
        if (HRESULT hr = EvaluateNode(nodeId); FAILED(hr)) return hr;
        const auto* node = m_graph.FindNode(nodeId);
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        if (IsComputeAnalysis(*node))
            if (auto view = m_evaluator.AnalysisBuffer(nodeId))
                return ReadValuesBuffer(BufferOf(view.get()).get(), node->customEffect->analysisFields, values);
        values = node->analysisOutput.fields;
        return S_OK;
    }

    HRESULT Context::Upload(uint32_t nodeId, const std::vector<Graph::AnalysisFieldValue>& fields)
    {
        const auto float4s = Graph::PackAnalysisFields(fields);
        const UINT elements = static_cast<UINT>(float4s.size() / 4);
        auto& uploaded = m_uploadedValues[nodeId];
        if (!uploaded.buffer || uploaded.elements != elements)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = elements * 16;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = 16;
            uploaded = {};
            if (HRESULT hr = m_devices.d3dDevice->CreateBuffer(&desc, nullptr, uploaded.buffer.put()); FAILED(hr)) return hr;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = DXGI_FORMAT_UNKNOWN;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            viewDesc.Buffer.NumElements = elements;
            if (HRESULT hr = m_devices.d3dDevice->CreateShaderResourceView(uploaded.buffer.get(), &viewDesc, uploaded.view.put());
                FAILED(hr))
            {
                uploaded = {};
                return hr;
            }
            uploaded.elements = elements;
        }
        m_devices.d3dContext->UpdateSubresource(uploaded.buffer.get(), 0, nullptr, float4s.data(), 0, 0);
        return S_OK;
    }

    HRESULT Context::ValuesBufferOf(uint32_t nodeId, IEffectGraphValuesBuffer** buffer)
    {
        if (HRESULT hr = EvaluateNode(nodeId); FAILED(hr)) return hr;
        const auto* node = m_graph.FindNode(nodeId);
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        if (IsComputeAnalysis(*node))
        {
            if (auto view = m_evaluator.AnalysisBuffer(nodeId))
            {
                winrt::make<ValuesBuffer>(this, nodeId, node->customEffect->analysisFields, BufferOf(view.get()), view)
                    .as<IEffectGraphValuesBuffer>().copy_to(buffer);
                return S_OK;
            }
        }
        if (node->analysisOutput.fields.empty()) return S_FALSE;
        if (HRESULT hr = Upload(nodeId, node->analysisOutput.fields); FAILED(hr)) return hr;
        const auto& uploaded = m_uploadedValues[nodeId];
        winrt::make<ValuesBuffer>(this, nodeId, Graph::AnalysisLayoutOf(node->analysisOutput.fields), uploaded.buffer, uploaded.view)
            .as<IEffectGraphValuesBuffer>().copy_to(buffer);
        return S_OK;
    }

    HRESULT Context::WriteValues(uint32_t nodeId, uint64_t epoch, UINT32 field, const float* floats, UINT32 count)
    {
        auto* node = epoch == m_epoch ? m_graph.FindNode(nodeId) : nullptr;
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        const bool compute = IsComputeAnalysis(*node);

        // The node's values on the CPU, which bindings served as constants
        // read: a compute node's come from its buffer when none were read back.
        std::vector<Graph::AnalysisFieldValue> values = node->analysisOutput.fields;
        if (compute && values.size() != node->customEffect->analysisFields.size())
        {
            auto view = m_evaluator.AnalysisBuffer(nodeId);
            if (!view) return E_NOT_VALID_STATE;
            if (HRESULT hr = ReadValuesBuffer(BufferOf(view.get()).get(), node->customEffect->analysisFields, values); FAILED(hr))
                return hr;
        }
        if (field >= values.size()) return E_INVALIDARG;
        auto& value = values[field];
        const uint32_t componentCount = Graph::AnalysisFieldComponentCount(value.type);
        const uint32_t arrayLength = Graph::AnalysisFieldArrayLength(value);
        const uint32_t expected = Graph::AnalysisFieldIsArray(value.type)
            ? (value.type == Graph::AnalysisFieldType::FloatArray ? arrayLength : arrayLength * componentCount)
            : componentCount;
        if (count != expected) return E_INVALIDARG;
        if (Graph::AnalysisFieldIsArray(value.type)) value.arrayData.assign(floats, floats + count);
        else std::copy(floats, floats + count, value.components.begin());

        // The GPU copy: a compute node's results buffer in place, or the upload.
        if (compute)
        {
            const auto row = Graph::PackAnalysisFields({ value });
            const auto layout = Graph::AnalysisLayoutOf(values);
            uint32_t start = 0;
            for (UINT32 i = 0; i < field; ++i) start += layout[i].pixelCount();
            if (HRESULT hr = m_evaluator.WriteAnalysis(nodeId, start, row.data(), static_cast<uint32_t>(row.size() / 4), &m_graph);
                FAILED(hr))
                return hr;
        }
        else if (m_uploadedValues.contains(nodeId))
        {
            if (HRESULT hr = Upload(nodeId, values); FAILED(hr)) return hr;
        }
        // A Hosted field's committed value moves only on CommitFields: the
        // copy to the texture left its texel alone, and so does the record.
        if (compute && m_deferredCommit && field < node->analysisOutput.fields.size() &&
            m_evaluator.FieldCommitPolicy(m_graph, nodeId, field) == Graph::CommitPolicy::Hosted)
            values[field] = node->analysisOutput.fields[field];
        node->analysisOutput.fields = std::move(values);

        // Whatever reads the node's values is evaluated again; the node is
        // not, which would write its own values back.
        for (auto& consumer : m_graph.MutableNodes())
        {
            bool reads = false;
            Graph::ForEachBindingSource(consumer, [&](uint32_t source) { reads |= source == nodeId; });
            if (reads) Graph::MarkNodeChanged(consumer);
        }
        return S_OK;
    }

    // ---- IEffectGraphContextInternal

    STDMETHODIMP_(Rendering::FrameResult) Context::RunFrame(const Rendering::FrameRequest& request)
    {
        if (HRESULT removed = DeviceRemovedReason(); FAILED(removed))
        {
            Rendering::FrameResult skipped;
            skipped.deviceRemoved = removed;
            return skipped;
        }
        SyncFrameInputs();
        auto frame = request;
        for (uint32_t id : UncachedRoots())
            if (std::ranges::find(frame.uncachedRoots, id) == frame.uncachedRoots.end()) frame.uncachedRoots.push_back(id);
        auto result = Rendering::FrameExecutor::Run(m_graph, m_evaluator, m_dc.get(), frame);
        result.deviceRemoved = DeviceRemovedReason();
        return result;
    }

    STDMETHODIMP Context::MoveToDevice(ID3D11Device* device)
    {
        if (!device) return E_INVALIDARG;
        if (!(device->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT))
            return EFFECTGRAPH_E_UNSUPPORTED_DEVICE;

        // The new stack first, so a failure leaves the context on its old device.
        Rendering::DeviceStack devices;
        winrt::com_ptr<ID2D1DeviceContext5> dc;
        if (HRESULT hr = Rendering::CreateDeviceStackOn(device, devices); FAILED(hr)) return hr;
        if (HRESULT hr = devices.CreateDeviceContext(dc); FAILED(hr)) return hr;
        winrt::com_ptr<ID2D1Factory1> factory;
        devices.d2dFactory->QueryInterface(IID_PPV_ARGS(factory.put()));
        if (HRESULT hr = Engine::RegisterEngineD2DEffects(factory.get()); FAILED(hr)) return hr;

        // Everything the old device made goes; every node is left dirty, so
        // sources reopen and effects rebuild on the next frame.
        m_evaluator.ReleaseCache(m_graph);
        m_sources.ReleaseCache();
        m_metadataEffects.clear();
        m_uploadedValues.clear();
        // Targets on the old device are gone; the next realize draws whole.
        m_incrementalTargets.clear();
        m_moveScratch = nullptr;
        for (auto& [id, input] : m_inputs)
        {
            const bool fromTexture = input.fromTexture;
            input = {};
            input.fromTexture = fromTexture;
        }
        m_dc = std::move(dc);
        m_devices = std::move(devices);
        return S_OK;
    }
}

// ---- Exports

EFFECTGRAPH_API HRESULT WINAPI EffectGraphCreateContext(ID3D11Device* device, const EFFECTGRAPH_CONTEXT_DESC* desc,
                                                        REFIID riid, void** context)
{
    if (!context) return E_POINTER;
    *context = nullptr;
    try
    {
        auto created = winrt::make_self<ShaderLab::Api::Context>();
        if (HRESULT hr = created->Initialize(device, desc); FAILED(hr)) return hr;
        return created->QueryInterface(riid, context);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

namespace
{
    HRESULT ResultOf(const ShaderLab::Effects::EffectBlobRegistration& registration)
    {
        using ShaderLab::Effects::EffectBlobStatus;
        switch (registration.status)
        {
        case EffectBlobStatus::Registered:    return registration.error.empty() ? S_OK : S_FALSE;
        case EffectBlobStatus::NoneIncluded:  return S_FALSE;
        case EffectBlobStatus::Incompatible:  return EFFECTGRAPH_E_INCOMPATIBLE_EFFECT;
        case EffectBlobStatus::NotRegistered: return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        default:                              return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
    }
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphRegisterEffects(const EFFECTGRAPH_EFFECT_SOURCE* source)
{
    if (!source || !source->description || (source->buildCount && !source->builds)) return E_INVALIDARG;
    ShaderLab::Effects::EffectBlobParts parts;
    parts.format = source->format;
    parts.contractHash = source->contractHash;
    parts.description = { static_cast<const uint8_t*>(source->description), source->descriptionSize };
    for (UINT32 i = 0; i < source->buildCount; ++i)
        if (source->builds[i].data)
            parts.records.emplace_back(static_cast<const uint8_t*>(source->builds[i].data), source->builds[i].size);
    return ResultOf(ShaderLab::Effects::RegisterEffectBlobParts(parts));
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphAddEffects(const EFFECTGRAPH_EFFECT_SOURCE* source)
{
    if (!source || !source->description || (source->buildCount && !source->builds)) return E_INVALIDARG;
    ShaderLab::Effects::EffectBlobParts parts;
    parts.format = source->format;
    parts.contractHash = source->contractHash;
    parts.description = { static_cast<const uint8_t*>(source->description), source->descriptionSize };
    for (UINT32 i = 0; i < source->buildCount; ++i)
        if (source->builds[i].data)
            parts.records.emplace_back(static_cast<const uint8_t*>(source->builds[i].data), source->builds[i].size);
    ShaderLab::Effects::QueueEffectBlobParts(std::move(parts));
    return S_OK;
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphRegisterEffect(const void* data, UINT32 size)
{
    if (!data || size == 0) return E_INVALIDARG;
    return ResultOf(ShaderLab::Effects::RegisterEffectBlob(
        std::span<const uint8_t>(static_cast<const uint8_t*>(data), size)));
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphLoadEffects(PCWSTR directory)
{
    if (!directory) return E_INVALIDARG;
    const auto& report = ShaderLab::Effects::ShaderLabEffects::Instance().LoadUserEffects(directory);
    return report.errors.empty() ? S_OK : S_FALSE;
}

EFFECTGRAPH_API void WINAPI EffectGraphShutdown()
{
    ShaderLab::Engine::ShutdownEngineServices();
}
