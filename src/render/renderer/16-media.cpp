    // Takes settings by reference from Render()'s private copy rather than
    // reaching for g_settings. The dashboards it delegates to read std::wstring
    // members (DrawCalendarDashboard -> settings.dateFormat), which would
    // otherwise race with LoadSettings() replacing the struct mid-frame.
    void DrawMedia(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                   double now) {
        const float height = rect.bottom - rect.top;

        // Island resize tracking: "settled" once the size has stopped changing for 0.15s.
        // Small per-frame drifts accumulate against the last latched size, so the
        // spring's tail can't sneak past the check.
        {
            const float curW = rect.right - rect.left;
            if (std::fabs(curW - islandSizeLastW_) > 0.25f ||
                std::fabs(height - islandSizeLastH_) > 0.25f) {
                islandSizeLastW_ = curW;
                islandSizeLastH_ = height;
                islandSizeMovedAt_ = now;
            }
            islandSettled_ = (now - islandSizeMovedAt_) > 0.15;
        }

        expandedAnim_ = settings.expandedMediaAnim;
        pillCoverAnim_ = settings.pillCoverAnim;

        // The old side panel is replaced by the immersive lyrics view, so the island never widens.
        const bool sideLyricsOn = false;
        // 0 = normal player, 1 = immersive lyrics (always 0 unless "Lyrics in the media tab" is on).
        const float immersiveMix = UpdateImmersiveMix(settings, height, now);
        g_immersiveMixPub.store(immersiveMix, std::memory_order_relaxed);
        const float sideLyricsExtra =
            sideLyricsOn ? std::max(0.0f, (rect.right - rect.left) - MediaLayout::kExpandedWidth)
                         : 0.0f;

        PublishContentGeometry(rect, sideLyricsExtra);

        const float radius = ContentIslandRadius(height);
        ComPtr<ID2D1Geometry> mask = CreateIslandMaskGeometry(rect, radius, settings.notchStyle);
        ID2D1Layer* layer = AcquireLayer();

        float expandedAlpha = std::clamp((height - MediaLayout::kExpandedMinHeight) / 60.0f, 0.0f, 1.0f);
        float collapsedAlpha = std::clamp((80.0f - height) / 30.0f, 0.0f, 1.0f);

        // ── Shared album art ─────────────────────────────────────────────────
        // One art element that morphs between the collapsed geometry (small circle
        // on the left) and the expanded geometry (64px rounded square) as the island
        // grows or shrinks, instead of cross-fading two different shapes.
        // artMorph: 0 = collapsed pill, 1 = fully expanded (reached at 120px, where
        // the expanded layer is already fully opaque and takes over drawing the art).
        constexpr float kCollapsedPillHeight = 44.0f;
        constexpr float kArtMorphEndHeight = 120.0f;
        constexpr float kCollapsedArtPad = 6.0f;
        constexpr float kCollapsedArtSize = kCollapsedPillHeight - kCollapsedArtPad * 2.0f;
        // Raw progress straight from the island's height.
        const float artMorphRaw = Clamp((height - kCollapsedPillHeight) /
                                        (kArtMorphEndHeight - kCollapsedPillHeight), 0.0f, 1.0f);

        // Shrinking follows the height exactly (unchanged). Expanding is not
        // allowed to run faster than kArtExpandSec for the full circle -> square
        // change, because the expand spring is so fast that the morph would
        // otherwise be over in a few frames.
        constexpr float kArtExpandSec = 0.26f;
        // The art only travels to its expanded spot on the Media tab. On any other
        // first tab (e.g. pinned lyrics) it just follows the height, with no throttle.
        const bool artOnMediaTab = NormalizedTabIndex(settings) == MediaTabIndex(settings);
        const bool freshMorph = artMorphLast_ >= 0.0 && (now - artMorphLast_) < 0.25;
        const float dtMorph = freshMorph ? Clamp(static_cast<float>(now - artMorphLast_), 0.001f, 0.05f) : 0.0f;
        artMorphLast_ = now;

        if (!freshMorph || artMorphRaw <= artMorphLin_ || !artOnMediaTab) {
            artMorphLin_ = artMorphRaw;
        } else {
            artMorphLin_ = std::min(artMorphRaw, artMorphLin_ + dtMorph / kArtExpandSec);
            g_layoutDirty = true;  // keep frames coming until the morph catches up
        }
        const float artMorph = SmoothStep01(artMorphLin_);

        // Source badge: hidden during the morph, fades in once the art hands over
        // to the expanded layer (this used to pop in instantly).
        if (artMorph >= 1.0f) {
            if (!freshMorph) {
                artBadgeAlpha_ = 1.0f;
            } else if (artBadgeAlpha_ < 1.0f) {
                artBadgeAlpha_ = std::min(1.0f, artBadgeAlpha_ + dtMorph / 0.20f);
                g_layoutDirty = true;
            }
        } else {
            artBadgeAlpha_ = 0.0f;
        }
        float pillClockSection = 0.0f;
        if (settings.mediaPillClock && idleTextFormat_) {
            pillClockSection = MeasureMediaPill(state, settings).sectionWidth;
        }
        const D2D1_RECT_F collapsedArtRect = D2D1::RectF(
            rect.left + pillClockSection + kCollapsedArtPad, rect.top + kCollapsedArtPad,
            rect.left + pillClockSection + kCollapsedArtPad + kCollapsedArtSize,
            rect.top + kCollapsedArtPad + kCollapsedArtSize);
        auto lerpF = [](float a, float b, float t) { return a + (b - a) * t; };
        const D2D1_RECT_F normalArtRect = D2D1::RectF(
            rect.left + MediaLayout::kArtInsetX, rect.top + MediaLayout::kArtInsetY,
            rect.left + MediaLayout::kArtInsetX + MediaLayout::kArtSize,
            rect.top + MediaLayout::kArtInsetY + MediaLayout::kArtSize);
        const D2D1_RECT_F immersiveArtRect = D2D1::RectF(
            rect.left + ImmersiveLayout::kArtLeft, rect.top + ImmersiveLayout::kArtTop,
            rect.left + ImmersiveLayout::kArtLeft + ImmersiveLayout::kArtSize,
            rect.top + ImmersiveLayout::kArtTop + ImmersiveLayout::kArtSize);
        // Equals the original rect / radius exactly while immersiveMix == 0.
        const D2D1_RECT_F expandedArtRect = D2D1::RectF(
            lerpF(normalArtRect.left, immersiveArtRect.left, immersiveMix),
            lerpF(normalArtRect.top, immersiveArtRect.top, immersiveMix),
            lerpF(normalArtRect.right, immersiveArtRect.right, immersiveMix),
            lerpF(normalArtRect.bottom, immersiveArtRect.bottom, immersiveMix));
        const float expandedArtRadius =
            lerpF(16.0f, ImmersiveLayout::kArtSize * 0.5f, immersiveMix);
        const D2D1_RECT_F morphArtRect = D2D1::RectF(
            lerpF(collapsedArtRect.left, expandedArtRect.left, artMorph),
            lerpF(collapsedArtRect.top, expandedArtRect.top, artMorph),
            lerpF(collapsedArtRect.right, expandedArtRect.right, artMorph),
            lerpF(collapsedArtRect.bottom, expandedArtRect.bottom, artMorph));
        const float morphArtRadius = lerpF(kCollapsedArtSize * 0.5f, expandedArtRadius, artMorph);

        // Expanded UI
        if (expandedAlpha > 0.01f && mask && layer) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), expandedAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE), layer);

            // Order must match ActiveTabCount()/FileTrayTabIndex().
            std::vector<int> activeTabs;
            // Lyrics goes first when pinned; otherwise it stays last. Must match MediaTabIndex().
            const bool lyricsFirst = settings.lyricsPinnedFirst && settings.lyrics;
            if (lyricsFirst) activeTabs.push_back(5);  // Lyrics (pinned first)
            activeTabs.push_back(0); // Media
            activeTabs.push_back(1); // Calendar
            if (settings.weather) activeTabs.push_back(2);
            if (settings.hardwareMonitorModule) activeTabs.push_back(3);
            if (settings.fileTrayModule) activeTabs.push_back(4);
            if (settings.lyrics && !lyricsFirst) activeTabs.push_back(5);  // Lyrics (last)

            // Same settings object as the list above, so the index can't be
            // normalised against a tab set that no longer matches.
            int maxTabs = static_cast<int>(activeTabs.size());
            int tabIdx = NormalizedTabIndex(settings);
            if (tabIdx >= maxTabs) tabIdx = maxTabs - 1;

            auto drawTabContent = [&](int activeTabId) {
            if (activeTabId == 0) {
                // With side lyrics on, the player lives in the left part of the island and
                // the rest of this branch (which uses `rect`) lays out exactly as before.
                const D2D1_RECT_F fullIslandRect = rect;
                const D2D1_RECT_F rect = D2D1::RectF(fullIslandRect.left, fullIslandRect.top,
                                                     fullIslandRect.right - sideLyricsExtra,
                                                     fullIslandRect.bottom);
                // Source switch envelope for everything that changes with the source.
                float swapAlpha = 1.0f, swapDx = 0.0f;
                UpdateSourceSwap(state, now, &swapAlpha, &swapDx);

                // Art, text and spectrum slide + fade as one group (closed in 13c).
                const bool contentGrouped = BeginSwapGroup(swapAlpha, swapDx);
                if (swapAlpha > 0.003f) {
                // Expanded Apple DI media: large square art on left, text center.
                const float artSize = MediaLayout::kArtSize;
                D2D1_RECT_F artRect = D2D1::RectF(rect.left + MediaLayout::kArtInsetX,
                                                  rect.top + MediaLayout::kArtInsetY,
                                                  rect.left + MediaLayout::kArtInsetX + artSize,
                                                  rect.top + MediaLayout::kArtInsetY + artSize);
                // Until the morph has finished, the shared art pass at the bottom of
                // DrawMedia owns the cover, so it is never drawn twice.
                if (artMorph >= 1.0f) {
                    DrawAlbumArtFlip(state.media, expandedArtRect, now, expandedArtRadius,
                                     immersiveMix < 0.01f);
                }

                // Immersive: title / artist / spectrum / source line cross-fade out.
                const bool normalFaded = BeginImmersiveFade(1.0f - immersiveMix);
                if (immersiveMix < 0.999f) {

                const float waveW = 32.0f;
                const float waveH = 20.0f;
                D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 24.0f - waveW,
                                                   rect.top + 20.0f + (artSize - waveH) * 0.5f,
                                                   rect.right - 24.0f,
                                                   rect.top + 20.0f + (artSize + waveH) * 0.5f);

                const float textLeft = artRect.right + 18.0f;
                const float textRight = waveRect.left - 16.0f;

                // Title — bold, prominent.
                D2D1_RECT_F titleRect = D2D1::RectF(textLeft, rect.top + 34.0f, textRight, rect.top + 54.0f);
                const std::wstring titleText =
                    state.media.title.empty() ? std::wstring(Loc(L"Unknown")) : state.media.title;
                if (!DrawTitleTransition(titleText, titleRect, textBrush_.Get(), now, !state.media.title.empty())) {
                    DrawMarqueeText(titleText, titleRect, textFormat_.Get(), textBrush_.Get(), now, 42.0f,
                                    marqueeTitleCache_);
                }

                // Artist — muted below title.
                D2D1_RECT_F artistRect = D2D1::RectF(textLeft, rect.top + 54.0f, textRight, rect.top + 74.0f);
                mutedBrush_->SetOpacity(0.80f);
                DrawSwapMarquee(swapArtist_, state.media.artist, artistRect, smallTextFormat_.Get(),
                                mutedBrush_.Get(), now, 30.0f, marqueeArtistCache_, 0.06f);
                mutedBrush_->SetOpacity(0.75f);

                if (!state.media.albumTitle.empty()) {
                    D2D1_RECT_F albumRect = D2D1::RectF(textLeft, rect.top + 68.0f, textRight, rect.top + 84.0f);
                    mutedBrush_->SetOpacity(0.70f);
                    DrawSwapMarquee(swapAlbum_, state.media.albumTitle, albumRect, smallTextFormat_.Get(),
                                    mutedBrush_.Get(), now, 28.0f, marqueeAlbumCache_, 0.12f);
                    mutedBrush_->SetOpacity(0.75f);
                }

                if (state.media.playing) {
                    DrawSpectrum(state, waveRect, settings, now);
                } else {
                    const float gap = 2.5f;
                    const float availableW = waveRect.right - waveRect.left;
                    const int count = 7;
                    const float barWidth = (availableW - gap * (count - 1)) / count;
                    const float centerY = (waveRect.top + waveRect.bottom) * 0.5f;
                    mutedBrush_->SetOpacity(0.5f);
                    for (int i = 0; i < count; ++i) {
                        const float dotX = waveRect.left + i * (barWidth + gap) + barWidth * 0.5f;
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, centerY), 1.2f, 1.2f), mutedBrush_.Get());
                    }
                }

                // Source line, under the artist (or under the album when there is one).
                const float srcLineTop = rect.top + (state.media.albumTitle.empty() ? 70.0f : 85.0f);
                DrawSourceLine(state.media, D2D1::RectF(textLeft, srcLineTop, textRight, srcLineTop + 14.0f), now);
                }  // end of `if (immersiveMix < 0.999f)`
                EndImmersiveFade(normalFaded);
                DrawImmersiveLyricsView(state, rect, settings, now, immersiveMix);
                }  // end of `if (swapAlpha > 0.003f)` opened in 13b
                EndSwapGroup(contentGrouped);

                // Immersive lyrics: draw the transport block a little smaller so the
                // lyrics get the room. Restored after the lyrics button below.
                D2D1_MATRIX_3X2_F playerZoneOldTransform;
                target_->GetTransform(&playerZoneOldTransform);
                if (immersiveMix > 0.001f) {
                    const float ps = 1.0f + (ImmersiveLayout::kPlayerScale - 1.0f) * immersiveMix;
                    target_->SetTransform(
                        D2D1::Matrix3x2F::Scale(
                            ps, ps,
                            D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                          rect.top + ImmersiveLayout::kPlayerPivotY)) *
                        playerZoneOldTransform);
                }

                // Scrubber + time labels fade (no slide).
                const bool timelineGrouped = BeginSwapGroup(swapAlpha, 0.0f);

                // Timeline (Scrubber)
                const float scrubberY = rect.top + MediaLayout::kScrubberY;
                double currentPosition = state.media.positionTicks / 10000000.0;
                double duration = state.media.endTicks / 10000000.0;
                if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                    currentPosition += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
                }
                currentPosition = std::max(0.0, std::min(currentPosition, duration));

                const bool isDraggingThisBar = g_scrubbing.load(std::memory_order_relaxed);
                float progress;
                if (isDraggingThisBar) {
                    progress = Clamp(g_scrubDragFraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    currentPosition = duration * progress;
                } else {
                    progress = duration > 0.0 ? static_cast<float>(currentPosition / duration) : 0.0f;
                }

                auto FormatTime = [](double seconds) -> std::wstring {
                    if (seconds <= 0.0 || _isnan(seconds)) return L"0:00";
                    int m = static_cast<int>(seconds) / 60;
                    int s = static_cast<int>(seconds) % 60;
                    wchar_t buf[16];
                    swprintf_s(buf, L"%d:%02d", m, s);
                    return buf;
                };

                std::wstring elapsedStr = FormatTime(currentPosition);
                std::wstring remainStr = L"-" + FormatTime(duration - currentPosition);

                const float scrubLeft = rect.left + MediaLayout::kScrubMargin;
                const float scrubRight = rect.right - MediaLayout::kScrubMargin;

                const float barLeft = scrubLeft + MediaLayout::kScrubBarLeftInset;
                const float barRight = scrubRight - MediaLayout::kScrubBarRightInset;
                const float timeGap = 8.0f;

                D2D1_RECT_F elRect = D2D1::RectF(scrubLeft, scrubberY - 10.0f, barLeft - timeGap, scrubberY + 10.0f);
                D2D1_RECT_F remRect = D2D1::RectF(barRight + timeGap, scrubberY - 10.0f, scrubRight, scrubberY + 10.0f);

                mutedBrush_->SetOpacity(0.8f);
                if (smallTextFormat_) {
                    smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    target_->DrawTextW(elapsedStr.c_str(), static_cast<UINT32>(elapsedStr.size()), smallTextFormat_.Get(), elRect, mutedBrush_.Get());

                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    target_->DrawTextW(remainStr.c_str(), static_cast<UINT32>(remainStr.size()), smallTextFormat_.Get(), remRect, mutedBrush_.Get());

                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }

                // The scrubber uses the shared accent track, so it matches the
                // volume, battery and timer bars exactly. The bar thickens while
                // dragging, which is the standard cue that it is grabbable.
                if (settings.progressStyle != ProgressStyle::Slim) {
                    DrawStyledProgress(settings.progressStyle, barLeft, barRight, scrubberY, progress,
                                       isDraggingThisBar, state.media.playing, now);
                } else {
                const float barHalf = isDraggingThisBar ? 3.5f : 2.5f;
                DrawAccentTrack(D2D1::RectF(barLeft, scrubberY - barHalf, barRight, scrubberY + barHalf),
                                progress, barHalf);

                const D2D1_COLOR_F scrubColor =
                    (currentAccent_.a > 0.0f) ? currentAccent_ : D2D1::ColorF(0x4cc9f0);
                const float scrubW = (barRight - barLeft) * progress;
                const float thumbX = barLeft + scrubW;
                const float thumbR = isDraggingThisBar ? 6.5f : 4.5f;

                // Halo first so the thumb sits on top of it.
                if (isDraggingThisBar) {
                    ComPtr<ID2D1SolidColorBrush> thumbHalo;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(scrubColor, 0.22f), &thumbHalo)) &&
                        thumbHalo) {
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR * 2.2f, thumbR * 2.2f),
                            thumbHalo.Get());
                    }
                }

                // A white thumb with an accent ring reads more precisely against
                // album art than a solid accent dot.
                ComPtr<ID2D1SolidColorBrush> thumbFill;
                if (SUCCEEDED(target_->CreateSolidColorBrush(
                        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), &thumbFill)) && thumbFill) {
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR, thumbR),
                                         thumbFill.Get());
                }
                ComPtr<ID2D1SolidColorBrush> thumbRing;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(scrubColor, 0.85f), &thumbRing)) &&
                    thumbRing) {
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR, thumbR),
                                         thumbRing.Get(), 1.6f);
                }
                }  // end Slim style

                EndSwapGroup(timelineGrouped);

                // Controls. Positions come from MediaLayout so the hit test in
                // OverlayWndProc stays in lockstep with what is drawn here.
                const float cy = rect.top + MediaLayout::kControlsY;
                const float cx = (rect.left + rect.right) * 0.5f;
                DrawMediaControls(state.media.playing,
                                  D2D1::Point2F(cx - MediaLayout::kControlSpacing, cy),
                                  D2D1::Point2F(cx, cy),
                                  D2D1::Point2F(cx + MediaLayout::kControlSpacing, cy),
                                  now);
                DrawSourceDock(state, rect, now);  // stays still during a switch
                if (settings.mediaTabLyrics) {
                    DrawImmersiveLyricsButton(rect, immersiveMix);
                }
                target_->SetTransform(playerZoneOldTransform);
                if (sideLyricsExtra > 1.0f) {
                    DrawMediaSideLyrics(state,
                                        D2D1::RectF(rect.right, fullIslandRect.top,
                                                    fullIslandRect.right, fullIslandRect.bottom),
                                        settings, now);
                }
            } else if (activeTabId == 1) {
                SYSTEMTIME local = {}; GetLocalTime(&local);
                DrawCalendarDashboard(state, rect, settings, now, 1.0f, local);
            } else if (activeTabId == 2) {
                bool hasWeather = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
                std::wstring wIcon = L"🌡️"; std::wstring wText = Loc(L"Loading...");
                if (hasWeather) {
                    wText = state.weather.weatherDesc;
                    GetWeatherIconAndText(state.weather.weatherCode, wIcon, wText);
                }
                DrawWeatherDashboard(state, rect, settings, now, 1.0f, hasWeather, wIcon, wText);
            } else if (activeTabId == 3) {
                DrawHardwareMonitorDashboard(state, rect, settings, 1.0f);
            } else if (activeTabId == 4) {
                DrawFileTrayDashboard(state, rect, settings, 1.0f);
            } else if (activeTabId == 5) {
                // Laid out at the final expanded size and pinned to the top, so growing
                // the island only reveals the lyrics instead of sliding them around.
                // Resting expanded size (380x184 times the hover scale), so the layout never
                // follows the spring's overshoot. At rest this equals the old max() result.
                const float lyricsW = MediaLayout::kExpandedWidth * hoverScale_;
                const float lyricsH = MediaLayout::kExpandedHeight * hoverScale_;
                const float lyricsCx = (rect.left + rect.right) * 0.5f;
                DrawLyricsDashboard(state,
                                    D2D1::RectF(lyricsCx - lyricsW * 0.5f, rect.top,
                                                lyricsCx + lyricsW * 0.5f, rect.top + lyricsH),
                                    settings, 1.0f);
            }
            };  // end drawTabContent

            DrawTabTransition(0, tabIdx, activeTabs, rect, now, drawTabContent);
            DrawTabPagination(rect, maxTabs);

            target_->PopLayer();
        }

        // Collapsed UI
        if (collapsedAlpha > 0.01f && mask && layer && !g_trayDragOver.load(std::memory_order_relaxed)) {
            g_idleTab = 0;
            ResetTabTransition();
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), collapsedAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE), layer);;

            const float cy = (rect.top + rect.bottom) * 0.5f;

            // Optional clock section on the left. Zero wide when the option is
            // off, so the cover sits exactly where it always did.
            MediaPillMetrics pillMetrics;
            if (settings.mediaPillClock && idleTextFormat_) {
                pillMetrics = MeasureMediaPill(state, settings);
            }
            const float clockSection = pillMetrics.sectionWidth;

            // The cover itself is drawn by the shared art pass at the end of
            // DrawMedia; this rect is only used to place the lyrics next to it.
            const D2D1_RECT_F artRect = collapsedArtRect;

            float shiftX = 0.0f;
            if (state.system.micActive || state.system.cameraActive) {
                shiftX = 22.0f;
            }

            D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 42.0f - shiftX, cy - 10.0f,
                                               rect.right - 14.0f - shiftX, cy + 10.0f);
            if (state.media.playing) {
                DrawSpectrum(state, waveRect, settings, now);
            } else {
                const float gap = 2.5f;
                const float availableW = waveRect.right - waveRect.left;
                const int count = std::max(1, static_cast<int>((availableW + gap) / (2.0f + gap)));
                const float barWidth = (availableW - gap * (count - 1)) / count;
                mutedBrush_->SetOpacity(0.5f);
                for (int i = 0; i < count; ++i) {
                    const float dotX = waveRect.left + i * (barWidth + gap) + barWidth * 0.5f;
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, cy), 1.2f, 1.2f), mutedBrush_.Get());
                }
            }

            // Lyrics between the album art and the visualiser. Fades in as the
            // pill widens, so there is no pop while the spring is still moving.
            if (CollapsedLyricsActive(state, settings)) {
                const float lyricsAlpha =
                    SmoothStep01(((rect.right - rect.left) - 175.0f) / 60.0f);
                if (lyricsAlpha > 0.01f) {
                    const float textLeft = artRect.right + 10.0f;
                    const float textRight = waveRect.left - 10.0f;
                    DrawCollapsedLyrics(state,
                                        D2D1::RectF(textLeft, rect.top + 4.0f, textRight, rect.bottom - 4.0f),
                                        now, lyricsAlpha);
                }
            }

            // Clock + divider on the left, same face, size and time format as the
            // idle strip. On hover the collapsed layer fades out and the normal
            // player shows.
            if (clockSection > 0.0f) {
                SYSTEMTIME local = {};
                GetLocalTime(&local);
                const std::wstring clockText = FormatIslandTime(local, settings.clockFollowSystem,
                                                                settings.use24HourClock,
                                                                settings.showSeconds);
                const float clockLeft = rect.left + MediaPillLayout::kClockPadLeft;
                textBrush_->SetOpacity(0.96f);
                target_->DrawTextW(clockText.c_str(), static_cast<UINT32>(clockText.size()),
                                   idleTextFormat_.Get(),
                                   D2D1::RectF(clockLeft, rect.top, clockLeft + pillMetrics.clockWidth, rect.bottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textBrush_->SetOpacity(1.0f);

                ComPtr<ID2D1SolidColorBrush> divider;
                target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_), &divider);
                if (divider) {
                    const float divX = clockLeft + pillMetrics.clockWidth + MediaPillLayout::kSlotGap;
                    const float divW = MediaPillLayout::kDividerWidth;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(divX, rect.top + MediaPillLayout::kDividerInsetY,
                                                      divX + divW, rect.bottom - MediaPillLayout::kDividerInsetY),
                                          divW * 0.5f, divW * 0.5f),
                        divider.Get());
                }
            }

            target_->PopLayer();
        }

        // ── Shared album art pass ────────────────────────────────────────────
        // Drawn once, outside both cross-fading layers, at full opacity, so the cover
        // morphs smoothly in position, size and corner radius while the island
        // resizes. Once fully expanded, the expanded layer takes over (flip animation,
        // source badge). Alpha is eased so switching tabs can't make it pop in.
        {
            const bool trayDrag = g_trayDragOver.load(std::memory_order_relaxed);
            const bool onMediaTab = NormalizedTabIndex(settings) == MediaTabIndex(settings);
            const float targetAlpha = trayDrag ? 0.0f : (onMediaTab ? 1.0f : collapsedAlpha);

            const bool recent = artOuterLast_ >= 0.0 && (now - artOuterLast_) < 0.25;
            const float dtArt = recent ? Clamp(static_cast<float>(now - artOuterLast_), 0.001f, 0.05f) : 0.016f;
            artOuterLast_ = now;
            // Off the Media tab the art has no business following a slow ease; it
            // fades exactly with the collapsed pill's contents.
            if (!recent || artMorph >= 1.0f || !onMediaTab) {
                artOuterAlpha_ = targetAlpha;
            } else {
                artOuterAlpha_ += (targetAlpha - artOuterAlpha_) * (1.0f - std::exp(-dtArt / 0.06f));
            }

            // While the island is hiding, the art follows the pill: it shrinks
            // toward the pill's centre and fades, instead of staying 32px and
            // sticking out until the window stops painting.
            float hideS = 1.0f;
            if (islandHiding_) {
                const float needW = pillClockSection + kCollapsedArtPad * 2.0f + kCollapsedArtSize;
                const float curW = rect.right - rect.left;
                hideS = SmoothStep01(std::min(height / kCollapsedPillHeight, curW / needW));
            }
            // On the Media tab the cover morphs circle -> square. On any other tab it
            // stays a small circle on the collapsed pill and never flies into the
            // expanded layout, so it can't show up on top of the lyrics.
            const D2D1_RECT_F artBaseRect = onMediaTab ? morphArtRect : collapsedArtRect;
            const float artBaseRadius = onMediaTab ? morphArtRadius : kCollapsedArtSize * 0.5f;

            D2D1_RECT_F artDrawRect = artBaseRect;
            float artDrawRadius = artBaseRadius;
            if (hideS < 1.0f) {
                const float pcx = (rect.left + rect.right) * 0.5f;
                const float pcy = (rect.top + rect.bottom) * 0.5f;
                const float acx = (artBaseRect.left + artBaseRect.right) * 0.5f;
                const float acy = (artBaseRect.top + artBaseRect.bottom) * 0.5f;
                const float ncx = pcx + (acx - pcx) * hideS;
                const float ncy = pcy + (acy - pcy) * hideS;
                const float hw = (artBaseRect.right - artBaseRect.left) * 0.5f * hideS;
                const float hh = (artBaseRect.bottom - artBaseRect.top) * 0.5f * hideS;
                artDrawRect = D2D1::RectF(ncx - hw, ncy - hh, ncx + hw, ncy + hh);
                artDrawRadius = artBaseRadius * hideS;
            }
            const float artAlphaEff = artOuterAlpha_ * hideS;

            if (artMorph < 1.0f && artAlphaEff > 0.01f) {
                ID2D1Layer* artLayer = (artAlphaEff < 0.99f) ? AcquireLayer() : nullptr;
                if (artLayer) {
                    target_->PushLayer(
                        D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                              Clamp(artAlphaEff, 0.0f, 1.0f), nullptr,
                                              D2D1_LAYER_OPTIONS_NONE),
                        artLayer);
                }
                DrawAlbumArtDrop(state.media, artDrawRect, now, artDrawRadius);
                if (artLayer) {
                    target_->PopLayer();
                }
            }
        }
    }

