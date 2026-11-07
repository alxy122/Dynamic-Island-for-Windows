    // Appointments tab: what is still ahead, grouped by day (today first). Events
    // that already ended are left out; one that is running stays, its time in the
    // accent colour. If the days do not fit, the last line says how many are left
    // and a click on it shows the next page (and, at the end, jumps back to the top).
    void DrawAppointmentsDashboard(const SharedState& state, D2D1_RECT_F rect,
                                   const Settings& settings, float scale) {
        if (!textBrush_ || !mutedBrush_ || !smallTextFormat_) return;
        const float padX = FileTrayLayout::kPadX * scale;
        g_apptMoreValid.store(false, std::memory_order_relaxed);

        // Header.
        const D2D1_RECT_F headerRect = D2D1::RectF(rect.left + padX, rect.top + 18.0f * scale,
                                                   rect.right - padX, rect.top + 38.0f * scale);
        if (boldTextFormat_) {
            textBrush_->SetOpacity(0.96f);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = Loc(L"Appointments");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(),
                               headerRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Collect what is still ahead, one row per event, keyed by local day.
        struct Row {
            int64_t day;
            const CalEvent* ev;
        };
        std::vector<Row> rows;
        const int64_t nowUtc = ApptNowUtc();
        const int64_t today = ics::FloorDiv(SystemUtcToLocalNaive(nowUtc), 86400);
        if (settings.appointments && state.calendar.events) {
            for (const CalEvent& e : *state.calendar.events) {
                const bool over = e.endUtc > e.startUtc ? e.endUtc <= nowUtc
                                                         : e.startUtc < nowUtc - ics::kNowGraceSecs;
                if (over) continue;
                const int64_t startDay = ics::FloorDiv(SystemUtcToLocalNaive(e.startUtc), 86400);
                const int64_t day = std::max(startDay, today);  // running multi-day events count as today
                if (day > today + 6) continue;
                rows.push_back({day, &e});
            }
            std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if (a.day != b.day) return a.day < b.day;
                if (a.ev->allDay != b.ev->allDay) return a.ev->allDay;  // all-day first
                return a.ev->startUtc < b.ev->startUtc;
            });
        }

        const float listTop = rect.top + FileTrayLayout::kListTop * scale;
        const float listBottom = rect.bottom - FileTrayLayout::kListBottomInset * scale;

        if (rows.empty()) {
            g_apptPageStart.store(0, std::memory_order_relaxed);
            const wchar_t* hint = Loc(L"No upcoming appointments");
            mutedBrush_->SetOpacity(0.80f);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), smallTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, listTop, rect.right - padX, listBottom),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        // The list may have shrunk since the page was chosen (an event ended).
        size_t start = static_cast<size_t>(std::max(0, g_apptPageStart.load(std::memory_order_relaxed)));
        if (start >= rows.size()) {
            start = 0;
            g_apptPageStart.store(0, std::memory_order_relaxed);
        }

        const float dayH = 15.0f * scale;
        const float rowH = 19.0f * scale;
        const float moreH = 16.0f * scale;
        const float timeW = 58.0f * scale;

        float y = listTop;
        int64_t lastDay = INT64_MIN;
        size_t end = start;  // one past the last row drawn
        for (size_t i = start; i < rows.size(); ++i) {
            const bool newDay = rows[i].day != lastDay;
            const float need = (newDay ? dayH : 0.0f) + rowH;
            // Leave room for the foot line unless this is the last row of a first page.
            const bool footNeeded = (i + 1 < rows.size()) || start > 0;
            const float limit = footNeeded ? listBottom - moreH : listBottom;
            if (y + need > limit && i > start) break;

            if (newDay) {
                lastDay = rows[i].day;
                wchar_t dateBuf[96] = {};
                const SYSTEMTIME st = ApptNaiveToSystemTime(rows[i].day * 86400);
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, dateBuf,
                                ARRAYSIZE(dateBuf), nullptr);
                std::wstring label;
                if (rows[i].day == today) label = std::wstring(Loc(L"Today")) + L" \u00B7 ";
                else if (rows[i].day == today + 1) label = std::wstring(Loc(L"Tomorrow")) + L" \u00B7 ";
                label += dateBuf;
                DrawEllipsisText(label, D2D1::RectF(rect.left + padX, y, rect.right - padX, y + dayH),
                                 smallTextFormat_.Get(), mutedBrush_.Get());
                y += dayH;
            }

            const CalEvent& e = *rows[i].ev;
            const float rowTop = y;

            // Calendar colour bar.
            ComPtr<ID2D1SolidColorBrush> bar;
            target_->CreateSolidColorBrush(D2D1::ColorF(e.color, 0.95f * settingsOpacity_), &bar);
            if (bar) {
                const float bw = 3.0f * scale;
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(rect.left + padX, rowTop + 3.0f * scale,
                                                  rect.left + padX + bw, rowTop + rowH - 3.0f * scale),
                                      bw * 0.5f, bw * 0.5f), bar.Get());
            }

            const bool running = !e.allDay && e.startUtc <= nowUtc;
            std::wstring when;
            if (e.allDay) {
                when = Loc(L"All day");
            } else {
                const SYSTEMTIME stLocal = ApptNaiveToSystemTime(SystemUtcToLocalNaive(e.startUtc));
                when = FormatIslandTime(stLocal, settings.clockFollowSystem, settings.use24HourClock, false);
            }
            const float textLeft = rect.left + padX + 10.0f * scale;
            ID2D1Brush* whenBrush = running && accentBrush_ ? static_cast<ID2D1Brush*>(accentBrush_.Get())
                                                            : static_cast<ID2D1Brush*>(mutedBrush_.Get());
            mutedBrush_->SetOpacity(0.85f);
            DrawEllipsisText(when, D2D1::RectF(textLeft, rowTop, textLeft + timeW - 4.0f * scale, rowTop + rowH),
                             smallTextFormat_.Get(), whenBrush);
            textBrush_->SetOpacity(0.94f);
            DrawEllipsisText(e.title, D2D1::RectF(textLeft + timeW, rowTop, rect.right - padX, rowTop + rowH),
                             smallTextFormat_.Get(), textBrush_.Get());

            y += rowH;
            end = i + 1;
        }

        // Foot line: "+N more" while rows remain, "back to top" on the last page.
        const bool more = end < rows.size();
        if (more || start > 0) {
            std::wstring foot;
            if (more) {
                wchar_t buf[48] = {};
                swprintf_s(buf, Loc(L"+%d more"), static_cast<int>(rows.size() - end));
                foot = std::wstring(buf) + L" \u25BE";
                g_apptNextPageStart.store(static_cast<int>(end), std::memory_order_relaxed);
            } else {
                foot = std::wstring(L"\u25B4 ") + Loc(L"Back to top");
                g_apptNextPageStart.store(0, std::memory_order_relaxed);
            }

            const D2D1_RECT_F footRect = D2D1::RectF(rect.left + padX, y + 1.0f * scale,
                                                     rect.right - padX, y + 1.0f * scale + moreH);
            const bool hovered = g_hoveredApptMore.load(std::memory_order_relaxed);
            if (hovered) DrawCard(footRect, 7.0f * scale);
            mutedBrush_->SetOpacity(hovered ? 0.98f : 0.78f);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawEllipsisText(foot, footRect, smallTextFormat_.Get(), mutedBrush_.Get());
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

            // Published in content space for the click and hover tests.
            g_apptMoreL.store((footRect.left - rect.left) / scale, std::memory_order_relaxed);
            g_apptMoreR.store((footRect.right - rect.left) / scale, std::memory_order_relaxed);
            g_apptMoreT.store((footRect.top - 3.0f * scale - rect.top) / scale, std::memory_order_relaxed);
            g_apptMoreB.store((footRect.bottom + 3.0f * scale - rect.top) / scale, std::memory_order_relaxed);
            g_apptMoreValid.store(true, std::memory_order_relaxed);
        }
        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // "● in 2h 15m · 14:30 · Standup", centred, title trimmed with an ellipsis.
    void DrawNextAppointmentLine(const AppointmentInfo& appt, const Settings& settings,
                                 D2D1_RECT_F rect, float scale, float top) {
        IDWriteTextFormat* fmt = smallTextFormat_ ? smallTextFormat_.Get() : textFormat_.Get();
        if (!fmt || !dwriteFactory_ || !mutedBrush_) return;

        const SYSTEMTIME startLocal = ApptNaiveToSystemTime(SystemUtcToLocalNaive(appt.startUtc));
        const std::wstring when = FormatIslandTime(startLocal, settings.clockFollowSystem,
                                                   settings.use24HourClock, false);
        const std::wstring text = ApptUtf8ToWide(ics::RelativeText(appt.secsUntil)) +
                                  L" \u00B7 " + when + L" \u00B7 " + appt.title;

        const float lineH = 16.0f * scale;
        const float dotD = 7.0f * scale;
        const float gap = 7.0f * scale;
        const float maxW = (rect.right - rect.left) - 2.0f * 30.0f * scale - dotD - gap;
        if (maxW <= 20.0f) return;

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                                    maxW, lineH, &layout)) || !layout) {
            return;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(layout.Get(), &ellipsis)) && ellipsis) {
            const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, ellipsis.Get());
        }
        DWRITE_TEXT_METRICS tm = {};
        layout->GetMetrics(&tm);

        const float textW = std::min(tm.width, maxW);
        const float totalW = dotD + gap + textW;
        const float x0 = (rect.left + rect.right) * 0.5f - totalW * 0.5f;

        ComPtr<ID2D1SolidColorBrush> dot;
        target_->CreateSolidColorBrush(D2D1::ColorF(appt.color, 0.95f * settingsOpacity_), &dot);
        if (dot) {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + dotD * 0.5f, top + lineH * 0.5f),
                                               dotD * 0.5f, dotD * 0.5f), dot.Get());
        }
        mutedBrush_->SetOpacity(0.92f);
        target_->DrawTextLayout(D2D1::Point2F(x0 + dotD + gap, top + (lineH - tm.height) * 0.5f),
                                layout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        mutedBrush_->SetOpacity(0.75f);
    }

