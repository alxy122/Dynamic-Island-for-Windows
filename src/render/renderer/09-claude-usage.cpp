    // One limit: label, percentage, bar and reset line. Above 50 % the bar and the number turn
    // yellow, above 80 % red (judged on the rounded number that is shown).
    void DrawUsageBlock(float left, float right, float top, float scale, const wchar_t* label,
                        bool ok, bool expired, double pct, const std::wstring& resetText) {
        const float w = right - left;
        textBrush_->SetOpacity(0.94f);
        DrawEllipsisText(label, D2D1::RectF(left, top, right - 70.0f * scale, top + 15.0f * scale),
                         smallTextFormat_.Get(), textBrush_.Get());

        // Warning colour, decided on the number the user actually sees.
        const bool live = ok && !expired;
        const double shown = std::round(pct);
        const int level = live ? usage::Level(shown) : 0;
        D2D1_COLOR_F levelColor = material_.accent;
        if (level == 1) levelColor = D2D1::ColorF(0xF4B400);
        else if (level == 2) levelColor = D2D1::ColorF(0xE5484D);
        levelColor.a = 0.95f * settingsOpacity_;

        wchar_t pctBuf[24] = {};
        if (live) swprintf_s(pctBuf, L"%.0f %%", shown);
        else wcscpy_s(pctBuf, ARRAYSIZE(pctBuf), L"\u2014");
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            textBrush_->SetOpacity(0.96f);
            ComPtr<ID2D1SolidColorBrush> warnBrush;
            if (level > 0) target_->CreateSolidColorBrush(levelColor, &warnBrush);
            target_->DrawTextW(pctBuf, static_cast<UINT32>(wcslen(pctBuf)), boldTextFormat_.Get(),
                               D2D1::RectF(right - 70.0f * scale, top - 1.0f * scale, right, top + 15.0f * scale),
                               warnBrush ? static_cast<ID2D1Brush*>(warnBrush.Get())
                                         : static_cast<ID2D1Brush*>(textBrush_.Get()),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float barH = 7.0f * scale;
        const D2D1_RECT_F track = D2D1::RectF(left, top + 19.0f * scale, right, top + 19.0f * scale + barH);
        if (raisedStrongBrush_) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, barH * 0.5f, barH * 0.5f),
                                          raisedStrongBrush_.Get());
        }
        if (ok && !expired && pct > 0.0) {
            const float span = std::max(barH, w * static_cast<float>(pct / 100.0));
            ComPtr<ID2D1SolidColorBrush> fill;
            target_->CreateSolidColorBrush(levelColor, &fill);
            if (fill) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(left, track.top, left + span, track.bottom),
                                      barH * 0.5f, barH * 0.5f), fill.Get());
            }
        }

        mutedBrush_->SetOpacity(0.80f);
        DrawEllipsisText(resetText, D2D1::RectF(left, top + 29.0f * scale, right, top + 44.0f * scale),
                         smallTextFormat_.Get(), mutedBrush_.Get());
    }

    // Claude usage tab: 5-hour session and weekly limit of the plan, as a percentage of
    // the limit (Anthropic does not expose token counts). Fed by the status line script.
    void DrawClaudeUsageDashboard(const SharedState& state, D2D1_RECT_F rect,
                                  const Settings& settings, float scale) {
        (void)settings;
        if (!textBrush_ || !mutedBrush_ || !smallTextFormat_) return;
        const float padX = FileTrayLayout::kPadX * scale;
        const ClaudeUsageSnapshot& u = state.claudeUsage;
        const int64_t nowUtc = LyricsCache::UnixNow();

        const D2D1_RECT_F headerRect = D2D1::RectF(rect.left + padX, rect.top + 18.0f * scale,
                                                   rect.right - padX, rect.top + 38.0f * scale);
        if (boldTextFormat_) {
            textBrush_->SetOpacity(0.96f);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = Loc(L"Claude usage");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(),
                               headerRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (!u.hasData) {
            const std::wstring hint = std::wstring(Loc(L"No usage data yet")) + L"\n" +
                                      Loc(L"Set up the status line script (see mod settings)");
            mutedBrush_->SetOpacity(0.80f);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(hint.c_str(), static_cast<UINT32>(hint.size()), smallTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, rect.top + 44.0f * scale, rect.right - padX,
                                           rect.bottom - 16.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        // How old the numbers are: they only refresh while Claude Code is being used.
        if (u.updatedAt > 0) {
            wchar_t ageBuf[96] = {};
            const std::wstring dur = LyricsCache::Utf8ToWide(usage::Duration(nowUtc - u.updatedAt));
            swprintf_s(ageBuf, Loc(L"Updated %s ago"), dur.c_str());
            mutedBrush_->SetOpacity(0.70f);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            target_->DrawTextW(ageBuf, static_cast<UINT32>(wcslen(ageBuf)), smallTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX + 110.0f * scale, rect.top + 20.0f * scale,
                                           rect.right - padX, rect.top + 38.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        auto resetLine = [&](bool ok, int64_t resetsAt, bool& expired) {
            expired = ok && resetsAt <= nowUtc;
            if (!ok) return std::wstring();
            if (expired) return std::wstring(Loc(L"Waiting for Claude Code"));
            wchar_t buf[96] = {};
            const std::wstring dur = LyricsCache::Utf8ToWide(usage::Duration(resetsAt - nowUtc));
            swprintf_s(buf, Loc(L"Resets in %s"), dur.c_str());
            return std::wstring(buf);
        };

        const float left = rect.left + padX;
        const float right = rect.right - padX;
        bool fiveExpired = false, sevenExpired = false;
        const std::wstring fiveText = resetLine(u.fiveOk, u.fiveResetsAt, fiveExpired);
        const std::wstring sevenText = resetLine(u.sevenOk, u.sevenResetsAt, sevenExpired);
        DrawUsageBlock(left, right, rect.top + 50.0f * scale, scale, Loc(L"Session"),
                       u.fiveOk, fiveExpired, u.fivePct, fiveText);
        DrawUsageBlock(left, right, rect.top + 104.0f * scale, scale, Loc(L"Week"),
                       u.sevenOk, sevenExpired, u.sevenPct, sevenText);

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

