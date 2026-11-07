    // ── Brightness banner (shares the Volume banner's look) ──────────────────
    void DrawLevelBanner(D2D1_RECT_F rect, const wchar_t* glyph, const std::wstring& label,
                         const wchar_t* value, float pct, bool drained) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), raisedStrongBrush_.Get());

        textBrush_->SetOpacity(0.95f);

        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
        mutedBrush_->SetOpacity(0.50f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F valueRect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), smallTextFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
        if (drained) {
            ComPtr<ID2D1SolidColorBrush> trackBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), trackBrush.Get());
            }
            D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                           track.left + (track.right - track.left) * pct,
                                           track.bottom);
            ComPtr<ID2D1SolidColorBrush> dim;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.textSecondary, 0.35f), &dim)) && dim) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
            }
        } else {
            DrawAccentTrack(track, pct, 2.5f);
        }
        accentBrush_->SetOpacity(1.0f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawBrightness(const SharedState& state, D2D1_RECT_F rect) {
        wchar_t value[32] = {};
        swprintf_s(value, L"%d%%", state.brightness.percent);
        DrawLevelBanner(rect, L"\uE706", Loc(L"Brightness"), value,
                        Clamp(state.brightness.percent / 100.0f, 0.0f, 1.0f), false);
    }

