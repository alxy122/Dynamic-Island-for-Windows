    // Load colour is *semantic*, not decorative: the accent while a component is
    // comfortable, amber under pressure, red when saturated. The previous design
    // assigned a fixed rainbow colour per metric, which carried no information
    // (CPU and NET UP were both green, GPU and NET DOWN both cyan) and fought the
    // album-art accent.
    D2D1_COLOR_F LoadStateColor(float fraction) const {
        if (fraction >= 0.90f) {
            return D2D1::ColorF(1.0f, 0.35f, 0.32f, 1.0f);
        }
        if (fraction >= 0.75f) {
            return D2D1::ColorF(1.0f, 0.69f, 0.13f, 1.0f);
        }
        return material_.accent;
    }

    void DrawHardwareMonitorDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale) {
        (void)settings;

        const float padX = 24.0f * scale;

        // Header, left aligned to match the File Tray and weather dashboards.
        textBrush_->SetOpacity(0.96f);
        const wchar_t* hwTitle = Loc(L"Hardware Monitor");
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(hwTitle, static_cast<UINT32>(wcslen(hwTitle)), boldTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, rect.top + 16.0f * scale,
                                           rect.right - padX, rect.top + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        wchar_t cpuBuf[32], ramBuf[40], gpuBuf[32], upBuf[32], downBuf[32], diskBuf[32];
        swprintf_s(cpuBuf, L"%d%%", state.system.cpuPercent);
        swprintf_s(ramBuf, L"%.1f / %.1f GB", state.system.memoryUsedGB, state.system.memoryTotalGB);
        if (state.system.gpuPercent >= 0) {
            swprintf_s(gpuBuf, L"%d%%", state.system.gpuPercent);
        } else {
            wcscpy_s(gpuBuf, L"--");
        }
        swprintf_s(upBuf, L"%.1f Mbps", state.system.netUpMbps);
        swprintf_s(downBuf, L"%.1f Mbps", state.system.netDownMbps);
        const int diskUsed = 100 - state.system.diskFreePercent;
        swprintf_s(diskBuf, L"%d%%", diskUsed);

        const float ramFraction = (state.system.memoryTotalGB > 0.01f)
            ? Clamp(state.system.memoryUsedGB / state.system.memoryTotalGB, 0.0f, 1.0f)
            : -1.0f;

        struct HwMetric {
            int glyph;
            const wchar_t* label;
            const wchar_t* value;
            float fraction;  // negative means "no meaningful 0-100 scale"
        };

        // Network throughput has no natural ceiling, so those two cards get no
        // load bar rather than a bar against an invented maximum.
        const HwMetric metrics[6] = {
            {3, L"CPU",      cpuBuf,  Clamp(state.system.cpuPercent / 100.0f, 0.0f, 1.0f)},
            {6, L"NET UP",   upBuf,   -1.0f},
            {4, L"RAM",      ramBuf,  ramFraction},
            {7, L"NET DOWN", downBuf, -1.0f},
            {5, L"GPU",      gpuBuf,  state.system.gpuPercent >= 0
                                          ? Clamp(state.system.gpuPercent / 100.0f, 0.0f, 1.0f)
                                          : -1.0f},
            {8, L"DISK",     diskBuf, Clamp(diskUsed / 100.0f, 0.0f, 1.0f)},
        };

        // 2 x 3 grid of cards. Cards give the grid its structure, so the old
        // full-height centre divider is gone.
        const float colGap = 9.0f * scale;
        const float colW = (rect.right - rect.left - padX * 2.0f - colGap) * 0.5f;
        const float rowH = 38.0f * scale;
        const float rowGap = 6.0f * scale;
        const float gridTop = rect.top + 38.0f * scale;

        for (int i = 0; i < 6; ++i) {
            const HwMetric& m = metrics[i];
            const int col = i % 2;
            const int row = i / 2;

            const float cardLeft = rect.left + padX + static_cast<float>(col) * (colW + colGap);
            const float cardTop = gridTop + static_cast<float>(row) * (rowH + rowGap);
            const D2D1_RECT_F card = D2D1::RectF(cardLeft, cardTop, cardLeft + colW, cardTop + rowH);

            DrawCard(card, 9.0f * scale);

            const bool hasBar = m.fraction >= 0.0f;
            const D2D1_COLOR_F tint = hasBar ? LoadStateColor(m.fraction) : material_.accent;

            if (ID2D1SolidColorBrush* glyphBrush = AcquireBrush(WithAlpha(tint, 0.85f * settingsOpacity_))) {
                DrawMetricGlyph(D2D1::Point2F(card.left + 16.0f * scale, card.top + 15.0f * scale),
                                15.0f * scale, m.glyph, glyphBrush);
            }

            const float textLeft = card.left + 30.0f * scale;
            const float textRight = card.right - 9.0f * scale;

            // Label sits above the value rather than sharing a line with it.
            // A single "Label: value" line collides once localized -- German
            // "Luftfeuchte" or Russian "Влажность" leave no room for the number.
            // The three bands (label / value / bar) are kept adjacent but
            // non-overlapping so nothing clips into its neighbour.
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.60f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 2.0f * scale,
                                               textRight, card.top + 15.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                // Without a bar the value takes the band the bar would have
                // used, so the two card shapes stay optically balanced.
                const float valueBottom = hasBar ? card.top + 30.0f * scale : card.bottom - 4.0f * scale;
                target_->DrawTextW(m.value, static_cast<UINT32>(wcslen(m.value)), textFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 15.0f * scale, textRight, valueBottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (hasBar) {
                const D2D1_RECT_F track = D2D1::RectF(textLeft, card.bottom - 6.0f * scale,
                                                      textRight, card.bottom - 3.5f * scale);
                ComPtr<ID2D1SolidColorBrush> trackBrush;
                if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                    target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), trackBrush.Get());
                }

                const float span = (track.right - track.left) * m.fraction;
                if (span > 0.5f) {
                    if (ID2D1SolidColorBrush* fillBrush = AcquireBrush(WithAlpha(tint, 0.95f))) {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                              1.25f * scale, 1.25f * scale),
                            fillBrush);
                    }
                }
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

