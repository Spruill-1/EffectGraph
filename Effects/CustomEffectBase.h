#pragma once

#include "pch_engine.h"

namespace ShaderLab::Effects
{
    // Registration XML for a ShaderLab custom effect: one input per name,
    // no D2D properties (shader and cbuffer are set by the host directly).
    std::wstring BuildCustomEffectXml(std::wstring_view displayName, std::wstring_view description,
                                      std::wstring_view category, const std::vector<std::wstring>& inputNames);

    // Input names I0 .. I<count-1>.
    std::vector<std::wstring> NumberedInputNames(UINT32 count);

    // What the custom shader effects share: COM identity, creation through
    // Direct2D with the impl handed back, the input count, and the shader
    // bytecode and constant buffer the host sets outside D2D's property
    // system. Derived implements TTransform (ID2D1DrawTransform or
    // ID2D1ComputeTransform): its rect mapping, Initialize and render
    // preparation. It declares
    //   static constexpr const wchar_t* cDisplayName, *cDescription;
    // and befriends this class, which constructs and deletes it.
    template <class Derived, class TTransform>
    class CustomEffectBase : public ID2D1EffectImpl, public TTransform
    {
    public:
        // Registers `clsid` with exactly `inputCount` inputs (1 to 8).
        // Registering a CLSID again succeeds; D2D counts registrations.
        static HRESULT RegisterWithInputCount(ID2D1Factory1* factory, REFCLSID clsid, UINT32 inputCount)
        {
            if (!factory || inputCount == 0 || inputCount > 8)
                return E_INVALIDARG;
            const std::wstring xml = BuildCustomEffectXml(Derived::cDisplayName, Derived::cDescription,
                                                          L"Custom", NumberedInputNames(inputCount));
            return factory->RegisterEffectFromString(clsid, xml.c_str(), nullptr, 0, &CreateFactory);
        }

        // D2D's creation callback.
        static HRESULT __stdcall CreateFactory(IUnknown** effect)
        {
            if (!effect) return E_POINTER;
            auto* impl = new (std::nothrow) Derived();
            *effect = impl ? static_cast<ID2D1EffectImpl*>(impl) : nullptr;
            sLastCreated = impl;
            return impl ? S_OK : E_OUTOFMEMORY;
        }

        // Creates an effect of `clsid` with `inputCount` inputs and returns
        // its impl too (null when the CLSID is not this class). The impl
        // lives as long as outEffect.
        static HRESULT Create(ID2D1DeviceContext* dc, REFCLSID clsid, UINT32 inputCount,
                              winrt::com_ptr<ID2D1Effect>& outEffect, Derived** outImpl)
        {
            *outImpl = nullptr;
            sLastCreated = nullptr;
            sPendingInputCount = inputCount;
            const HRESULT hr = dc->CreateEffect(clsid, outEffect.put());
            sPendingInputCount = 0;
            if (SUCCEEDED(hr))
                *outImpl = sLastCreated;
            sLastCreated = nullptr;
            return hr;
        }

        // ---- IUnknown ----
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            *ppv = nullptr;
            if (riid == __uuidof(ID2D1EffectImpl) || riid == __uuidof(IUnknown))
                *ppv = static_cast<ID2D1EffectImpl*>(this);
            else if (riid == __uuidof(TTransform))
                *ppv = static_cast<TTransform*>(this);
            else if (riid == __uuidof(ID2D1Transform))
                *ppv = static_cast<ID2D1Transform*>(this);
            else if (riid == __uuidof(ID2D1TransformNode))
                *ppv = static_cast<ID2D1TransformNode*>(this);
            else
                return E_NOINTERFACE;
            AddRef();
            return S_OK;
        }
        IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
        IFACEMETHODIMP_(ULONG) Release() override
        {
            const ULONG count = InterlockedDecrement(&m_refCount);
            if (count == 0)
                delete static_cast<Derived*>(this);
            return count;
        }

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP SetGraph(ID2D1TransformGraph* transformGraph) override
        {
            m_transformGraph.copy_from(transformGraph);
            return transformGraph->SetSingleTransformNode(static_cast<TTransform*>(this));
        }

        // ---- ID2D1TransformNode ----
        IFACEMETHODIMP_(UINT32) GetInputCount() const override { return m_inputCount; }

        // ---- Host-facing API (outside the D2D property system) ----

        // Bytecode to load at the next PrepareForRender.
        HRESULT LoadShaderBytecode(const BYTE* data, UINT32 dataSize)
        {
            if (!data || dataSize == 0)
                return E_INVALIDARG;
            m_shaderBytecode.assign(data, data + dataSize);
            m_shaderDirty = true;
            return S_OK;
        }

        // The GUID the shader is loaded under; set before LoadShaderBytecode.
        void SetShaderGuid(const GUID& guid) { m_shaderGuid = guid; }

        // Constant buffer contents, padded with zeros to a multiple of 16
        // bytes as D3D11 requires; uploaded by the derived class.
        void SetConstantBufferData(const BYTE* data, UINT32 dataSize)
        {
            m_constantBuffer.assign((static_cast<size_t>(dataSize) + 15) & ~size_t{ 15 }, BYTE{ 0 });
            if (data && dataSize > 0)
                std::memcpy(m_constantBuffer.data(), data, dataSize);
            m_cbDirty = true;
        }

    protected:
        CustomEffectBase()
        {
            // The count Create was given, so Initialize's SetSingleTransformNode
            // sees the right GetInputCount at once.
            if (sPendingInputCount > 0)
                m_inputCount = sPendingInputCount;
        }
        ~CustomEffectBase() = default;

        // Makes this effect its graph's single transform node.
        HRESULT AttachToGraph(ID2D1TransformGraph* transformGraph)
        {
            m_transformGraph.copy_from(transformGraph);
            return transformGraph->SetSingleTransformNode(static_cast<TTransform*>(this));
        }

        LONG m_refCount{ 1 };
        winrt::com_ptr<ID2D1EffectContext>  m_effectContext;
        winrt::com_ptr<ID2D1TransformGraph> m_transformGraph;
        std::vector<BYTE> m_shaderBytecode;
        GUID              m_shaderGuid{};
        std::vector<BYTE> m_constantBuffer;
        UINT32            m_inputCount{ 1 };
        bool              m_cbDirty{ false };       // constant buffer awaits upload
        bool              m_shaderDirty{ false };   // bytecode awaits loading

    private:
        // D2D hands back no impl, so Create passes the input count to the
        // constructor and takes the new instance back through these.
        static inline thread_local Derived* sLastCreated = nullptr;
        static inline thread_local UINT32   sPendingInputCount = 0;
    };
}
