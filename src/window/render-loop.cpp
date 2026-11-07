DWORD WINAPI RenderThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"Dynamic Island for Windows", WS_POPUP, 0, 0, 520, 140,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        Wh_Log(L"Failed to create Dynamic Island overlay window.");
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    g_hwnd = hwnd;
    if (g_shellHookMessage == 0) g_shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");
    if (g_taskbarCreatedMessage == 0) g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    using ChangeWindowMessageFilterEx_t = BOOL(WINAPI*)(HWND, UINT, DWORD, PVOID);
    static auto pChangeWindowMessageFilterEx = reinterpret_cast<ChangeWindowMessageFilterEx_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (pChangeWindowMessageFilterEx) {
        if (g_shellHookMessage) pChangeWindowMessageFilterEx(hwnd, g_shellHookMessage, 1 /*MSGFLT_ALLOW*/, nullptr);
        if (g_taskbarCreatedMessage) pChangeWindowMessageFilterEx(hwnd, g_taskbarCreatedMessage, 1 /*MSGFLT_ALLOW*/, nullptr);
    } else {
        using ChangeWindowMessageFilter_t = BOOL(WINAPI*)(UINT, DWORD);
        static auto pChangeWindowMessageFilter = reinterpret_cast<ChangeWindowMessageFilter_t>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilter"));
        if (pChangeWindowMessageFilter) {
            if (g_shellHookMessage) pChangeWindowMessageFilter(g_shellHookMessage, 1 /*MSGFLT_ADD*/);
            if (g_taskbarCreatedMessage) pChangeWindowMessageFilter(g_taskbarCreatedMessage, 1 /*MSGFLT_ADD*/);
        }
    }
    EnableBlurBehind(hwnd);
    ApplyBackdropMaterial(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ApplyHideShowHotkey();

    if (g_settings.autoHideIdleSeconds == 0) {
        g_manuallyHidden = false;
        Wh_SetIntValue(L"ManuallyHidden", 0);
    } else {
        g_manuallyHidden = Wh_GetIntValue(L"ManuallyHidden", 0) != 0;
    }
    if (g_manuallyHidden.load()) {
        ShowWindow(hwnd, SW_HIDE);
    }

    Renderer renderer;
    if (!renderer.Initialize(hwnd)) {
        DestroyWindow(hwnd);
        g_hwnd = nullptr;
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    // Restore the File Tray from disk. Done here because the shell icons need the
    // COM apartment this thread already initialised.
    LoadFileTray();

    // A 1ms timer resolution costs power, so it is requested only while the island
    // is actually animating. It used to be requested once here and released at
    // shutdown, so a parked or idle island held it for the mod's whole lifetime.
    //
    // Since Windows 10 2004 this affects only the calling process's timers rather
    // than the system clock globally, but the power cost is the reason to scope it
    // either way.
    //
    // The flag is only set when timeBeginPeriod actually succeeded, so the
    // begin/end pairs stay balanced -- these calls are reference counted per
    // process, and an unmatched timeEndPeriod would decrement someone else's
    // request.
    bool highResTimer = false;

    // How long the resolution is held after animation stops. Long enough to ride
    // out the gaps between 60Hz content frames and brief pauses between spring
    // animations without flapping, short enough that a settled island gives it back
    // promptly. A plain local rather than a static, so an unload/reload cycle cannot
    // carry a stale timestamp across.
    constexpr double kHighResTimerHoldSec = 0.5;
    double lastAnimatingAt = -1.0;

    auto setHighResTimer = [&](bool want) {
        if (want == highResTimer) {
            return;
        }
        if (want) {
            if (timeBeginPeriod(1) == TIMERR_NOERROR) {
                highResTimer = true;
            }
        } else {
            timeEndPeriod(1);
            highResTimer = false;
        }
    };

    SpringValue widthSpring;
    SpringValue heightSpring;
    SpringValue nudgeSpring;
    SpringValue volumeBarSpring;
    widthSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 120.0f) * g_settings.sizeScale);
    heightSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 36.0f) * g_settings.sizeScale);
    nudgeSpring.Reset(0.0f);

    IslandKind previousPrimary = IslandKind::Idle;
    auto previousFrame = std::chrono::steady_clock::now();
    auto nextFrameTarget = previousFrame;
    double nextBatteryPoll = 0.0;
    double nextProgressPoll = 0.0;
    double nextSystemPoll = 0.0;
    double nextBrightnessPoll = 0.0;
    double nextPrivacyPoll = 0.0;
    bool wasManuallyHidden = false;
    bool wasAutoHiddenParked = false;
    bool parkedForFullscreen = false;

    static double lastInteractionTime = NowSeconds();
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_APP_NEW_EVENT) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
                lastInteractionTime = NowSeconds();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        BindVolumeEndpoint();

        bool justUnhidden = false;

        if (g_manuallyHidden.load()) {
            // Manual hide always takes precedence over — and invalidates —
            // any auto-park bookkeeping, since the window's shown/hidden
            // state is now fully owned by the manual toggle. Without this,
            // toggling manual-hide off after having been auto-parked would
            // leave g_autoHiddenParked stale-true and the window stuck
            // hidden.
            g_autoHiddenParked = false;
            wasAutoHiddenParked = false;

            if (!wasManuallyHidden) {
                // Just hid: drop to zero-CPU parking immediately, no
                // lingering render/animation work this frame.
                ShowWindow(hwnd, SW_HIDE);
                DwmBackdrop::HideAll();
                g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
                wasManuallyHidden = true;
            }
            previousFrame = std::chrono::steady_clock::now();
            // Fully parked: no polling, no timer wakeups — only the stop
            // event or a posted/queued message (hotkey, settings change,
            // clipboard update, etc.) wakes this thread while hidden. Nothing
            // is being paced, so give the system timer resolution back.
            setHighResTimer(false);
            MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
            nextFrameTarget = std::chrono::steady_clock::now();
            continue;
        }

        // Sits ahead of the parked branch below on purpose: that branch can block
        // for up to 1.5s, and the mouse thread needs to hear about a transition
        // before that, not after.
        {
            static bool prevParkedForHook = false;
            const bool parkedNow = g_autoHiddenParked.load(std::memory_order_relaxed);
            if (parkedNow != prevParkedForHook) {
                prevParkedForHook = parkedNow;
                NotifyMouseThreadParkedChanged();
            }
        }

        if (g_autoHiddenParked.load()) {
            if (g_settings.autoHideIdleSeconds == 0 && !parkedForFullscreen) {
                g_autoHiddenParked = false;
            } else {
                previousFrame = std::chrono::steady_clock::now();
                // Auto-parked, so nothing is animating; same reasoning as above.
                setHighResTimer(false);
                if (parkedForFullscreen) {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1500, QS_ALLINPUT);
                    const bool stillFullscreen =
                        g_settings.autoHideFullscreen && IsForegroundFullscreen(hwnd);
                    g_isFullscreen.store(stillFullscreen, std::memory_order_relaxed);
                    if (!stillFullscreen) {
                        g_fullscreenOverrideVisible = false;
                        g_autoHiddenParked = false;
                    } else if (g_fullscreenOverrideVisible.load()) {
                        g_autoHiddenParked = false;
                    }
                } else {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
                }
                nextFrameTarget = std::chrono::steady_clock::now();
                continue;
            }
        }

        if (wasManuallyHidden) {
            // Just un-hid. Show now; springs get snapped straight to their
            // freshly-computed targets further down this same iteration so
            // there's no stale pop-in animation from wherever they were
            // left off before hiding.
            wasManuallyHidden = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        if (wasAutoHiddenParked && !g_autoHiddenParked.load()) {
            // Something woke us (mouse near the island, a transient alert,
            // the hotkey, fullscreen ending, etc.) — reveal and let this
            // frame's normal logic decide whether to actually stay visible
            // or immediately re-collapse and re-park.
            wasAutoHiddenParked = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        const double now = NowSeconds();
        // True while a tab transition is genuinely being painted. The registry /
        // device polls below run on this thread and can take several ms, which
        // shows up as a hitch mid-animation, so they wait for the spring to settle.
        const bool tabBusy = g_tabAnimating.load(std::memory_order_relaxed) &&
                             (now - g_tabLastPaintAt.load(std::memory_order_relaxed)) < 0.25;
        // The island itself is resizing (spring state from the previous frame).
        const bool springBusy =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f;
        const bool pollBusy = tabBusy || springBusy;
        if (now >= nextBatteryPoll) {
            UpdateBatterySnapshot();
            nextBatteryPoll = now + 15.0;
        }
        if (now >= nextProgressPoll) {
            UpdateProgressSnapshot();
            nextProgressPoll = now + 0.25;
        }
        if (!pollBusy && now >= nextPrivacyPoll) {
            UpdatePrivacyIndicators();
            nextPrivacyPoll = now + 2.0;  // poll every 2 s
        }

        bool timerJustCompleted = false;
        SharedState snapshot;
        {
            std::lock_guard lock(g_stateMutex);
            snapshot = g_state;
            if (g_state.timer.active && g_state.timer.running && now >= g_state.timer.endsAt) {
                g_state.timer.active = false;
                g_state.timer.running = false;
                g_state.timer.justFinished = true;
                g_state.timer.finishedExpiresAt = now + 6.0;
                timerJustCompleted = true;
            }
            if (g_state.timer.justFinished && now >= g_state.timer.finishedExpiresAt) {
                g_state.timer.justFinished = false;
            }
            snapshot.timer = g_state.timer;
            if (g_state.clipboard.active && now >= g_state.clipboard.expiresAt) {
                g_state.clipboard.active = false;
                snapshot.clipboard.active = false;
            }
            if (g_state.notification.active && now >= g_state.notification.expiresAt) {
                g_state.notification.active = false;
                snapshot.notification.active = false;
            }
            if (g_state.volume.active && now >= g_state.volume.expiresAt) {
                g_state.volume.active = false;
                snapshot.volume.active = false;
            }
            if (g_state.brightness.active && now >= g_state.brightness.expiresAt) {
                g_state.brightness.active = false;
                snapshot.brightness.active = false;
            }
            if (g_state.capsLock.active && now >= g_state.capsLock.expiresAt) {
                g_state.capsLock.active = false;
                snapshot.capsLock.active = false;
            }
            if (g_state.battery.active && now >= g_state.battery.expiresAt) {
                g_state.battery.active = false;
                snapshot.battery.active = false;
            }
            if (g_state.device.active && now >= g_state.device.expiresAt) {
                g_state.device.active = false;
                snapshot.device.active = false;
            }
        }
            if (timerJustCompleted) {
                TriggerNudge();
        }

        // Quick Lookup: hand keyboard focus back once the panel has timed out or the
        // module was switched off in settings.
        if (g_lookupUi.open &&
            (!g_settings.quickLookup || !snapshot.lookup.active || now >= snapshot.lookup.expiresAt)) {
            CloseLookupPanel(hwnd);
        }

        const std::vector<IslandKind> kinds = ChooseActivities(snapshot, g_settings, now);
        g_lyricsTabAvailable.store(
            g_settings.lyrics && MediaVisible(snapshot, g_settings),
            std::memory_order_relaxed);
        Activity primary = ActivityForKind(kinds[0], g_settings, snapshot);
        std::optional<Activity> secondary;
        if (kinds.size() >= 2) {
            secondary = ActivityForKind(kinds[1], g_settings, snapshot);
        }

        const bool pinned = g_pinnedExpanded.load();

        if (primary.kind != previousPrimary) {
            if (primary.kind != IslandKind::Idle &&
                !(g_settings.stripMode && primary.kind == IslandKind::Media)) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
            }
            lastInteractionTime = now;
        }
        previousPrimary = primary.kind;

        POINT cursor = {};
        GetCursorPos(&cursor);

        // Pill rectangle in screen space, from the same layout Render() uses. The window
        // is a big fixed stage now, so its own rect says nothing about where the pill is.
        const bool flushTopNow = g_settings.notchStyle || g_settings.borderMergedMode;
        const StageLayout hoverStage =
            ComputeStageLayout(g_settings, widthSpring.value, heightSpring.value);
        const float hoverTop = hoverStage.pillTop +
            (flushTopNow ? std::max(0.0f, nudgeSpring.value) : nudgeSpring.value);
        RECT pillRect = {
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop)),
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft + widthSpring.value)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop + heightSpring.value))
        };

        bool hover = false;
        if (widthSpring.value > 1.0f && heightSpring.value > 1.0f) {
            RECT hoverRect = pillRect;
            if (g_settings.stripMode) {
                // Padding follows the chosen sensitivity while the island is still strip-sized,
                // and eases back to the Medium margin as it opens so it can't flicker shut mid-expand.
                const float stripH = StripModeLayout::kHeight * g_settings.sizeScale;
                const float openH = MediaLayout::kExpandedHeight * g_settings.sizeScale;
                const float openT = Clamp((heightSpring.value - stripH) / std::max(1.0f, openH - stripH), 0.0f, 1.0f);
                const float padMul = StripModeLayout::HoverPadScale(g_settings.stripSensitivity) * (1.0f - openT) + openT;
                const int padX = static_cast<int>(std::lround(StripModeLayout::kHoverPadX * g_settings.sizeScale * padMul));
                const int padY = static_cast<int>(std::lround(StripModeLayout::kHoverPadY * g_settings.sizeScale * padMul));
                hoverRect.left -= padX;
                hoverRect.right += padX;
                hoverRect.top -= padY;
                hoverRect.bottom += padY;
            }
            hover = PtInRect(&hoverRect, cursor) != FALSE;
        } else if (g_settings.unhideOnHover) {
            RECT dockRect = GetIslandDockRect();
            hover = PtInRect(&dockRect, cursor) != FALSE;
        }

        // A file drag hovering over the island counts as hover, so it expands onto
        // the File Tray even with "Expand on hover" turned off.
        if (g_trayDragOver.load(std::memory_order_relaxed)) {
            hover = true;
        }

        bool needsRender = false;

        if (!hover && g_clickExpanded.load()) {
            g_clickExpanded = false;
            needsRender = true;
        }
        if (!hover && g_hoveredMediaButton.load() != -1) {
            g_hoveredMediaButton = -1;
            needsRender = true;
        }
        // Fixes windhawk-mods#4738: active playback used to count as a continuous
        // event, so the island stayed visible for as long as anything was playing
        // and kept resetting the auto-hide timer. Only the 5s window after a
        // *title* change is transient now, so the pill behaves like the clipboard
        // and battery alerts -- it surfaces, then hides again while playback
        // continues in the background.
        const bool recentTrackChange = g_settings.mediaAutoExpand &&
                                       !MediaExpandBlocked(snapshot.media) &&
                                       primary.kind == IslandKind::Media &&
                                       snapshot.media.playing &&
                                       !snapshot.media.title.empty() &&
                                       (now - snapshot.media.titleChangedAt < 5.0);

        bool isTransientAlert = (primary.kind == IslandKind::Clipboard ||
                                 primary.kind == IslandKind::Notification ||
                                 primary.kind == IslandKind::Volume ||
                                 primary.kind == IslandKind::Brightness ||
                                 primary.kind == IslandKind::BatteryLow ||
                                 primary.kind == IslandKind::CapsLock ||
                                 primary.kind == IslandKind::Device ||
                                 primary.kind == IslandKind::Bluetooth ||
                                 primary.kind == IslandKind::DoNotDisturb ||
                                 primary.kind == IslandKind::Lookup ||
                                 primary.kind == IslandKind::SongId ||
                                 recentTrackChange);

        const bool unhideGraceActive = (now < g_hotkeyUnhideUntil.load());
        const bool hoverUnhides = g_settings.unhideOnHover && hover;

        bool currentlyHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !pinned) {
                currentlyHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                currentlyHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
            }
        }

        bool isHoverExpanded = g_settings.expandOnHover ? hover : (hover && g_clickExpanded.load());
        const bool gameMetricsPresent = primary.kind == IslandKind::Idle &&
            (g_settings.gameOverlay || g_gameOverlayPinned.load());
        if (gameMetricsPresent) {
            isHoverExpanded = false;
        }

        if (currentlyHidden && !g_settings.unhideOnHover) {
            isHoverExpanded = false;
        } else if (isHoverExpanded || hoverUnhides || pinned || isTransientAlert || unhideGraceActive) {
            lastInteractionTime = now;
        }

        // A paused player comes back while the island is open (see MediaVisible).
        // The activities for this frame are already chosen, so the switch happens
        // on the next one.
        if (g_islandOpen.exchange(isHoverExpanded || pinned) != (isHoverExpanded || pinned)) {
            g_layoutDirty = true;
        }

        bool isHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !isHoverExpanded && !hoverUnhides && !pinned) {
                isHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                if (hoverUnhides) {
                    isHidden = false;
                } else {
                    isHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
                }
            }
        }

        static bool isFullscreen = false;
        static double lastFullscreenCheck = -1.0;  // -1.0 guarantees the very first iteration checks
        if (now - lastFullscreenCheck > 0.5) {
            const bool newFullscreen = g_settings.autoHideFullscreen && IsForegroundFullscreen(hwnd);
            if (isFullscreen && !newFullscreen) {
                // Fullscreen ended — re-arm so the next fullscreen session
                // hides again even if the hotkey was used to reveal the
                // island this time.
                g_fullscreenOverrideVisible = false;
            }
            isFullscreen = newFullscreen;
            g_isFullscreen.store(isFullscreen, std::memory_order_relaxed);
            lastFullscreenCheck = now;
        }

        // Reclaim the top of the z-order if another always-on-top window has
        // been raised over the island. Cheap and rate-limited, and skipped
        // while the island is hidden or suppressed so it cannot un-hide itself.
        static double lastTopmostCheck = 0.0;
        if (!isFullscreen && !g_manuallyHidden.load() && !g_autoHiddenParked.load() &&
            now - lastTopmostCheck > 1.0) {
            EnsureTopmost(hwnd, &pillRect);
            lastTopmostCheck = now;
        }

        const bool micIndicatorActive = snapshot.system.micActive && g_settings.privacyDots && g_settings.privacyDotsMic;
        const bool camIndicatorActive = snapshot.system.cameraActive && g_settings.privacyDots && g_settings.privacyDotsCam;
        const bool privacyActive = micIndicatorActive || camIndicatorActive;

        // Strip mode: rest as a thin strip unless something needs the full island.
        const bool stripActive = g_settings.stripMode && !hover && !pinned && !isTransientAlert &&
                                 !privacyActive && !snapshot.timer.justFinished;

        // Whether any surface that actually draws CPU / RAM / disk / GPU / network
        // figures is on screen: the in-game overlay, or the idle dashboard's
        // Hardware Monitor tab while expanded (hovered or pinned) and scrolled into
        // view.
        //
        // Hoisted out of the poll block below because two decisions need it: whether
        // to sample the expensive GPU and network counters at all, and whether a
        // change in those numbers is worth repainting for.
        const int metricTabIdx = NormalizedTabIndex(g_settings);
        const bool onHardwareMonitorTab = (metricTabIdx == HardwareMonitorTabIndex(g_settings));
        const bool hwMonitorVisible = (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) &&
            !isFullscreen && !gameMetricsPresent && (pinned || isHoverExpanded) && onHardwareMonitorTab;
        const bool gameOverlayVisible = gameMetricsPresent && !isFullscreen && !stripActive;
        const bool systemMetricsVisible = hwMonitorVisible || gameOverlayVisible;

        if (g_settings.brightness && !pollBusy && now >= nextBrightnessPoll) {
            UpdateBrightnessSnapshot();
            nextBrightnessPoll = now + 0.25;
        }

        if (!pollBusy && !stripActive && now >= nextSystemPoll) {
            const bool needGpuStats = gameOverlayVisible || hwMonitorVisible;
            const bool needNetStats = hwMonitorVisible;  // net is only ever drawn in the HW dashboard

            UpdateSystemSnapshot(needGpuStats, needNetStats);
            if (!g_endpointVolume) {
                g_volumeEndpointStale = true;  // no audio device yet, or binding failed
            }
            nextSystemPoll = now + 1.0;
        }

        if (primary.kind == IslandKind::Idle) {
            if (!isFullscreen && (pinned || isHoverExpanded)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
            } else if (primary.width > 0.0f) {
                // Collapsed: size the strip to the text it will actually render
                // (windhawk-mods#5086). ActivityForKind's 96/170px was wrong both
                // ways -- dead air around a short "9:41", and clipping on
                // "10:41:32 PM" at larger Text size.
                // The > 0 guard preserves ActivityForKind's fully-hidden case.
                primary.width =
                    renderer.MeasureIdleStrip(snapshot, g_settings, now).totalWidth *
                    g_settings.sizeScale;
            }
        }
        if (!isFullscreen && primary.kind == IslandKind::Idle &&
            (g_settings.gameOverlay || g_gameOverlayPinned.load())) {
            // Width follows the metrics actually enabled (#25) so switching some
            // off shrinks the strip instead of leaving empty space, and compact
            // mode narrows it enough to sit neatly in the taskbar area. Both the
            // size and the painting come from GameOverlayLayout, so the strip
            // cannot end up sized for a card width DrawGameOverlay no longer uses.
            const int metricCount = (g_settings.gameOverlayShowCpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowRam ? 1 : 0) +
                                    (g_settings.gameOverlayShowGpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowDisk ? 1 : 0);

            const bool compact = g_settings.gameOverlayCompact;
            primary.width = GameOverlayLayout::Width(compact, g_settings.gameOverlayShowFps,
                                                     metricCount) * g_settings.sizeScale;
            primary.height = GameOverlayLayout::For(compact).height * g_settings.sizeScale;
        }
        if (primary.kind == IslandKind::Media) {
            if (!isFullscreen && (isHoverExpanded || pinned || recentTrackChange)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
                // (Immersive lyrics: the island keeps its normal 380px width.)
            } else {
                // Optional clock section on the left of the collapsed pill.
                const float clockSection = g_settings.mediaPillClock
                    ? renderer.MeasureMediaPill(snapshot, g_settings).sectionWidth
                    : 0.0f;
                if (CollapsedLyricsActive(snapshot, g_settings)) {
                    // Collapsed but carrying lyrics: wider pill, same height.
                    primary.width = (kCollapsedLyricsWidth + clockSection) * g_settings.sizeScale;
                } else if (g_settings.mediaPillClock) {
                    primary.width = (MediaPillLayout::kBaseWidth + clockSection) * g_settings.sizeScale;
                }
            }
        }

        if (primary.kind == IslandKind::SongId) {
            secondary.reset();  // no split layout next to the recognition card
        }

        if (primary.kind == IslandKind::Lookup) {
            secondary.reset();  // the panel stands alone (see HandleLookupClick)
            if (primary.width > 1.0f) {
                // The Lookup card is sized to its (wrapped, line-capped) content.
                const Settings lookupSettings = GetSettingsCopy();
                primary.height = renderer.MeasureLookupCard(snapshot, lookupSettings) *
                                 lookupSettings.sizeScale;
            }
        }

        if (stripActive && primary.width > 0.0f) {
            primary.width = StripModeLayout::kWidth * g_settings.sizeScale;
            primary.height = StripModeLayout::kHeight * g_settings.sizeScale;
            secondary.reset();
        }

        const bool fullscreenSuppressed =
            isFullscreen && !g_fullscreenOverrideVisible.load(std::memory_order_relaxed);
        if ((isHidden || fullscreenSuppressed) && !privacyActive && !pinned && !isHoverExpanded && !isTransientAlert) {
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
        }

        const bool mediaWaveformVisible =
            g_settings.media && snapshot.media.playing && !stripActive &&
            ((primary.kind == IslandKind::Media && primary.width > 1.0f && primary.height > 1.0f) ||
             (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f && secondary->height > 1.0f));
        g_audioCaptureNeeded.store(mediaWaveformVisible, std::memory_order_relaxed);

        float targetWidth = primary.width;
        float targetHeight = primary.height;
        if (secondary) {
            targetWidth = primary.width + secondary->width + 12.0f * g_settings.sizeScale;
            targetHeight = std::max(primary.height, secondary->height);
        }

        widthSpring.target = targetWidth;
        heightSpring.target = targetHeight;

        if (justUnhidden || (unhideGraceActive && widthSpring.value < 0.5f && targetWidth > 1.0f)) {
            widthSpring.Reset(targetWidth);
            heightSpring.Reset(targetHeight);
            nudgeSpring.Reset(0.0f);
        }

        const auto currentFrame = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(currentFrame - previousFrame).count();
        previousFrame = currentFrame;
        dt = Clamp(dt, 0.001f, 0.050f);

        float styleStiffnessMult = 1.0f;
        float styleDampingMult = 1.0f;
        if (g_settings.animationStyle == AnimationStyle::Smooth) {
            styleStiffnessMult = 1.0f;
            styleDampingMult = 1.35f; // Critically damped, no bounciness
        } else if (g_settings.animationStyle == AnimationStyle::Bouncy) {
            styleStiffnessMult = 1.1f;
            styleDampingMult = 0.70f; // Underdamped, lively elasticity
        } else if (g_settings.animationStyle == AnimationStyle::Snappy) {
            styleStiffnessMult = 1.5f;
            styleDampingMult = 1.25f; // High stiffness and quick settle
        }

        const float speed = g_settings.animationSpeed;
        float widthStiffness = 280.0f * styleStiffnessMult;
        float widthDamping = 24.0f * styleDampingMult;
        if (targetWidth > widthSpring.value) {
            widthStiffness = 380.0f * styleStiffnessMult;
            widthDamping = 26.0f * styleDampingMult;
        } else if (targetWidth < widthSpring.value) {
            widthStiffness = 200.0f * styleStiffnessMult;
            widthDamping = 28.0f * styleDampingMult;
        }

        float heightStiffness = 280.0f * styleStiffnessMult;
        float heightDamping = 24.0f * styleDampingMult;
        if (targetHeight > heightSpring.value) {
            heightStiffness = 380.0f * styleStiffnessMult;
            heightDamping = 26.0f * styleDampingMult;
        } else if (targetHeight < heightSpring.value) {
            heightStiffness = 200.0f * styleStiffnessMult;
            heightDamping = 28.0f * styleDampingMult;
        }

        widthSpring.Step(dt * speed, widthStiffness, widthDamping);
        if (widthSpring.value < 0.0f) {
            widthSpring.value = 0.0f;
            widthSpring.velocity = 0.0f;
        }

        heightSpring.Step(dt * speed, heightStiffness, heightDamping);
        if (heightSpring.value < 0.0f) {
            heightSpring.value = 0.0f;
            heightSpring.velocity = 0.0f;
        }

        nudgeSpring.Step(dt * speed, 280.0f * styleStiffnessMult, 24.0f * styleDampingMult);

        // Tracks the live level while the flyout is closed, so opening it doesn't sweep from a stale value.
        if (snapshot.volume.active) {
            volumeBarSpring.target = static_cast<float>(snapshot.volume.percent);
        } else {
            volumeBarSpring.Reset(static_cast<float>(snapshot.system.volumePercent));
        }
        // Fixed, overdamped constants: the animation style must not make the bar overshoot.
        volumeBarSpring.Step(dt * speed, 520.0f, 46.0f);
        snapshot.volume.displayPercent = volumeBarSpring.value;

        {
            std::lock_guard lock(g_stateMutex);
            g_state.system.renderFps = ClampInt(static_cast<int>(1.0f / std::max(dt, 0.001f) + 0.5f), 0, 1000);
        }

        const bool draggingOrHover = hover || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && PtInRect(&pillRect, cursor));

        // Ctrl+hover see-through: holding Ctrl while hovering makes the island
        // transparent and click-through in ANY state, so clicks pass through
        // to windows underneath. Releasing Ctrl or moving away restores it.
        const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool ctrlHoverCT = hover && ctrlHeld && !g_lookupUi.open;

        SetClickThrough(hwnd, (primary.kind == IslandKind::Idle && !draggingOrHover && !pinned) ||
                                  stripActive || ctrlHoverCT);

        // Check if animating structurally
        if (std::abs(widthSpring.velocity) > 0.01f || std::abs(widthSpring.target - widthSpring.value) > 0.01f ||
            std::abs(heightSpring.velocity) > 0.01f || std::abs(heightSpring.target - heightSpring.value) > 0.01f ||
            std::abs(nudgeSpring.velocity) > 0.01f || std::abs(nudgeSpring.target - nudgeSpring.value) > 0.01f ||
            std::abs(volumeBarSpring.velocity) > 0.01f || std::abs(volumeBarSpring.target - volumeBarSpring.value) > 0.01f) {
            needsRender = true;
        }

        // Active Monitor Tracking & dynamic DPI scaling
        {
            HMONITOR currentAnchor = nullptr;
            GetAnchorWorkRect(&currentAnchor);
            const float currentDpi = GetMonitorDpiScale(currentAnchor);
            static HMONITOR s_lastAnchorMon = nullptr;
            static float s_lastDpi = 0.0f;
            if (currentAnchor != s_lastAnchorMon || currentDpi != s_lastDpi) {
                s_lastAnchorMon = currentAnchor;
                s_lastDpi = currentDpi;
                std::lock_guard lock(g_settingsMutex);
                if (g_settings.autoDpiScale) {
                    g_settings.sizeScale = g_settings.baseSizeScale * currentDpi;
                } else {
                    g_settings.sizeScale = g_settings.baseSizeScale;
                }
                g_layoutDirty = true;
            }
        }

        // Quick Lookup panel: caret blink, and keeps the published hit-test geometry fresh.
        if (g_lookupUi.open && primary.kind == IslandKind::Lookup) {
            static double s_lastLookupPaint = 0.0;
            if (now - s_lastLookupPaint >= 0.2) {
                s_lastLookupPaint = now;
                needsRender = true;
            }
        }

        // A tab transition keeps painting until its spring settles. It used to
        // request that through g_layoutDirty, which also made Render() run
        // SetWindowPos / backdrop-region upkeep on every frame of the animation.
        if (g_tabAnimating.load(std::memory_order_relaxed)) {
            if (tabBusy) {
                needsRender = true;
            } else {
                // The tab view stopped drawing mid-motion (island collapsed or an
                // alert took over); don't keep the loop spinning for it.
                g_tabAnimating.store(false, std::memory_order_relaxed);
            }
        }

        // Check if layout was explicitly invalidated
        if (g_layoutDirty.load()) {
            needsRender = true;
        }

        // Hover or pinned state changes visual elements slightly
        static bool prevHover = false;
        static bool prevPinned = false;
        if (hover != prevHover || pinned != prevPinned) {
            needsRender = true;
            prevHover = hover;
            prevPinned = pinned;
        }

        // Activities that animate continuously: the waveform bars, the marquee
        // scroll and the battery pulse. These still only want ~60Hz -- they look no
        // different above it, and they should not piggyback on whatever high Target
        // FPS the user picked for structural resize animation.
        //
        // The rate limit lives in the pacer at the bottom of the loop, not here.
        // This used to gate needsRender behind a 1/60s (16.667ms) timer and then
        // fall through to the flat 16ms idle wait, which cannot satisfy it: 16ms is
        // shorter than the gate, so the next pass failed the check and waited a
        // second time. The result was a paint roughly every 32ms -- about 31fps
        // instead of 60, which is what made playing media look choppy.
        // Media only counts as continuous while something on it is actually moving.
        //
        // ChooseActivities selects the Media pill for any *available* SMTC session,
        // playing or not, so keying off the kind alone meant a paused Spotify or a
        // browser tab with a paused video -- which can sit there for hours, since
        // browsers keep the session alive -- held the island at a 60fps render loop
        // and 1ms timer resolution indefinitely. Nothing on it moves in that state:
        // the collapsed pill draws the album art plus a row of flat bars, taking the
        // !playing branch that skips DrawWaveform entirely.
        //
        // Paused media now falls back to the ordinary change detection above (title
        // and art generation, springs, hover), so the timer is released 0.5s after
        // the last paint.
        //
        // The marquees are expanded-only, hence the hover/pinned terms. No need to
        // include recentTrackChange: it already requires snapshot.media.playing, so
        // it cannot be true while paused.
        // Checks the secondary pill too. The island can show two pills side by side,
        // so a playing media pill sitting next to a running timer or a progress ring
        // is the secondary one -- and looking only at primary left its waveform
        // frozen.
        const bool mediaShown = primary.kind == IslandKind::Media ||
                                (secondary && secondary->kind == IslandKind::Media);
        const bool mediaAnimating =
            mediaShown && !stripActive && (snapshot.media.playing || isHoverExpanded || pinned);

        // Transient alert pills. All expire within a few seconds, so treating them as
        // continuous cannot run away.
        //
        // Device, Bluetooth and Do Not Disturb are here only when the countdown bar
        // is switched on, since that bar is the one thing on them that moves.
        const auto isTransientPill = [](IslandKind kind) {
            return kind == IslandKind::BatteryLow || kind == IslandKind::Clipboard ||
                   kind == IslandKind::Notification;
        };
        const auto hasCountdownBar = [](IslandKind kind) {
            return kind == IslandKind::Device || kind == IslandKind::Bluetooth ||
                   kind == IslandKind::DoNotDisturb;
        };
        const bool countdownBarsOn = g_settings.statusCountdownProgress;

        const bool songIdAnimating = primary.kind == IslandKind::SongId;
        const bool continuousAnimation =
            mediaAnimating || songIdAnimating || isTransientPill(primary.kind) ||
            (secondary && isTransientPill(secondary->kind)) ||
            (countdownBarsOn && (hasCountdownBar(primary.kind) ||
                                 (secondary && hasCountdownBar(secondary->kind))));
        if (continuousAnimation) {
            needsRender = true;
        }

        // Privacy dots
        if (snapshot.system.micActive || snapshot.system.cameraActive) {
            needsRender = true;
        }

        // Idle dashboard clock changes once a minute
        static SYSTEMTIME prevTime = {};
        const bool clockOnScreen = !stripActive &&
            (primary.kind == IslandKind::Idle ||
             (primary.kind == IslandKind::Media && g_settings.mediaPillClock));
        if (clockOnScreen && !isHidden) {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            if (local.wMinute != prevTime.wMinute) {
                needsRender = true;
                prevTime = local;
            }
        }

        // Text that ticks once a second: the focus timer countdown and, when Show
        // seconds is on, the clock.
        //
        // Both used to ride on a side effect. The system poll refreshed CPU load
        // every second and the change detection compared it unconditionally, so the
        // whole island repainted about once a second whether anything visible had
        // changed or not. Gating those metric comparisons on visibility removed that,
        // which left DrawTimer recomputing its m:ss from NowSeconds() on a surface
        // nothing marked dirty -- a 25 minute session sat at 25:00 until some
        // unrelated event forced a paint -- and left the seconds clock updating once
        // a minute despite its own setting promising every second.
        //
        // Keyed to the displayed value rather than to elapsed time, so each visible
        // change paints exactly once. Deliberately not folded into
        // continuousAnimation: these need one frame per second, not the 1ms timer
        // resolution that continuous animation asks for.
        int shownSecond = -1;
        const bool timerShown = primary.kind == IslandKind::Timer ||
                               (secondary && secondary->kind == IslandKind::Timer);
        if (timerShown && !stripActive && snapshot.timer.running) {
            shownSecond = static_cast<int>(std::ceil(snapshot.timer.endsAt - now));
        } else if (clockOnScreen && g_settings.showSeconds && !isHidden) {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            shownSecond = st.wSecond;
        }
        static int s_prevShownSecond = -1;
        if (shownSecond != s_prevShownSecond) {
            s_prevShownSecond = shownSecond;
            needsRender = true;
        }

        // Compare data snapshot to detect changes
        static uint64_t prevArtGen = 0;
        static uint64_t prevSrcIconGen = 0;
        static uint64_t prevNotifIconGen = 0;
        static uint64_t prevClipIconGen = 0;
        static int prevCpu = -1;
        static int prevRam = -1;
        static int prevDisk = -1;
        static int prevVol = -1;
        static int prevBright = -1;
        static bool prevMuted = false;
        static int prevBat = -1;
        static bool prevCharging = false;
        static int prevProg = -1;
        static std::wstring prevMediaTitle;
        static bool prevPlaying = false;

        // CPU / RAM / disk are compared only while a surface that draws them is on
        // screen. UpdateSystemSnapshot refreshes them every second and CPU load
        // essentially always differs between samples, so comparing them
        // unconditionally repainted the whole island once a second for numbers that
        // appear nowhere on the collapsed pill.
        const bool systemMetricsChanged =
            systemMetricsVisible && (snapshot.system.cpuPercent != prevCpu ||
                                     snapshot.system.memoryPercent != prevRam ||
                                     snapshot.system.diskFreePercent != prevDisk);

        if (snapshot.media.artGeneration != prevArtGen ||
            snapshot.media.sourceIconGeneration != prevSrcIconGen ||
            snapshot.media.title != prevMediaTitle ||
            // Play/pause has to be in here now that the metric tick no longer
            // repaints every second as a side effect. Pausing while collapsed swaps
            // the live waveform for the static bars, and without this the frozen
            // last playing frame stayed up until some unrelated repaint came along.
            snapshot.media.playing != prevPlaying ||
            snapshot.notification.icon->generation != prevNotifIconGen ||
            snapshot.clipboard.appIcon->generation != prevClipIconGen ||
            systemMetricsChanged ||
            snapshot.system.volumePercent != prevVol ||
            snapshot.system.volumeMuted != prevMuted ||
            snapshot.brightness.percent != prevBright ||
            snapshot.battery.percent != prevBat ||
            snapshot.battery.charging != prevCharging ||
            snapshot.progress.percent != prevProg) {
            needsRender = true;
            prevArtGen = snapshot.media.artGeneration;
            prevSrcIconGen = snapshot.media.sourceIconGeneration;
            prevMediaTitle = snapshot.media.title;
            prevPlaying = snapshot.media.playing;
            prevNotifIconGen = snapshot.notification.icon->generation;
            prevClipIconGen = snapshot.clipboard.appIcon->generation;
            prevCpu = snapshot.system.cpuPercent;
            prevRam = snapshot.system.memoryPercent;
            prevDisk = snapshot.system.diskFreePercent;
            prevVol = snapshot.system.volumePercent;
            prevMuted = snapshot.system.volumeMuted;
            prevBright = snapshot.brightness.percent;
            prevBat = snapshot.battery.percent;
            prevCharging = snapshot.battery.charging;
            prevProg = snapshot.progress.percent;
        }

        // Track whether Ctrl+hover click-through state changed so we re-render
        static bool prevCtrlHoverCT = false;
        if (ctrlHoverCT != prevCtrlHoverCT) {
            needsRender = true;
            prevCtrlHoverCT = ctrlHoverCT;
        }

        if (needsRender) {
            // When Ctrl+hover click-through is active, reduce pill opacity so the
            // island becomes visually see-through to match the pass-through behavior.
            Settings renderSettings = GetSettingsCopy();
            if (ctrlHoverCT) {
                renderSettings.pillOpacity = Clamp(renderSettings.pillOpacity * 0.35f, 0.15f, 0.45f);
            } else if (renderSettings.themePreset == ThemePreset::Graphite && g_pillOpacityOverride.load() < 0) {
                const bool isExpanded = isHoverExpanded || pinned || isTransientAlert || (widthSpring.value > 260.0f);
                renderSettings.pillOpacity = isExpanded ? 0.98f : 0.88f;
            }
            // No usable backdrop (old Windows / effects off): keep the solid background.
            if (renderSettings.backdropMaterial != BackdropMaterial::None &&
                !DwmBackdrop::Available()) {
                renderSettings.backdropMaterial = BackdropMaterial::None;
            }
            renderer.Render(snapshot, renderSettings, primary, secondary,
                            widthSpring.value, heightSpring.value, nudgeSpring.value,
                            hover, pinned, now);
        }

        // --- Zero-CPU parking for auto-hidden states (idle timeout / fullscreen) ---
        // Only parks once the collapse animation has actually settled at 0,
        // so the shrink still animates before we cut over to OS-hidden.
        const bool wantsAutoHiddenPark =
            !g_manuallyHidden.load() && !pinned && !isHoverExpanded && !isTransientAlert &&
            !privacyActive && (isHidden || fullscreenSuppressed) &&
            widthSpring.value < 0.5f && heightSpring.value < 0.5f &&
            std::fabs(widthSpring.velocity) < 0.5f && std::fabs(heightSpring.velocity) < 0.5f;

        if (wantsAutoHiddenPark) {
            parkedForFullscreen = fullscreenSuppressed;
            wasAutoHiddenParked = true;
            g_autoHiddenParked = true;
            ShowWindow(hwnd, SW_HIDE);
            DwmBackdrop::HideAll();
            g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
            setHighResTimer(false);
            continue;
        }

        int targetFps = g_settings.targetFps;
        if (targetFps <= 0) {
            static int s_cachedRate = 60;
            static double s_rateCheckedAt = -1e9;
            if (now - s_rateCheckedAt > 2.0) {
                s_cachedRate = GetMonitorRefreshRate(hwnd);
                s_rateCheckedAt = now;
            }
            targetFps = s_cachedRate;
        }
        targetFps = ClampInt(targetFps, 30, 1000);
        double targetFrameMs = 1000.0 / static_cast<double>(targetFps);

        // Structural animation -- the springs resizing or nudging the island -- is
        // what benefits from a high Target FPS. Continuous content does not, so once
        // the springs have settled the interval is relaxed to 60Hz and the precise
        // pacer below hits it accurately.
        //
        // std::max, not a plain assignment: a user who deliberately set Target FPS
        // to 40 should keep 40 rather than being pushed up to 60.
        const bool springsAnimating =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f ||
            std::fabs(nudgeSpring.value) > 0.5f ||
            std::fabs(nudgeSpring.velocity) > 0.5f ||
            std::fabs(volumeBarSpring.value - volumeBarSpring.target) > 0.5f ||
            std::fabs(volumeBarSpring.velocity) > 0.5f ||
            g_tabAnimating.load(std::memory_order_relaxed);

        if (continuousAnimation && !springsAnimating) {
            constexpr double kContinuousFrameMs = 1000.0 / 60.0;
            targetFrameMs = std::max(targetFrameMs, kContinuousFrameMs);
        }

        // Keyed to actual animation, not to repainting.
        //
        // A single repaint does not need 1ms pacing -- it needs one frame, which the
        // pacer delivers fine at the default resolution. Only things that draw a
        // *sequence* of frames care: continuous content and spring motion.
        //
        // Keying it to needsRender was wrong twice over. Per frame it thrashed,
        // because needsRender alternates when 60Hz content runs under a higher
        // Target FPS. And with the hold added, any one-off repaint still took the
        // resolution for the full hold -- including the metric tick, which fired
        // every second, so a plain idle island requested and released it once a
        // second and held it roughly half the time with nothing moving at all.
        //
        // continuousAnimation and springsAnimating are exactly the two cases that
        // want precise frame spacing, so they drive it directly.
        if (continuousAnimation || springsAnimating) {
            lastAnimatingAt = now;
        }
        setHighResTimer(lastAnimatingAt >= 0.0 && now - lastAnimatingAt < kHighResTimerHoldSec);

        if (!needsRender) {
            // Nothing changed on screen, so poll at ~60Hz rather than the target
            // frame rate. A settled strip only needs to notice the cursor, so ~20Hz.
            WaitForSingleObject(g_stopEvent, (stripActive && !springsAnimating) ? 50 : 16);
            nextFrameTarget = std::chrono::steady_clock::now();
        } else if (springsAnimating && g_settings.targetFps <= 0 && SUCCEEDED(DwmFlush())) {
            // Auto FPS while the island is resizing: block until DWM's next
            // composition so every frame lands exactly on a vsync. A timer drifts
            // against the display's refresh and produces judder even at high fps.
            nextFrameTarget = std::chrono::steady_clock::now();
        } else {
            // When animating, achieve ultra-smooth target refresh rate (e.g. 144Hz, 240Hz, 360Hz+).
            nextFrameTarget += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(targetFrameMs));

            auto nowTime = std::chrono::steady_clock::now();
            if (nowTime < nextFrameTarget) {
                double remainingMs = std::chrono::duration<double, std::milli>(nextFrameTarget - nowTime).count();
                if (remainingMs >= 1.5) {
                    // Sleep for the bulk of the remaining time using OS event wait (zero CPU usage)
                    WaitForSingleObject(g_stopEvent, static_cast<DWORD>(remainingMs - 0.5));
                }
                // Spinning only pays off for fast spring motion on high refresh rates.
                // For 60Hz content a frame landing under 1ms late is invisible, so sleep only.
                if (springsAnimating && targetFrameMs < 10.0) {
                    while (std::chrono::steady_clock::now() < nextFrameTarget &&
                           WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                        std::this_thread::yield();
                    }
                }
            } else {
                // If we fell behind, reset target to avoid speed-up catch-up loop
                nextFrameTarget = nowTime;
            }
        }
    }

    // Balances whichever state the loop exited in; a no-op if already released.
    setHighResTimer(false);

    UnbindVolumeEndpoint();
    DwmBackdrop::DestroyAll();
    renderer.Shutdown();
    DestroyWindow(hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(kWindowClass, wc.hInstance);

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}



