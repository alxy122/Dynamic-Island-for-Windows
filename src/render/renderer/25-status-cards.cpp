    void DrawCountdownProgress(float left, float right, float bottom, float progress) {
        if (!g_settings.statusCountdownProgress) return;
        const float h = 2.5f;
        D2D1_RECT_F track = D2D1::RectF(left, bottom - h, right, bottom);
        ComPtr<ID2D1SolidColorBrush> trackBrush;
        target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &trackBrush);
        target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f, 1.25f), trackBrush.Get());
        D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                       track.left + (track.right - track.left) * Clamp(progress, 0.0f, 1.0f),
                                       track.bottom);
        accentBrush_->SetOpacity(0.70f);
        target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 1.25f, 1.25f), accentBrush_.Get());
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawClipboard(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 40.0f || rect.right - rect.left < 100.0f) return;
        const double now = NowSeconds();
        const float ttl = 2.5f;
        const float remaining = Clamp(static_cast<float>(state.clipboard.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14.0f, cy - badgeSz * 0.5f,
                                        rect.left + 14.0f + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        if (state.clipboard.image && state.clipboard.imagePreview) {
            DrawCoverFitBitmapPixels(*state.clipboard.imagePreview,
                                     badge, br,
                                     clipboardImageBitmap_,
                                     clipboardImageGeneration_, 1.0f);
        } else if (state.clipboard.appIcon) {
            DrawRoundedBitmapPixels(*state.clipboard.appIcon,
                                    badge, br,
                                    clipboardIconBitmap_,
                                    clipboardIconGeneration_, 0.96f);
        } else {
            const wchar_t* glyph = state.clipboard.image
                ? (usingFluentIcons_ ? L"\uE91B" : L"\uE114")
                : (usingFluentIcons_ ? L"\uF0E3" : L"\uE8C8");
            textBrush_->SetOpacity(0.95f);

            if (iconFormat_) {
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target_->DrawTextW(glyph,
                                   static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            textBrush_->SetOpacity(0.90f);
        }

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        D2D1_RECT_F titleRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        mutedBrush_->SetOpacity(0.48f);
        const std::wstring clipTitle =
            state.clipboard.appName.empty()
                ? (state.clipboard.image ? std::wstring(L"Image copied") : std::wstring(L"Clipboard"))
                : state.clipboard.appName + L"  \u00b7  Clipboard";
        target_->DrawTextW(clipTitle.c_str(), static_cast<UINT32>(clipTitle.size()),
                           smallTextFormat_.Get(), titleRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F textRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        DrawMarqueeText(state.clipboard.text.empty() ? std::wstring(Loc(L"Copied")) : state.clipboard.text,
                        textRect, textFormat_.Get(), textBrush_.Get(), now, 34.0f, marqueeClipboardCache_);

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawNotification(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 48.0f || rect.right - rect.left < 120.0f) return;
        const double now = NowSeconds();
        const float ttl = 4.0f;
        const float remaining = Clamp(static_cast<float>(state.notification.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        // Apple DI: app icon is a large iOS-style rounded square.
        const float iconSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - iconSz * 0.5f,
                                        rect.left + 14 + iconSz, cy + iconSz * 0.5f);
        const float br = iconSz * 0.35f;  // Softer iOS superellipse-like squircle.

        // Icon background plate.
        ComPtr<ID2D1SolidColorBrush> plateBrush;
        target_->CreateSolidColorBrush(material_.raisedStrong, &plateBrush);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), plateBrush.Get());

        if (state.notification.icon) {
            DrawRoundedBitmapPixels(*state.notification.icon, badge, br,
                                    notificationIconBitmap_, notificationIconGeneration_, 1.0f);

            // Draw a red dot (badge) at the top-right of the app icon
            ComPtr<ID2D1SolidColorBrush> badgeColor;
            target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f), &badgeColor);

            const float dotR = iconSz * 0.13f;
            const float dotX = badge.right - dotR * 0.5f;
            const float dotY = badge.top + dotR * 0.5f;

            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), badgeColor.Get());

            ComPtr<ID2D1SolidColorBrush> badgeBorder;
            target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &badgeBorder);
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), badgeBorder.Get(), 0.9f);
        } else {
            const wchar_t* glyph = usingFluentIcons_ ? L"\uEA8F" : L"\uE7E7";
            textBrush_->SetOpacity(0.95f);
            if (iconFormat_) {
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target_->DrawTextW(glyph,
                                   static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            textBrush_->SetOpacity(0.90f);
        }

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        D2D1_RECT_F appRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        mutedBrush_->SetOpacity(0.75f);
        target_->DrawTextW(state.notification.app.c_str(),
                           static_cast<UINT32>(state.notification.app.size()),
                           smallTextFormat_.Get(), appRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F titleRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        DrawMarqueeText(state.notification.title.empty() ? L"Notification" : state.notification.title,
                        titleRect, textFormat_.Get(), textBrush_.Get(), now, 28.0f, marqueeNotificationCache_);
        textBrush_->SetOpacity(0.90f);

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.50f);
    }

    void DrawVolume(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const bool muted = state.volume.muted || state.volume.percent == 0;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f; // Softer squircle corners

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const wchar_t* glyph = muted ? L"\uE74F" : (usingFluentIcons_ ? L"\uE767" : L"\uE993");
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
        const std::wstring deviceLabel =
            state.volume.deviceName.empty() ? std::wstring(Loc(L"Volume")) : state.volume.deviceName;
        target_->DrawTextW(deviceLabel.c_str(), static_cast<UINT32>(deviceLabel.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[32] = {};
        if (muted) {
            wcscpy_s(value, ARRAYSIZE(value), Loc(L"Muted"));
        } else {
            swprintf_s(value, L"%d%%", state.volume.percent);
        }
        D2D1_RECT_F valueRect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), smallTextFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
        // Shared accent track, so the volume bar matches the media scrubber.
        const float pct = Clamp(state.volume.displayPercent / 100.0f, 0.0f, 1.0f);
        if (muted) {
            // Muted still shows the level, just drained of colour.
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

    void DrawTimer(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const double now = NowSeconds();
        double remaining = state.timer.justFinished
            ? 0.0
            : (state.timer.running ? std::max(0.0, state.timer.endsAt - now)
                                    : state.timer.remainingAtPause);
        const int totalSec = state.timer.totalSeconds > 0 ? state.timer.totalSeconds : 1;
        const float progress = state.timer.justFinished
            ? 1.0f
            : Clamp(1.0f - static_cast<float>(remaining / totalSec), 0.0f, 1.0f);

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const float ringR = badgeSz * 0.30f;
        D2D1_POINT_2F ringCenter = D2D1::Point2F((badge.left + badge.right) * 0.5f,
                                                  (badge.top + badge.bottom) * 0.5f);
        ComPtr<ID2D1SolidColorBrush> ringTrack;
        target_->CreateSolidColorBrush(material_.raisedStrong, &ringTrack);
        target_->DrawEllipse(D2D1::Ellipse(ringCenter, ringR, ringR), ringTrack.Get(), 2.0f);

        ComPtr<ID2D1PathGeometry> geometry;
        d2dFactory_->CreatePathGeometry(&geometry);
        ComPtr<ID2D1GeometrySink> sink;
        geometry->Open(&sink);
        const float start = -3.14159265f * 0.5f;
        const float sweep = 2.0f * 3.14159265f * progress;
        const int segments = std::max(2, static_cast<int>(40 * progress));
        auto pointAt = [&](float a) {
            return D2D1::Point2F(ringCenter.x + std::cos(a) * ringR,
                                  ringCenter.y + std::sin(a) * ringR);
        };
        sink->BeginFigure(pointAt(start), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= segments; ++i) {
            sink->AddLine(pointAt(start + sweep * i / segments));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        ComPtr<ID2D1SolidColorBrush> ringFg;
        D2D1_COLOR_F fgColor = state.timer.isBreak
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f, 0.58f, 0.0f, 1.0f);
        if (state.timer.justFinished) {
            fgColor = D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f);
        }
        target_->CreateSolidColorBrush(fgColor, &ringFg);
        target_->DrawGeometry(geometry.Get(), ringFg.Get(), 2.4f);

        if (state.timer.active && !state.timer.running) {
            ComPtr<ID2D1SolidColorBrush> pauseBrush;
            target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &pauseBrush);
            const float h = ringR * 0.7f;
            target_->FillRectangle(
                D2D1::RectF(ringCenter.x - 2.6f, ringCenter.y - h * 0.5f,
                            ringCenter.x - 0.8f, ringCenter.y + h * 0.5f), pauseBrush.Get());
            target_->FillRectangle(
                D2D1::RectF(ringCenter.x + 0.8f, ringCenter.y - h * 0.5f,
                            ringCenter.x + 2.6f, ringCenter.y + h * 0.5f), pauseBrush.Get());
        }

        const float tx = badge.right + 14;
        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = state.timer.justFinished
            ? (state.timer.isBreak ? L"Break Complete" : L"Focus Complete")
            : (state.timer.isBreak ? L"Break Timer" : L"Focus Timer");
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[32] = {};
        int rem = static_cast<int>(std::ceil(remaining));
        swprintf_s(value, L"%d:%02d", rem / 60, rem % 60);
        D2D1_RECT_F valueRect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawCapsLock(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 110.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const wchar_t* glyph = nullptr;
        std::wstring label;
        bool isOn = false;

        if (state.capsLock.isNumEvent) {
            glyph = L"1";
            label = Loc(L"Num Lock");
            isOn = state.capsLock.numOn;
        } else {
            glyph = L"A";
            label = Loc(L"Caps Lock");
            isOn = state.capsLock.capsOn;
        }

        // Draw central bold keycap glyph, vertically and horizontally centered
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), clockFormat_.Get(), badge,
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Draw physical glowing status LED inside the keycap at top-right with padding
        ComPtr<ID2D1SolidColorBrush> ledBrush;
        D2D1_COLOR_F ledColor = isOn
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)   // Green glowing LED for ON
            : D2D1::ColorF(1.0f,  1.0f,  1.0f,  0.22f);  // Dim white for OFF
        target_->CreateSolidColorBrush(ledColor, &ledBrush);

        const float ledR = 2.2f;
        D2D1_POINT_2F ledCenter = D2D1::Point2F(badge.right - 5.5f, badge.top + 5.5f);
        target_->FillEllipse(D2D1::Ellipse(ledCenter, ledR, ledR), ledBrush.Get());

        // Draw label text ("Caps Lock" / "Num Lock") - increased font size and vertically centered
        const float tx = badge.right + 14.0f;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 9.0f, rect.right - 46.0f, cy + 11.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           textFormat_.Get(), labelRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Draw status string (ON/OFF) - increased font size and vertically centered
        std::wstring status = isOn ? Loc(L"On") : Loc(L"Off");
        D2D1_RECT_F statusRect = D2D1::RectF(rect.right - 44.0f, cy - 9.0f, rect.right - 14.0f, cy + 11.0f);
        if (isOn) {
            textBrush_->SetOpacity(0.95f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void DrawDevice(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 100.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const bool connected = (state.device.eventType == DeviceEventType::Connected);

        // Badge circle with colored dot
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const wchar_t* glyph = usingFluentIcons_ ? L"\uECF0" : L"\uE88E";
        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        textBrush_->SetOpacity(0.90f);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        D2D1_COLOR_F dotColor = connected
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f,  0.27f, 0.22f, 1.0f);
        target_->CreateSolidColorBrush(dotColor, &dotBrush);

        D2D1_POINT_2F dotCenter = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
        target_->FillEllipse(D2D1::Ellipse(dotCenter, 4.5f, 4.5f), dotBrush.Get());

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        const double now = NowSeconds();
        const float ttl = 3.0f;
        const float remaining = Clamp(static_cast<float>(state.device.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = connected ? L"Device Connected" : L"Device Removed";
        D2D1_RECT_F labelRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        textBrush_->SetOpacity(0.95f);
        const std::wstring& name = state.device.deviceName.empty()
            ? (state.device.isBluetoothLike ? std::wstring(L"Bluetooth") : std::wstring(L"USB Device"))
            : state.device.deviceName;
        D2D1_RECT_F nameRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                           textFormat_.Get(), nameRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawDoNotDisturb(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 110.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14.0f, cy - badgeSz * 0.5f,
                                        rect.left + 14.0f + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const bool isOn = state.doNotDisturb.enabled;
        const wchar_t* glyph = isOn ? L"\uE7ED" : L"\uEA8F";
        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        ComPtr<ID2D1SolidColorBrush> ledBrush;
        D2D1_COLOR_F ledColor = isOn
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f,  1.0f,  1.0f,  0.22f);
        target_->CreateSolidColorBrush(ledColor, &ledBrush);

        const float ledR = 2.2f;
        D2D1_POINT_2F ledCenter = D2D1::Point2F(badge.right - 5.5f, badge.top + 5.5f);
        target_->FillEllipse(D2D1::Ellipse(ledCenter, ledR, ledR), ledBrush.Get());

        const float tx = badge.right + 14.0f;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 9.0f, rect.right - 46.0f, cy + 11.0f);
        textBrush_->SetOpacity(0.95f);
        std::wstring label = Loc(L"Do Not Disturb");
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           textFormat_.Get(), labelRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        std::wstring status = isOn ? Loc(L"On") : Loc(L"Off");
        D2D1_RECT_F statusRect = D2D1::RectF(rect.right - 44.0f, cy - 9.0f, rect.right - 14.0f, cy + 11.0f);
        if (isOn) {
            textBrush_->SetOpacity(0.95f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const double now = NowSeconds();
        const float ttl = 3.0f;
        const float remaining = Clamp(static_cast<float>(state.doNotDisturb.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;
        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 4.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
        textBrush_->SetOpacity(0.90f);
    }

    void DrawBluetoothCategoryIcon(D2D1_RECT_F badge, BluetoothDeviceCategory category) {
        const wchar_t* glyph = L"\uE702";
        switch (category) {
            case BluetoothDeviceCategory::Headphones:
                glyph = L"\uE7F6";
                break;
            case BluetoothDeviceCategory::Speaker:
                glyph = L"\uE7F5";
                break;
            case BluetoothDeviceCategory::Mouse:
                glyph = L"\uE962";
                break;
            case BluetoothDeviceCategory::Keyboard:
                glyph = L"\uE92E";
                break;
            case BluetoothDeviceCategory::Phone:
                glyph = L"\uE8EA";
                break;
            case BluetoothDeviceCategory::Generic:
            default:
                glyph = L"\uE702";
                break;
        }

        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        textBrush_->SetOpacity(0.90f);
    }

    void DrawBluetoothDevice(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const double now = NowSeconds();
        const float ttl = 4.0f;
        const float remaining = Clamp(static_cast<float>(state.bluetoothDevice.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        const bool connected = state.bluetoothDevice.connected;
        const int battery = state.bluetoothDevice.batteryPercent;
        const bool hasBattery = g_settings.bluetoothShowBattery && battery >= 0;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        DrawBluetoothCategoryIcon(badge, state.bluetoothDevice.category);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        D2D1_COLOR_F dotColor = connected
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f, 0.27f, 0.22f, 1.0f);
        target_->CreateSolidColorBrush(dotColor, &dotBrush);
        D2D1_POINT_2F dotCenter = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
        target_->FillEllipse(D2D1::Ellipse(dotCenter, 4.5f, 4.5f), dotBrush.Get());

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        const float rightEdge = hasBattery ? rect.right - 62.0f : rect.right - 14.0f;

        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = std::wstring(Loc(L"Bluetooth")) + L" " + (connected ? Loc(L"Connected") : Loc(L"Disconnected"));
        D2D1_RECT_F labelRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rightEdge, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rightEdge, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        textBrush_->SetOpacity(0.95f);
        const std::wstring& name = state.bluetoothDevice.deviceName.empty()
            ? std::wstring(L"Bluetooth Device")
            : state.bluetoothDevice.deviceName;
        D2D1_RECT_F nameRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rightEdge, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rightEdge, cy + 16.0f);
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                           textFormat_.Get(), nameRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (hasBattery) {
            const float colCenter = rect.right - 31.0f;
            const float bw = 16.0f;
            const float bh = 8.5f;
            const float nubW = 1.6f;
            const float totalW = bw + nubW;
            const float batLeft = colCenter - totalW * 0.5f;
            const float batY = showBar ? cy - 10.0f : cy - 9.0f;

            D2D1_RECT_F batRect = D2D1::RectF(batLeft, batY - bh * 0.5f,
                                              batLeft + bw, batY + bh * 0.5f);
            ComPtr<ID2D1SolidColorBrush> batBorder;
            target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.75f), &batBorder);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(batRect, 1.5f, 1.5f), batBorder.Get(), 1.2f);
            D2D1_RECT_F nub = D2D1::RectF(batRect.right, batY - 2.0f, batRect.right + nubW, batY + 2.0f);
            target_->FillRectangle(nub, batBorder.Get());

            const float pct = Clamp(battery / 100.0f, 0.0f, 1.0f);
            D2D1_RECT_F fill = D2D1::RectF(batRect.left + 1.5f, batRect.top + 1.5f,
                                           batRect.left + 1.5f + (bw - 3.0f) * pct, batRect.bottom - 1.5f);
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            D2D1_COLOR_F fillColor = battery <= 20 ? D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f)
                                                    : D2D1::ColorF(1, 1, 1, 0.92f);
            target_->CreateSolidColorBrush(fillColor, &fillBrush);
            target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 0.8f, 0.8f), fillBrush.Get());

            wchar_t pctBuf[16] = {};
            swprintf_s(pctBuf, L"%d%%", battery);
            D2D1_RECT_F pctRect = showBar
                ? D2D1::RectF(colCenter - 25.0f, cy - 2.0f, colCenter + 25.0f, cy + 14.0f)
                : D2D1::RectF(colCenter - 25.0f, cy - 1.0f, colCenter + 25.0f, cy + 15.0f);
            textBrush_->SetOpacity(0.85f);
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                target_->DrawTextW(pctBuf, static_cast<UINT32>(wcslen(pctBuf)), smallTextFormat_.Get(),
                                   pctRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
        textBrush_->SetOpacity(0.90f);
    }

