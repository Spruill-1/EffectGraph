#pragma once

#include "pch_engine.h"
#include "EffectGraphInternal.h"
#include "../../Graph/GraphModel.h"
#include "../../Rendering/DeviceStack.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/FrameExecutor.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Effects/SourceNodeFactory.h"

// The classes behind the EffectGraph interfaces. Not exported: callers only
// ever hold the interfaces. C++/WinRT's implements answers QueryInterface only
// for the interfaces it lists, so each class lists every level of its chain.
namespace ShaderLab::Api
{
    class Context;
    class NodeBase;

    // Lets the context find the object behind a node interface it is handed,
    // and so reject a node from another context.
    MIDL_INTERFACE("d0020199-8f9d-4d16-a15d-499bdc6bb782")
    INodeIdentity : public IUnknown
    {
        virtual NodeBase* STDMETHODCALLTYPE Object() = 0;
    };

    // What an input node reads, and the image the graph sees for it.
    struct InputSource
    {
        bool fromTexture{ false };                 // CreateTextureInput, not CreateImageInput
        winrt::com_ptr<ID3D11Texture2D> texture;   // null for an image input, and after MoveToDevice
        winrt::com_ptr<ID2D1Bitmap1> bitmap;       // the texture, wrapped
        winrt::com_ptr<ID2D1Effect> decode;        // to linear scRGB, when the encoding needs it
        winrt::com_ptr<ID2D1Image> output;         // the node's output
    };

    class Context : public winrt::implements<Context, IEffectGraphContextInternal, IEffectGraphContext, IEffectGraphContext2>
    {
    public:
        ~Context();

        HRESULT Initialize(ID3D11Device* device, const EFFECTGRAPH_CONTEXT_DESC* desc);

        // IEffectGraphContext
        STDMETHOD(CreateEffect)(PCWSTR name, IEffectGraphNode** node) override;
        STDMETHOD(CreateEffectById)(REFCLSID effectId, IEffectGraphNode** node) override;
        STDMETHOD(CreateD2DEffect)(REFCLSID effectId, IEffectGraphNode** node) override;
        STDMETHOD(CreateTextureInput)(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding,
                                      IEffectGraphNode** node) override;
        STDMETHOD(CreateImageInput)(ID2D1Image* image, IEffectGraphNode** node) override;
        STDMETHOD(RemoveNode)(IEffectGraphNode* node) override;
        STDMETHOD(Realize)(IEffectGraphNode* node, ID2D1Image** image) override;
        STDMETHOD(RealizeTo)(IEffectGraphNode* node, ID3D11Texture2D* target) override;
        STDMETHOD_(void, GetD3DDevice)(ID3D11Device** device) override;
        STDMETHOD_(void, GetD2DDevice)(ID2D1Device** device) override;
        STDMETHOD(MoveToDevice)(ID3D11Device* device) override;

        // IEffectGraphContext2
        STDMETHOD(RealizeToIncremental)(IEffectGraphNode* node, ID3D11Texture2D* target, UINT32 flags,
                                        RECT* drawn, UINT32 drawnCapacity, UINT32* drawnCount,
                                        EFFECTGRAPH_MOVE_RECT* moved, UINT32 movedCapacity, UINT32* movedCount) override;
        STDMETHOD(PollReadbacks)() override;

        // IEffectGraphContextInternal
        STDMETHOD_(Graph::GraphModel&, Model)() override { return m_graph; }
        STDMETHOD_(Rendering::GraphEvaluator&, Evaluator)() override { return m_evaluator; }
        STDMETHOD_(Effects::SourceNodeFactory&, Sources)() override { return m_sources; }
        STDMETHOD_(const Rendering::DeviceStack&, Devices)() override { return m_devices; }
        STDMETHOD_(ID2D1DeviceContext5*, DeviceContext)() override { return m_dc.get(); }
        STDMETHOD_(uint32_t, NodeId)(IEffectGraphNode* node) override;
        STDMETHOD(NodeFor)(uint32_t nodeId, IEffectGraphNode** node) override;
        STDMETHOD_(Rendering::FrameResult, RunFrame)(const Rendering::FrameRequest& request) override;
        STDMETHOD_(void, SetDisplayProfile)(const Rendering::DisplayProfile& profile) override { m_displayProfile = profile; }
        STDMETHOD_(void, ReplaceGraph)(Graph::GraphModel&& graph) override;

        // For the node objects.
        uint64_t Epoch() const { return m_epoch; }
        bool IsInput(uint32_t nodeId) const { return m_inputs.contains(nodeId); }
        void ForgetNodeObject(uint32_t nodeId, const NodeBase* object);
        // The id of a live node of this context, or an EFFECTGRAPH_E_* error.
        HRESULT IdOf(IEffectGraphNode* node, uint32_t& nodeId) const;
        // A D2D effect of the class, created once, that answers property
        // metadata (names, types, defaults) for built-in nodes. Null when the
        // class cannot be created.
        ID2D1Effect* MetadataEffect(const GUID& clsid);
        HRESULT SetInputTexture(uint32_t nodeId, ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding);
        HRESULT SetInputImage(uint32_t nodeId, ID2D1Image* image);
        // IEffectGraphInput2::InvalidateRects: the rects clipped to what the
        // input reads, then marked changed.
        HRESULT InvalidateInputRects(uint32_t nodeId, const RECT* rects, UINT32 count);
        // IEffectGraphInput2::MoveRects: each move clipped to what the input
        // reads, then marked changed as a move, and kept for reporting.
        HRESULT MoveInputRects(uint32_t nodeId, const EFFECTGRAPH_MOVE_RECT* moves, UINT32 count);
        // IEffectGraphNode2::GetInvalidRects.
        HRESULT InvalidRectsOf(uint32_t nodeId, RECT* rects, UINT32 capacity, UINT32* count, BOOL* whole);
        // Evaluates what `nodeId` depends on, as Realize does.
        HRESULT EvaluateNode(uint32_t nodeId) { return EvaluateFor(nodeId, nullptr, {}); }
        // The node's analysis fields after evaluating it, read from the GPU
        // for a compute node.
        HRESULT ValuesOf(uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values);
        HRESULT ValuesBufferOf(uint32_t nodeId, IEffectGraphValuesBuffer** buffer);
        // A values buffer's contents, laid out by `layout`.
        HRESULT ReadValuesBuffer(ID3D11Buffer* buffer, const std::vector<Graph::AnalysisFieldDescriptor>& layout,
                                 std::vector<Graph::AnalysisFieldValue>& values);
        // ValuesBuffer::SetFloats: the buffer, the node's CPU values and the
        // nodes bound to it.
        HRESULT WriteValues(uint32_t nodeId, uint64_t epoch, UINT32 field, const float* floats, UINT32 count);
        // EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT.
        bool DeferredCommit() const { return m_deferredCommit; }

    private:
        HRESULT AddNodeAndWrap(Graph::EffectNode node, IEffectGraphNode** out);
        HRESULT WrapTexture(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding, InputSource& source);
        // Points every input node at its image and prepares changed sources:
        // what a frame needs before FrameExecutor::Run.
        void SyncFrameInputs();
        // Why the device is gone (DXGI_ERROR_DEVICE_REMOVED, ...), or S_OK.
        HRESULT DeviceRemovedReason() const;
        // One frame for `nodeId`, then the node's state as an HRESULT. With
        // `acknowledge`, a successful draw clears the node's pending region;
        // with `uncacheRoot`, the node's D2D output is not cached that frame.
        HRESULT EvaluateFor(uint32_t nodeId, ID2D1Image* target,
                            std::function<void(ID2D1DeviceContext5*)> draw, bool acknowledge = false,
                            bool uncacheRoot = false);
        // The roots drawn incrementally (an IncrementalTarget record drawn
        // uncached), plus `also` when it is not 0: every frame leaves them
        // uncached, so their CACHED property does not flip between a
        // RealizeTo and a RealizeToIncremental. Empty with dirtyRegions off.
        std::vector<uint32_t> UncachedRoots(uint32_t also = 0) const;
        // A D2D target bitmap over `target`, checked as RealizeTo needs it.
        HRESULT TargetBitmap(ID3D11Texture2D* target, winrt::com_ptr<ID2D1Bitmap1>& bitmap) const;
        // verifyDirtyRegions: compares `target` after an incremental draw of
        // `nodeId` with `before` (the target before it) outside `drawn`, and
        // with a whole render everywhere; counts mismatching pixels.
        // The moves the draw applied are replayed onto `before` first; their
        // destinations, recorded in `drift` (a pixel mask kept per target)
        // until a draw covers them, may differ from the whole render by
        // cMoveDitherCodes (a dithering shift-invariant effect).
        void VerifyIncrementalDraw(uint32_t nodeId, ID3D11Texture2D* target, ID3D11Texture2D* before,
                                   const std::vector<D2D1_RECT_L>& drawn, const std::vector<Graph::DirtyMove>& moves,
                                   std::vector<uint8_t>& drift);
        // What an input reads: the texture, or the image's bounds when finite.
        std::optional<D2D1_RECT_L> InputBounds(const InputSource& source) const;
        // Copies each move within `target`, in order, through m_moveScratch
        // (a copy within one subresource is undefined in D3D11). False when
        // the scratch could not be made; nothing was copied then.
        bool ApplyMoves(ID3D11Texture2D* target, const std::vector<Graph::DirtyMove>& moves);

        Rendering::DeviceStack m_devices;
        winrt::com_ptr<ID2D1DeviceContext5> m_dc;
        Graph::GraphModel m_graph;
        Rendering::GraphEvaluator m_evaluator;
        Effects::SourceNodeFactory m_sources;
        std::unordered_map<uint32_t, InputSource> m_inputs;
        Rendering::DisplayProfile m_displayProfile;
        // Weak: a node object removes itself when it is released.
        std::unordered_map<uint32_t, NodeBase*> m_nodeObjects;
        std::vector<std::pair<GUID, winrt::com_ptr<ID2D1Effect>>> m_metadataEffects;
        // Bumped when the graph is replaced, so node objects from before see
        // their node as removed even though a new node may reuse the id.
        uint64_t m_epoch{ 1 };
        // Uploaded values of nodes whose fields are worked out on the CPU.
        struct UploadedValues
        {
            winrt::com_ptr<ID3D11Buffer> buffer;
            winrt::com_ptr<ID3D11ShaderResourceView> view;
            UINT elements{ 0 };
        };
        std::unordered_map<uint32_t, UploadedValues> m_uploadedValues;
        HRESULT Upload(uint32_t nodeId, const std::vector<Graph::AnalysisFieldValue>& fields);

        // What each node last realized incrementally into, so the next
        // incremental realize can tell whether the target still holds it.
        // The reference pins the texture, so a new one cannot reuse the
        // address. A target written by any other realize since is dropped.
        struct IncrementalTarget
        {
            winrt::com_ptr<ID3D11Texture2D> texture;
            UINT width{ 0 };
            UINT height{ 0 };
            DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
            // Drawn with the root uncached; a draw that differs redraws whole,
            // since a cached root rounds differently.
            bool uncached{ false };
            // The last generation whose moves this root's realizes reported.
            uint64_t reportedThrough{ 0 };
            // The generation of this root's last realize that drew, failed or
            // not: a move from then or before is a carry-over (PlanMoves).
            uint64_t attemptedThrough{ 0 };
            // Moves it would report were pruned (cMaxMoveBacklog): its next
            // realize draws whole and reports none.
            bool backlogLost{ false };
            // verifyDirtyRegions: pixels moved since a draw last covered them.
            std::vector<uint8_t> drift;
        };
        std::unordered_map<uint32_t, IncrementalTarget> m_incrementalTargets;
        // EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK.
        bool m_refuseFallbackBuilds{ false };
        // EFFECTGRAPH_CONTEXT_FLAGS_DEFERRED_ANALYSIS_COMMIT.
        bool m_deferredCommit{ false };
        // Forgets every node's claim on `target` except `keepFor`'s.
        void ForgetIncrementalTarget(ID3D11Texture2D* target, uint32_t keepFor = 0);

        // Moves the inputs were handed (MoveRects), as clipped, until every
        // root realized incrementally has reported them in `moved`.
        struct IngestedMove
        {
            uint32_t input{ 0 };
            EFFECTGRAPH_MOVE_RECT move{};
            uint64_t generation{ 0 };
            uint64_t sequence{ 0 };
        };
        std::vector<IngestedMove> m_ingestedMoves;
        // Sequences of moves a second MoveRects on the same input, before a
        // realize, collapsed to dirty rects; sorted.
        std::vector<uint64_t> m_collapsedMoves;
        // The generation the last realize left (moves since are "before a
        // realize"), and the last generation anything was ingested in: a
        // MoveRects after it advances the generation, so the rects and moves
        // before count as older than its moves.
        uint64_t m_windowStart{ 0 };
        uint64_t m_lastIngestGeneration{ 0 };
        // Region generations a root may go without a successful realize
        // before the moves it has not reported are dropped (it then draws
        // whole once). Every realize and most MoveRects calls advance one.
        static constexpr uint64_t cMaxMoveBacklog = 1024;
        // Drops what every root has reported, and what no root can; never
        // more than cMaxMoveBacklog generations are held. Runs after every
        // ingest and every incremental realize, failed or not, so the lists
        // stay bounded whatever the host's realize calls.
        void PruneIngestedMoves();
        // The node and every node it reads, transitively.
        std::unordered_set<uint32_t> ReadsOf(uint32_t nodeId) const;
        // What a move is copied through; grown to the largest move.
        winrt::com_ptr<ID3D11Texture2D> m_moveScratch;
    };

    // What every node object shares: which node it stands for, and the
    // behaviour behind IEffectGraphNode and ID2D1Properties.
    class NodeBase
    {
    public:
        NodeBase(Context* context, uint32_t nodeId);
        virtual ~NodeBase();
        NodeBase(const NodeBase&) = delete;
        NodeBase& operator=(const NodeBase&) = delete;

        uint32_t Id() const { return m_nodeId; }
        const Context* Owner() const { return m_context.get(); }
        // The object's IEffectGraphNode, with a reference added.
        virtual void GetInterface(IEffectGraphNode** node) = 0;

        // IEffectGraphNode
        void GetContextImpl(IEffectGraphContext** context);
        HRESULT SetInputImpl(UINT32 index, IEffectGraphNode* input);
        void GetInputImpl(UINT32 index, IEffectGraphNode** input);
        UINT32 GetInputCountImpl();
        HRESULT BindImpl(PCWSTR property, IEffectGraphNode* source, PCWSTR field);
        HRESULT UnbindImpl(PCWSTR property);
        HRESULT GetValuesImpl(IEffectGraphValues** values);
        HRESULT GetValuesBufferImpl(IEffectGraphValuesBuffer** buffer);
        UINT32 GetErrorLengthImpl();
        HRESULT GetErrorImpl(PWSTR buffer, UINT32 bufferCount);
        UINT32 GetNameLengthImpl();
        HRESULT GetNameImpl(PWSTR buffer, UINT32 bufferCount);
        HRESULT SetNameImpl(PCWSTR name);

        // ID2D1Properties
        UINT32 GetPropertyCountImpl() const;
        HRESULT GetPropertyNameImpl(UINT32 index, PWSTR name, UINT32 nameCount) const;
        UINT32 GetPropertyNameLengthImpl(UINT32 index) const;
        D2D1_PROPERTY_TYPE GetTypeImpl(UINT32 index) const;
        UINT32 GetPropertyIndexImpl(PCWSTR name) const;
        HRESULT SetValueByNameImpl(PCWSTR name, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize);
        HRESULT SetValueImpl(UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize);
        HRESULT GetValueByNameImpl(PCWSTR name, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const;
        HRESULT GetValueImpl(UINT32 index, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const;
        UINT32 GetValueSizeImpl(UINT32 index) const;
        HRESULT GetSubPropertiesImpl(UINT32 index, ID2D1Properties** subProperties) const;

        // IEffectGraphNode2
        HRESULT GetInvalidRectsImpl(RECT* rects, UINT32 capacity, UINT32* count, BOOL* whole);
        HRESULT TryGetValuesImpl(IEffectGraphValues** values);
        HRESULT CommitFieldsImpl(const UINT32* fieldIndices, const float* values, UINT32 count);
        HRESULT GetCommittedValuesImpl(IEffectGraphValues** values);
        HRESULT SetCommitPolicyImpl(PCWSTR property, EFFECTGRAPH_COMMIT_POLICY policy);

        // IEffectGraphInput, for InputNode
        void InvalidateImpl();
        HRESULT SetTextureImpl(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding);
        HRESULT SetImageImpl(ID2D1Image* image);
        // IEffectGraphInput2
        HRESULT InvalidateRectsImpl(const RECT* rects, UINT32 count);
        HRESULT MoveRectsImpl(const EFFECTGRAPH_MOVE_RECT* moves, UINT32 count);

    protected:
        struct Property
        {
            std::wstring name;
            D2D1_PROPERTY_TYPE type{ D2D1_PROPERTY_TYPE_UNKNOWN };
        };
        // The node while it is still in the graph, else null.
        Graph::EffectNode* Live() const;
        // The node's properties in index order: a D2D effect's own order for
        // a built-in, else the declared parameters, then any other property.
        std::vector<Property> Properties(const Graph::EffectNode& node) const;
        std::optional<Property> PropertyAt(UINT32 index) const;
        std::optional<Property> PropertyNamed(PCWSTR name) const;
        // The value a property has now: the node's, or the D2D default.
        std::optional<Graph::PropertyValue> CurrentValue(const Graph::EffectNode& node, const Property& property) const;
        HRESULT Write(const Property& property, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize);
        HRESULT Read(const Property& property, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const;

        winrt::com_ptr<Context> m_context;
        uint32_t m_nodeId;
        uint64_t m_epoch;
    };

    // The COM object for a node: the shared behaviour plus the interfaces in
    // `Extra` (IEffectGraphInput for an input node).
    template <typename D, typename... Extra>
    class NodeT : public winrt::implements<D, IEffectGraphNode, ID2D1Properties, INodeIdentity, IEffectGraphNode2, Extra...>,
                  public NodeBase
    {
    public:
        NodeT(Context* context, uint32_t nodeId) : NodeBase(context, nodeId) {}

        void GetInterface(IEffectGraphNode** node) override
        {
            this->QueryInterface(__uuidof(IEffectGraphNode), reinterpret_cast<void**>(node));
        }

        // INodeIdentity
        NodeBase* STDMETHODCALLTYPE Object() override { return this; }

        // ID2D1Properties
        STDMETHOD_(UINT32, GetPropertyCount)() const override { return GetPropertyCountImpl(); }
        STDMETHOD(GetPropertyName)(UINT32 index, PWSTR name, UINT32 nameCount) const override { return GetPropertyNameImpl(index, name, nameCount); }
        STDMETHOD_(UINT32, GetPropertyNameLength)(UINT32 index) const override { return GetPropertyNameLengthImpl(index); }
        STDMETHOD_(D2D1_PROPERTY_TYPE, GetType)(UINT32 index) const override { return GetTypeImpl(index); }
        STDMETHOD_(UINT32, GetPropertyIndex)(PCWSTR name) const override { return GetPropertyIndexImpl(name); }
        STDMETHOD(SetValueByName)(PCWSTR name, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize) override { return SetValueByNameImpl(name, type, data, dataSize); }
        STDMETHOD(SetValue)(UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize) override { return SetValueImpl(index, type, data, dataSize); }
        STDMETHOD(GetValueByName)(PCWSTR name, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const override { return GetValueByNameImpl(name, type, data, dataSize); }
        STDMETHOD(GetValue)(UINT32 index, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const override { return GetValueImpl(index, type, data, dataSize); }
        STDMETHOD_(UINT32, GetValueSize)(UINT32 index) const override { return GetValueSizeImpl(index); }
        STDMETHOD(GetSubProperties)(UINT32 index, ID2D1Properties** subProperties) const override { return GetSubPropertiesImpl(index, subProperties); }

        // IEffectGraphNode
        STDMETHOD_(void, GetContext)(IEffectGraphContext** context) override { GetContextImpl(context); }
        STDMETHOD(SetInput)(UINT32 index, IEffectGraphNode* input) override { return SetInputImpl(index, input); }
        STDMETHOD_(void, GetInput)(UINT32 index, IEffectGraphNode** input) override { GetInputImpl(index, input); }
        STDMETHOD_(UINT32, GetInputCount)() override { return GetInputCountImpl(); }
        STDMETHOD(Bind)(PCWSTR property, IEffectGraphNode* source, PCWSTR field) override { return BindImpl(property, source, field); }
        STDMETHOD(Unbind)(PCWSTR property) override { return UnbindImpl(property); }
        STDMETHOD(GetValues)(IEffectGraphValues** values) override { return GetValuesImpl(values); }
        STDMETHOD(GetValuesBuffer)(IEffectGraphValuesBuffer** buffer) override { return GetValuesBufferImpl(buffer); }
        STDMETHOD_(UINT32, GetErrorLength)() override { return GetErrorLengthImpl(); }
        STDMETHOD(GetError)(PWSTR buffer, UINT32 bufferCount) override { return GetErrorImpl(buffer, bufferCount); }
        STDMETHOD_(UINT32, GetNameLength)() override { return GetNameLengthImpl(); }
        STDMETHOD(GetName)(PWSTR buffer, UINT32 bufferCount) override { return GetNameImpl(buffer, bufferCount); }
        STDMETHOD(SetName)(PCWSTR name) override { return SetNameImpl(name); }

        // IEffectGraphNode2
        STDMETHOD(GetInvalidRects)(RECT* rects, UINT32 capacity, UINT32* count, BOOL* whole) override
        {
            return GetInvalidRectsImpl(rects, capacity, count, whole);
        }
        STDMETHOD(TryGetValues)(IEffectGraphValues** values) override { return TryGetValuesImpl(values); }
        STDMETHOD(CommitFields)(const UINT32* fieldIndices, const float* values, UINT32 count) override
        {
            return CommitFieldsImpl(fieldIndices, values, count);
        }
        STDMETHOD(GetCommittedValues)(IEffectGraphValues** values) override { return GetCommittedValuesImpl(values); }
        STDMETHOD(SetCommitPolicy)(PCWSTR property, EFFECTGRAPH_COMMIT_POLICY policy) override
        {
            return SetCommitPolicyImpl(property, policy);
        }
    };

    class Node : public NodeT<Node>
    {
    public:
        Node(Context* context, uint32_t nodeId) : NodeT(context, nodeId) {}
    };

    class InputNode : public NodeT<InputNode, IEffectGraphInput, IEffectGraphInput2>
    {
    public:
        InputNode(Context* context, uint32_t nodeId) : NodeT(context, nodeId) {}

        // IEffectGraphInput
        STDMETHOD_(void, Invalidate)() override { InvalidateImpl(); }
        STDMETHOD(SetTexture)(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding) override { return SetTextureImpl(texture, encoding); }
        STDMETHOD(SetImage)(ID2D1Image* image) override { return SetImageImpl(image); }

        // IEffectGraphInput2
        STDMETHOD(InvalidateRects)(const RECT* rects, UINT32 count) override { return InvalidateRectsImpl(rects, count); }
        STDMETHOD(MoveRects)(const EFFECTGRAPH_MOVE_RECT* moves, UINT32 count) override { return MoveRectsImpl(moves, count); }
    };

    // A field's place in a layout, for GetFieldInfo.
    void FieldInfoOf(const std::vector<Graph::AnalysisFieldDescriptor>& layout, size_t index, EFFECTGRAPH_FIELD_INFO& info);

    // A node's values buffer: the D3D11 objects, the layout they were written
    // in, and the node to write through to.
    class ValuesBuffer : public winrt::implements<ValuesBuffer, IEffectGraphValuesBuffer>
    {
    public:
        ValuesBuffer(Context* context, uint32_t nodeId, std::vector<Graph::AnalysisFieldDescriptor> layout,
                     winrt::com_ptr<ID3D11Buffer> buffer, winrt::com_ptr<ID3D11ShaderResourceView> view);

        STDMETHOD_(UINT32, GetFieldCount)() override { return static_cast<UINT32>(m_layout.size()); }
        STDMETHOD_(UINT32, GetFieldIndex)(PCWSTR name) override;
        STDMETHOD(GetFieldInfo)(UINT32 index, EFFECTGRAPH_FIELD_INFO* info) override;
        STDMETHOD_(void, GetBuffer)(ID3D11Buffer** buffer) override;
        STDMETHOD_(void, GetView)(ID3D11ShaderResourceView** view) override;
        STDMETHOD(ReadValues)(IEffectGraphValues** values) override;
        STDMETHOD(SetFloats)(UINT32 index, const float* values, UINT32 valueCount) override;

    private:
        winrt::com_ptr<Context> m_context;
        uint32_t m_nodeId;
        uint64_t m_epoch;
        std::vector<Graph::AnalysisFieldDescriptor> m_layout;
        winrt::com_ptr<ID3D11Buffer> m_buffer;
        winrt::com_ptr<ID3D11ShaderResourceView> m_view;
    };

    // A snapshot of a node's analysis fields.
    class Values : public winrt::implements<Values, IEffectGraphValues>
    {
    public:
        explicit Values(std::vector<Graph::AnalysisFieldValue> fields) : m_fields(std::move(fields)) {}

        STDMETHOD_(UINT32, GetFieldCount)() override { return static_cast<UINT32>(m_fields.size()); }
        STDMETHOD_(UINT32, GetFieldIndex)(PCWSTR name) override;
        STDMETHOD(GetFieldName)(UINT32 index, PWSTR buffer, UINT32 bufferCount) override;
        STDMETHOD(GetFieldInfo)(UINT32 index, EFFECTGRAPH_FIELD_INFO* info) override;
        STDMETHOD(GetFloats)(UINT32 index, float* values, UINT32 valueCount) override;
        STDMETHOD(GetFloat)(PCWSTR name, float* value) override;

    private:
        std::vector<Graph::AnalysisFieldValue> m_fields;
    };

    // Copies `text` with its terminator into a caller's buffer, or fails with
    // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER).
    HRESULT CopyString(std::wstring_view text, PWSTR buffer, UINT32 bufferCount);
}
