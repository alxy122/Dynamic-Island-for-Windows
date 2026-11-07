        // Karaoke effect: a soft wave rides the wipe boundary across the active line.
    // The layout and per-cluster rects are cached (karaokeCache_) and only rebuilt
    // when the line changes.
    void DrawKaraokeLetterPop(const wchar_t* text, UINT32 textLength, IDWriteTextFormat* format,
                              D2D1_RECT_F lineRect, float wipeX, float baseOpacity,
                              float unsungOpacity, bool glowEnabled, float glowStrength = 1.0f) {
        if (!text || textLength == 0 || !format || baseOpacity <= 0.01f) {
            return;
        }

        const float layoutWidth = lineRect.right - lineRect.left;
        const float layoutHeight = lineRect.bottom - lineRect.top;
        if (layoutWidth <= 0.0f || layoutHeight <= 0.0f) {
            return;
        }

        const std::wstring textView(text, textLength);
        if (karaokeCache_.text != textView || karaokeCache_.format != format ||
            std::fabs(karaokeCache_.layoutWidth - layoutWidth) > 0.5f ||
            std::fabs(karaokeCache_.layoutHeight - layoutHeight) > 0.5f ||
            !karaokeCache_.layout) {
            karaokeCache_.layout.Reset();
            karaokeCache_.clusters.clear();
            karaokeCache_.valid = false;
            karaokeCache_.text = textView;
            karaokeCache_.format = format;
            karaokeCache_.layoutWidth = layoutWidth;
            karaokeCache_.layoutHeight = layoutHeight;

            dwriteFactory_->CreateTextLayout(text, textLength, format, layoutWidth, layoutHeight,
                                             &karaokeCache_.layout);
            if (karaokeCache_.layout) {
                // Walk shaped glyph clusters (not raw UTF-16 units) so scripts like
                // Devanagari, where one letter is several code units, stay intact.
                UINT32 clusterCount = 0;
                karaokeCache_.layout->GetClusterMetrics(nullptr, 0, &clusterCount);
                if (clusterCount > 0) {
                    std::vector<DWRITE_CLUSTER_METRICS> rawClusters(clusterCount);
                    if (SUCCEEDED(karaokeCache_.layout->GetClusterMetrics(rawClusters.data(), clusterCount,
                                                                          &clusterCount))) {
                        karaokeCache_.clusters.reserve(clusterCount);
                        UINT32 clusterStart = 0;
                        for (UINT32 c = 0; c < clusterCount; ++c) {
                            const UINT32 clusterLength = rawClusters[c].length;
                            const UINT32 i = clusterStart;
                            clusterStart += clusterLength;

                            KaraokeLayoutCache::ClusterInfo info;
                            info.textStart = i;
                            info.textLength = clusterLength;
                            info.isWhitespace = rawClusters[c].isWhitespace || clusterLength == 0;

                            if (!info.isWhitespace) {
                                DWRITE_HIT_TEST_METRICS hit{};
                                UINT32 hitCount = 0;
                                if (SUCCEEDED(karaokeCache_.layout->HitTestTextRange(
                                        i, clusterLength, 0.0f, 0.0f, &hit, 1, &hitCount)) &&
                                    hitCount > 0 && hit.width > 0.0f) {
                                    info.rect = D2D1::RectF(hit.left, hit.top, hit.left + hit.width,
                                                            hit.top + hit.height);
                                } else {
                                    info.isWhitespace = true;
                                }
                            }

                            karaokeCache_.clusters.push_back(info);
                        }
                        karaokeCache_.valid = true;
                    }
                }
            }
        }

        if (!karaokeCache_.valid) {
            // Plain two-pass wipe if the layout could not be built.
            mutedBrush_->SetOpacity(baseOpacity * unsungOpacity);
            target_->DrawTextW(text, textLength, format, lineRect, mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (wipeX > lineRect.left) {
                D2D1_RECT_F sungClip = D2D1::RectF(lineRect.left, lineRect.top,
                                                   std::min(wipeX, lineRect.right), lineRect.bottom);
                target_->PushAxisAlignedClip(sungClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                textBrush_->SetOpacity(baseOpacity);
                target_->DrawTextW(text, textLength, format, lineRect, textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                target_->PopAxisAlignedClip();
            }
            return;
        }

        constexpr float kWaveTrailWidth = 150.0f;
        constexpr float kWaveLeadWidth = 46.0f;
        constexpr int kGlowStampCount = 4;
        constexpr float kGlowRadius = 0.85f;
        constexpr float kGlowAlpha = 0.075f;

        for (const auto& cluster : karaokeCache_.clusters) {
            if (cluster.isWhitespace) {
                continue;
            }

            const D2D1_RECT_F hitRect = D2D1::RectF(
                cluster.rect.left + lineRect.left, cluster.rect.top + lineRect.top,
                cluster.rect.right + lineRect.left, cluster.rect.bottom + lineRect.top);

            const float charCenterX = (hitRect.left + hitRect.right) * 0.5f;
            const float distFromWipe = charCenterX - wipeX;  // <= 0 means already sung

            float pop;
            if (distFromWipe <= 0.0f) {
                const float t = Clamp(-distFromWipe / kWaveTrailWidth, 0.0f, 1.0f);
                pop = 0.5f * (1.0f + std::cos(t * 3.14159265f));
            } else {
                const float t = Clamp(distFromWipe / kWaveLeadWidth, 0.0f, 1.0f);
                pop = 0.5f * (1.0f + std::cos(t * 3.14159265f));
            }

            const bool sung = distFromWipe <= 0.0f;
            D2D1_RECT_F charRect = D2D1::RectF(hitRect.left, lineRect.top, lineRect.right, lineRect.bottom);
            const wchar_t* clusterText = text + cluster.textStart;

            if (glowEnabled && glowStrength > 0.01f && pop > 0.02f) {
                const float alpha = kGlowAlpha * pop * baseOpacity * glowStrength;
                if (alpha > 0.003f) {
                    textBrush_->SetOpacity(alpha);
                    for (int s = 0; s < kGlowStampCount; ++s) {
                        const float a = (2.0f * 3.14159265f * s) / kGlowStampCount;
                        const float ox = std::cos(a) * kGlowRadius;
                        const float oy = std::sin(a) * kGlowRadius;
                        D2D1_RECT_F stampRect = D2D1::RectF(
                            charRect.left + ox, charRect.top + oy,
                            charRect.right + ox, charRect.bottom + oy);
                        target_->DrawTextW(clusterText, cluster.textLength, format, stampRect,
                                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    textBrush_->SetOpacity(baseOpacity);
                }
            }

            if (sung) {
                textBrush_->SetOpacity(baseOpacity);
                target_->DrawTextW(clusterText, cluster.textLength, format, charRect, textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            } else {
                mutedBrush_->SetOpacity(baseOpacity * unsungOpacity);
                target_->DrawTextW(clusterText, cluster.textLength, format, charRect, mutedBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    void DrawLyricsDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                             float scale) {
        const double now = NowSeconds();

        // If the tab wasn't on screen a moment ago (island was collapsed), its scroll
        // position, highlight and visualizer fade are stale. Snap them to the current
        // state instead of animating them while the island is still expanding.
        {
            const bool lyricsFresh = lyricsLastDrawn_ >= 0.0 && (now - lyricsLastDrawn_) < 0.25;
            lyricsLastDrawn_ = now;
            g_lastLyricsViewTime.store(now, std::memory_order_relaxed);
            if (!lyricsFresh) {
                lyricsScrollAnimStartTime_ = -1.0;  // triggers the snap branch below
                lyricsWaveformFadeTime_ = -1.0;     // snaps the visualizer alpha
            }
        }

        // Lyric effects ramp: 0 while the island is still resizing, then eases to 1 over 0.3s.
        // A fresh open of the tab on an already-settled island goes straight to 1 (old behaviour).
        {
            const bool fxRecent = lyricsFxLast_ >= 0.0 && (now - lyricsFxLast_) < 0.25;
            const float fxDt = fxRecent ? Clamp(static_cast<float>(now - lyricsFxLast_), 0.001f, 0.05f) : 0.0f;
            lyricsFxLast_ = now;
            if (!islandSettled_) {
                lyricsFxMix_ = 0.0f;
            } else if (!fxRecent) {
                lyricsFxMix_ = 1.0f;
            } else {
                lyricsFxMix_ = std::min(1.0f, lyricsFxMix_ + fxDt / 0.30f);
            }
            if (!islandSettled_ || lyricsFxMix_ < 1.0f) {
                g_layoutDirty = true;  // keep frames coming until the effects are fully in
            }
        }

        // ── Header: title on the left, tiny live visualizer on the right ─────
        textBrush_->SetOpacity(0.96f);
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = Loc(L"Lyrics");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(),
                               D2D1::RectF(rect.left + 26.0f * scale, rect.top + 14.0f * scale,
                                           rect.right - 96.0f * scale, rect.top + 36.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        {
            const float waveformTarget = (state.media.playing && settings.lyricsVisualizer) ? 1.0f : 0.0f;
            if (lyricsWaveformFadeTime_ < 0.0) {
                lyricsWaveformAlpha_ = waveformTarget;
            } else {
                const float dt = Clamp(static_cast<float>(now - lyricsWaveformFadeTime_), 0.0f, 0.25f);
                const float t = 1.0f - std::exp(-8.0f * dt);
                lyricsWaveformAlpha_ += (waveformTarget - lyricsWaveformAlpha_) * t;
            }
            lyricsWaveformFadeTime_ = now;

            if (lyricsWaveformAlpha_ > 0.01f) {
                const D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 24.0f * scale - 56.0f * scale,
                                                         rect.top + 17.0f * scale,
                                                         rect.right - 24.0f * scale,
                                                         rect.top + 35.0f * scale);
                ID2D1Layer* waveFadeLayer = AcquireLayer();
                if (waveFadeLayer) {
                    target_->PushLayer(D2D1::LayerParameters(waveRect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                              D2D1::IdentityMatrix(), lyricsWaveformAlpha_, nullptr,
                                                              D2D1_LAYER_OPTIONS_NONE),
                                       waveFadeLayer);
                    DrawSpectrum(state, waveRect, settings, now);
                    target_->PopLayer();
                }
            }
        }

        const D2D1_RECT_F bodyRect = D2D1::RectF(rect.left + 24.0f * scale, rect.top + 44.0f * scale,
                                                 rect.right - 24.0f * scale, rect.bottom - 14.0f * scale);

        if (textFormat_) {
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        if (state.lyrics.fetching) {
            const float pulse = 0.55f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6));
            mutedBrush_->SetOpacity(pulse);
            const wchar_t* msg = Loc(L"Fetching lyrics...");
            target_->DrawTextW(msg, static_cast<UINT32>(wcslen(msg)), textFormat_.Get(), bodyRect,
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        if (!state.lyrics.hasData || state.lyrics.lines.empty()) {
            mutedBrush_->SetOpacity(0.55f);
            const wchar_t* msg = Loc(L"No lyrics found for this track.");
            target_->DrawTextW(msg, static_cast<UINT32>(wcslen(msg)), textFormat_.Get(), bodyRect,
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        if (state.lyrics.synced) {
            double currentPositionSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                currentPositionSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }

            // The reported position lags what you hear by a bit; nudge it forward.
            // Raise this if lyrics feel late, lower it if they feel early.
            const double kLyricsLookaheadSec = LyricsLookaheadSec();  // 0.49 baseline + speaker/Bluetooth offset
            if (state.media.playing) {
                currentPositionSec += kLyricsLookaheadSec;
            }
            const int64_t posMs = std::max<int64_t>(0, static_cast<int64_t>(currentPositionSec * 1000.0));

            int currentIndex = 0;
            for (size_t i = 0; i < state.lyrics.lines.size(); ++i) {
                if (state.lyrics.lines[i].timeMs <= posMs) {
                    currentIndex = static_cast<int>(i);
                } else {
                    break;
                }
            }

            // Fixed-duration ease-in-out scroll toward the active line.
            constexpr float kScrollAnimDurationSec = 0.55f;
            constexpr float kHighlightFadeDurationSec = 0.65f;
            const std::wstring trackKey = state.lyrics.matchedArtist + L"|" + state.lyrics.matchedTitle;
            // While the island is resizing, keep scroll/highlight snapped to the current line
            // (same snap as a fresh open). Animations resume once it settles, and the glow
            // decay starts from that moment.
            if (trackKey != lyricsScrollTrackKey_ || lyricsScrollAnimStartTime_ < 0.0 ||
                lyricsFxMix_ <= 0.0f) {
                lyricsScrollPos_ = static_cast<float>(currentIndex);
                lyricsScrollAnimStartPos_ = lyricsScrollPos_;
                lyricsScrollAnimTargetPos_ = lyricsScrollPos_;
                lyricsScrollAnimStartTime_ = now;
                lyricsScrollTrackKey_ = trackKey;
                lyricsHighlightIndex_ = currentIndex;
                lyricsHighlightPrevIndex_ = currentIndex;
                lyricsHighlightFadeStartTime_ = now - kHighlightFadeDurationSec;
            } else {
                if (static_cast<float>(currentIndex) != lyricsScrollAnimTargetPos_) {
                    lyricsScrollAnimStartPos_ = lyricsScrollPos_;
                    lyricsScrollAnimTargetPos_ = static_cast<float>(currentIndex);
                    lyricsScrollAnimStartTime_ = now;

                    lyricsHighlightPrevIndex_ = lyricsHighlightIndex_;
                    lyricsHighlightIndex_ = currentIndex;
                    lyricsHighlightFadeStartTime_ = now;
                }

                const float elapsed = static_cast<float>(now - lyricsScrollAnimStartTime_);
                const float t = Clamp(elapsed / kScrollAnimDurationSec, 0.0f, 1.0f);
                lyricsScrollPos_ = lyricsScrollAnimStartPos_ +
                    (lyricsScrollAnimTargetPos_ - lyricsScrollAnimStartPos_) * SmoothStep01(t);
            }

            constexpr int kContextLines = 2;
            const float lineHeight = 42.0f * scale;
            const float centerY = (bodyRect.top + bodyRect.bottom) * 0.5f;
            const int baseIndex = static_cast<int>(std::floor(lyricsScrollPos_));

            // Larger, heavier type for the focused line. Honors the Font family
            // and Text size settings.
            constexpr float kLyricsActiveFontSize = 19.5f;
            const float activeFontSize = kLyricsActiveFontSize * scale * settings.textScale;
            const wchar_t* activeFamily =
                settings.fontFamily.empty() ? L"Segoe UI Variable Display" : settings.fontFamily.c_str();

            auto makeActiveFormat = [&](float size, ComPtr<IDWriteTextFormat>& out) {
                out.Reset();
                if (!dwriteFactory_) {
                    return;
                }
                HRESULT hr = dwriteFactory_->CreateTextFormat(
                    activeFamily, nullptr, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
                if (FAILED(hr) || !out) {
                    out.Reset();
                    dwriteFactory_->CreateTextFormat(
                        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
                }
                if (out) {
                    out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
                karaokeCache_.layout.Reset();  // cache is keyed by format pointer
            };

            if (std::fabs(lyricsActiveTextFormatSize_ - activeFontSize) > 0.01f || !lyricsActiveTextFormat_) {
                makeActiveFormat(activeFontSize, lyricsActiveTextFormat_);
                lyricsActiveTextFormatSize_ = activeFontSize;
            }
            IDWriteTextFormat* activeFormat =
                lyricsActiveTextFormat_ ? lyricsActiveTextFormat_.Get() : boldTextFormat_.Get();

            const float kMaxNeighborOpacity = settings.lyricsNeighborLinesVisible ? 0.55f : 0.22f;
            const bool instantHighlightSwitch = settings.lyricsNeighborLinesVisible;
            constexpr float kKaraokeUnsungOpacity = 0.6f;
            constexpr float kKaraokeGlowTrailWidth = 46.0f;
            constexpr float kKaraokeGlowLeadWidth = 6.0f;
            constexpr float kKaraokeGlowPeakAlpha = 0.30f;

            constexpr float kFocusFalloffLines = 2.2f;
            auto EdgeFade = [&](float distanceFromCenter) -> float {
                const float t = 1.0f - Clamp(std::fabs(distanceFromCenter) / kFocusFalloffLines, 0.0f, 1.0f);
                return SmoothStep01(t);
            };

            for (int offset = -kContextLines; offset <= kContextLines; ++offset) {
                const int idx = baseIndex + offset;
                if (idx < 0 || idx >= static_cast<int>(state.lyrics.lines.size())) continue;

                const float distance = static_cast<float>(idx) - lyricsScrollPos_;
                const float y = centerY + distance * lineHeight;

                const float edgeFade = EdgeFade(distance);
                if (edgeFade <= 0.01f) continue;

                D2D1_RECT_F lineRect = D2D1::RectF(
                    bodyRect.left, y - lineHeight * 0.5f,
                    bodyRect.right, y + lineHeight * 0.5f);

                const std::wstring& line = state.lyrics.lines[idx].text;
                const wchar_t* toDraw = line.empty() ? L"\u266A" : line.c_str();
                const UINT32 toDrawLen = static_cast<UINT32>(line.empty() ? 1 : line.size());

                float activeBlend = 0.0f;
                float glowPulse = 0.0f;
                float karaokeProgress = -1.0f;  // -1 = not the karaoke-wiped line right now
                if (idx == lyricsHighlightIndex_) {
                    const double sinceBecameActive = now - lyricsHighlightFadeStartTime_;
                    const float fadeT = Clamp(static_cast<float>(sinceBecameActive) / kHighlightFadeDurationSec, 0.0f, 1.0f);
                    activeBlend = instantHighlightSwitch ? 1.0f : SmoothStep01(fadeT);

                    constexpr float kGlowHoldSec = 0.15f;
                    constexpr float kGlowDecaySec = 0.85f;
                    if (sinceBecameActive < kHighlightFadeDurationSec) {
                        glowPulse = fadeT;
                    } else {
                        const double afterHold = sinceBecameActive - kHighlightFadeDurationSec - kGlowHoldSec;
                        if (afterHold < 0.0) {
                            glowPulse = 1.0f;
                        } else {
                            glowPulse = 1.0f - SmoothStep01(Clamp(static_cast<float>(afterHold / kGlowDecaySec), 0.0f, 1.0f));
                        }
                    }

                    if (settings.karaokeLyrics) {
                        const int64_t lineStartMs = state.lyrics.lines[idx].timeMs;
                        int64_t lineEndMs = lineStartMs + 4000;  // fallback for the last line
                        if (idx + 1 < static_cast<int>(state.lyrics.lines.size())) {
                            lineEndMs = state.lyrics.lines[idx + 1].timeMs;
                        } else if (state.media.endTicks > 0) {
                            lineEndMs = state.media.endTicks / 10000;
                        }
                        const int64_t span = lineEndMs - lineStartMs;
                        karaokeProgress = span > 0
                            ? Clamp(static_cast<float>(posMs - lineStartMs) / static_cast<float>(span), 0.0f, 1.0f)
                            : 1.0f;
                    }
                } else if (idx == lyricsHighlightPrevIndex_) {
                    if (instantHighlightSwitch) {
                        activeBlend = 0.0f;
                        glowPulse = 0.0f;
                    } else {
                        const float fadeT = Clamp(static_cast<float>(now - lyricsHighlightFadeStartTime_) / kHighlightFadeDurationSec, 0.0f, 1.0f);
                        activeBlend = std::pow(1.0f - SmoothStep01(fadeT), 1.4f);
                        glowPulse = 1.0f - SmoothStep01(fadeT);
                    }
                }

                // ── Bright / active pass ─────────────────────────────────────
                if (activeBlend > 0.01f) {
                    D2D1_RECT_F activeLineRect = D2D1::RectF(
                        lineRect.left, y - lineHeight * 0.66f,
                        lineRect.right, y + lineHeight * 0.66f);

                    const float availableWidth = activeLineRect.right - activeLineRect.left;
                    IDWriteTextFormat* drawFormat = activeFormat;

                    ComPtr<IDWriteTextLayout> measureLayout;
                    dwriteFactory_->CreateTextLayout(toDraw, toDrawLen, activeFormat,
                                                     availableWidth, lineHeight, &measureLayout);
                    float textWidth = availableWidth;
                    if (measureLayout) {
                        DWRITE_TEXT_METRICS metrics{};
                        measureLayout->GetMetrics(&metrics);
                        textWidth = metrics.widthIncludingTrailingWhitespace;
                    }

                    // Too wide for the active size: shrink just enough to fit.
                    if (textWidth > availableWidth && textWidth > 0.0f) {
                        const float fitFontSize = std::max(
                            9.0f, kLyricsActiveFontSize * scale * settings.textScale *
                                      (availableWidth / textWidth) * 0.96f);
                        if (std::fabs(lyricsActiveFitTextFormatSize_ - fitFontSize) > 0.1f ||
                            !lyricsActiveFitTextFormat_) {
                            makeActiveFormat(fitFontSize, lyricsActiveFitTextFormat_);
                            lyricsActiveFitTextFormatSize_ = fitFontSize;
                        }
                        if (lyricsActiveFitTextFormat_) {
                            drawFormat = lyricsActiveFitTextFormat_.Get();
                        }
                    }

                    if (karaokeProgress >= 0.0f) {
                        const float renderedTextWidth =
                            (drawFormat == lyricsActiveFitTextFormat_.Get())
                                ? availableWidth * 0.96f
                                : std::min(textWidth, availableWidth);
                        const float wipeX = activeLineRect.left + karaokeProgress * renderedTextWidth;

                        if (settings.karaokeLetterGlow) {
                            DrawKaraokeLetterPop(toDraw, toDrawLen, drawFormat, activeLineRect,
                                                 wipeX, activeBlend * edgeFade, kKaraokeUnsungOpacity,
                                                 settings.karaokeLetterGlow, lyricsFxMix_);
                        } else {
                            mutedBrush_->SetOpacity(activeBlend * edgeFade * kKaraokeUnsungOpacity);
                            target_->DrawTextW(toDraw, toDrawLen, drawFormat, activeLineRect,
                                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

                            if (wipeX > activeLineRect.left) {
                                D2D1_RECT_F sungClip = D2D1::RectF(
                                    activeLineRect.left, activeLineRect.top,
                                    std::min(wipeX, activeLineRect.right), activeLineRect.bottom);
                                target_->PushAxisAlignedClip(sungClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                                textBrush_->SetOpacity(activeBlend * edgeFade);
                                target_->DrawTextW(toDraw, toDrawLen, drawFormat, activeLineRect,
                                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                                target_->PopAxisAlignedClip();
                            }
                        }

                        // Moving glow riding the wipe edge, drawn as text so it
                        // only lights actual glyph pixels.
                        if (karaokeProgress < 1.0f && lyricsFxMix_ > 0.01f) {
                            const float trailWidth = kKaraokeGlowTrailWidth * scale;
                            const float leadWidth = kKaraokeGlowLeadWidth * scale;
                            const float peakAlpha = kKaraokeGlowPeakAlpha * activeBlend * edgeFade * lyricsFxMix_;
                            const float peakOffset = trailWidth / (trailWidth + leadWidth);
                            D2D1_GRADIENT_STOP glowStops[3] = {
                                {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                                {peakOffset, D2D1::ColorF(1.0f, 1.0f, 1.0f, peakAlpha)},
                                {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                            };
                            ComPtr<ID2D1GradientStopCollection> glowStopCollection;
                            target_->CreateGradientStopCollection(glowStops, 3, &glowStopCollection);
                            if (glowStopCollection) {
                                ComPtr<ID2D1LinearGradientBrush> glowBrush;
                                target_->CreateLinearGradientBrush(
                                    D2D1::LinearGradientBrushProperties(
                                        D2D1::Point2F(wipeX - trailWidth, y),
                                        D2D1::Point2F(wipeX + leadWidth, y)),
                                    glowStopCollection.Get(), &glowBrush);
                                if (glowBrush) {
                                    target_->DrawTextW(toDraw, toDrawLen, drawFormat, activeLineRect,
                                                       glowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                                }
                            }
                        }
                    } else {
                        // Whole-line highlight (karaoke off, or the line fading out).
                        const float glowStrength = glowPulse * edgeFade * lyricsFxMix_;
                        if (glowStrength > 0.02f) {
                            constexpr int kGlowStampCount = 8;
                            constexpr float kGlowRadiusInner = 0.9f;
                            constexpr float kGlowRadiusOuter = 2.0f;
                            constexpr float kGlowRadiusBloom = 4.2f;
                            const float innerAlpha = 0.05f * glowStrength;
                            const float outerAlpha = 0.028f * glowStrength;
                            const float bloomAlpha = 0.016f * glowStrength;

                            for (int ring = 0; ring < 3; ++ring) {
                                const float radius = (ring == 0 ? kGlowRadiusInner
                                                       : ring == 1 ? kGlowRadiusOuter
                                                                   : kGlowRadiusBloom) * scale;
                                const float alpha = (ring == 0 ? innerAlpha
                                                      : ring == 1 ? outerAlpha
                                                                  : bloomAlpha);
                                if (alpha <= 0.002f) continue;

                                textBrush_->SetOpacity(alpha);
                                for (int i = 0; i < kGlowStampCount; ++i) {
                                    const float a = (2.0f * 3.14159265f * i) / kGlowStampCount;
                                    const float ox = std::cos(a) * radius;
                                    const float oy = std::sin(a) * radius;
                                    D2D1_RECT_F stampRect = D2D1::RectF(
                                        activeLineRect.left + ox, activeLineRect.top + oy,
                                        activeLineRect.right + ox, activeLineRect.bottom + oy);
                                    target_->DrawTextW(toDraw, toDrawLen, drawFormat, stampRect,
                                                       textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                                }
                            }
                        }

                        textBrush_->SetOpacity(activeBlend * edgeFade);
                        target_->DrawTextW(toDraw, toDrawLen, drawFormat, activeLineRect,
                                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }

                // ── Dim / neighbor pass ──────────────────────────────────────
                const float dimOpacity = kMaxNeighborOpacity * (1.0f - activeBlend) * edgeFade;
                if (dimOpacity > 0.01f) {
                    mutedBrush_->SetOpacity(dimOpacity);
                    target_->DrawTextW(toDraw, toDrawLen, textFormat_.Get(), lineRect,
                                       mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }

            textBrush_->SetOpacity(0.96f);
            mutedBrush_->SetOpacity(0.75f);
        } else {
            // Unsynced (plain) lyrics: a simple static list.
            const float lineHeight = 19.0f * scale;
            const size_t maxLines = std::min<size_t>(
                state.lyrics.lines.size(),
                static_cast<size_t>((bodyRect.bottom - bodyRect.top) / lineHeight));

            textBrush_->SetOpacity(0.82f);
            for (size_t i = 0; i < maxLines; ++i) {
                D2D1_RECT_F lineRect = D2D1::RectF(
                    bodyRect.left, bodyRect.top + i * lineHeight,
                    bodyRect.right, bodyRect.top + (i + 1) * lineHeight);
                const std::wstring& line = state.lyrics.lines[i].text;
                target_->DrawTextW(line.c_str(), static_cast<UINT32>(line.size()), textFormat_.Get(),
                                   lineRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            textBrush_->SetOpacity(0.96f);
        }
    }

