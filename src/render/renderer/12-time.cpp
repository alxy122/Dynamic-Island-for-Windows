    void DrawTimeDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, SYSTEMTIME& local) {
        (void)now;

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;

        if (settings.clockAccentGlow) {
            D2D1_COLOR_F accentColor = accentBrush_ ? accentBrush_->GetColor() : D2D1::ColorF(0x4cc9f0);
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.24f)},
                {1.0f, D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> glowStops;
            target_->CreateGradientStopCollection(stops, 2, &glowStops);

            if (glowStops) {
                const D2D1_POINT_2F glowCenter = D2D1::Point2F(cx, cy - 8.0f * scale);
                ComPtr<ID2D1RadialGradientBrush> glowBrush;
                target_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(glowCenter, D2D1::Point2F(0, 0),
                                                         150.0f * scale, 70.0f * scale),
                    glowStops.Get(), &glowBrush);
                if (glowBrush) {
                    target_->FillEllipse(D2D1::Ellipse(glowCenter, 150.0f * scale, 70.0f * scale), glowBrush.Get());
                }
            }
        }

        // Time and date as a single typographic block. `dateFirst` promotes the
        // date to the headline for people who care about it more than the clock
        // (#61); either way the pair stays optically centred as a unit.
        const std::wstring timeText = FormatIslandTime(local, settings.clockFollowSystem,
                                                       settings.use24HourClock, settings.showSeconds);
        const std::wstring dateText = FormatIslandDate(local, settings.dateFormat, L"dddd, MMMM d");

        IDWriteTextFormat* bigFmt = timeDashboardFormat_ ? timeDashboardFormat_.Get() : hugeTextFormat_.Get();
        IDWriteTextFormat* smallFmt = dateDashboardFormat_ ? dateDashboardFormat_.Get() : boldTextFormat_.Get();

        const std::wstring& headline = settings.dateFirst ? dateText : timeText;
        const std::wstring& subline = settings.dateFirst ? timeText : dateText;
        IDWriteTextFormat* headlineFmt = settings.dateFirst ? smallFmt : bigFmt;
        IDWriteTextFormat* sublineFmt = settings.dateFirst ? bigFmt : smallFmt;

        if (settings.dateFirst) {
            // Date on top reads as a label, so give the big clock the lower slot.
            const D2D1_RECT_F headRect = D2D1::RectF(rect.left, cy - 46.0f * scale, rect.right, cy - 22.0f * scale);
            textBrush_->SetOpacity(0.92f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headlineFmt,
                               headRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F subRect = D2D1::RectF(rect.left, cy - 20.0f * scale, rect.right, cy + 44.0f * scale);
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), sublineFmt,
                               subRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            const D2D1_RECT_F headRect = D2D1::RectF(rect.left, cy - 44.0f * scale, rect.right, cy + 20.0f * scale);
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headlineFmt,
                               headRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F subRect = D2D1::RectF(rect.left, cy + 22.0f * scale, rect.right, cy + 46.0f * scale);
            mutedBrush_->SetOpacity(0.85f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), sublineFmt,
                               subRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        textBrush_->SetOpacity(0.98f);

        const bool micActive = state.system.micActive && settings.privacyDots && settings.privacyDotsMic;
        const bool camActive = state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam;

        if (camActive || micActive) {
            std::wstring label;
            D2D1_COLOR_F dotColor;

            if (camActive && micActive) {
                dotColor = settings.privacyDotsCamHex;
                if (!state.system.cameraApp.empty() && !state.system.micApp.empty() && state.system.cameraApp == state.system.micApp) {
                    label = state.system.cameraApp + L" is using camera & microphone";
                } else if (!state.system.cameraApp.empty() && !state.system.micApp.empty()) {
                    label = state.system.cameraApp + L" & " + state.system.micApp + L" using camera & mic";
                } else if (!state.system.cameraApp.empty()) {
                    label = state.system.cameraApp + L" is using camera & microphone";
                } else if (!state.system.micApp.empty()) {
                    label = state.system.micApp + L" is using camera & microphone";
                } else {
                    label = L"Camera & microphone in use";
                }
            } else if (camActive) {
                dotColor = settings.privacyDotsCamHex;
                if (!state.system.cameraApp.empty()) {
                    label = state.system.cameraApp + L" is using your camera";
                } else {
                    label = L"Camera in use";
                }
            } else {
                dotColor = settings.privacyDotsMicHex;
                if (!state.system.micApp.empty()) {
                    label = state.system.micApp + L" is using your microphone";
                } else {
                    label = L"Microphone in use";
                }
            }

            IDWriteTextFormat* fmt = smallTextFormat_ ? smallTextFormat_.Get() : textFormat_.Get();
            if (fmt && dwriteFactory_) {
                ComPtr<IDWriteTextLayout> textLayout;
                HRESULT hr = dwriteFactory_->CreateTextLayout(
                    label.c_str(), static_cast<UINT32>(label.size()),
                    fmt, 500.0f, 30.0f, &textLayout);

                if (SUCCEEDED(hr) && textLayout) {
                    DWRITE_TEXT_METRICS tm = {};
                    textLayout->GetMetrics(&tm);

                    const float pillH = 22.0f * scale;
                    const float pillW = tm.width + 28.0f * scale;
                    const float pillX = cx - pillW * 0.5f;
                    const float pillY = rect.bottom - 18.0f * scale - pillH;

                    ComPtr<ID2D1SolidColorBrush> badgeBg;
                    target_->CreateSolidColorBrush(WithAlpha(material_.raised, material_.raised.a * settingsOpacity_), &badgeBg);
                    if (badgeBg) {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH), pillH * 0.5f, pillH * 0.5f),
                            badgeBg.Get());
                    }

                    ComPtr<ID2D1SolidColorBrush> dotBrush;
                    dotColor.a = settingsOpacity_;
                    target_->CreateSolidColorBrush(dotColor, &dotBrush);
                    if (dotBrush) {
                        const float badgeDotR = 3.5f * scale;
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(pillX + 10.0f * scale, pillY + pillH * 0.5f), badgeDotR, badgeDotR),
                            dotBrush.Get());
                    }

                    mutedBrush_->SetOpacity(0.90f);
                    target_->DrawTextLayout(
                        D2D1::Point2F(pillX + 18.0f * scale, pillY + (pillH - tm.height) * 0.5f),
                        textLayout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                    mutedBrush_->SetOpacity(0.75f);
                }
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

