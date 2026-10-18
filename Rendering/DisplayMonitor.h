#pragma once

#include "pch_engine.h"
#include "DisplayInfo.h"
#include "DisplayProfile.h"

namespace ShaderLab::Rendering
{
    // Monitors display capability changes (HDR toggle, SDR-white-level
    // slider, luminance, primaries, window moved between monitors) and
    // notifies subscribers so the rendering pipeline can adapt.
    //
    // Detection is event-driven via WinRT: a DisplayInformation bound to
    // the app window (IDisplayInformationStaticsInterop::GetForWindow)
    // raises AdvancedColorInfoChanged whenever any advanced-color
    // parameter of the window's current display changes — including the
    // Windows Settings "SDR content brightness" slider, HDR toggles, and
    // monitor moves (the object hooks the window's message loop).
    // Requires Windows 11 22H2 (10.0.22621), the app's declared minimum.
    //
    // Threading contract:
    //   - Initialize/Shutdown must be called on the thread that owns the
    //     HWND and runs a DispatcherQueue (the WinUI UI thread). The
    //     change event fires on that same thread, so after Shutdown()
    //     returns no callback can be in flight.
    //   - The DisplayInformation object is agile; QueryCurrentCapabilities
    //     may be called from any thread (MCP routes run it on the render
    //     worker in the GUI host and on the main thread headless).
    //   - The change callback is invoked WITHOUT internal locks held; it
    //     receives the LIVE caps even while a simulated profile is active
    //     (subscribers should re-read ActiveProfile()).
    class DisplayMonitor
    {
    public:
        DisplayMonitor() = default;
        ~DisplayMonitor();

        DisplayMonitor(const DisplayMonitor&) = delete;
        DisplayMonitor& operator=(const DisplayMonitor&) = delete;

        // Bind to the window's display and subscribe to change events.
        // Idempotent: re-initializing releases the previous binding first.
        // On failure (no DispatcherQueue, invalid HWND) the monitor serves
        // struct-default capabilities and never fires the callback.
        void Initialize(HWND appHwnd);

        // Headless hosts (no HWND, no DispatcherQueue): take a one-shot
        // capability snapshot of the primary monitor via GetForMonitor.
        // No change events are delivered — snapshot-only. Falls back to
        // struct defaults when no display is reachable (CI, session 0).
        void InitializeForPrimaryMonitor();

        // Unsubscribe from change events and release the display binding.
        void Shutdown();

        // Re-query display capabilities right now (thread-safe; returns
        // struct defaults when no display binding exists).
        DisplayCapabilities QueryCurrentCapabilities() const;

        // Returns the most recently cached capabilities (no OS query).
        // When a simulated profile is active, returns the simulated caps.
        DisplayCapabilities CachedCapabilities() const
        {
            std::lock_guard lock(m_capsMutex);
            if (m_simulatedProfile.has_value())
                return m_simulatedProfile->caps;
            return m_caps;
        }

        // Non-empty when the display binding or the last capability query
        // failed — the monitor is serving struct defaults. Surfaced over
        // MCP (get_display_info "monitorStatus") so a broken binding is
        // diagnosable instead of silently masquerading as an SDR panel.
        std::wstring LastError() const
        {
            std::lock_guard lock(m_capsMutex);
            return m_lastError;
        }

        // Register / clear the change callback.
        void SetCallback(DisplayChangeCallback callback);

        // Simulated profile support — override live display with a preset or ICC profile.
        void SetSimulatedProfile(const DisplayProfile& profile);
        void ClearSimulatedProfile();

        bool IsSimulated() const
        {
            std::lock_guard lock(m_capsMutex);
            return m_simulatedProfile.has_value();
        }

        // Returns the simulated profile if active, otherwise constructs one from live caps.
        DisplayProfile ActiveProfile() const;

        // Returns a profile from live display caps (ignoring any simulation).
        DisplayProfile LiveProfile() const;

    private:
        // Re-query, diff against the cache, fire the callback if changed.
        void OnDisplayChanged();

        // Field-wise comparison with float tolerances (0.5 nit luminance,
        // 0.01 nit black level, 0.001 chromaticity).
        static bool CapsChanged(const DisplayCapabilities& a,
                                const DisplayCapabilities& b);

        // m_displayInfo is written on the UI thread (Initialize/Shutdown)
        // and read cross-thread; guarded by m_capsMutex. The revoker is
        // only touched on the UI thread.
        winrt::Windows::Graphics::Display::DisplayInformation m_displayInfo{ nullptr };
        winrt::Windows::Graphics::Display::DisplayInformation::AdvancedColorInfoChanged_revoker m_aciRevoker;

        DisplayCapabilities             m_caps{};
        std::optional<DisplayProfile>   m_simulatedProfile;
        // mutable: QueryCurrentCapabilities (const) records query failures.
        mutable std::wstring            m_lastError;
        mutable std::mutex              m_capsMutex;
        DisplayChangeCallback           m_callback;
        std::mutex                      m_callbackMutex;
    };
}
