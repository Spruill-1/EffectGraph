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
            if (!descriptor.hlslSource.empty() && Effects::EffectClsid(descriptor.effectId) == effectId)
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
        m_evaluator.InvalidateNode(m_graph, id);
        m_sources.PruneOrphans(m_graph.Nodes());
        return S_OK;
    }

    STDMETHODIMP_(void) Context::ReplaceGraph(Graph::GraphModel&& graph)
    {
        m_uploadedValues.clear();
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
            if (!node.dirty && !m_sources.HasVideoProvider(node.id)) continue;
            // Preparing an unchanged file marks the source clean, which the
            // GUI wants on a redraw; here a source marked changed must still
            // re-render what reads it. Evaluating it clears the flag.
            const bool changed = node.dirty;
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

    HRESULT Context::EvaluateFor(uint32_t nodeId, ID2D1Image* target, std::function<void(ID2D1DeviceContext5*)> draw)
    {
        if (HRESULT removed = DeviceRemovedReason(); FAILED(removed)) return removed;
        SyncFrameInputs();
        Rendering::FrameRequest request;
        request.demand = Rendering::FrameDemand::Roots;
        request.roots = { nodeId };
        request.target = target;
        request.draw = std::move(draw);
        const auto result = Rendering::FrameExecutor::Run(m_graph, m_evaluator, m_dc.get(), request);
        // A device removed during the frame fails EndDraw with
        // D2DERR_RECREATE_TARGET and nodes with odd errors; say why instead.
        if (HRESULT removed = DeviceRemovedReason(); FAILED(removed)) return removed;
        if (FAILED(result.endDraw)) return result.endDraw;

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

    STDMETHODIMP Context::RealizeTo(IEffectGraphNode* node, ID3D11Texture2D* target)
    {
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        if (!target) return E_INVALIDARG;
        if (m_graph.FindNode(id)->outputPins.empty()) return E_INVALIDARG;

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
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put())))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;

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
            if (HRESULT hr = m_evaluator.WriteAnalysis(nodeId, start, row.data(), static_cast<uint32_t>(row.size() / 4)); FAILED(hr))
                return hr;
        }
        else if (m_uploadedValues.contains(nodeId))
        {
            if (HRESULT hr = Upload(nodeId, values); FAILED(hr)) return hr;
        }
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
        auto result = Rendering::FrameExecutor::Run(m_graph, m_evaluator, m_dc.get(), request);
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
