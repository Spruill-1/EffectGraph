#include "pch_engine.h"
#include "DisplayMonitor.h"

#include <windows.graphics.display.interop.h>

namespace WGD = winrt::Windows::Graphics::Display;

namespace ShaderLab::Rendering
{
    namespace
    {
        uint32_t ModeFromKind(WGD::AdvancedColorKind kind)
        {
            switch (kind)
            {
            case WGD::AdvancedColorKind::HighDynamicRange: return 2u;
            case WGD::AdvancedColorKind::WideColorGamut:   return 1u;
            default:                                       return 0u;
            }
        }

        DisplayCapabilities CapsFromAdvancedColorInfo(WGD::AdvancedColorInfo const& aci)
        {
            DisplayCapabilities caps{};

            caps.activeColorMode = ModeFromKind(aci.CurrentAdvancedColorKind());
            caps.hdrEnabled      = (caps.activeColorMode == 2u);
            caps.hdrSupported    = aci.IsAdvancedColorKindAvailable(WGD::AdvancedColorKind::HighDynamicRange);
            caps.wcgSupported    = aci.IsAdvancedColorKindAvailable(WGD::AdvancedColorKind::WideColorGamut);
            // AdvancedColorInfo has no separate user-toggle probe; mirror
            // the active kind (see DisplayInfo.h field docs).
            caps.hdrUserEnabled  = caps.hdrEnabled;
            caps.wcgUserEnabled  = (caps.activeColorMode == 1u);

            // Scanout depth isn't exposed; WCG/HDR modes composite FP16 and
            // scan out 10-bit+, plain SDR is 8-bit.
            caps.bitsPerColor = (caps.activeColorMode != 0u) ? 10u : 8u;

            // Virtual/remote outputs can report zeroed luminance — keep the
            // struct defaults (270-nit class panel) rather than 0 nits.
            if (const float maxNits = aci.MaxLuminanceInNits(); maxNits > 0.0f)
                caps.maxLuminanceNits = maxNits;
            if (const float maxFF = aci.MaxAverageFullFrameLuminanceInNits(); maxFF > 0.0f)
                caps.maxFullFrameLuminanceNits = maxFF;
            caps.minLuminanceNits = (std::max)(aci.MinLuminanceInNits(), 0.0f);

            // Tracks the Windows Settings "SDR content brightness" slider
            // when HDR is active; 80 nits (scRGB 1.0) otherwise.
            if (const float sdrWhite = aci.SdrWhiteLevelInNits(); sdrWhite > 0.0f)
                caps.sdrWhiteLevelNits = sdrWhite;

            // EDID chromaticities. All-zero points mean the output has no
            // colorimetry data (virtual display) — keep the sRGB defaults.
            const auto r = aci.RedPrimary();
            const auto g = aci.GreenPrimary();
            const auto b = aci.BluePrimary();
            const auto w = aci.WhitePoint();
            if (r.X + r.Y + g.X + g.Y + b.X + b.Y > 0.01)
            {
                caps.redPrimaryX   = static_cast<float>(r.X);
                caps.redPrimaryY   = static_cast<float>(r.Y);
                caps.greenPrimaryX = static_cast<float>(g.X);
                caps.greenPrimaryY = static_cast<float>(g.Y);
                caps.bluePrimaryX  = static_cast<float>(b.X);
                caps.bluePrimaryY  = static_cast<float>(b.Y);
                caps.whitePointX   = static_cast<float>(w.X);
                caps.whitePointY   = static_cast<float>(w.Y);
            }

            return caps;
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    DisplayMonitor::~DisplayMonitor()
    {
        Shutdown();
    }

    void DisplayMonitor::Initialize(HWND appHwnd)
    {
        Shutdown();

        try
        {
            // GetForWindow requires a top-level HWND owned by this thread
            // and a running DispatcherQueue; it hooks the window's message
            // loop so the returned DisplayInformation tracks monitor moves
            // and raises AdvancedColorInfoChanged on this thread.
            auto interop = winrt::get_activation_factory<
                WGD::DisplayInformation, IDisplayInformationStaticsInterop>();

            WGD::DisplayInformation info{ nullptr };
            winrt::check_hresult(interop->GetForWindow(
                appHwnd,
                winrt::guid_of<WGD::DisplayInformation>(),
                winrt::put_abi(info)));

            {
                std::lock_guard lock(m_capsMutex);
                m_displayInfo = info;
            }

            m_aciRevoker = info.AdvancedColorInfoChanged(
                winrt::auto_revoke,
                [this](WGD::DisplayInformation const&,
                       winrt::Windows::Foundation::IInspectable const&)
                {
                    OnDisplayChanged();
                });

            {
                std::lock_guard lock(m_capsMutex);
                m_lastError.clear();
            }
        }
        catch (const winrt::hresult_error& e)
        {
            // No display binding — serve struct defaults, never fire.
            std::lock_guard lock(m_capsMutex);
            m_displayInfo = nullptr;
            m_lastError = L"GetForWindow failed: " + std::wstring(e.message());
        }
        catch (...)
        {
            std::lock_guard lock(m_capsMutex);
            m_displayInfo = nullptr;
            m_lastError = L"GetForWindow failed (non-hresult exception)";
        }

        const auto caps = QueryCurrentCapabilities();
        {
            std::lock_guard lock(m_capsMutex);
            m_caps = caps;
        }
    }

    void DisplayMonitor::InitializeForPrimaryMonitor()
    {
        Shutdown();

        try
        {
            // Snapshot-only binding for windowless hosts. Event
            // registration would need a DispatcherQueue, which headless
            // doesn't run — so no AdvancedColorInfoChanged subscription.
            auto interop = winrt::get_activation_factory<
                WGD::DisplayInformation, IDisplayInformationStaticsInterop>();

            const HMONITOR primary =
                MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

            WGD::DisplayInformation info{ nullptr };
            winrt::check_hresult(interop->GetForMonitor(
                primary,
                winrt::guid_of<WGD::DisplayInformation>(),
                winrt::put_abi(info)));

            {
                std::lock_guard lock(m_capsMutex);
                m_displayInfo = info;
                m_lastError.clear();
            }
        }
        catch (const winrt::hresult_error& e)
        {
            // No reachable display (CI, session 0) — struct defaults.
            std::lock_guard lock(m_capsMutex);
            m_displayInfo = nullptr;
            m_lastError = L"GetForMonitor failed: " + std::wstring(e.message());
        }
        catch (...)
        {
            std::lock_guard lock(m_capsMutex);
            m_displayInfo = nullptr;
            m_lastError = L"GetForMonitor failed (non-hresult exception)";
        }

        const auto caps = QueryCurrentCapabilities();
        {
            std::lock_guard lock(m_capsMutex);
            m_caps = caps;
        }
    }

    void DisplayMonitor::Shutdown()
    {
        // Revoke on the owning (UI) thread: the event fires on this
        // thread's DispatcherQueue, so after revoke() returns no handler
        // is in flight and none will start.
        m_aciRevoker.revoke();

        std::lock_guard lock(m_capsMutex);
        m_displayInfo = nullptr;
    }

    // -----------------------------------------------------------------------
    // Capability query
    // -----------------------------------------------------------------------

    DisplayCapabilities DisplayMonitor::QueryCurrentCapabilities() const
    {
        WGD::DisplayInformation info{ nullptr };
        {
            std::lock_guard lock(m_capsMutex);
            info = m_displayInfo;
        }
        if (!info)
            return DisplayCapabilities{};

        try
        {
            // DisplayInformation is agile — safe from any thread.
            return CapsFromAdvancedColorInfo(info.GetAdvancedColorInfo());
        }
        catch (const winrt::hresult_error& e)
        {
            std::lock_guard lock(m_capsMutex);
            m_lastError = L"GetAdvancedColorInfo failed: " + std::wstring(e.message());
            return DisplayCapabilities{};
        }
        catch (...)
        {
            std::lock_guard lock(m_capsMutex);
            m_lastError = L"GetAdvancedColorInfo failed (non-hresult exception)";
            return DisplayCapabilities{};
        }
    }

    // -----------------------------------------------------------------------
    // Change detection & callback dispatch
    // -----------------------------------------------------------------------

    bool DisplayMonitor::CapsChanged(const DisplayCapabilities& a,
                                     const DisplayCapabilities& b)
    {
        const auto nits   = [](float x, float y) { return std::abs(x - y) > 0.5f; };
        const auto black  = [](float x, float y) { return std::abs(x - y) > 0.01f; };
        const auto chroma = [](float x, float y) { return std::abs(x - y) > 0.001f; };

        return a.hdrEnabled      != b.hdrEnabled
            || a.activeColorMode != b.activeColorMode
            || a.bitsPerColor    != b.bitsPerColor
            || a.hdrSupported    != b.hdrSupported
            || a.hdrUserEnabled  != b.hdrUserEnabled
            || a.wcgSupported    != b.wcgSupported
            || a.wcgUserEnabled  != b.wcgUserEnabled
            || nits(a.maxLuminanceNits, b.maxLuminanceNits)
            || nits(a.maxFullFrameLuminanceNits, b.maxFullFrameLuminanceNits)
            || nits(a.sdrWhiteLevelNits, b.sdrWhiteLevelNits)
            || black(a.minLuminanceNits, b.minLuminanceNits)
            || chroma(a.redPrimaryX,   b.redPrimaryX)
            || chroma(a.redPrimaryY,   b.redPrimaryY)
            || chroma(a.greenPrimaryX, b.greenPrimaryX)
            || chroma(a.greenPrimaryY, b.greenPrimaryY)
            || chroma(a.bluePrimaryX,  b.bluePrimaryX)
            || chroma(a.bluePrimaryY,  b.bluePrimaryY)
            || chroma(a.whitePointX,   b.whitePointX)
            || chroma(a.whitePointY,   b.whitePointY);
    }

    void DisplayMonitor::OnDisplayChanged()
    {
        // Query before locking — the WinRT read must not run under
        // m_capsMutex (CachedCapabilities is called on hot paths).
        const auto newCaps = QueryCurrentCapabilities();

        bool changed = false;
        {
            std::lock_guard lock(m_capsMutex);
            changed = CapsChanged(m_caps, newCaps);
            m_caps = newCaps;
        }
        if (!changed)
            return;

        // Copy the callback out so subscriber code never runs under our
        // lock (re-entrant SetCallback would otherwise self-deadlock).
        DisplayChangeCallback cb;
        {
            std::lock_guard lock(m_callbackMutex);
            cb = m_callback;
        }
        if (cb)
            cb(newCaps);
    }

    void DisplayMonitor::SetCallback(DisplayChangeCallback callback)
    {
        std::lock_guard lock(m_callbackMutex);
        m_callback = std::move(callback);
    }

    // -----------------------------------------------------------------------
    // Simulated profile support
    // -----------------------------------------------------------------------

    void DisplayMonitor::SetSimulatedProfile(const DisplayProfile& profile)
    {
        // Presets and ICC profiles set only the profile's primaries.
        DisplayProfile simulated = profile;
        simulated.isSimulated = true;
        SyncCapsPrimaries(simulated);
        {
            std::lock_guard lock(m_capsMutex);
            m_simulatedProfile = simulated;
        }

        DisplayChangeCallback cb;
        {
            std::lock_guard lock(m_callbackMutex);
            cb = m_callback;
        }
        if (cb)
            cb(simulated.caps);
    }

    void DisplayMonitor::ClearSimulatedProfile()
    {
        {
            std::lock_guard lock(m_capsMutex);
            m_simulatedProfile.reset();
        }

        // Re-query live state outside the lock, then publish.
        const auto liveCaps = QueryCurrentCapabilities();
        {
            std::lock_guard lock(m_capsMutex);
            m_caps = liveCaps;
        }

        DisplayChangeCallback cb;
        {
            std::lock_guard lock(m_callbackMutex);
            cb = m_callback;
        }
        if (cb)
            cb(liveCaps);
    }

    DisplayProfile DisplayMonitor::ActiveProfile() const
    {
        std::lock_guard lock(m_capsMutex);
        if (m_simulatedProfile.has_value())
            return *m_simulatedProfile;
        return ProfileFromCaps(m_caps);
    }

    DisplayProfile DisplayMonitor::LiveProfile() const
    {
        std::lock_guard lock(m_capsMutex);
        return ProfileFromCaps(m_caps);
    }
}
