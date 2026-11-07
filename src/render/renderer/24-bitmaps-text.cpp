    void DrawAlbumArt(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius = 9.0f, bool drawBadge = true) {
        ComPtr<ID2D1Geometry> mask = GetShapeGeometry(rect, radius, false);
        ID2D1Layer* layer = AcquireLayer();
        const bool roundedClip = mask && layer;
        if (roundedClip) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        if (media.art) {
            EnsureArtBitmap(media);

            D2D1_RECT_F dst = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
            target_->DrawBitmap(artBitmap_.Get(), dst, 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            accentBrush_->SetOpacity(0.24f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
            accentBrush_->SetOpacity(1.0f);
            if (media.sourceIcon) {
                D2D1_RECT_F iconRect = D2D1::RectF(rect.left + 11, rect.top + 11,
                                                  rect.right - 11, rect.bottom - 11);
                DrawBitmapPixels(*media.sourceIcon, iconRect, mediaSourceIconBitmap_,
                                 mediaSourceIconGeneration_, 0.95f);
            } else if (textFormat_) {
                // Centred in the placeholder (it used to sit in the top left corner).
                const DWRITE_TEXT_ALIGNMENT oldAlign = textFormat_->GetTextAlignment();
                const DWRITE_PARAGRAPH_ALIGNMENT oldPara = textFormat_->GetParagraphAlignment();
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target_->DrawTextW(media.sourceBadge.empty() ? L"\u25b6" : media.sourceBadge.c_str(),
                                   static_cast<UINT32>(media.sourceBadge.empty() ? 1 : media.sourceBadge.size()),
                                   textFormat_.Get(), rect, textBrush_.Get());
                textFormat_->SetTextAlignment(oldAlign);
                textFormat_->SetParagraphAlignment(oldPara);
            }
        }

        if (drawBadge && media.sourceIcon && artBadgeAlpha_ > 0.01f) {
            D2D1_RECT_F badge = D2D1::RectF(rect.right - 24, rect.bottom - 22,
                                           rect.right - 3, rect.bottom - 3);
            DrawCircularBitmapPixels(*media.sourceIcon,
                                     D2D1::Point2F((badge.left + badge.right) * 0.5f,
                                                   (badge.top + badge.bottom) * 0.5f),
                                     9.5f, mediaSourceIconBitmap_,
                                     mediaSourceIconGeneration_, 0.98f * artBadgeAlpha_);
        }

        if (roundedClip) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
    }

    void DrawBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F rect,
                          ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                          float opacity = 1.0f) {
        if (pixels.bgra.empty()) {
            return;
        }

        if (cachedGeneration != pixels.generation || !cache) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height),
                                  pixels.bgra.data(), pixels.width * 4,
                                  &props, &cache);
            cachedGeneration = pixels.generation;
        }

        if (cache) {
            target_->DrawBitmap(cache.Get(), rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    // Draws a bitmap filling a rounded-rect badge with a small inset for polish.
    void DrawRoundedBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F badge,
                                 float cornerRadius,
                                 ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                 float opacity = 1.0f) {
        if (pixels.bgra.empty()) return;

        // 2px inset so the icon has clean edges inside the badge.
        const float pad = 2.0f;
        D2D1_RECT_F iconRect = D2D1::RectF(badge.left + pad, badge.top + pad,
                                           badge.right - pad, badge.bottom - pad);
        const float innerR = std::max(0.0f, cornerRadius - pad);

        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        d2dFactory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(iconRect, innerR, innerR), &mask);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (mask && layer) {
            target_->PushLayer(
                D2D1::LayerParameters(iconRect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                layer.Get());
            DrawBitmapPixels(pixels, iconRect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, iconRect, cache, cachedGeneration, opacity);
        }
    }

    // Like DrawRoundedBitmapPixels, but center-crops (cover-fit) instead of
    // stretching, so a non-square clipboard image isn't squashed into the
    // square badge.
    void DrawCoverFitBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F badge,
                                  float cornerRadius,
                                  ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                  float opacity = 1.0f) {
        if (pixels.bgra.empty() || !pixels.width || !pixels.height) return;

        const float pad = 2.0f;
        D2D1_RECT_F iconRect = D2D1::RectF(badge.left + pad, badge.top + pad,
                                           badge.right - pad, badge.bottom - pad);
        const float innerR = std::max(0.0f, cornerRadius - pad);
        const float boxW = iconRect.right - iconRect.left;
        const float boxH = iconRect.bottom - iconRect.top;
        if (boxW <= 0.0f || boxH <= 0.0f) return;

        const float srcAspect = static_cast<float>(pixels.width) / static_cast<float>(pixels.height);
        const float boxAspect = boxW / boxH;
        float drawW = boxW;
        float drawH = boxH;
        if (srcAspect > boxAspect) {
            drawH = boxH;
            drawW = boxH * srcAspect;
        } else {
            drawW = boxW;
            drawH = boxW / srcAspect;
        }
        const float cx = (iconRect.left + iconRect.right) * 0.5f;
        const float cy = (iconRect.top + iconRect.bottom) * 0.5f;
        D2D1_RECT_F drawRect = D2D1::RectF(cx - drawW * 0.5f, cy - drawH * 0.5f,
                                           cx + drawW * 0.5f, cy + drawH * 0.5f);

        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        d2dFactory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(iconRect, innerR, innerR), &mask);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (mask && layer) {
            target_->PushLayer(
                D2D1::LayerParameters(iconRect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                layer.Get());
            DrawBitmapPixels(pixels, drawRect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, drawRect, cache, cachedGeneration, opacity);
        }
    }

    void DrawCircularBitmapPixels(const BitmapPixels& pixels, D2D1_POINT_2F center, float radius,
                                  ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                  float opacity = 1.0f) {
        if (pixels.bgra.empty()) {
            return;
        }

        D2D1_RECT_F rect = D2D1::RectF(center.x - radius, center.y - radius,
                                      center.x + radius, center.y + radius);
        ComPtr<ID2D1EllipseGeometry> ellipse;
        d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(center, radius, radius), &ellipse);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (ellipse && layer) {
            target_->PushLayer(D2D1::LayerParameters(
                                  rect, ellipse.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                              layer.Get());
            DrawBitmapPixels(pixels, rect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, rect, cache, cachedGeneration, opacity);
        }

        ComPtr<ID2D1SolidColorBrush> border;
        target_->CreateSolidColorBrush(material_.hairline, &border);
        if (border) {
            target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), border.Get(), 1.0f);
        }
    }

    void DrawMarqueeText(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                         ID2D1Brush* brush, double now, float speed, MarqueeLayoutCache& cache) {
        if (!format || !brush || text.empty()) {
            return;
        }

        const float wrapHeight = rect.bottom - rect.top;
        // Only rebuild the layout when text/format/height actually changed.
        // Scroll offset is applied via the translated draw origin below, so
        // it never invalidates the cache — this is what lets the marquee
        // scroll every frame without calling CreateTextLayout every frame.
        if (cache.text != text || cache.format != format ||
            std::fabs(cache.wrapWidth - wrapHeight) > 0.01f || !cache.layout) {
            cache.layout.Reset();
            dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                             format, 2000.0f, wrapHeight, &cache.layout);
            cache.text = text;
            cache.format = format;
            cache.wrapWidth = wrapHeight;
            cache.metrics = {};
            if (cache.layout) {
                cache.layout->GetMetrics(&cache.metrics);
            }
        }

        if (!cache.layout) {
            return;
        }

        const float available = rect.right - rect.left;

        D2D1_RECT_F clipRect = rect;
        clipRect.top -= 10.0f;
        clipRect.bottom += 10.0f;
        target_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (cache.metrics.widthIncludingTrailingWhitespace <= available) {
            target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), cache.layout.Get(), brush,
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            const float cycle = cache.metrics.widthIncludingTrailingWhitespace + 38.0f;
            const float offset = std::fmod(static_cast<float>(now) * speed, cycle);
            target_->DrawTextLayout(D2D1::Point2F(rect.left - offset, rect.top), cache.layout.Get(),
                                    brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            target_->DrawTextLayout(D2D1::Point2F(rect.left - offset + cycle, rect.top),
                                    cache.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        target_->PopAxisAlignedClip();
    }

    void DrawWaveform(const SharedState& state, D2D1_RECT_F rect) {
        const float gap = 2.5f;
        const float minBarWidth = 2.0f;
        const float availableW = rect.right - rect.left;
        size_t count = std::max<size_t>(1, static_cast<size_t>((availableW + gap) / (minBarWidth + gap)));
        count = std::min<size_t>(count, 32);

        const float barWidth = (availableW - gap * (count - 1)) / count;
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        const float maxH = (rect.bottom - rect.top) * 0.86f;

        // Use a step size of 4 samples (approx 40ms) so bars aren't identical
        const size_t step = 4;

        const float t = specT_;
        for (size_t i = 0; i < count; ++i) {
            const size_t offset = (count - i) * step;
            const size_t source = (state.waveformWrite + state.waveform.size() - offset) %
                                  state.waveform.size();
            const float amp = Clamp(state.waveform[source], 0.03f, 1.0f);
            float h = std::max(3.0f, amp * maxH);
            float glow = 0.0f;

            // Track change: domino ripple. Each bar collapses to a dot, then springs
            // back up with overshoot; the ripple runs with the skip direction.
            if (t >= 0.0f && count > 1) {
                float f = static_cast<float>(i) / static_cast<float>(count - 1);
                if (specDir_ < 0.0f) f = 1.0f - f;
                const float u = (t - 0.34f * f) / 0.66f;
                float scale = 1.0f;
                if (u < 0.0f) {
                    scale = 1.0f;
                } else if (u < 0.22f) {
                    scale = 1.0f - SkipSmooth(u / 0.22f);
                } else {
                    const float v = Clamp((u - 0.22f) / 0.78f, 0.0f, 1.0f);
                    scale = 1.0f - std::exp(-5.5f * v) * std::cos(9.5f * v);
                    glow = std::exp(-6.0f * v);
                }
                h = std::max(2.0f, h * Clamp(scale, 0.0f, 1.5f));
            }

            const float x = rect.left + i * (barWidth + gap);
            D2D1_RECT_F bar = D2D1::RectF(x, centerY - h * 0.5f, x + barWidth, centerY + h * 0.5f);
            accentBrush_->SetOpacity(Clamp(0.45f + 0.5f * amp + 0.35f * glow, 0.0f, 1.0f));
            target_->FillRoundedRectangle(D2D1::RoundedRect(bar, barWidth * 0.5f, barWidth * 0.5f),
                                         accentBrush_.Get());
        }
        accentBrush_->SetOpacity(1.0f);
    }

