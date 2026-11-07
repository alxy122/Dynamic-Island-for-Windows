    // ── Song-title change animation ──────────────────────────────────────────
    // Scripts whose characters shape/reorder together can't be animated one
    // glyph at a time, so those titles simply fall back to the plain redraw.
    static bool TitleNeedsShaping(wchar_t c) {
        return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0590 && c <= 0x0DFF) ||
               (c >= 0x0E00 && c <= 0x0EFF) || c == 0x200D;
    }

    // Same formula DrawMarqueeText uses for the title (speed 42), so the hand-off
    // back to the normal marquee at the end of the animation doesn't jump.
    static float TitleScrollOffset(const TitleScroll& sc, double now) {
        return sc.scrolls ? std::fmod(static_cast<float>(now) * 42.0f, sc.cycle) : 0.0f;
    }

    // allLayouts = false keeps only the glyphs visible right now (the outgoing
    // title is frozen, so nothing else of it will ever be seen).
    bool BuildTitleGlyphs(const std::wstring& text, float wrapHeight, float available, double now,
                          bool allLayouts, std::vector<TitleGlyph>& out, TitleScroll& scroll) {
        out.clear();
        scroll = TitleScroll{};
        if (!dwriteFactory_ || !textFormat_ || text.empty() || text.size() > 120) return false;

        ComPtr<IDWriteTextLayout> full;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                    textFormat_.Get(), 2000.0f, wrapHeight, &full)) || !full) {
            return false;
        }
        DWRITE_TEXT_METRICS tm = {};
        full->GetMetrics(&tm);
        scroll.width = tm.widthIncludingTrailingWhitespace;
        scroll.scrolls = scroll.width > available;
        scroll.cycle = scroll.width + 38.0f;
        scroll.offset0 = TitleScrollOffset(scroll, now);

        for (UINT32 i = 0; i < text.size();) {
            const wchar_t c = text[i];
            if (TitleNeedsShaping(c)) return false;
            const UINT32 len = (IS_HIGH_SURROGATE(c) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) ? 2 : 1;
            if (c != L' ' && c != L'\t' && c != 0x00A0 && c != 0x3000) {
                TitleGlyph g;
                g.ch0 = c;
                g.ch1 = (len == 2) ? text[i + 1] : 0;
                float px = 0.0f, py = 0.0f;
                DWRITE_HIT_TEST_METRICS htm = {};
                if (FAILED(full->HitTestTextPosition(i, FALSE, &px, &py, &htm))) return false;
                g.x = px;
                g.sx = px - scroll.offset0;
                g.eligible = (g.sx + htm.width > 0.0f) && (g.sx < available);
                if (allLayouts || g.eligible) {
                    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str() + i, len, textFormat_.Get(),
                                                                2000.0f, wrapHeight, &g.layout)) || !g.layout) {
                        return false;
                    }
                    DWRITE_TEXT_METRICS gm = {};
                    g.layout->GetMetrics(&gm);
                    g.w = gm.widthIncludingTrailingWhitespace;
                    g.h = gm.height;
                    const float hv = std::sin(static_cast<float>(i) * 12.9898f + static_cast<float>(c) * 78.233f) * 43758.5453f;
                    g.seed = hv - std::floor(hv);
                    out.push_back(std::move(g));
                }
            }
            i += len;
        }
        return !out.empty();
    }

    // Longest common subsequence of identical, visible glyphs; ties prefer the
    // pairs that travel the least. Order is preserved, so matched letters never cross.
    static void MatchTitleGlyphs(std::vector<TitleGlyph>& a, std::vector<TitleGlyph>& b) {
        const size_t n = a.size(), m = b.size();
        std::vector<int> dp((n + 1) * (m + 1), 0);
        auto at = [&](size_t i, size_t j) -> int& { return dp[i * (m + 1) + j]; };
        auto same = [&](size_t i, size_t j) {
            return a[i].eligible && b[j].eligible && a[i].ch0 == b[j].ch0 && a[i].ch1 == b[j].ch1;
        };
        auto pairScore = [&](size_t i, size_t j) {
            return 1000 - static_cast<int>(std::min(std::fabs(a[i].sx - b[j].sx), 900.0f));
        };
        for (size_t i = 1; i <= n; ++i) {
            for (size_t j = 1; j <= m; ++j) {
                int best = std::max(at(i - 1, j), at(i, j - 1));
                if (same(i - 1, j - 1)) best = std::max(best, at(i - 1, j - 1) + pairScore(i - 1, j - 1));
                at(i, j) = best;
            }
        }
        size_t i = n, j = m;
        while (i > 0 && j > 0) {
            if (same(i - 1, j - 1) && at(i, j) == at(i - 1, j - 1) + pairScore(i - 1, j - 1)) {
                a[i - 1].match = static_cast<int>(j - 1);
                b[j - 1].match = static_cast<int>(i - 1);
                --i; --j;
            } else if (at(i - 1, j) >= at(i, j - 1)) {
                --i;
            } else {
                --j;
            }
        }
    }

    void StartTitleTransition(const std::wstring& oldText, const std::wstring& newText,
                              D2D1_RECT_F rect, double now) {
        const float h = rect.bottom - rect.top;
        const float avail = rect.right - rect.left;
        if (!BuildTitleGlyphs(oldText, h, avail, now, false, titleOld_, titleOldScroll_) ||
            !BuildTitleGlyphs(newText, h, avail, now, true, titleNew_, titleNewScroll_)) {
            titleOld_.clear();
            titleNew_.clear();
            return;
        }
        MatchTitleGlyphs(titleOld_, titleNew_);
        titleAvail_ = avail;
        titleAnimStart_ = now;
        titleAnimDur_ = 1.0f;
        titleAnimActive_ = true;
    }

    // t: 0 = solid, 1 = gone. Rises, sways, stretches upward and thins out; extra
    // translucent copies trail above it so the edge looks soft instead of cut.
    // Used in reverse (t counting down, smaller amp) to make letters condense in.
    void DrawSmokeGlyph(const TitleGlyph& g, D2D1_POINT_2F origin, float t, float amp,
                        ID2D1Brush* brush, float baseOpacity) {
        if (t >= 0.999f || !g.layout) return;
        if (t <= 0.0f) {
            brush->SetOpacity(baseOpacity);
            target_->DrawTextLayout(origin, g.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            return;
        }
        D2D1_MATRIX_3X2_F oldT;
        target_->GetTransform(&oldT);

        const float seed = g.seed;
        const float rise = 16.0f * amp * std::pow(t, 1.15f);
        const float sway = std::sin(t * (4.5f + 3.0f * seed) + seed * 6.2832f) * 4.5f * amp * std::sqrt(t);
        const float angle = (seed - 0.5f) * 50.0f * t * amp;
        const float sx = 1.0f + 0.55f * t * amp;
        const float sy = 1.0f + 1.40f * t * amp;
        const float alpha = std::pow(1.0f - t, 1.6f) * baseOpacity;
        const float soft = SkipSmooth(t / 0.25f);  // soft copies fade in as it starts to dissolve
        const float spread = t * 3.2f * amp;
        const D2D1_POINT_2F pivot = D2D1::Point2F(origin.x + g.w * 0.5f, origin.y + g.h * 0.82f);

        for (int k = 0; k < 4; ++k) {
            const float dx = (k == 1) ? -spread * 0.9f : (k == 3 ? spread * 0.9f : 0.0f);
            const float dy = -static_cast<float>(k) * spread * 0.7f;
            const float a = (k == 0) ? alpha * (1.0f - 0.55f * soft) : alpha * 0.28f * soft;
            if (a <= 0.003f) continue;
            brush->SetOpacity(Clamp(a, 0.0f, 1.0f));
            target_->SetTransform(
                D2D1::Matrix3x2F::Scale(sx, sy, pivot) *
                D2D1::Matrix3x2F::Rotation(angle * (1.0f + 0.25f * static_cast<float>(k)), pivot) *
                D2D1::Matrix3x2F::Translation(sway + dx, -rise + dy) * oldT);
            target_->DrawTextLayout(origin, g.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        target_->SetTransform(oldT);
        brush->SetOpacity(baseOpacity);
    }

    // Returns true when it drew the title itself (animation running); false means
    // the caller should draw the title the normal way.
    bool DrawTitleTransition(const std::wstring& text, D2D1_RECT_F rect, ID2D1Brush* brush,
                             double now, bool realTitle) {
        // The title is only drawn while the expanded media card is visible, so a
        // long gap means the old title was never on screen: just snap.
        const bool recentlyDrawn = titleLastDrawn_ >= 0.0 && (now - titleLastDrawn_) < 0.25;
        titleLastDrawn_ = now;

        if (text != titleShown_) {
            const std::wstring oldText = titleShown_;
            const bool oldWasReal = !titleShownPlaceholder_;
            titleShown_ = text;
            titleShownPlaceholder_ = !realTitle;
            titleAnimActive_ = false;
            titleOld_.clear();
            titleNew_.clear();
            if (expandedAnim_ && recentlyDrawn && realTitle && oldWasReal && !oldText.empty()) {
                StartTitleTransition(oldText, text, rect, now);
            }
        } else if (titleAnimActive_ && (!recentlyDrawn || !expandedAnim_)) {
            titleAnimActive_ = false;
            titleOld_.clear();
            titleNew_.clear();
        }

        if (!titleAnimActive_) return false;

        const float time = static_cast<float>(now - titleAnimStart_);
        if (time >= titleAnimDur_) {
            titleAnimActive_ = false;
            titleOld_.clear();
            titleNew_.clear();
            return false;
        }

        const float baseOpacity = brush->GetOpacity();
        const float avail = std::max(titleAvail_, 1.0f);
        const float newOffset = TitleScrollOffset(titleNewScroll_, now);
        target_->PushAxisAlignedClip(
            D2D1::RectF(rect.left - 4.0f, rect.top - 20.0f, rect.right + 4.0f, rect.bottom + 6.0f),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Old letters with no partner: dissolve into smoke, left to right. The old
        // title is frozen at the spot where it was when the track changed.
        const float oldSpan = std::max(std::min(titleOldScroll_.width, avail), 1.0f);
        for (const TitleGlyph& g : titleOld_) {
            if (g.match >= 0 || !g.eligible) continue;
            const float f = Clamp(g.sx / oldSpan, 0.0f, 1.0f);
            const float t = Clamp((time - 0.22f * f) / 0.60f, 0.0f, 1.0f);
            DrawSmokeGlyph(g, D2D1::Point2F(rect.left + g.sx, rect.top), t, 1.0f, brush, baseOpacity);
        }

        // Letters present in both titles: glide to their new position (which keeps
        // scrolling if the new title is a marquee).
        brush->SetOpacity(baseOpacity);
        for (const TitleGlyph& g : titleOld_) {
            if (g.match < 0) continue;
            const TitleGlyph& n = titleNew_[static_cast<size_t>(g.match)];
            const float e = SkipEaseInOut((time - 0.04f) / 0.55f);
            const float targetX = n.x - newOffset;
            const float x = g.sx + (targetX - g.sx) * e;
            const float lift = -1.5f * std::sin(3.14159265f * e);
            target_->DrawTextLayout(D2D1::Point2F(rect.left + x, rect.top + lift), g.layout.Get(), brush,
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // New letters with no partner: condense out of the smoke. A scrolling title
        // is drawn at its live marquee position, including the wrapped copy.
        const float newSpan = std::max(std::min(titleNewScroll_.width, avail), 1.0f);
        for (const TitleGlyph& g : titleNew_) {
            if (g.match >= 0) continue;
            const float xs = g.x - newOffset;
            const float f = Clamp(xs / newSpan, 0.0f, 1.0f);
            const float u = Clamp((time - 0.28f - 0.20f * f) / 0.50f, 0.0f, 1.0f);
            if (xs + g.w > 0.0f && xs < avail) {
                DrawSmokeGlyph(g, D2D1::Point2F(rect.left + xs, rect.top), 1.0f - u, 0.45f, brush, baseOpacity);
            }
            if (titleNewScroll_.scrolls) {
                const float xs2 = xs + titleNewScroll_.cycle;
                if (xs2 + g.w > 0.0f && xs2 < avail) {
                    DrawSmokeGlyph(g, D2D1::Point2F(rect.left + xs2, rect.top), 1.0f - u, 0.45f, brush, baseOpacity);
                }
            }
        }

        brush->SetOpacity(baseOpacity);
        target_->PopAxisAlignedClip();
        g_layoutDirty = true;
        return true;
    }

    // ── Progress bar styles (Wavy / Squiggle / Bar) ──────────────────────────
    // Slim is the original bar and keeps its own code path in DrawMedia. The
    // hit test is untouched: every shape here stays inside the scrubber's hit area.
    void DrawStyledProgress(ProgressStyle style, float barLeft, float barRight, float cy,
                            float progress, bool dragging, bool playing, double now) {
        const float width = barRight - barLeft;
        if (width <= 8.0f) return;
        progress = Clamp(progress, 0.0f, 1.0f);
        const float thumbX = barLeft + width * progress;

        ComPtr<ID2D1SolidColorBrush> accent, track;
        if (FAILED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 1.0f), &accent)) || !accent) return;
        if (FAILED(target_->CreateSolidColorBrush(material_.raisedStrong, &track)) || !track) return;
        EnsureRoundJoinStyle();

        // Wave strength eases toward 0 when paused and back to 1 when playing.
        {
            const bool fresh = progressLastTime_ >= 0.0 && (now - progressLastTime_) < 0.25;
            const float target = playing ? 1.0f : 0.0f;
            if (!fresh) {
                progressAmp_ = target;
            } else {
                const float dt = static_cast<float>(now - progressLastTime_);
                progressAmp_ += (target - progressAmp_) * (1.0f - std::exp(-dt / 0.16f));
                if (std::fabs(target - progressAmp_) < 0.004f) {
                    progressAmp_ = target;
                } else {
                    g_layoutDirty = true;
                }
            }
            progressLastTime_ = now;
        }
        const float amp = progressAmp_;

        auto drawEndDot = [&](float x) {
            ComPtr<ID2D1SolidColorBrush> dot;
            if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.9f), &dot)) && dot) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 1.7f, 1.7f), dot.Get());
            }
        };

        if (style == ProgressStyle::Bar) {
            // Thick rounded bar split by a vertical line thumb, with a dot at the end.
            const float half = dragging ? 4.5f : 4.0f;
            const float gap = dragging ? 5.0f : 4.0f;
            const float fillRight = thumbX - gap;
            const float trackLeft = thumbX + gap;
            if (fillRight - barLeft > 1.0f) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(barLeft, cy - half, fillRight, cy + half), half, half),
                    accent.Get());
            }
            if (barRight - trackLeft > 1.0f) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(trackLeft, cy - half, barRight, cy + half), half, half),
                    track.Get());
                drawEndDot(barRight - half);
            }
            const float tw = dragging ? 4.0f : 3.0f;
            const float th = dragging ? 24.0f : 20.0f;
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX - tw * 0.5f, cy - th * 0.5f, thumbX + tw * 0.5f, cy + th * 0.5f),
                                  tw * 0.5f, tw * 0.5f),
                accent.Get());
            return;
        }

        // Wavy and Squiggle: a travelling sine wave up to the thumb, a thin flat
        // line after it. Squiggle is shorter, shallower and about half the speed.
        const bool wavy = (style == ProgressStyle::Wavy);
        const float A = (wavy ? 3.2f : 1.8f) * amp;
        const float lambda = wavy ? 26.0f : 15.0f;
        const double freq = wavy ? 0.9 : 0.45;
        const float strokeW = wavy ? 3.4f : 3.0f;
        const float k = 6.2831853f / lambda;
        const float phase = static_cast<float>(std::fmod(now * freq, 1.0) * 6.2831853);
        const float endTaper = wavy ? 16.0f : 10.0f;  // flattens into the thumb

        if (thumbX - barLeft > 1.5f) {
            std::vector<D2D1_POINT_2F> pts;
            pts.reserve(static_cast<size_t>((thumbX - barLeft) / 1.25f) + 3);
            for (float x = barLeft;; x += 1.25f) {
                const float xx = std::min(x, thumbX);
                const float taper = std::min(SkipSmooth((xx - barLeft) / 10.0f),
                                             SkipSmooth((thumbX - xx) / endTaper));
                pts.push_back(D2D1::Point2F(xx, cy + A * taper * std::sin(k * (xx - barLeft) - phase)));
                if (x >= thumbX) break;
            }
            ComPtr<ID2D1PathGeometry> path;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&path)) && path) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(path->Open(&sink)) && sink) {
                    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    if (pts.size() > 1) sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        target_->DrawGeometry(path.Get(), accent.Get(), strokeW, roundJoinStyle_.Get());
                    }
                }
            }
        }

        const float thumbR = dragging ? 7.0f : 5.5f;
        const float trackStart = thumbX + (wavy ? thumbR + 3.0f : 4.0f);
        if (barRight - trackStart > 2.0f) {
            const float tw = wavy ? 2.2f : 2.8f;
            target_->DrawLine(D2D1::Point2F(trackStart, cy), D2D1::Point2F(barRight - 2.0f, cy),
                              track.Get(), tw, roundJoinStyle_.Get());
            if (wavy) drawEndDot(barRight - 1.7f);
        }

        if (wavy) {
            if (dragging) {
                ComPtr<ID2D1SolidColorBrush> halo;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.22f), &halo)) && halo) {
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbR * 2.0f, thumbR * 2.0f),
                                         halo.Get());
                }
            }
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbR, thumbR), accent.Get());
        } else {
            const float tw = dragging ? 4.0f : 3.0f;
            const float th = dragging ? 22.0f : 18.0f;
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX - tw * 0.5f, cy - th * 0.5f, thumbX + tw * 0.5f, cy + th * 0.5f),
                                  tw * 0.5f, tw * 0.5f),
                accent.Get());
        }
    }

    // ── Artist / album: gentle swap ──────────────────────────────────────────
    // The old line drifts up and fades while the new one rises into place. Only
    // runs when the line was already on screen; otherwise the text just appears.
    void DrawSwapMarquee(SwapTextState& st, const std::wstring& text, D2D1_RECT_F rect,
                         IDWriteTextFormat* format, ID2D1Brush* brush, double now, float speed,
                         MarqueeLayoutCache& cache, float delay) {
        constexpr float kDur = 0.38f;
        const bool recentlyDrawn = st.lastDrawn >= 0.0 && (now - st.lastDrawn) < 0.25;
        st.lastDrawn = now;

        if (text != st.shown) {
            if (recentlyDrawn && expandedAnim_) {
                st.prev = st.shown;
                st.start = now;
            } else {
                st.prev.clear();
                st.start = -1.0;
            }
            st.shown = text;
        } else if (st.start >= 0.0 && !recentlyDrawn) {
            st.start = -1.0;
        }

        const float elapsed = st.start >= 0.0 ? static_cast<float>(now - st.start) : -1.0f;
        if (elapsed < 0.0f || elapsed >= delay + kDur) {
            st.start = -1.0;
            DrawMarqueeText(text, rect, format, brush, now, speed, cache);
            return;
        }

        const float u = Clamp((elapsed - delay) / kDur, 0.0f, 1.0f);
        const float e = SkipEaseInOut(u);
        const float base = brush->GetOpacity();
        const float dy = 5.0f;
        D2D1_RECT_F up = rect;
        up.top -= dy * e;
        up.bottom -= dy * e;
        D2D1_RECT_F down = rect;
        down.top += dy * (1.0f - e);
        down.bottom += dy * (1.0f - e);

        brush->SetOpacity(base * (1.0f - SkipSmooth(u * 1.4f)));
        DrawMarqueeText(st.prev, up, format, brush, now, speed, st.prevCache);
        brush->SetOpacity(base * SkipSmooth((u - 0.25f) / 0.75f));
        DrawMarqueeText(text, down, format, brush, now, speed, cache);
        brush->SetOpacity(base);
        g_layoutDirty = true;
    }

