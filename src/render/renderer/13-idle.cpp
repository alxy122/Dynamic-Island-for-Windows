    void DrawIdleDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                           double now) {
        if (settings.gameOverlay || g_gameOverlayPinned.load()) {
            DrawGameOverlay(state, rect, 1.0f);
            return;
        }
        if (!clockFormat_) return;

        // Clip to the island's real silhouette (pill / notch / w11 rounded
        // rect), not its bounding box — a plain rect clip leaves the corners
        // outside the rounded shape unclipped, which is what was showing up
        // as a faint square "border" around the round island while collapsing.
        // Publish the same content-space geometry the media surface does, so the
        // File Tray's row hit test works while the island is idle too.
        PublishContentGeometry(rect);

        const float dashHeight = rect.bottom - rect.top;
        const float dashRadius = ContentIslandRadius(dashHeight);
        ComPtr<ID2D1Geometry> dashMask = CreateIslandMaskGeometry(rect, dashRadius, g_settings.notchStyle);
        ID2D1Layer* dashLayer = AcquireLayer();
        const bool haveDashMask = dashMask && dashLayer;
        if (haveDashMask) {
            target_->PushLayer(D2D1::LayerParameters(rect, dashMask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), dashLayer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring collapsedTime = FormatIslandTime(local, settings.clockFollowSystem,
                                                            settings.use24HourClock,
                                                            settings.showSeconds);
        const wchar_t* timeBuf = collapsedTime.c_str();

        const float scale = 1.0f;
        const float width = rect.right - rect.left;

        bool hasWeather = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
        std::wstring wIcon = L"🌡️";
        std::wstring wText = Loc(L"Loading...");
        if (hasWeather) {
            wText = state.weather.weatherDesc;
            GetWeatherIconAndText(state.weather.weatherCode, wIcon, wText);
        }

        // Cross-fade the collapsed status-bar view and the expanded tab view
        // across a small width band around the switchover point instead of
        // a hard cut. Previously this was a plain if/else on width, so the
        // clock's glow (and everything else in the expanded view) just got
        // clipped smaller as the pill shrank and then vanished outright the
        // instant width crossed the threshold — a pop, not a fade. Blending
        // both views by opacity (with an eased curve) makes it a dissolve.
        (void)width;
        auto SmoothFade = [](float t) {
            t = Clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        // Height-driven, like the media surface: collapsed content is gone by
        // ~70px, expanded content starts at ~56px and is solid by ~120px.
        const float collapsedAlpha = SmoothFade((70.0f - dashHeight) / 30.0f);
        const float expandedAlpha = SmoothFade((dashHeight - 56.0f) / 64.0f);

        ID2D1Layer* dashFadeLayer = AcquireLayer();

        if (collapsedAlpha > 0.01f) {
            if (dashFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), collapsedAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   dashFadeLayer);
            }

            // Collapsed Mode (Apple Dynamic Island status bar).
            //
            // Slots are placed from the same measurements that sized the pill, so
            // the clock gets exactly the room it needs and the divider sits between
            // the two strings instead of at an arbitrary geometric centre. The old
            // version split the pill 50/50 at centerX and padded both ends by 6px,
            // which is what produced the dead air on short strings
            // (windhawk-mods#5086).
            const IdleStripMetrics idleMetrics = MeasureIdleStrip(state, settings, now);
            IDWriteTextFormat* idleFmt =
                idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

            // The dot is anchored to the right edge by DrawPrivacyDots, so reserve
            // its lane rather than shrinking the text box out from under the clock.
            const float privacyReserve =
                idleMetrics.hasPrivacy ? IdleStripLayout::kPrivacyReserve * scale : 0.0f;
            const float innerLeft = rect.left + IdleStripLayout::kPadX * scale;
            const float innerRight =
                rect.right - IdleStripLayout::kPadX * scale - privacyReserve;

            float blockWidth = idleMetrics.clockWidth;
            if (idleMetrics.hasWeather) {
                blockWidth += (IdleStripLayout::kSlotGap * 2.0f +
                               IdleStripLayout::kDividerWidth) * scale +
                              idleMetrics.weatherWidth;
            }

            // Centre the block in whatever room the pill currently has. Mid-spring
            // the rect is wider or narrower than the natural width, and clamping
            // the slack at zero stops the slots from inverting when it is narrower.
            const float slack = std::max(0.0f, (innerRight - innerLeft) - blockWidth);
            float slotX = innerLeft + slack * 0.5f;

            // Each slot is exactly its measured width, and the measurement used the
            // widest-digit form, so the live string is centred inside a box it is
            // guaranteed to fit rather than drifting as the digits change.
            textBrush_->SetOpacity(0.96f);
            const D2D1_RECT_F timeRect =
                D2D1::RectF(slotX, rect.top, slotX + idleMetrics.clockWidth, rect.bottom);
            target_->DrawTextW(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), idleFmt,
                               timeRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            slotX += idleMetrics.clockWidth;

            if (idleMetrics.hasWeather) {
                slotX += IdleStripLayout::kSlotGap * scale;

                ComPtr<ID2D1SolidColorBrush> divider;
                target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_),
                    &divider);
                if (divider) {
                    const float divW = IdleStripLayout::kDividerWidth * scale;
                    const float divTop = rect.top + IdleStripLayout::kDividerInsetY * scale;
                    const float divBottom = rect.bottom - IdleStripLayout::kDividerInsetY * scale;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(slotX, divTop, slotX + divW, divBottom),
                                          divW * 0.5f, divW * 0.5f), divider.Get());
                }
                slotX += (IdleStripLayout::kDividerWidth + IdleStripLayout::kSlotGap) * scale;

                wchar_t weatherLabel[32] = {};
                if (hasWeather) swprintf_s(weatherLabel, L"%s %.0f\x00B0", wIcon.c_str(), state.weather.temperature);
                else wcscpy_s(weatherLabel, ARRAYSIZE(weatherLabel), L"\U0001F321\uFE0F --\x00B0");

                const D2D1_RECT_F wRect =
                    D2D1::RectF(slotX, rect.top, slotX + idleMetrics.weatherWidth, rect.bottom);
                target_->DrawTextW(weatherLabel, static_cast<UINT32>(wcslen(weatherLabel)), idleFmt,
                                   wRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
            textBrush_->SetOpacity(1.0f);

            if (dashFadeLayer) {
                target_->PopLayer();
            }
        }

        if (expandedAlpha > 0.01f) {
            if (dashFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), expandedAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   dashFadeLayer);
            }

            // Expanded Mode. Order must match ActiveTabCount()/FileTrayTabIndex().
            std::vector<int> activeTabs;
            activeTabs.push_back(3); // Time (shown first when the island expands/on hover)
            activeTabs.push_back(0); // Calendar
            if (ClaudeUsageTabEnabled(settings)) activeTabs.push_back(7);  // Claude usage
            if (settings.weather) activeTabs.push_back(1);
            if (settings.hardwareMonitorModule) activeTabs.push_back(2);
            if (settings.fileTrayModule) activeTabs.push_back(4);

            int maxTabs = static_cast<int>(activeTabs.size());
            int tabIdx = NormalizedTabIndex(settings);
            if (tabIdx >= maxTabs) tabIdx = maxTabs - 1;

            // Lay the expanded view out at its final size, centred horizontally and
            // pinned to the top, so growing the pill only reveals it (same as media)
            // instead of re-centring and rescaling the content every frame.
            const float fullW = std::max(MediaLayout::kExpandedWidth, rect.right - rect.left);
            const float fullH = std::max(MediaLayout::kExpandedHeight, rect.bottom - rect.top);
            const float fullCx = (rect.left + rect.right) * 0.5f;
            const D2D1_RECT_F fullRect = D2D1::RectF(fullCx - fullW * 0.5f, rect.top,
                                                     fullCx + fullW * 0.5f, rect.top + fullH);

            DrawTabTransition(1, tabIdx, activeTabs, fullRect, now, [&](int activeTabId) {
                if (activeTabId == 3) DrawTimeDashboard(state, fullRect, settings, now, scale, local);
                else if (activeTabId == 0) DrawCalendarDashboard(state, fullRect, settings, now, scale, local);
                else if (activeTabId == 7) DrawClaudeUsageDashboard(state, fullRect, settings, scale);
                else if (activeTabId == 1) DrawWeatherDashboard(state, fullRect, settings, now, scale, hasWeather, wIcon, wText);
                else if (activeTabId == 2) DrawHardwareMonitorDashboard(state, fullRect, settings, scale);
                else if (activeTabId == 4) DrawFileTrayDashboard(state, fullRect, settings, scale);
            });

            DrawTabPagination(fullRect, maxTabs);

            if (dashFadeLayer) {
                target_->PopLayer();
            }
        }

        if (expandedAlpha <= 0.01f && !g_trayDragOver.load(std::memory_order_relaxed)) {
            g_idleTab = 0;
            ResetTabTransition();
        }

        if (haveDashMask) target_->PopLayer(); else target_->PopAxisAlignedClip();
    }

