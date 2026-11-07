Settings g_settings;

// Guards every access to g_settings. LoadSettings() replaces the whole struct
// from Windhawk's settings thread *and* from the tray context menu (which runs
// on the render thread), while the render, weather and media threads read it
// concurrently. Settings holds std::wstring / std::vector members, so an
// unsynchronised assignment can free a buffer a reader is still walking -- not
// a stale-value glitch but a genuine use-after-free.
//
// This is a strict LEAF lock: never acquire another mutex while holding it.
// That keeps it deadlock-free even where a caller already owns g_stateMutex
// (see WeatherThreadProc), because no path can ever take the two in the
// opposite order.
std::mutex g_settingsMutex;

// Returns a private copy of the current settings. Callers then read from the
// copy for the rest of the frame, which also means a settings change landing
// mid-frame can't tear a single render across two configurations.
Settings GetSettingsCopy() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

std::mutex g_stateMutex;
SharedState g_state;
std::atomic<uint64_t> g_artGenerationCounter = 0;

HWND g_hwnd = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_settingsChangedEvent = nullptr;
HANDLE g_renderThread = nullptr;
HANDLE g_mediaThread = nullptr;
HANDLE g_audioThread = nullptr;
HANDLE g_weatherThread = nullptr;
HANDLE g_calendarThread = nullptr;
HANDLE g_usageThread = nullptr;
HANDLE g_calendarRefreshEvent = nullptr;  // wakes CalendarThreadProc
HANDLE g_lyricsThread = nullptr;
HANDLE g_notificationThread = nullptr;
HANDLE g_bluetoothThread = nullptr;
std::atomic<bool> g_running = false;
std::atomic<int> g_idleTab = 0;
// True while a media pill exists, i.e. while the Lyrics tab can be shown.
// Set by the render loop; read by ActiveTabCount().
std::atomic<bool> g_lyricsTabAvailable{false};

// Lyrics sync offset. Written rarely (settings load, right-click nudge, default-output
// change) and only read by draw code, so no locks are needed.
std::atomic<bool> g_defaultOutputIsBluetooth{false};  // set by BindVolumeEndpoint(), event-driven
std::atomic<int> g_lyricsEffectiveOffsetMs{0};        // speakers / wired: setting + right-click nudge
std::atomic<int> g_lyricsEffectiveOffsetBtMs{0};      // Bluetooth output: setting + right-click nudge
// Set when the default render endpoint must be re-read. Declared up here (not next to
// VolumeNotifyClient) so the Bluetooth watcher, which comes earlier in the file, can set it.
std::atomic<bool> g_volumeEndpointStale{true};
std::atomic<bool> g_layoutDirty = true;
std::atomic<bool> g_clickExpanded = false;
std::atomic<int> g_pressedMediaButton = -1;
std::atomic<int> g_hoveredMediaButton = -1;
std::atomic<unsigned> g_skipTriggerPrev{0};  // bumped on click, the render thread plays the prev button's animation
std::atomic<unsigned> g_skipTriggerNext{0};  // same for the next button

// Media source selection (source dock + right-click submenu).
std::mutex g_mediaSourceMutex;                     // leaf lock for the two strings below
std::wstring g_preferredMediaSource;               // AUMID the user picked; empty = Auto
std::wstring g_switchTarget;                       // pending animated switch
std::wstring g_dockOverrideAumid;                  // temporary pick from the dock; guarded by g_mediaSourceMutex
std::wstring g_islandPausedAumid;                  // source paused with the island's own button; guarded by g_mediaSourceMutex
std::atomic<unsigned> g_dockOverrideRev{0};        // bumped on every dock pick / Auto reset
std::atomic<int> g_switchDir{1};
std::atomic<unsigned> g_switchSeq{0};
std::atomic<double> g_userSourceSwitchAt{-100.0};  // NowSeconds() of the last user switch
HANDLE g_mediaRefreshEvent = nullptr;              // wakes MediaThreadProc
std::atomic<int> g_hoveredSourceSlot{-1};
std::atomic<int> g_pressedSourceSlot{-1};
std::atomic<int> g_hoveredFileTrayRow = -1;  // row index under the cursor on the File Tray card

// Appointments tab paging. The wheel switches tabs, so a long list is paged by
// clicking the "+N more" line at its foot.
std::atomic<int> g_apptPageStart{0};       // first row of the list shown right now
std::atomic<int> g_apptNextPageStart{0};   // where a click on the foot line goes (0 = back to the top)
std::atomic<bool> g_apptMoreValid{false};  // the foot line was drawn this frame
std::atomic<bool> g_hoveredApptMore{false};
std::atomic<float> g_apptMoreL{0.0f}, g_apptMoreT{0.0f}, g_apptMoreR{0.0f}, g_apptMoreB{0.0f};
std::atomic<int> g_hoveredFileTrayAction = -1;  // -2 = bin, -3 = paste, -4 = previous page, -5 = next page, >=0 = cut button of that row
std::atomic<int> g_fileTrayScroll{0};           // index of the first visible row (newest-first order)
std::atomic<bool> g_fileTrayPagerOn{true};      // Modules.FileTrayNavigation: pager shown
std::atomic<bool> g_fileTrayWheelOn{false};     // Modules.FileTrayNavigation: wheel scroll + scrollbar
std::atomic<bool> g_trayDragOver{false};      // a file drag from outside is hovering the island
std::atomic<bool> g_trayInternalDrag{false};  // a drag that started from the tray itself is running
std::atomic<int> g_tabEdgeBump{0};            // +1 / -1: wheel or swipe pushed past the last / first tab
std::atomic<double> g_tabLastPaintAt{0.0};    // last time the tab view painted a moving frame
std::atomic<bool> g_tabAnimating{false};      // a tab transition is in motion (render loop paces frames for it)
bool g_oleInitialized = false;
std::atomic<bool> g_scrubbing = false;           // true while press-dragging the media timeline scrubber
std::atomic<float> g_scrubDragFraction = 0.0f;   // live 0..1 drag position while g_scrubbing is true
std::atomic<double> g_lastLiveSeekTime = 0.0;    // throttle gate for live seeks while dragging
std::atomic<bool> g_audioCaptureNeeded = false;  // gates the WASAPI loopback thread
std::atomic<bool> g_spectrumBandsNeeded = false; // only Pulse Orb / Plasma Thread read the frequency bands
std::atomic<bool> g_manuallyHidden = false;      // user-toggled hide, via menu or hotkey
UINT g_registeredHotkeyModifiers = 0;            // modifiers currently registered with the OS
UINT g_registeredHotkeyVk = 0;                   // vk currently registered with the OS
bool g_hotkeyRegistered = false;

// --- Zero-CPU parking for idle/fullscreen auto-hide (mirrors g_manuallyHidden) ---
std::atomic<bool> g_autoHiddenParked = false;          // true while OS-hidden due to idle/fullscreen auto-hide
std::atomic<bool> g_fullscreenOverrideVisible = false; // user forced the island visible via hotkey while fullscreen
std::atomic<bool> g_isFullscreen = false;              // cached fullscreen state, shared with the hotkey handler
std::atomic<double> g_hotkeyUnhideUntil = 0.0;         // grace deadline to keep island visible after hotkey / settings unhide
FILETIME g_prevIdleTime = {};
FILETIME g_prevKernelTime = {};
FILETIME g_prevUserTime = {};
UINT g_shellHookMessage = 0;
UINT g_taskbarCreatedMessage = 0;
bool g_brightnessInitialized = false;
int g_lastBrightness = -1;
BYTE g_brightnessPowerSource = 255;  // ACLineStatus of the previous sample
std::atomic<double> g_lastNudgeTime = 0.0;
std::atomic<double> g_lastLyricsViewTime = 0.0;

std::mutex g_bluetoothBatteryCacheMutex;
std::unordered_map<std::wstring, int> g_bluetoothBatteryCache;  // Bluetooth device Id -> last known battery percent (-1 = never learned)
std::atomic<uint64_t> g_bluetoothConnectGeneration = 0;
std::atomic<bool> g_isDnDActive = false;
void* g_wnfDndSubscription = nullptr;

std::atomic<bool> g_gameOverlayPinned{false};
std::atomic<bool> g_pinnedExpanded{false};
std::atomic<int> g_pillOpacityOverride{-1};

constexpr GUID kSubTypeIeeeFloat = {
    0x00000003,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};



