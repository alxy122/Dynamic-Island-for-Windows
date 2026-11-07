    void DrawGameOverlay(const SharedState& state, D2D1_RECT_F rect, float unused_scale) {
        (void)unused_scale;
        const float scale = 1.0f;
        const bool compact = g_settings.gameOverlayCompact;
        const GameOverlayLayout::Metrics m = GameOverlayLayout::For(compact);

        const float cardTop = rect.top + m.padY;
        const float cardBottom = rect.bottom - m.padY;
        float cursorX = rect.left + m.padX;

        // ── FPS, given hero treatment ────────────────────────────────────────
        // Frame rate is the number a player actually watches, so it gets a wider
        // card and a larger figure than the percentages beside it. It has no
        // 0-100 ceiling, so it gets no load bar and keeps the plain accent.
        if (g_settings.gameOverlayShowFps) {
            const D2D1_RECT_F fpsCard = D2D1::RectF(cursorX, cardTop, cursorX + m.fpsW, cardBottom);
            DrawCard(fpsCard, m.radius);

            ComPtr<ID2D1SolidColorBrush> fpsIcon;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.accent, 0.90f * settingsOpacity_), &fpsIcon)) && fpsIcon) {
                DrawMetricGlyph(D2D1::Point2F(fpsCard.left + 15.0f * scale, fpsCard.top + 15.0f * scale),
                                15.0f * scale, 9, fpsIcon.Get());
            }

            const float textLeft = fpsCard.left + 28.0f * scale;
            const float textRight = fpsCard.right - 9.0f * scale;

            if (g_settings.showMetricText && smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.58f);
                target_->DrawTextW(L"FPS", 3, smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, fpsCard.top + 2.0f * scale,
                                               textRight, fpsCard.top + 16.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            wchar_t fpsValue[16] = {};
            swprintf_s(fpsValue, L"%d", state.system.renderFps);
            IDWriteTextFormat* fpsFmt = clockFormat_ ? clockFormat_.Get() : textFormat_.Get();
            if (fpsFmt) {
                // clockFormat_ is centre-aligned by default; the strip reads as a
                // left-aligned column, so override for this draw and restore.
                fpsFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textBrush_->SetOpacity(0.97f);
                // Shares the label's left margin so the glyph sits in its own
                // column and the text column has one straight edge, rather than
                // the value hanging out to the left of the label above it.
                const float top = g_settings.showMetricText ? fpsCard.top + 16.0f * scale : cardTop;
                target_->DrawTextW(fpsValue, static_cast<UINT32>(wcslen(fpsValue)), fpsFmt,
                                   D2D1::RectF(textLeft, top,
                                               textRight, cardBottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                fpsFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            cursorX = fpsCard.right + m.fpsGap;
        }

        // ── Load cards ───────────────────────────────────────────────────────
        // Laid out from whichever metrics are enabled (#25), so turning some off
        // closes the gap instead of leaving a hole. Icons come from the shared
        // DrawMetricGlyph family, so CPU here is the same symbol as CPU on the
        // hardware dashboard.
        struct GameCard {
            const wchar_t* label;
            int percent;
            int glyph;
            bool enabled;
        };
        const GameCard cards[] = {
            {L"CPU", state.system.cpuPercent,              3, g_settings.gameOverlayShowCpu},
            {L"RAM", state.system.memoryPercent,           4, g_settings.gameOverlayShowRam},
            {L"GPU", state.system.gpuPercent,              5, g_settings.gameOverlayShowGpu},
            {L"DISK", 100 - state.system.diskFreePercent,  8, g_settings.gameOverlayShowDisk},
        };

        for (const GameCard& card : cards) {
            if (!card.enabled) {
                continue;
            }
            if (cursorX + m.cardW > rect.right - m.padX + 0.5f) {
                break;  // ran out of room
            }
            DrawGameMetricCard(D2D1::RectF(cursorX, cardTop, cursorX + m.cardW, cardBottom),
                               card.label, card.percent, card.glyph, m.radius);
            cursorX += m.cardW + m.gap;
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawGameMetricCard(D2D1_RECT_F rect, const wchar_t* label, int percent, int glyph, float radius) {
        const float scale = 1.0f;

        // Same semantic load colour the hardware dashboard uses: accent while a
        // component is comfortable, amber under pressure, red when saturated.
        // This replaces a per-metric rainbow (cyan CPU, magenta RAM, green GPU,
        // orange disk) that encoded nothing and fought the album-art accent.
        const bool known = percent >= 0;
        const float pct = known ? Clamp(percent / 100.0f, 0.0f, 1.0f) : 0.0f;
        const D2D1_COLOR_F tint = known ? LoadStateColor(pct) : material_.accent;

        // Fill only. This card used to carry a hairline border *and* a
        // metric-coloured ring on top of it -- two bright outlines per card,
        // four cards across, which is the edge lighting this design drops.
        DrawCard(rect, radius);

        ComPtr<ID2D1SolidColorBrush> glyphBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                WithAlpha(tint, 0.85f * settingsOpacity_), &glyphBrush)) && glyphBrush) {
            DrawMetricGlyph(D2D1::Point2F(rect.left + 15.0f * scale, rect.top + 15.0f * scale),
                            14.0f * scale, glyph, glyphBrush.Get());
        }

        const float textLeft = rect.left + 28.0f * scale;
        const float textRight = rect.right - 8.0f * scale;

        // Label above value above bar, matching the hardware dashboard exactly.
        if (g_settings.showMetricText && smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            mutedBrush_->SetOpacity(0.58f);
            target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), smallTextFormat_.Get(),
                               D2D1::RectF(textLeft, rect.top + 2.0f * scale,
                                           textRight, rect.top + 16.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t value[16] = {};
        if (known) {
            swprintf_s(value, L"%d%%", percent);
        } else {
            wcscpy_s(value, ARRAYSIZE(value), L"--");
        }

        if (textFormat_) {
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity(0.95f);
            // Aligned with the label above it, so the glyph owns the left column
            // and label/value share one straight edge.
            const float valueTop = g_settings.showMetricText ? rect.top + 17.0f * scale
                                                             : rect.top + 4.0f * scale;
            target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                               D2D1::RectF(textLeft, valueTop,
                                           textRight, rect.bottom - 8.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // The bar spans the card rather than just the text column: at this size it
        // reads as the card's own meter, and a 38px stub under the value did not.
        const D2D1_RECT_F track = D2D1::RectF(rect.left + 13.0f * scale, rect.bottom - 7.0f * scale,
                                              rect.right - 13.0f * scale, rect.bottom - 4.5f * scale);
        ComPtr<ID2D1SolidColorBrush> trackBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), trackBrush.Get());
        }
        const float span = (track.right - track.left) * pct;
        if (span > 0.5f) {
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(tint, 0.95f), &fillBrush)) && fillBrush) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                      1.25f * scale, 1.25f * scale),
                    fillBrush.Get());
            }
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

