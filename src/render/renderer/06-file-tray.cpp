    // Cute little bin for the File Tray header: lid with handle, tapered body, two slats.
    void DrawTrayBinButton(D2D1_RECT_F box, bool enabled, bool hovered) {
        const float s = box.right - box.left;
        const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f,
                                              (box.top + box.bottom) * 0.5f);
        const D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.38f, 0.36f, 1.0f);

        if (enabled && hovered) {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(danger, 0.20f * settingsOpacity_), &bg)) && bg) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(box, s * 0.32f, s * 0.32f), bg.Get());
            }
        }

        D2D1_COLOR_F ink = enabled ? (hovered ? danger : material_.textSecondary)
                                   : WithAlpha(material_.textSecondary, 0.35f);
        ink.a *= settingsOpacity_;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush) {
            return;
        }

        ComPtr<ID2D1StrokeStyle> round;
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);

        const float stroke = std::max(1.2f, 0.075f * s);

        // Lid and handle.
        target_->DrawLine(D2D1::Point2F(c.x - 0.31f * s, c.y - 0.17f * s),
                          D2D1::Point2F(c.x + 0.31f * s, c.y - 0.17f * s),
                          brush.Get(), stroke, round.Get());
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.30f * s,
                                          c.x + 0.11f * s, c.y - 0.17f * s),
                              0.04f * s, 0.04f * s),
            brush.Get(), stroke, round.Get());

        // Tapered body.
        ComPtr<ID2D1PathGeometry> body;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&body)) && body) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(body->Open(&sink)) && sink) {
                sink->BeginFigure(D2D1::Point2F(c.x - 0.23f * s, c.y - 0.07f * s), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(c.x - 0.18f * s, c.y + 0.30f * s));
                sink->AddLine(D2D1::Point2F(c.x + 0.18f * s, c.y + 0.30f * s));
                sink->AddLine(D2D1::Point2F(c.x + 0.23f * s, c.y - 0.07f * s));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                target_->DrawGeometry(body.Get(), brush.Get(), stroke, round.Get());
            }
        }

        // Slats.
        for (int i = -1; i <= 1; i += 2) {
            const float x = c.x + static_cast<float>(i) * 0.08f * s;
            target_->DrawLine(D2D1::Point2F(x, c.y + 0.03f * s), D2D1::Point2F(x, c.y + 0.20f * s),
                              brush.Get(), stroke * 0.85f, round.Get());
        }
    }

    // Little clipboard for the File Tray header: board, clip and two text lines.
    void DrawTrayPasteButton(D2D1_RECT_F box, bool hovered) {
        const float s = box.right - box.left;
        const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f,
                                              (box.top + box.bottom) * 0.5f);

        if (hovered) {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.accent, 0.20f * settingsOpacity_), &bg)) && bg) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(box, s * 0.32f, s * 0.32f), bg.Get());
            }
        }

        D2D1_COLOR_F ink = hovered ? material_.accent : material_.textSecondary;
        ink.a *= settingsOpacity_;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush) {
            return;
        }

        ComPtr<ID2D1StrokeStyle> round;
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);

        const float stroke = std::max(1.2f, 0.075f * s);

        // Board.
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.25f * s, c.y - 0.24f * s,
                                          c.x + 0.25f * s, c.y + 0.31f * s),
                              0.07f * s, 0.07f * s),
            brush.Get(), stroke, round.Get());

        // Clip.
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.32f * s,
                                          c.x + 0.11f * s, c.y - 0.15f * s),
                              0.05f * s, 0.05f * s),
            brush.Get());

        // Text lines.
        target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.00f * s),
                          D2D1::Point2F(c.x + 0.12f * s, c.y + 0.00f * s),
                          brush.Get(), stroke * 0.85f, round.Get());
        target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.14f * s),
                          D2D1::Point2F(c.x + 0.05f * s, c.y + 0.14f * s),
                          brush.Get(), stroke * 0.85f, round.Get());
    }

    // Small round "x" at the right end of a File Tray row.
    void DrawTrayRemoveButton(D2D1_POINT_2F c, float size, bool hovered, bool rowHovered) {
        const D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.38f, 0.36f, 1.0f);

        if (hovered) {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(danger, 0.22f * settingsOpacity_), &bg)) && bg) {
                target_->FillEllipse(D2D1::Ellipse(c, size * 0.5f, size * 0.5f), bg.Get());
            }
        }

        D2D1_COLOR_F ink = hovered ? danger
                                   : WithAlpha(material_.textSecondary, rowHovered ? 0.85f : 0.55f);
        ink.a *= settingsOpacity_;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush) {
            return;
        }

        ComPtr<ID2D1StrokeStyle> round;
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);

        const float r = size * 0.22f;
        const float stroke = std::max(1.2f, size * 0.085f);
        target_->DrawLine(D2D1::Point2F(c.x - r, c.y - r), D2D1::Point2F(c.x + r, c.y + r),
                          brush.Get(), stroke, round.Get());
        target_->DrawLine(D2D1::Point2F(c.x - r, c.y + r), D2D1::Point2F(c.x + r, c.y - r),
                          brush.Get(), stroke, round.Get());
    }

    // Round chevron button for the File Tray pager. dir: -1 = previous, +1 = next.
    void DrawTrayPagerButton(D2D1_POINT_2F c, float size, int dir, bool enabled, bool hovered) {
        if (enabled && hovered) {
            if (ID2D1SolidColorBrush* bg =
                    AcquireBrush(WithAlpha(material_.accent, 0.20f * settingsOpacity_))) {
                target_->FillEllipse(D2D1::Ellipse(c, size * 0.5f, size * 0.5f), bg);
            }
        }

        D2D1_COLOR_F ink = enabled ? (hovered ? material_.accent : material_.textSecondary)
                                   : WithAlpha(material_.textSecondary, 0.30f);
        ink.a *= settingsOpacity_;
        ID2D1SolidColorBrush* brush = AcquireBrush(ink);
        if (!brush) {
            return;
        }

        const float r = size * 0.22f;
        const float stroke = std::max(1.3f, size * 0.09f);
        const float d = static_cast<float>(dir);
        const D2D1_POINT_2F tip = D2D1::Point2F(c.x + d * r * 0.6f, c.y);
        const D2D1_POINT_2F a = D2D1::Point2F(c.x - d * r * 0.6f, c.y - r * 1.3f);
        const D2D1_POINT_2F b = D2D1::Point2F(c.x - d * r * 0.6f, c.y + r * 1.3f);
        target_->DrawLine(a, tip, brush, stroke, RoundStroke());
        target_->DrawLine(tip, b, brush, stroke, RoundStroke());
    }

    // "<  2/4  >" in the File Tray header, shown only when rows overflow.
    void DrawTrayPager(D2D1_RECT_F rect, float midY, float scale, int total, int offset) {
        const int pages = FileTrayPageCount(total);
        const int page = FileTrayCurrentPage(total, offset);
        const int action = g_hoveredFileTrayAction.load(std::memory_order_relaxed);

        const float btn = FileTrayLayout::kPagerBtnSize * scale;
        const float labelW = FileTrayLayout::kPagerLabelW * scale;
        const float right = rect.right - FileTrayLayout::PagerRightInset() * scale;

        const D2D1_POINT_2F nextC = D2D1::Point2F(right - btn * 0.5f, midY);
        const D2D1_POINT_2F prevC = D2D1::Point2F(
            right - FileTrayLayout::PagerWidth() * scale + btn * 0.5f, midY);
        DrawTrayPagerButton(prevC, btn, -1, offset > 0, action == -4);
        DrawTrayPagerButton(nextC, btn, 1, offset < FileTrayMaxScroll(total), action == -5);

        if (smallTextFormat_) {
            wchar_t label[24] = {};
            swprintf_s(label, L"%d/%d", page, pages);
            const D2D1_RECT_F labelRect = D2D1::RectF(right - btn - labelW, midY - 9.0f * scale,
                                                      right - btn, midY + 9.0f * scale);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity(0.90f);
            target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), smallTextFormat_.Get(),
                               labelRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            textBrush_->SetOpacity(0.96f);
        }
    }

    // File Tray (#33): a shelf for files dragged onto the island. Rows are built
    // from the shared DrawCard primitive so the shelf matches every other
    // dashboard, with the real Explorer icon for each file.
    void DrawFileTrayDashboard(const SharedState& state, D2D1_RECT_F rect,
                               const Settings& settings, float scale) {
        const float padX = FileTrayLayout::kPadX * scale;

        // Header: title on the left, count chip on the right.
        const D2D1_RECT_F headerRect = D2D1::RectF(rect.left + padX, rect.top + 18.0f * scale,
                                                   rect.right - padX, rect.top + 38.0f * scale);
        textBrush_->SetOpacity(0.96f);
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = Loc(L"File Tray");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(),
                               headerRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const size_t total = state.fileTrayItems.size();

        const float clearSize = FileTrayLayout::kClearBtnSize * scale;
        const float clearTop = rect.top + FileTrayLayout::kClearBtnTop * scale;
        const D2D1_RECT_F clearBox = D2D1::RectF(rect.right - padX - clearSize, clearTop,
                                                 rect.right - padX, clearTop + clearSize);
        const float pasteGap = FileTrayLayout::kPasteBtnGap * scale;
        const D2D1_RECT_F pasteBox = D2D1::RectF(clearBox.left - pasteGap - clearSize, clearTop,
                                                 clearBox.left - pasteGap, clearTop + clearSize);

        if (total > 0 && smallTextFormat_) {
            wchar_t countBuf[48] = {};
            swprintf_s(countBuf, L"%zu %s", total,
                       Loc(total == 1 ? L"item" : L"items"));
            const bool pagerOn = settings.fileTrayPager &&
                                 static_cast<int>(total) > FileTrayRestCapacity();
            const float countRight = pagerOn
                ? rect.right - (FileTrayLayout::PagerRightInset() +
                                FileTrayLayout::PagerWidth() + 8.0f) * scale
                : pasteBox.left - 8.0f * scale;
            const D2D1_RECT_F countRect = D2D1::RectF(headerRect.left, headerRect.top,
                                                      countRight, headerRect.bottom);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(countBuf, static_cast<UINT32>(wcslen(countBuf)),
                               smallTextFormat_.Get(), countRect, mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        DrawTrayPasteButton(pasteBox, g_hoveredFileTrayAction.load(std::memory_order_relaxed) == -3);
        DrawTrayBinButton(clearBox, total > 0,
                          g_hoveredFileTrayAction.load(std::memory_order_relaxed) == -2);
        if (settings.fileTrayPager && static_cast<int>(total) > FileTrayRestCapacity()) {
            DrawTrayPager(rect, (clearBox.top + clearBox.bottom) * 0.5f, scale,
                          static_cast<int>(total), FileTrayScrollOffset(static_cast<int>(total)));
        }

        const float listTop = rect.top + FileTrayLayout::kListTop * scale;
        const float listBottom = rect.bottom - FileTrayLayout::kListBottomInset * scale;

        if (state.fileTrayItems.empty()) {
            // Empty state: a dashed drop target rather than a bare label, so it
            // reads as an invitation.
            const D2D1_RECT_F dropRect = D2D1::RectF(rect.left + padX, listTop,
                                                      rect.right - padX, listBottom);
            ComPtr<ID2D1SolidColorBrush> dash;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * 1.6f), &dash)) && dash) {
                ComPtr<ID2D1StrokeStyle> dashStyle;
                D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
                props.dashStyle = D2D1_DASH_STYLE_DASH;
                props.dashCap = D2D1_CAP_STYLE_ROUND;
                d2dFactory_->CreateStrokeStyle(props, nullptr, 0, &dashStyle);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(dropRect, 12.0f * scale, 12.0f * scale),
                                              dash.Get(), 1.4f, dashStyle.Get());
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                const wchar_t* hint = Loc(L"Drag files here");
                mutedBrush_->SetOpacity(0.80f);
                target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), textFormat_.Get(),
                                   dropRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        const float rowH = FileTrayLayout::kRowHeight * scale;
        const float rowGap = FileTrayLayout::kRowGap * scale;
        const int capacity = FileTrayLayout::VisibleRowCapacity((rect.bottom - rect.top) / scale);

        // Newest first: the file just dropped is the one the user wants. Rows
        // scrolled past (scrollOffset of them) are skipped.
        const int scrollOffset = FileTrayScrollOffset(static_cast<int>(total));
        int drawn = 0;
        for (auto it = state.fileTrayItems.rbegin() + scrollOffset;
             it != state.fileTrayItems.rend() && drawn < capacity; ++it, ++drawn) {
            const FileTrayItem& item = *it;
            const float rowTop = listTop + drawn * (rowH + rowGap);
            const D2D1_RECT_F row = D2D1::RectF(rect.left + padX, rowTop, rect.right - padX, rowTop + rowH);

            const bool hovered = (g_hoveredFileTrayRow.load(std::memory_order_relaxed) == drawn);
            DrawCard(row, 9.0f * scale, hovered);

            // Real shell icon when we managed to extract one, otherwise a glyph.
            const D2D1_RECT_F iconRect = D2D1::RectF(row.left + 8.0f * scale, row.top + 6.0f * scale,
                                                      row.left + 26.0f * scale, row.bottom - 6.0f * scale);
            if (item.icon) {
                DrawBitmapPixels(*item.icon, iconRect, fileTrayIconBitmap_, fileTrayIconGeneration_, 0.98f);
            } else if (iconFormat_) {
                const wchar_t* glyph = item.isText ? L"\uE8D2"
                                                   : (item.isDirectory ? L"\uE8B7" : L"\uE7C3");
                accentBrush_->SetOpacity(0.85f);
                target_->DrawTextW(glyph, 1, iconFormat_.Get(), iconRect, accentBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                accentBrush_->SetOpacity(1.0f);
            }

            wchar_t sizeBuf[40] = {};
            if (item.isText) {
                swprintf_s(sizeBuf, L"%zu ch", item.text.size());
            } else if (item.isDirectory) {
                wcscpy_s(sizeBuf, L"—");
            } else if (item.sizeBytes >= 1073741824ull) {
                swprintf_s(sizeBuf, L"%.1f GB", static_cast<double>(item.sizeBytes) / 1073741824.0);
            } else if (item.sizeBytes >= 1048576ull) {
                swprintf_s(sizeBuf, L"%.1f MB", static_cast<double>(item.sizeBytes) / 1048576.0);
            } else if (item.sizeBytes >= 1024ull) {
                swprintf_s(sizeBuf, L"%.0f KB", static_cast<double>(item.sizeBytes) / 1024.0);
            } else {
                swprintf_s(sizeBuf, L"%llu B", static_cast<unsigned long long>(item.sizeBytes));
            }

            const float removeSize = FileTrayLayout::kRemoveBtnSize * scale;
            const float removeMargin = FileTrayLayout::kRemoveBtnMargin * scale;
            const float removeSpace = removeSize + removeMargin + 4.0f * scale;
            const bool removeHovered =
                (g_hoveredFileTrayAction.load(std::memory_order_relaxed) == drawn);
            DrawTrayRemoveButton(D2D1::Point2F(row.right - removeMargin - removeSize * 0.5f,
                                               (row.top + row.bottom) * 0.5f),
                                 removeSize, removeHovered, hovered);

            const float sizeW = 66.0f * scale;
            const D2D1_RECT_F nameRect = D2D1::RectF(row.left + 32.0f * scale, row.top,
                                                      row.right - removeSpace - sizeW - 4.0f * scale, row.bottom);
            if (smallTextFormat_) {
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.94f);
                target_->DrawTextW(item.name.c_str(), static_cast<UINT32>(item.name.size()),
                                   smallTextFormat_.Get(), nameRect, textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);

                const D2D1_RECT_F sizeRect = D2D1::RectF(row.right - removeSpace - sizeW, row.top,
                                                          row.right - removeSpace, row.bottom);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                mutedBrush_->SetOpacity(0.70f);
                target_->DrawTextW(sizeBuf, static_cast<UINT32>(wcslen(sizeBuf)),
                                   smallTextFormat_.Get(), sizeRect, mutedBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }

        // Scrollbar on the right edge, only while some rows are out of view.
        if (settings.fileTrayScroll && static_cast<int>(total) > FileTrayRestCapacity()) {
            const int totalRows = static_cast<int>(total);
            const float trackH = static_cast<float>(capacity) * rowH +
                                 static_cast<float>(capacity - 1) * rowGap;
            const float barX = rect.right - padX + FileTrayLayout::kScrollbarGap * scale;
            const float barW = FileTrayLayout::kScrollbarWidth * scale;
            const D2D1_RECT_F track = D2D1::RectF(barX, listTop, barX + barW, listTop + trackH);

            if (ID2D1SolidColorBrush* trackBrush = AcquireBrush(material_.raisedStrong)) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(track, barW * 0.5f, barW * 0.5f),
                                              trackBrush);
            }

            const float thumbH = std::max(14.0f * scale,
                                          trackH * static_cast<float>(capacity) /
                                              static_cast<float>(totalRows));
            const int maxScroll = FileTrayMaxScroll(totalRows);
            const float t = maxScroll > 0
                ? static_cast<float>(scrollOffset) / static_cast<float>(maxScroll)
                : 0.0f;
            const float thumbTop = track.top + (trackH - thumbH) * t;
            if (ID2D1SolidColorBrush* thumbBrush =
                    AcquireBrush(WithAlpha(material_.accent, 0.80f * settingsOpacity_))) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, thumbTop, track.right, thumbTop + thumbH),
                                      barW * 0.5f, barW * 0.5f),
                    thumbBrush);
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

