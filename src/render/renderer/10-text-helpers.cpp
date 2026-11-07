    // One line of text, vertically centred in `box`, trimmed with an ellipsis.
    void DrawEllipsisText(const std::wstring& text, D2D1_RECT_F box, IDWriteTextFormat* fmt,
                          ID2D1Brush* brush) {
        if (text.empty() || !fmt || !brush || !dwriteFactory_) return;
        const float w = box.right - box.left;
        const float h = box.bottom - box.top;
        if (w <= 1.0f || h <= 1.0f) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                                    w, h, &layout)) || !layout) {
            return;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(layout.Get(), &ellipsis)) && ellipsis) {
            const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, ellipsis.Get());
        }
        target_->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

