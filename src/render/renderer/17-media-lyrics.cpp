    // ═══ Immersive lyrics (Modules.MediaTabLyrics) ═══════════════════════════
    // Cross-fades the 380x184 media card into a lyrics-focused view: small cover
    // circle, one-line header, spring-scrolled lyric lines, accent glow behind.
    // Scrubber, controls and source dock are untouched.

    // Eased 0..1 mix between normal player and lyrics view. Always 0 when the option
    // is off, and the view resets whenever the island collapses.
    float UpdateImmersiveMix(const Settings& settings, float height, double now) {
        // Option off: the feature is fully inert, and the saved choice is cleared.
        if (!settings.mediaTabLyrics) {
            if (g_immersiveLyrics.load(std::memory_order_relaxed)) {
                g_immersiveLyrics.store(false, std::memory_order_relaxed);
            }
            immersiveMixLin_ = 0.0f;
            immersiveMixLast_ = -1.0;
            return 0.0f;
        }

        const float target = g_immersiveLyrics.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        // Collapsed: keep the choice, and pre-set the mix so the next expand opens
        // straight into the same view (no fade, no flash of the normal player).
        if (height < 52.0f) {
            immersiveMixLin_ = target;
            immersiveMixLast_ = -1.0;
            return SmoothStep01(target);
        }

        const bool fresh = immersiveMixLast_ >= 0.0 && (now - immersiveMixLast_) < 0.25;
        if (!fresh) {
            immersiveMixLin_ = target;
        } else {
            const float dt = Clamp(static_cast<float>(now - immersiveMixLast_), 0.001f, 0.05f);
            const float step = dt / static_cast<float>(ImmersiveLayout::kFadeSec);
            if (target > immersiveMixLin_) {
                immersiveMixLin_ = std::min(target, immersiveMixLin_ + step);
            } else {
                immersiveMixLin_ = std::max(target, immersiveMixLin_ - step);
            }
            if (immersiveMixLin_ != target) {
                g_layoutDirty = true;  // keep frames coming until it settles
            }
        }
        immersiveMixLast_ = now;
        return SmoothStep01(immersiveMixLin_);
    }

    bool BeginImmersiveFade(float alpha) {
        if (alpha >= 0.999f) return false;
        ID2D1Layer* layer = AcquireLayer();
        if (!layer) return false;
        target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), Clamp(alpha, 0.0f, 1.0f),
                                                  nullptr, D2D1_LAYER_OPTIONS_NONE),
                           layer);
        return true;
    }

    void EndImmersiveFade(bool began) {
        if (began) target_->PopLayer();
    }

    // Speech-bubble button. Lights up in the accent colour while the lyrics view is open.
    void DrawImmersiveLyricsButton(D2D1_RECT_F rect, float mix) {
        if (!EnsureRoundJoinStyle()) return;
        const float r = ImmersiveLayout::kBtnRadius;
        const D2D1_POINT_2F c = D2D1::Point2F(rect.right - ImmersiveLayout::kBtnInsetRight - r,
                                              rect.top + MediaLayout::kControlsY);
        const bool hovered = g_hoveredLyricsBtn.load(std::memory_order_relaxed);

        const D2D1_COLOR_F restBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.09f : 0.040f);
        const D2D1_COLOR_F onBg = D2D1::ColorF(currentAccent_.r, currentAccent_.g, currentAccent_.b,
                                               hovered ? 0.32f : 0.24f);
        if (ID2D1SolidColorBrush* bg = AcquireBrush(MixColor(restBg, onBg, mix))) {
            target_->FillEllipse(D2D1::Ellipse(c, r, r), bg);
        }

        const float restOpacity = hovered ? 0.92f : 0.62f;
        const float opacity = Clamp(restOpacity + (1.0f - restOpacity) * mix, 0.0f, 1.0f);
        const float sw = 1.4f;
        ID2D1StrokeStyle* st = roundJoinStyle_.Get();
        const D2D1_RECT_F body = D2D1::RectF(c.x - 7.0f, c.y - 7.2f, c.x + 7.0f, c.y + 3.8f);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(body, 3.0f, 3.0f);

        if (mix > 0.01f) {
            accentBrush_->SetOpacity(0.30f * mix);
            target_->FillRoundedRectangle(rr, accentBrush_.Get());
        }
        accentBrush_->SetOpacity(opacity);
        target_->DrawRoundedRectangle(rr, accentBrush_.Get(), sw, st);
        // Tail
        target_->DrawLine(D2D1::Point2F(c.x - 4.5f, body.bottom), D2D1::Point2F(c.x - 4.5f, c.y + 7.2f),
                          accentBrush_.Get(), sw, st);
        target_->DrawLine(D2D1::Point2F(c.x - 4.5f, c.y + 7.2f), D2D1::Point2F(c.x - 0.8f, body.bottom),
                          accentBrush_.Get(), sw, st);
        // Text lines
        target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 3.4f), D2D1::Point2F(c.x + 3.8f, c.y - 3.4f),
                          accentBrush_.Get(), 1.2f, st);
        target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 0.2f), D2D1::Point2F(c.x + 1.4f, c.y - 0.2f),
                          accentBrush_.Get(), 1.2f, st);
        accentBrush_->SetOpacity(1.0f);
    }

    void EnsureImmersiveFormat(const Settings& settings) {
        const float ts = Clamp(settings.textScale, 0.7f, 1.6f);
        if (immFormat_ && std::fabs(immFmtScale_ - ts) < 0.001f && immFmtFamily_ == settings.fontFamily) {
            return;
        }
        immFormat_.Reset();
        immCache_.clear();
        const wchar_t* family =
            settings.fontFamily.empty() ? L"Segoe UI Variable Display" : settings.fontFamily.c_str();
        HRESULT hr = dwriteFactory_->CreateTextFormat(
            family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f * ts, L"", &immFormat_);
        if (FAILED(hr) || !immFormat_) {
            immFormat_.Reset();
            dwriteFactory_->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 15.0f * ts, L"", &immFormat_);
        }
        if (immFormat_) {
            immFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            immFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        immFmtScale_ = ts;
        immFmtFamily_ = settings.fontFamily;
    }

    void DrawImmersiveLyricsView(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                                 double now, float mix) {
        if (mix <= 0.003f || !textFormat_ || !smallTextFormat_ || !dwriteFactory_) {
            return;
        }

        const float width = rect.right - rect.left;
        const float cx = (rect.left + rect.right) * 0.5f;
        const float zoneTop = rect.top + ImmersiveLayout::kLyricsTop;
        const float zoneBottom = rect.top + ImmersiveLayout::kLyricsBottom;
        const float cy = (zoneTop + zoneBottom) * 0.5f;
        const float availW = width - ImmersiveLayout::kPadX * 2.0f;

        const bool fresh = immLastDrawn_ >= 0.0 && (now - immLastDrawn_) < 0.25;
        const float dt = fresh ? Clamp(static_cast<float>(now - immLastDrawn_), 0.001f, 0.05f) : 0.016f;
        immLastDrawn_ = now;
        g_lastLyricsViewTime.store(now, std::memory_order_relaxed);

        ID2D1Layer* fadeLayer = AcquireLayer();
        if (fadeLayer) {
            target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), Clamp(mix, 0.0f, 1.0f),
                                                      nullptr, D2D1_LAYER_OPTIONS_NONE),
                               fadeLayer);
        }

        // ── Accent glow behind the lyrics, breathing gently with the audio level ──
        {
            float level = 0.0f;
            if (state.media.playing) {
                const size_t n = state.waveform.size();
                level = Clamp(state.waveform[(state.waveformWrite + n - 1) % n], 0.0f, 1.0f);
            }
            immGlow_ += (level - immGlow_) * (1.0f - std::exp(-dt / 0.12f));

            const D2D1_COLOR_F a = material_.accent;
            const D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
                {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
            };
            const D2D1_POINT_2F gc = D2D1::Point2F(cx, cy + 6.0f);
            const float rx = width * 0.46f;
            const float ry = 66.0f;
            if (ID2D1RadialGradientBrush* glow = UseRadialGradient(bloomGrad_, stops, 2, gc, rx, ry)) {
                const float strength = (0.10f + 0.10f * immGlow_) * Clamp(settings.accentBloom, 0.0f, 2.0f);
                glow->SetOpacity(Clamp(strength, 0.0f, 0.8f) * settingsOpacity_);
                target_->FillEllipse(D2D1::Ellipse(gc, rx, ry), glow);
            }
        }

        // ── One-line header: "Title · Artist" next to the small cover circle ──
        {
            const float textLeft = rect.left + ImmersiveLayout::kHeaderTextLeft;
            const float textRight = rect.right - ImmersiveLayout::kHeaderRightPad;
            const float midY = rect.top + ImmersiveLayout::kArtTop + ImmersiveLayout::kArtSize * 0.5f;
            std::wstring head = state.media.title.empty() ? std::wstring(Loc(L"Unknown")) : state.media.title;
            if (!state.media.artist.empty()) {
                head += L"  \u00b7  " + state.media.artist;
            }
            textBrush_->SetOpacity(0.70f);
            DrawMarqueeText(head, D2D1::RectF(textLeft, midY - 8.0f, textRight, midY + 8.0f),
                            smallTextFormat_.Get(), textBrush_.Get(), now, 30.0f,
                            marqueeImmersiveCache_);
        }

        // ── Lyrics ──
        target_->PushAxisAlignedClip(D2D1::RectF(rect.left + 18.0f, zoneTop, rect.right - 18.0f, zoneBottom),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const auto& lines = state.lyrics.lines;
        const bool usable =
            state.lyrics.hasData && state.lyrics.synced && !lines.empty() &&
            state.lyrics.matchedTitle == state.media.title &&
            state.lyrics.matchedArtist == state.media.artist;

        if (!usable) {
            immCache_.clear();
            immTrackKey_.clear();
            const wchar_t* msg = state.lyrics.fetching ? Loc(L"Fetching lyrics...")
                                 : (state.lyrics.hasData ? Loc(L"No synced lyrics")
                                                         : Loc(L"No lyrics found"));
            const float opacity = state.lyrics.fetching
                ? 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6))
                : 0.60f;
            DWRITE_TEXT_METRICS tm{};
            if (auto layout = MakeLineLayout(msg, textFormat_.Get(), availW, &tm, true)) {
                mutedBrush_->SetOpacity(opacity);
                target_->DrawTextLayout(D2D1::Point2F(cx - tm.width * 0.5f, cy - tm.height * 0.5f),
                                        layout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        } else {
            EnsureImmersiveFormat(settings);

            // Same position math as the Lyrics tab, including the sync offset.
            double posSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                posSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }
            if (state.media.playing) {
                posSec += LyricsLookaheadSec();
            }
            const int64_t posMs = std::max<int64_t>(0, static_cast<int64_t>(posSec * 1000.0));

            int lo = 0;
            int hi = static_cast<int>(lines.size());
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (lines[mid].timeMs <= posMs) lo = mid + 1; else hi = mid;
            }
            const int idx = lo - 1;  // -1 = intro, before the first line

            // Spring-eased scroll toward the active line.
            const std::wstring trackKey = state.lyrics.matchedArtist + L"|" + state.lyrics.matchedTitle;
            if (!fresh || trackKey != immTrackKey_) {
                immTrackKey_ = trackKey;
                immCache_.clear();
                immScroll_ = static_cast<float>(idx);
                immVel_ = 0.0f;
            }
            const float target = static_cast<float>(idx);
            if (std::fabs(immScroll_ - target) > 8.0f) {  // big seek: don't scroll through every line
                immScroll_ = target;
                immVel_ = 0.0f;
            }
            float remaining = dt;
            while (remaining > 0.0f) {
                const float h = std::min(remaining, 0.004f);
                immVel_ += (-ImmersiveLayout::kSpringStiffness * (immScroll_ - target) -
                            ImmersiveLayout::kSpringDamping * immVel_) * h;
                immScroll_ += immVel_ * h;
                remaining -= h;
            }
            if (std::fabs(immScroll_ - target) < 0.0015f && std::fabs(immVel_) < 0.01f) {
                immScroll_ = target;
                immVel_ = 0.0f;
            }

            auto lineFor = [&](int i) -> CollapsedLyricLine& {
                auto it = immCache_.find(i);
                if (it == immCache_.end()) {
                    if (immCache_.size() > 16) immCache_.clear();
                    CollapsedLyricLine built;
                    const std::wstring text =
                        (i >= 0 && i < static_cast<int>(lines.size()) && !lines[i].text.empty())
                            ? lines[i].text : std::wstring(L"\u266A");
                    BuildCollapsedLyricLine(built, text, immFormat_.Get());
                    it = immCache_.emplace(i, std::move(built)).first;
                }
                return it->second;
            };

            const float neighbour = settings.lyricsNeighborLinesVisible ? 0.62f : 0.40f;
            auto alphaFor = [&](float ad) {
                if (ad <= 1.0f) return 1.0f + (neighbour - 1.0f) * SmoothStep01(ad);
                // Past the neighbours: fade fully out instead of leaving ghost lines
                // crammed against the zone edge.
                return neighbour * (1.0f - SmoothStep01((ad - 1.0f) / 0.45f));
            };
            auto offsetFor = [&](float d) {
                const float ad = std::fabs(d);
                const float y = ad <= 1.0f
                    ? ImmersiveLayout::kLinePitch * ad
                    : ImmersiveLayout::kLinePitch + ImmersiveLayout::kOuterPitch * (ad - 1.0f);
                return d < 0.0f ? -y : y;
            };

            const D2D1_COLOR_F tc = textBrush_->GetColor();
            const D2D1_GRADIENT_STOP wipeStops[2] = {
                {0.0f, D2D1::ColorF(tc.r, tc.g, tc.b, 1.0f)},
                {1.0f, D2D1::ColorF(tc.r, tc.g, tc.b, 0.0f)},
            };

            D2D1_MATRIX_3X2_F oldTransform;
            target_->GetTransform(&oldTransform);

            const int first = static_cast<int>(std::floor(immScroll_)) - 3;
            for (int i = first; i <= first + 7; ++i) {
                if (i < -1 || i >= static_cast<int>(lines.size())) continue;
                const float d = static_cast<float>(i) - immScroll_;
                const float ad = std::fabs(d);
                if (ad > 1.6f) continue;
                const float alpha = alphaFor(ad);
                if (alpha <= 0.01f) continue;

                CollapsedLyricLine& L = lineFor(i);
                if (!L.layout || L.width <= 0.0f) continue;

                const float focus = 1.0f - SmoothStep01(Clamp(ad, 0.0f, 1.0f));
                float s = 0.80f + 0.28f * focus;
                if (L.width * s > availW) {
                    s = std::max(availW / L.width, s * 0.62f);
                }
                const float yc = cy + offsetFor(d);
                const D2D1_POINT_2F origin = D2D1::Point2F(cx - L.width * 0.5f, yc - L.height * 0.5f);

                target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(cx, yc)) * oldTransform);

                if (settings.karaokeLyrics && i == idx && idx >= 0) {
                    // Dim base, then a soft-edged bright wipe following the song.
                    mutedBrush_->SetOpacity(alpha * (0.62f + 0.38f * (1.0f - focus)));
                    target_->DrawTextLayout(origin, L.layout.Get(), mutedBrush_.Get(),
                                            D2D1_DRAW_TEXT_OPTIONS_NONE);

                    int64_t startMs = lines[i].timeMs;
                    int64_t endMs = startMs + 4000;
                    if (i + 1 < static_cast<int>(lines.size())) {
                        endMs = lines[i + 1].timeMs;
                    } else if (state.media.endTicks > 0) {
                        endMs = state.media.endTicks / 10000;
                    }
                    const int64_t span = endMs - startMs;
                    const float p = span > 0
                        ? Clamp(static_cast<float>(posMs - startMs) / static_cast<float>(span), 0.0f, 1.0f)
                        : 1.0f;
                    const float wipeX = origin.x - 8.0f + (L.width + 16.0f) * p;
                    if (focus > 0.01f) {
                        if (ID2D1LinearGradientBrush* wipe = UseLinearGradient(
                                immKaraokeGrad_, wipeStops, 2,
                                D2D1::Point2F(wipeX - 8.0f, yc), D2D1::Point2F(wipeX + 8.0f, yc))) {
                            wipe->SetOpacity(alpha * focus);
                            target_->DrawTextLayout(origin, L.layout.Get(), wipe,
                                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
                        }
                    }
                } else {
                    if (focus < 0.999f) {
                        mutedBrush_->SetOpacity(alpha * (1.0f - focus));
                        target_->DrawTextLayout(origin, L.layout.Get(), mutedBrush_.Get(),
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                    if (focus > 0.001f) {
                        textBrush_->SetOpacity(alpha * focus);
                        target_->DrawTextLayout(origin, L.layout.Get(), textBrush_.Get(),
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                }
            }
            target_->SetTransform(oldTransform);
        }

        target_->PopAxisAlignedClip();
        if (fadeLayer) {
            target_->PopLayer();
        }
        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Builds a wrapped lyric line for the media-tab lyrics panel, shrinking the font
    // until it fits maxHeight (down to about 10px).
    void BuildSideLyricLine(CollapsedLyricLine& out, const std::wstring& text, float width,
                            float maxHeight, const Settings& settings) {
        out.text = text;
        out.layout.Reset();
        out.width = 0.0f;
        out.height = 0.0f;
        out.isTitle = false;
        if (!dwriteFactory_ || text.empty()) {
            return;
        }

        const wchar_t* family =
            settings.fontFamily.empty() ? L"Segoe UI Variable Display" : settings.fontFamily.c_str();
        const float ts = Clamp(settings.textScale, 0.7f, 1.6f);
        const float minSize = 10.0f * ts;
        float size = 16.0f * ts;

        for (int attempt = 0; attempt < 8; ++attempt) {
            ComPtr<IDWriteTextFormat> fmt;
            HRESULT hr = dwriteFactory_->CreateTextFormat(
                family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
            if (FAILED(hr) || !fmt) {
                fmt.Reset();
                hr = dwriteFactory_->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
                if (FAILED(hr) || !fmt) {
                    return;
                }
            }
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                        fmt.Get(), width, 4096.0f, &layout)) ||
                !layout) {
                return;
            }
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            out.layout = layout;
            out.width = tm.width;
            out.height = tm.height;

            if (tm.height <= maxHeight || size <= minSize) {
                break;
            }
            size = std::max(minSize, size - 1.5f * ts);
        }
    }

    // Lyrics side panel for the expanded media tab. `area` is the strip to the right of
    // the normal player. Previous / next lines are dimmed, the current line is large and
    // wrapped, and it slides in when the line changes.
    void DrawMediaSideLyrics(const SharedState& state, D2D1_RECT_F area, const Settings& settings,
                             double now) {
        const float areaW = area.right - area.left;
        if (areaW < 8.0f || !textFormat_ || !smallTextFormat_ || !dwriteFactory_) {
            return;
        }

        // Fades in as the island widens, so nothing pops while the spring is moving.
        const float reveal = SmoothStep01((areaW - 6.0f) / (MediaLayout::kSideLyricsWidth * 0.55f));
        if (reveal <= 0.01f) {
            return;
        }

        constexpr float kPadX = 18.0f;
        constexpr float kSlidePx = 12.0f;
        constexpr double kAnimSec = 0.30;
        const float textLeft = area.left + kPadX;
        const float textW = MediaLayout::kSideLyricsWidth - kPadX * 2.0f;

        const bool fresh = sideLyricLastDrawn_ >= 0.0 && (now - sideLyricLastDrawn_) < 0.25;
        sideLyricLastDrawn_ = now;

        ID2D1Layer* fadeLayer = AcquireLayer();
        if (fadeLayer) {
            target_->PushLayer(D2D1::LayerParameters(area, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), reveal, nullptr,
                                                      D2D1_LAYER_OPTIONS_NONE),
                               fadeLayer);
        } else {
            target_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        // Thin separator between the player and the lyrics.
        if (ID2D1SolidColorBrush* divider =
                AcquireBrush(WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_))) {
            target_->FillRectangle(
                D2D1::RectF(area.left, area.top + 24.0f, area.left + 1.0f, area.bottom - 24.0f),
                divider);
        }

        const auto& lines = state.lyrics.lines;
        const bool usable =
            state.lyrics.hasData && state.lyrics.synced && !lines.empty() &&
            state.lyrics.matchedTitle == state.media.title &&
            state.lyrics.matchedArtist == state.media.artist;

        if (!usable) {
            sideLyricIdx_ = -2;
            sideLyricCur_ = CollapsedLyricLine{};
            sideLyricPrev_ = CollapsedLyricLine{};

            const wchar_t* msg =
                state.lyrics.fetching ? Loc(L"Fetching lyrics...") : Loc(L"No lyrics found");
            float opacity = 0.60f;
            if (state.lyrics.fetching) {
                opacity = 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6));
            }
            DWRITE_TEXT_METRICS tm{};
            if (auto layout = MakeLineLayout(msg, textFormat_.Get(), textW, &tm, true)) {
                mutedBrush_->SetOpacity(opacity);
                target_->DrawTextLayout(
                    D2D1::Point2F(textLeft, (area.top + area.bottom) * 0.5f - tm.height * 0.5f),
                    layout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        } else {
            // Same position math as the Lyrics tab, including the lookahead nudge.
            double posSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                posSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }
            if (state.media.playing) {
                posSec += LyricsLookaheadSec();
                            }
                            const int64_t posMs = std::max<int64_t>(0, static_cast<int64_t>(posSec * 1000.0));

            int lo = 0;
            int hi = static_cast<int>(lines.size());
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (lines[mid].timeMs <= posMs) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            const int idx = lo - 1;  // -1 = before the first line

            const float midTop = area.top + 46.0f;
            const float midH = std::max(20.0f, (area.bottom - 46.0f) - midTop);

            const bool styleChanged = std::fabs(sideLyricScale_ - settings.textScale) > 0.001f ||
                                      sideLyricFamily_ != settings.fontFamily;
            const bool trackChanged = state.media.title != sideLyricTitle_ ||
                                      state.media.artist != sideLyricArtist_;

            if (idx != sideLyricIdx_ || styleChanged || trackChanged || !sideLyricCur_.layout) {
                if (styleChanged || trackChanged || !fresh || sideLyricIdx_ == -2) {
                    sideLyricPrev_ = CollapsedLyricLine{};
                    sideLyricAnimStart_ = now - 10.0;  // snap, no animation
                } else {
                    sideLyricPrev_ = std::move(sideLyricCur_);
                    sideLyricAnimStart_ = now;
                }
                sideLyricIdx_ = idx;
                sideLyricTitle_ = state.media.title;
                sideLyricArtist_ = state.media.artist;
                sideLyricScale_ = settings.textScale;
                sideLyricFamily_ = settings.fontFamily;

                const std::wstring text = (idx >= 0 && !lines[idx].text.empty())
                                              ? lines[idx].text
                                              : std::wstring(L"\u266A");
                BuildSideLyricLine(sideLyricCur_, text, textW, midH, settings);
            }

            // Ease-out-cubic transition.
            const float t = Clamp(static_cast<float>((now - sideLyricAnimStart_) / kAnimSec), 0.0f, 1.0f);
            if (t < 1.0f) {
                g_layoutDirty = true;  // keep frames coming until it settles
            }
            const float inv = 1.0f - t;
            const float e = 1.0f - inv * inv * inv;

            auto drawDim = [&](int lineIdx, float centerY) {
                if (lineIdx < 0 || lineIdx >= static_cast<int>(lines.size())) {
                    return;
                }
                const std::wstring& s = lines[lineIdx].text;
                if (s.empty()) {
                    return;
                }
                DWRITE_TEXT_METRICS tm{};
                if (auto layout = MakeLineLayout(s, smallTextFormat_.Get(), textW, &tm, true)) {
                    mutedBrush_->SetOpacity(0.50f * e);
                    target_->DrawTextLayout(D2D1::Point2F(textLeft, centerY - tm.height * 0.5f),
                                            layout.Get(), mutedBrush_.Get(),
                                            D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
            };
            drawDim(idx - 1, area.top + 26.0f);
            drawDim(idx + 1, area.bottom - 26.0f);

            target_->PushAxisAlignedClip(
                D2D1::RectF(area.left, area.top + 40.0f, area.right, area.bottom - 40.0f),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // Outgoing line rises and fades slightly faster than the new one arrives.
            if (t < 1.0f && sideLyricPrev_.layout) {
                const CollapsedLyricLine& p = sideLyricPrev_;
                const float y = midTop + std::max(0.0f, (midH - p.height) * 0.5f) - e * kSlidePx;
                textBrush_->SetOpacity(0.90f * (1.0f - Clamp(t * 1.6f, 0.0f, 1.0f)));
                target_->DrawTextLayout(D2D1::Point2F(textLeft, y), p.layout.Get(), textBrush_.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            if (sideLyricCur_.layout) {
                const CollapsedLyricLine& c = sideLyricCur_;
                const float y = midTop + std::max(0.0f, (midH - c.height) * 0.5f) + (1.0f - e) * kSlidePx;
                textBrush_->SetOpacity(0.97f * e);
                target_->DrawTextLayout(D2D1::Point2F(textLeft, y), c.layout.Get(), textBrush_.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            target_->PopAxisAlignedClip();
        }

        if (fadeLayer) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Builds the cached layout for one lyric line.
    void BuildCollapsedLyricLine(CollapsedLyricLine& out, const std::wstring& text,
                                 IDWriteTextFormat* fmt) {
        out.text = text;
        out.layout.Reset();
        out.width = 0.0f;
        out.height = 0.0f;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                       fmt, 4096.0f, 40.0f, &out.layout)) &&
            out.layout) {
            // Other draw code flips the shared format's alignment temporarily;
            // pin it here so positioning below never depends on that.
            out.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            out.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS tm{};
            out.layout->GetMetrics(&tm);
            out.width = tm.widthIncludingTrailingWhitespace;
            out.height = tm.height;
        }
    }

    // Draws the current synced lyric line inside `area`. When the line changes,
    // the old one slides up and fades out while the new one rises in from below.
    void DrawCollapsedLyrics(const SharedState& state, D2D1_RECT_F area, double now, float alpha) {
        g_lastLyricsViewTime.store(now, std::memory_order_relaxed);
        IDWriteTextFormat* fmt = textFormat_.Get();
        if (!fmt || !dwriteFactory_ || alpha <= 0.01f) {
            return;
        }
        const float availW = area.right - area.left;
        if (availW < 40.0f) {
            return;
        }

        const auto& lines = state.lyrics.lines;

        // Lyrics only count if they belong to the track that is playing right now.
        // This also stops the previous song's lyrics showing while the lyrics
        // thread has not noticed the track change yet.
        const bool lyricsUsable =
            state.lyrics.hasData && state.lyrics.synced && !lines.empty() &&
            state.lyrics.matchedTitle == state.media.title &&
            state.lyrics.matchedArtist == state.media.artist;

        int idx = -1;  // -1 = before the first line, or no usable lyrics
        int64_t posMs = 0;
        if (lyricsUsable) {
            // Same position math as the Lyrics tab, including the lookahead nudge.
            double posSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                posSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }
            if (state.media.playing) {
                posSec += LyricsLookaheadSec();
                            }
                            posMs = std::max<int64_t>(0, static_cast<int64_t>(posSec * 1000.0));

            // Binary search for the last line whose timestamp has passed.
            int lo = 0;
            int hi = static_cast<int>(lines.size());
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (lines[mid].timeMs <= posMs) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            idx = lo - 1;
        }

        // The media title is only a fallback for when there are no usable lyrics
        // (still loading, not found, unsynced). Once lyrics are usable, the intro
        // and instrumental gaps show the note glyph, as before.
        constexpr int kTitleKey = -3;  // distinct from idx == -1 (before the first line)
        const bool showTitle = !lyricsUsable;
        const int showIdx = showTitle ? kTitleKey : idx;

        std::wstring fallback = state.media.title;
        if (fallback.empty()) fallback = state.media.sourceName;
        if (fallback.empty()) fallback = Loc(L"Media");

        // Rebuild layouts only when what is shown actually changed.
        const bool trackChanged = state.media.title != collapsedLyricTitle_ ||
                                  state.media.artist != collapsedLyricArtist_;
        const bool fallbackTextChanged = showTitle && collapsedLyricCur_.text != fallback;
        if (trackChanged || showIdx != collapsedLyricIdx_ || fallbackTextChanged) {
            if (trackChanged || collapsedLyricIdx_ == -2) {
                // New track or first draw: snap, no animation.
                collapsedLyricPrev_ = CollapsedLyricLine{};
                collapsedLyricAnimStart_ = now - 10.0;
            } else {
                collapsedLyricPrev_ = std::move(collapsedLyricCur_);
                collapsedLyricAnimStart_ = now;
            }
            collapsedLyricTitle_ = state.media.title;
            collapsedLyricArtist_ = state.media.artist;
            collapsedLyricIdx_ = showIdx;

            const std::wstring text = showTitle
                ? fallback
                : ((idx >= 0 && !lines[idx].text.empty()) ? lines[idx].text
                                                          : std::wstring(L"\u266A"));
            BuildCollapsedLyricLine(collapsedLyricCur_, text, fmt);
            collapsedLyricCur_.isTitle = showTitle;
        }
        if (!collapsedLyricCur_.layout) {
            return;
        }

        // Quick ease-out-cubic transition (~0.30s).
        constexpr double kAnimSec = 0.30;
        constexpr float kSlidePx = 13.0f;
        const float t = Clamp(static_cast<float>((now - collapsedLyricAnimStart_) / kAnimSec), 0.0f, 1.0f);
        const float inv = 1.0f - t;
        const float e = 1.0f - inv * inv * inv;

        const float cy = (area.top + area.bottom) * 0.5f;
        target_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Outgoing line: rises and fades a bit faster than the new one arrives.
        if (t < 1.0f && collapsedLyricPrev_.layout) {
            const CollapsedLyricLine& p = collapsedLyricPrev_;
            float x = area.left;
            if (p.width <= availW) {
                x = area.left + (availW - p.width) * 0.5f;
            } else if (!p.isTitle) {
                x = area.left - (p.width - availW);  // lyric rests at its panned end
            }
            const float y = cy - p.height * 0.5f - e * kSlidePx;
            textBrush_->SetOpacity(alpha * (1.0f - Clamp(t * 1.6f, 0.0f, 1.0f)) * 0.9f);
            target_->DrawTextLayout(D2D1::Point2F(x, y), p.layout.Get(), textBrush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // Incoming / current line.
        {
            const CollapsedLyricLine& c = collapsedLyricCur_;
            float x = area.left + (availW - c.width) * 0.5f;  // centred when it fits
            const float overflow = c.width - availW;
            float titleCycle = 0.0f;  // > 0 only when the title is scrolling as a marquee
            if (overflow > 0.0f) {
                if (c.isTitle) {
                    // Long title: scroll it in a loop, like the other marquees.
                    titleCycle = c.width + 38.0f;
                    x = area.left - std::fmod(static_cast<float>(now) * 30.0f, titleCycle);
                } else if (showIdx >= 0) {
                    // Long lyric: start at the left edge and pan across as it is sung.
                    const int64_t startMs = lines[showIdx].timeMs;
                    int64_t endMs = startMs + 4000;
                    if (showIdx + 1 < static_cast<int>(lines.size())) {
                        endMs = lines[showIdx + 1].timeMs;
                    } else if (state.media.endTicks > 0) {
                        endMs = state.media.endTicks / 10000;
                    }
                    const int64_t span = endMs - startMs;
                    const float prog = span > 0
                        ? Clamp(static_cast<float>(posMs - startMs) / static_cast<float>(span), 0.0f, 1.0f)
                        : 1.0f;
                    x = area.left - overflow * SmoothStep01((prog - 0.12f) / 0.70f);
                }
            }
            const float y = cy - c.height * 0.5f + (1.0f - e) * kSlidePx;
            textBrush_->SetOpacity(alpha * e * (c.isTitle ? 0.90f : 0.97f));
            target_->DrawTextLayout(D2D1::Point2F(x, y), c.layout.Get(), textBrush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (titleCycle > 0.0f) {
                target_->DrawTextLayout(D2D1::Point2F(x + titleCycle, y), c.layout.Get(),
                                        textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        target_->PopAxisAlignedClip();
        textBrush_->SetOpacity(0.96f);
    }

