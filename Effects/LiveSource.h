#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"
#include "SourceTextureUtil.h"

namespace ShaderLab::Effects
{
    // A Source whose frames a host produces: a screen or window capture, a
    // camera, a network stream. The host registers a LiveSourceKind; a node
    // carrying that kind's marker gets one ILiveSource, which
    // SourceNodeFactory opens in PrepareSourceNode and ticks in
    // TickLiveSources, all on the factory's thread.
    //
    // CurrentImage() is linear scRGB (1.0 = 80 nits), like every Source.
    // SourceTextureUtil.h has the helpers for a texture-backed source.
    class ILiveSource
    {
    public:
        virtual ~ILiveSource() = default;

        // Opens for `node`'s properties. False with LastError() set when it
        // cannot; the factory then retries with Reopen once a second.
        virtual bool Open(const Graph::EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice) = 0;

        // Opens what the last Open opened again, after a failure or loss.
        virtual bool Reopen(ID2D1DeviceContext5* dc) = 0;

        // False when `node`'s properties no longer describe what is open; the
        // factory then replaces this source with a new one.
        virtual bool Matches(const Graph::EffectNode& node) const { (void)node; return true; }

        virtual bool IsOpen() const = 0;
        virtual CaptureResult NextFrame(ID2D1DeviceContext5* dc) = 0;
        virtual ID2D1Image* CurrentImage() const = 0;

        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual uint64_t FrameCount() const = 0;
        // True when frames arrive as FP16 rather than a decoded 8-bit fallback.
        virtual bool DeliversFP16() const = 0;
        virtual const std::wstring& LastError() const = 0;

        // Names `node` after what is open (a window title, a monitor), once
        // an Open or Reopen succeeds. Default: leave the node alone.
        virtual void Describe(Graph::EffectNode& node) const { (void)node; }
    };

    // A kind of live source, registered by the host at startup.
    struct LiveSourceKind
    {
        // The bool property that marks a Source node as this kind, saved with
        // the graph ("IsDxgiDuplicateOutput").
        std::wstring marker;
        // Names the kind in error text: "Failed to open <displayName>: ...".
        std::wstring displayName;
        // Makes the source for a node. `target` is what the host handed
        // SourceNodeFactory::SetLiveSourceTarget for the node (a picked
        // window, say), or null. Null with `error` set when the node cannot
        // have a source yet; that is reported on the node and not retried.
        std::function<std::unique_ptr<ILiveSource>(
            winrt::Windows::Foundation::IInspectable const& target, std::wstring& error)> create;
    };

    // Registers `kind` process-wide; a second kind with the same marker
    // replaces the first. Call before any graph using it is evaluated.
    void RegisterLiveSourceKind(LiveSourceKind kind);

    // The kind whose marker `node` carries, or null.
    std::shared_ptr<const LiveSourceKind> FindLiveSourceKind(const Graph::EffectNode& node);
}
