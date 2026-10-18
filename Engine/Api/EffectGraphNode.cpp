#include "pch_engine.h"
#include "EffectGraphImpl.h"
#include "../../Graph/GraphEdits.h"
#include "../../Rendering/Evaluator/BindingResolver.h"

namespace ShaderLab::Api
{
    namespace
    {
        namespace Numerics = winrt::Windows::Foundation::Numerics;

        D2D1_PROPERTY_TYPE TypeOf(const Graph::PropertyValue& value)
        {
            return std::visit([](auto&& v) -> D2D1_PROPERTY_TYPE
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>) return D2D1_PROPERTY_TYPE_FLOAT;
                else if constexpr (std::is_same_v<T, int32_t>) return D2D1_PROPERTY_TYPE_INT32;
                else if constexpr (std::is_same_v<T, uint32_t>) return D2D1_PROPERTY_TYPE_UINT32;
                else if constexpr (std::is_same_v<T, bool>) return D2D1_PROPERTY_TYPE_BOOL;
                else if constexpr (std::is_same_v<T, std::wstring>) return D2D1_PROPERTY_TYPE_STRING;
                else if constexpr (std::is_same_v<T, Numerics::float2>) return D2D1_PROPERTY_TYPE_VECTOR2;
                else if constexpr (std::is_same_v<T, Numerics::float3>) return D2D1_PROPERTY_TYPE_VECTOR3;
                else if constexpr (std::is_same_v<T, Numerics::float4>) return D2D1_PROPERTY_TYPE_VECTOR4;
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>) return D2D1_PROPERTY_TYPE_MATRIX_5X4;
                else return D2D1_PROPERTY_TYPE_BLOB;   // std::vector<float>
            }, value);
        }

        template <typename T>
        std::optional<T> Pod(const BYTE* data, UINT32 size)
        {
            if (size != sizeof(T)) return std::nullopt;
            T value{};
            std::memcpy(&value, data, sizeof(T));
            return value;
        }

        // A D2D-typed value as the graph stores it; nullopt for a type the
        // graph cannot hold (matrices other than 5x4, colour contexts, ...).
        std::optional<Graph::PropertyValue> FromBytes(D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 size)
        {
            switch (type)
            {
            case D2D1_PROPERTY_TYPE_FLOAT: if (auto v = Pod<float>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_ENUM:
            case D2D1_PROPERTY_TYPE_UINT32: if (auto v = Pod<uint32_t>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_INT32: if (auto v = Pod<int32_t>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_BOOL: if (auto v = Pod<BOOL>(data, size)) return *v != FALSE; break;
            case D2D1_PROPERTY_TYPE_VECTOR2: if (auto v = Pod<Numerics::float2>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_VECTOR3: if (auto v = Pod<Numerics::float3>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_VECTOR4: if (auto v = Pod<Numerics::float4>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_MATRIX_5X4: if (auto v = Pod<D2D1_MATRIX_5X4_F>(data, size)) return *v; break;
            case D2D1_PROPERTY_TYPE_STRING:
                if (size >= sizeof(wchar_t) && size % sizeof(wchar_t) == 0)
                {
                    const auto* text = reinterpret_cast<const wchar_t*>(data);
                    return std::wstring(text, wcsnlen(text, size / sizeof(wchar_t)));
                }
                break;
            case D2D1_PROPERTY_TYPE_BLOB:
                if (size % sizeof(float) == 0)
                {
                    std::vector<float> values(size / sizeof(float));
                    if (size) std::memcpy(values.data(), data, size);
                    return values;
                }
                break;
            default: break;
            }
            return std::nullopt;
        }

        std::vector<BYTE> ToBytes(const Graph::PropertyValue& value)
        {
            return std::visit([](auto&& v) -> std::vector<BYTE>
            {
                using T = std::decay_t<decltype(v)>;
                auto raw = [](const void* data, size_t size)
                {
                    const auto* bytes = static_cast<const BYTE*>(data);
                    return std::vector<BYTE>(bytes, bytes + size);
                };
                if constexpr (std::is_same_v<T, bool>) { const BOOL b = v ? TRUE : FALSE; return raw(&b, sizeof(b)); }
                else if constexpr (std::is_same_v<T, std::wstring>) return raw(v.c_str(), (v.size() + 1) * sizeof(wchar_t));
                else if constexpr (std::is_same_v<T, std::vector<float>>) return raw(v.data(), v.size() * sizeof(float));
                else return raw(&v, sizeof(v));
            }, value);
        }

        // Integer and boolean values are accepted for a float property:
        // ShaderLab's option parameters are floats, D2D callers pass enums.
        std::optional<Graph::PropertyValue> ConvertForTarget(const Graph::PropertyValue& value, D2D1_PROPERTY_TYPE target)
        {
            const D2D1_PROPERTY_TYPE given = TypeOf(value);
            if (target == D2D1_PROPERTY_TYPE_UNKNOWN || given == target) return value;
            if (target == D2D1_PROPERTY_TYPE_ENUM && given == D2D1_PROPERTY_TYPE_UINT32) return value;
            if (target == D2D1_PROPERTY_TYPE_FLOAT)
            {
                if (auto* u = std::get_if<uint32_t>(&value)) return static_cast<float>(*u);
                if (auto* i = std::get_if<int32_t>(&value)) return static_cast<float>(*i);
                if (auto* b = std::get_if<bool>(&value)) return *b ? 1.0f : 0.0f;
            }
            return std::nullopt;
        }
    }

    NodeBase::NodeBase(Context* context, uint32_t nodeId)
        : m_nodeId(nodeId), m_epoch(context->Epoch())
    {
        m_context.copy_from(context);
    }

    NodeBase::~NodeBase()
    {
        m_context->ForgetNodeObject(m_nodeId, this);
    }

    Graph::EffectNode* NodeBase::Live() const
    {
        if (m_epoch != m_context->Epoch()) return nullptr;
        return m_context->Model().FindNode(m_nodeId);
    }

    // ---- IEffectGraphNode

    void NodeBase::GetContextImpl(IEffectGraphContext** context)
    {
        if (context) m_context->QueryInterface(__uuidof(IEffectGraphContext), reinterpret_cast<void**>(context));
    }

    HRESULT NodeBase::SetInputImpl(UINT32 index, IEffectGraphNode* input)
    {
        auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        if (index >= node->inputPins.size()) return E_INVALIDARG;
        auto& graph = m_context->Model();
        if (!input)
        {
            graph.DisconnectInput(m_nodeId, index);
            Graph::MarkNodeChanged(graph, m_nodeId);
            return S_OK;
        }
        uint32_t sourceId = 0;
        if (HRESULT hr = m_context->IdOf(input, sourceId); FAILED(hr)) return hr;
        if (sourceId == m_nodeId || graph.WouldCreateCycle(sourceId, m_nodeId)) return EFFECTGRAPH_E_CYCLE;
        if (graph.FindNode(sourceId)->outputPins.empty()) return E_INVALIDARG;   // a data-only node has no image
        return graph.Connect(sourceId, 0, m_nodeId, index) ? S_OK : E_INVALIDARG;
    }

    void NodeBase::GetInputImpl(UINT32 index, IEffectGraphNode** input)
    {
        if (!input) return;
        *input = nullptr;
        if (!Live()) return;
        for (const auto* edge : m_context->Model().GetInputEdges(m_nodeId))
            if (edge && edge->destPin == index)
                m_context->NodeFor(edge->sourceNodeId, input);
    }

    UINT32 NodeBase::GetInputCountImpl()
    {
        const auto* node = Live();
        return node ? static_cast<UINT32>(node->inputPins.size()) : 0;
    }

    HRESULT NodeBase::BindImpl(PCWSTR property, IEffectGraphNode* source, PCWSTR field)
    {
        if (!property || !field) return E_INVALIDARG;
        if (!Live()) return EFFECTGRAPH_E_NODE_REMOVED;
        uint32_t sourceId = 0;
        if (HRESULT hr = m_context->IdOf(source, sourceId); FAILED(hr)) return hr;
        auto& graph = m_context->Model();
        if (sourceId == m_nodeId || graph.WouldCreateCycle(sourceId, m_nodeId)) return EFFECTGRAPH_E_CYCLE;
        const std::wstring error = graph.BindProperty(m_nodeId, property, sourceId, field);
        if (!error.empty()) return E_INVALIDARG;
        Graph::MarkNodeChanged(graph, m_nodeId);
        return S_OK;
    }

    HRESULT NodeBase::UnbindImpl(PCWSTR property)
    {
        if (!property) return E_INVALIDARG;
        if (!Live()) return EFFECTGRAPH_E_NODE_REMOVED;
        auto& graph = m_context->Model();
        if (!graph.UnbindProperty(m_nodeId, property)) return S_FALSE;
        Graph::MarkNodeChanged(graph, m_nodeId);
        return S_OK;
    }

    HRESULT NodeBase::GetValuesImpl(IEffectGraphValues** values)
    {
        if (!values) return E_POINTER;
        *values = nullptr;
        if (!Live()) return EFFECTGRAPH_E_NODE_REMOVED;
        std::vector<Graph::AnalysisFieldValue> fields;
        if (HRESULT hr = m_context->ValuesOf(m_nodeId, fields); FAILED(hr)) return hr;
        winrt::make<Values>(std::move(fields)).as<IEffectGraphValues>().copy_to(values);
        return S_OK;
    }

    HRESULT NodeBase::GetValuesBufferImpl(IEffectGraphValuesBuffer** buffer)
    {
        if (!buffer) return E_POINTER;
        *buffer = nullptr;
        if (!Live()) return EFFECTGRAPH_E_NODE_REMOVED;
        return m_context->ValuesBufferOf(m_nodeId, buffer);
    }

    UINT32 NodeBase::GetErrorLengthImpl()
    {
        const auto* node = Live();
        return node ? static_cast<UINT32>(node->runtimeError.size()) : 0;
    }

    HRESULT NodeBase::GetErrorImpl(PWSTR buffer, UINT32 bufferCount)
    {
        const auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        return CopyString(node->runtimeError, buffer, bufferCount);
    }

    UINT32 NodeBase::GetNameLengthImpl()
    {
        const auto* node = Live();
        return node ? static_cast<UINT32>(node->name.size()) : 0;
    }

    HRESULT NodeBase::GetNameImpl(PWSTR buffer, UINT32 bufferCount)
    {
        const auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        return CopyString(node->name, buffer, bufferCount);
    }

    HRESULT NodeBase::SetNameImpl(PCWSTR name)
    {
        if (!name) return E_INVALIDARG;
        auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        Graph::RenameNode(*node, name);
        return S_OK;
    }

    // ---- IEffectGraphInput

    void NodeBase::InvalidateImpl()
    {
        if (Live()) Graph::MarkNodeChanged(m_context->Model(), m_nodeId);
    }

    HRESULT NodeBase::SetTextureImpl(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding)
    {
        if (!Live()) return EFFECTGRAPH_E_NODE_REMOVED;
        return m_context->SetInputTexture(m_nodeId, texture, encoding);
    }

    HRESULT NodeBase::SetImageImpl(ID2D1Image* image)
    {
        if (!Live()) return EFFECTGRAPH_E_NODE_REMOVED;
        return m_context->SetInputImage(m_nodeId, image);
    }

    // ---- ID2D1Properties

    std::vector<NodeBase::Property> NodeBase::Properties(const Graph::EffectNode& node) const
    {
        std::vector<Property> properties;
        if (node.type == Graph::NodeType::BuiltInEffect && node.effectClsid)
        {
            if (ID2D1Effect* effect = m_context->MetadataEffect(*node.effectClsid))
            {
                for (UINT32 i = 0; i < effect->GetPropertyCount(); ++i)
                {
                    std::wstring name(effect->GetPropertyNameLength(i) + 1, L'\0');
                    effect->GetPropertyName(i, name.data(), static_cast<UINT32>(name.size()));
                    name.resize(name.size() - 1);
                    properties.push_back({ std::move(name), effect->GetType(i) });
                }
                return properties;
            }
        }
        if (node.customEffect)
        {
            for (const auto& parameter : node.customEffect->parameters)
            {
                auto it = node.properties.find(parameter.name);
                properties.push_back({ parameter.name,
                    it != node.properties.end() ? TypeOf(it->second) : TypeOf(parameter.defaultValue) });
            }
        }
        for (const auto& [name, value] : node.properties)
        {
            if (std::ranges::none_of(properties, [&](const Property& p) { return p.name == name; }))
                properties.push_back({ name, TypeOf(value) });
        }
        return properties;
    }

    std::optional<NodeBase::Property> NodeBase::PropertyAt(UINT32 index) const
    {
        const auto* node = Live();
        if (!node) return std::nullopt;
        auto properties = Properties(*node);
        if (index >= properties.size()) return std::nullopt;
        return properties[index];
    }

    std::optional<NodeBase::Property> NodeBase::PropertyNamed(PCWSTR name) const
    {
        const auto* node = Live();
        if (!node || !name) return std::nullopt;
        for (auto& property : Properties(*node))
            if (property.name == name) return property;
        return std::nullopt;
    }

    std::optional<Graph::PropertyValue> NodeBase::CurrentValue(const Graph::EffectNode& node, const Property& property) const
    {
        // A bound property reads what its source gives it. A binding served on
        // the GPU is not written back to the node, so resolve it here.
        if (node.propertyBindings.contains(property.name))
        {
            Rendering::Evaluator::PropertyMap effective;
            Rendering::Evaluator::ResolveBindings(node, m_context->Model(), effective);
            if (auto it = effective.find(property.name); it != effective.end()) return it->second;
        }
        if (auto it = node.properties.find(property.name); it != node.properties.end()) return it->second;
        if (node.type != Graph::NodeType::BuiltInEffect || !node.effectClsid) return std::nullopt;

        // A built-in property nobody set: an index-string key, else D2D's default.
        const auto properties = Properties(node);
        const auto at = std::ranges::find_if(properties, [&](const Property& p) { return p.name == property.name; });
        if (at == properties.end()) return std::nullopt;
        const auto index = static_cast<UINT32>(at - properties.begin());
        if (auto it = node.properties.find(std::to_wstring(index)); it != node.properties.end()) return it->second;
        ID2D1Effect* effect = m_context->MetadataEffect(*node.effectClsid);
        if (!effect) return std::nullopt;
        std::vector<BYTE> bytes(effect->GetValueSize(index));
        if (FAILED(effect->GetValue(index, property.type, bytes.data(), static_cast<UINT32>(bytes.size()))))
            return std::nullopt;
        return FromBytes(property.type, bytes.data(), static_cast<UINT32>(bytes.size()));
    }

    HRESULT NodeBase::Write(const Property& property, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize)
    {
        auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        if (!data && dataSize) return E_INVALIDARG;
        const D2D1_PROPERTY_TYPE given = type == D2D1_PROPERTY_TYPE_UNKNOWN ? property.type : type;
        auto parsed = FromBytes(given, data, dataSize);
        if (!parsed) return E_INVALIDARG;
        auto value = ConvertForTarget(*parsed, property.type);
        if (!value) return E_INVALIDARG;
        Graph::SetNodeProperty(*node, property.name, std::move(*value));
        return S_OK;
    }

    HRESULT NodeBase::Read(const Property& property, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const
    {
        const auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        auto value = CurrentValue(*node, property);
        if (!value) return D2DERR_INVALID_PROPERTY;
        const D2D1_PROPERTY_TYPE stored = TypeOf(*value);
        if (type != D2D1_PROPERTY_TYPE_UNKNOWN && type != property.type && type != stored) return E_INVALIDARG;
        const auto bytes = ToBytes(*value);
        if (dataSize < bytes.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        if (!bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
        return S_OK;
    }

    UINT32 NodeBase::GetPropertyCountImpl() const
    {
        const auto* node = Live();
        return node ? static_cast<UINT32>(Properties(*node).size()) : 0;
    }

    HRESULT NodeBase::GetPropertyNameImpl(UINT32 index, PWSTR name, UINT32 nameCount) const
    {
        auto property = PropertyAt(index);
        if (!property) return D2DERR_INVALID_PROPERTY;
        return CopyString(property->name, name, nameCount);
    }

    UINT32 NodeBase::GetPropertyNameLengthImpl(UINT32 index) const
    {
        auto property = PropertyAt(index);
        return property ? static_cast<UINT32>(property->name.size()) : 0;
    }

    D2D1_PROPERTY_TYPE NodeBase::GetTypeImpl(UINT32 index) const
    {
        auto property = PropertyAt(index);
        return property ? property->type : D2D1_PROPERTY_TYPE_UNKNOWN;
    }

    UINT32 NodeBase::GetPropertyIndexImpl(PCWSTR name) const
    {
        const auto* node = Live();
        if (!node || !name) return D2D1_INVALID_PROPERTY_INDEX;
        const auto properties = Properties(*node);
        for (UINT32 i = 0; i < properties.size(); ++i)
            if (properties[i].name == name) return i;
        return D2D1_INVALID_PROPERTY_INDEX;
    }

    HRESULT NodeBase::SetValueByNameImpl(PCWSTR name, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize)
    {
        auto property = PropertyNamed(name);
        if (!property) return Live() ? D2DERR_INVALID_PROPERTY : EFFECTGRAPH_E_NODE_REMOVED;
        return Write(*property, type, data, dataSize);
    }

    HRESULT NodeBase::SetValueImpl(UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize)
    {
        auto property = PropertyAt(index);
        if (!property) return Live() ? D2DERR_INVALID_PROPERTY : EFFECTGRAPH_E_NODE_REMOVED;
        return Write(*property, type, data, dataSize);
    }

    HRESULT NodeBase::GetValueByNameImpl(PCWSTR name, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const
    {
        auto property = PropertyNamed(name);
        if (!property) return Live() ? D2DERR_INVALID_PROPERTY : EFFECTGRAPH_E_NODE_REMOVED;
        return Read(*property, type, data, dataSize);
    }

    HRESULT NodeBase::GetValueImpl(UINT32 index, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const
    {
        auto property = PropertyAt(index);
        if (!property) return Live() ? D2DERR_INVALID_PROPERTY : EFFECTGRAPH_E_NODE_REMOVED;
        return Read(*property, type, data, dataSize);
    }

    UINT32 NodeBase::GetValueSizeImpl(UINT32 index) const
    {
        const auto* node = Live();
        auto property = PropertyAt(index);
        if (!node || !property) return 0;
        auto value = CurrentValue(*node, *property);
        return value ? static_cast<UINT32>(ToBytes(*value).size()) : 0;
    }

    HRESULT NodeBase::GetSubPropertiesImpl(UINT32 index, ID2D1Properties** subProperties) const
    {
        if (!subProperties) return E_POINTER;
        *subProperties = nullptr;
        const auto* node = Live();
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        // A built-in's metadata (min, max, enum names) is D2D's own.
        if (node->type == Graph::NodeType::BuiltInEffect && node->effectClsid)
            if (ID2D1Effect* effect = m_context->MetadataEffect(*node->effectClsid))
                return effect->GetSubProperties(index, subProperties);
        return D2DERR_NO_SUBPROPERTIES;
    }

    // ---- Values

    STDMETHODIMP_(UINT32) Values::GetFieldIndex(PCWSTR name)
    {
        if (!name) return EFFECTGRAPH_INVALID_INDEX;
        for (UINT32 i = 0; i < m_fields.size(); ++i)
            if (m_fields[i].name == name) return i;
        return EFFECTGRAPH_INVALID_INDEX;
    }

    STDMETHODIMP Values::GetFieldName(UINT32 index, PWSTR buffer, UINT32 bufferCount)
    {
        if (index >= m_fields.size()) return E_INVALIDARG;
        return CopyString(m_fields[index].name, buffer, bufferCount);
    }

    STDMETHODIMP Values::GetFieldInfo(UINT32 index, EFFECTGRAPH_FIELD_INFO* info)
    {
        if (!info) return E_POINTER;
        if (index >= m_fields.size()) return E_INVALIDARG;
        FieldInfoOf(Graph::AnalysisLayoutOf(m_fields), index, *info);
        return S_OK;
    }

    void FieldInfoOf(const std::vector<Graph::AnalysisFieldDescriptor>& layout, size_t index, EFFECTGRAPH_FIELD_INFO& info)
    {
        const auto& field = layout[index];
        info.type = static_cast<EFFECTGRAPH_FIELD_TYPE>(field.type);
        info.componentCount = Graph::AnalysisFieldComponentCount(field.type);
        info.elementCount = Graph::AnalysisFieldIsArray(field.type) ? field.arrayLength : 1;
        UINT32 start = 0;
        for (size_t i = 0; i < index; ++i) start += layout[i].pixelCount();
        info.bufferElement = start;
    }

    ValuesBuffer::ValuesBuffer(Context* context, uint32_t nodeId, std::vector<Graph::AnalysisFieldDescriptor> layout,
                               winrt::com_ptr<ID3D11Buffer> buffer, winrt::com_ptr<ID3D11ShaderResourceView> view)
        : m_nodeId(nodeId), m_layout(std::move(layout)), m_buffer(std::move(buffer)), m_view(std::move(view))
    {
        m_context.copy_from(context);
        m_epoch = context->Epoch();
    }

    STDMETHODIMP_(UINT32) ValuesBuffer::GetFieldIndex(PCWSTR name)
    {
        if (!name) return EFFECTGRAPH_INVALID_INDEX;
        for (size_t i = 0; i < m_layout.size(); ++i)
            if (m_layout[i].name == name) return static_cast<UINT32>(i);
        return EFFECTGRAPH_INVALID_INDEX;
    }

    STDMETHODIMP ValuesBuffer::GetFieldInfo(UINT32 index, EFFECTGRAPH_FIELD_INFO* info)
    {
        if (!info) return E_POINTER;
        if (index >= m_layout.size()) return E_INVALIDARG;
        FieldInfoOf(m_layout, index, *info);
        return S_OK;
    }

    STDMETHODIMP_(void) ValuesBuffer::GetBuffer(ID3D11Buffer** buffer)
    {
        if (buffer) m_buffer.copy_to(buffer);
    }

    STDMETHODIMP_(void) ValuesBuffer::GetView(ID3D11ShaderResourceView** view)
    {
        if (view) m_view.copy_to(view);
    }

    STDMETHODIMP ValuesBuffer::ReadValues(IEffectGraphValues** values)
    {
        if (!values) return E_POINTER;
        *values = nullptr;
        std::vector<Graph::AnalysisFieldValue> fields;
        if (HRESULT hr = m_context->ReadValuesBuffer(m_buffer.get(), m_layout, fields); FAILED(hr)) return hr;
        winrt::make<Values>(std::move(fields)).as<IEffectGraphValues>().copy_to(values);
        return S_OK;
    }

    STDMETHODIMP ValuesBuffer::SetFloats(UINT32 index, const float* values, UINT32 valueCount)
    {
        if (!values && valueCount) return E_INVALIDARG;
        if (index >= m_layout.size()) return E_INVALIDARG;
        return m_context->WriteValues(m_nodeId, m_epoch, index, values, valueCount);
    }

    STDMETHODIMP Values::GetFloats(UINT32 index, float* values, UINT32 valueCount)
    {
        if (!values && valueCount) return E_INVALIDARG;
        if (index >= m_fields.size()) return E_INVALIDARG;
        const auto& field = m_fields[index];
        std::span<const float> data = Graph::AnalysisFieldIsArray(field.type)
            ? std::span<const float>(field.arrayData)
            : std::span<const float>(field.components.data(), Graph::AnalysisFieldComponentCount(field.type));
        const size_t copied = (std::min)(data.size(), static_cast<size_t>(valueCount));
        std::copy_n(data.begin(), copied, values);
        return copied < data.size() ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : S_OK;
    }

    STDMETHODIMP Values::GetFloat(PCWSTR name, float* value)
    {
        if (!value) return E_POINTER;
        const UINT32 index = GetFieldIndex(name);
        if (index == EFFECTGRAPH_INVALID_INDEX) return E_INVALIDARG;
        const auto& field = m_fields[index];
        *value = Graph::AnalysisFieldIsArray(field.type)
            ? (field.arrayData.empty() ? 0.0f : field.arrayData[0]) : field.components[0];
        return S_OK;
    }
}
