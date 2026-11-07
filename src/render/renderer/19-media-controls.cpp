    void UpdateMediaButtonAnimations(double now, bool playing) {
        float dt = 0.016f;
        if (lastMediaBtnTime_ > 0.0) {
            dt = static_cast<float>(std::max(0.001, std::min(now - lastMediaBtnTime_, 0.05)));
        }
        lastMediaBtnTime_ = now;

        const int pressedCmd = g_pressedMediaButton.load();
        bool isStillAnimating = false;

        for (int i = 0; i < 3; ++i) {
            const float target = (pressedCmd == i) ? 1.0f : 0.0f;
            const float tau = (target > mediaBtnPress_[i]) ? 0.025f : 0.075f;
            const float k = 1.0f - std::exp(-dt / tau);
            mediaBtnPress_[i] += (target - mediaBtnPress_[i]) * k;

            if (std::abs(mediaBtnPress_[i] - target) < 0.002f) {
                mediaBtnPress_[i] = target;
            } else {
                isStillAnimating = true;
            }
        }

        // Play/pause morph. Animates only if the button was on screen a moment
        // ago; otherwise (island was collapsed, first draw) it snaps to the state.
        {
            constexpr double kMorphDuration = 0.34;
            const bool recentlyDrawn = playMorphLastDrawn_ >= 0.0 && (now - playMorphLastDrawn_) < 0.25;
            playMorphLastDrawn_ = now;
            const float target = playing ? 0.0f : 1.0f;
            if (!playMorphInit_ || !recentlyDrawn || !expandedAnim_) {
                playMorphInit_ = true;
                playMorph_ = playMorphFrom_ = playMorphTarget_ = target;
                playMorphStart_ = -1.0;
            } else if (target != playMorphTarget_) {
                playMorphFrom_ = playMorph_;
                playMorphTarget_ = target;
                playMorphStart_ = now;
            }
            if (playMorphStart_ >= 0.0) {
                const float p = static_cast<float>((now - playMorphStart_) / kMorphDuration);
                if (p >= 1.0f) {
                    playMorph_ = playMorphTarget_;
                    playMorphStart_ = -1.0;
                } else {
                    playMorph_ = playMorphFrom_ + (playMorphTarget_ - playMorphFrom_) * SkipEaseInOut(p);
                    isStillAnimating = true;
                }
            }
        }

        // Depth-swap: start on a new click, then advance with wall-clock time.
        constexpr double kSkipDuration = 0.46;
        const unsigned trig[2] = {g_skipTriggerPrev.load(), g_skipTriggerNext.load()};
        for (int i = 0; i < 2; ++i) {
            if (trig[i] != skipSeenTrigger_[i]) {
                skipSeenTrigger_[i] = trig[i];
                lastSkipClickAt_ = now;
                lastSkipDir_ = (i == 0) ? -1.0f : 1.0f;
                // Ignore a re-click while the swap is still in its first half so
                // the motion never snaps back; the command is sent regardless.
                if (expandedAnim_ && (skipAnimStart_[i] < 0.0 || (now - skipAnimStart_[i]) / kSkipDuration >= 0.5)) {
                    skipAnimStart_[i] = now;
                }
            }
            if (skipAnimStart_[i] >= 0.0) {
                const double p = (now - skipAnimStart_[i]) / kSkipDuration;
                if (p >= 1.0) {
                    skipAnimStart_[i] = -1.0;
                    skipProgress_[i] = -1.0f;
                } else {
                    skipProgress_[i] = static_cast<float>(p);
                    isStillAnimating = true;
                }
            } else {
                skipProgress_[i] = -1.0f;
            }
        }

        if (isStillAnimating) {
            g_layoutDirty = true;
        }
    }

    // ── Skip icon: two rounded triangles + "depth swap" animation ────────────
    static constexpr float kSkipTriW = 0.626f;   // triangle width / height
    static constexpr float kSkipPitch = 0.451f;  // front-left minus back-left, / height

    HRESULT BuildRoundedTriangle(float x0, float w, float H, ComPtr<ID2D1PathGeometry>& out) {
        const D2D1_POINT_2F v[3] = {
            D2D1::Point2F(x0, -0.5f * H), D2D1::Point2F(x0, 0.5f * H), D2D1::Point2F(x0 + w, 0.0f)};
        const float d[3] = {0.11f * H, 0.11f * H, 0.14f * H};
        D2D1_POINT_2F a[3], b[3];
        for (int i = 0; i < 3; ++i) {
            const D2D1_POINT_2F p = v[(i + 2) % 3], n = v[(i + 1) % 3];
            float px = p.x - v[i].x, py = p.y - v[i].y;
            float nx = n.x - v[i].x, ny = n.y - v[i].y;
            const float pl = std::sqrt(px * px + py * py), nl = std::sqrt(nx * nx + ny * ny);
            a[i] = D2D1::Point2F(v[i].x + px / pl * d[i], v[i].y + py / pl * d[i]);
            b[i] = D2D1::Point2F(v[i].x + nx / nl * d[i], v[i].y + ny / nl * d[i]);
        }
        ComPtr<ID2D1PathGeometry> geom;
        HRESULT hr = d2dFactory_->CreatePathGeometry(&geom);
        if (FAILED(hr) || !geom) return FAILED(hr) ? hr : E_FAIL;
        ComPtr<ID2D1GeometrySink> sink;
        hr = geom->Open(&sink);
        if (FAILED(hr) || !sink) return FAILED(hr) ? hr : E_FAIL;
        sink->BeginFigure(b[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(a[1]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[1], b[1]));
        sink->AddLine(a[2]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[2], b[2]));
        sink->AddLine(a[0]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[0], b[0]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        hr = sink->Close();
        if (FAILED(hr)) return hr;
        out = geom;
        return S_OK;
    }

    bool EnsureSkipGeometry() {
        if (skipTriFull_ && skipTriNotch_) return true;
        if (!d2dFactory_) return false;
        const float H = MediaLayout::kNavButtonRadius * 0.78f;
        const float w = kSkipTriW * H, pitch = kSkipPitch * H;

        ComPtr<ID2D1PathGeometry> full, back, front;
        if (FAILED(BuildRoundedTriangle(0.0f, w, H, full))) return false;
        if (FAILED(BuildRoundedTriangle(0.0f, w, H, back))) return false;
        if (FAILED(BuildRoundedTriangle(pitch, w, H, front))) return false;

        // The back triangle is cut by a slightly enlarged copy of the front one,
        // leaving a clean gap that works on translucent backgrounds too.
        ComPtr<ID2D1TransformedGeometry> grown;
        if (FAILED(d2dFactory_->CreateTransformedGeometry(
                front.Get(), D2D1::Matrix3x2F::Scale(1.16f, 1.16f, D2D1::Point2F(pitch + w / 3.0f, 0.0f)),
                &grown)) || !grown) return false;

        ComPtr<ID2D1PathGeometry> notch;
        if (FAILED(d2dFactory_->CreatePathGeometry(&notch)) || !notch) return false;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(notch->Open(&sink)) || !sink) return false;
        if (FAILED(back->CombineWithGeometry(grown.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.05f, sink.Get()))) {
            return false;
        }
        if (FAILED(sink->Close())) return false;

        skipTriFull_ = full;
        skipTriNotch_ = notch;
        return true;
    }

    static float SkipSmooth(float x) {            // smoothstep 0..1
        x = Clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
    static float SkipEaseInOut(float x) {         // easeInOutCubic
        x = Clamp(x, 0.0f, 1.0f);
        return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
    }

    // forward = true -> "next" (>>), false -> "previous" (<<, mirrored).
    // p < 0 = resting state, otherwise animation progress 0..1.
    void DrawSkipIcon(D2D1_POINT_2F center, bool forward, float p, float opacity, float press) {
        if (!EnsureSkipGeometry()) return;

        const float H = MediaLayout::kNavButtonRadius * 0.78f;
        const float w = kSkipTriW * H, pitch = kSkipPitch * H;
        const float pairLeft = -0.5f * (pitch + w);

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        const float sf = 1.0f - 0.13f * press;
        const D2D1_MATRIX_3X2_F base =
            D2D1::Matrix3x2F::Scale(forward ? 1.0f : -1.0f, 1.0f) *
            D2D1::Matrix3x2F::Translation(center.x, center.y) *
            D2D1::Matrix3x2F::Scale(sf, sf, center) * oldTransform;

        auto fill = [&](ID2D1Geometry* g, float x, float scale, float alpha) {
            if (alpha <= 0.003f) return;
            const D2D1_MATRIX_3X2_F m =
                D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(w / 3.0f, 0.0f)) *
                D2D1::Matrix3x2F::Translation(x, 0.0f) * base;
            target_->SetTransform(m);
            accentBrush_->SetOpacity(Clamp(opacity * alpha, 0.0f, 1.0f));
            target_->FillGeometry(g, accentBrush_.Get());
        };

        if (p < 0.0f) {
            fill(skipTriNotch_.Get(), pairLeft, 1.0f, 1.0f);
            fill(skipTriFull_.Get(), pairLeft + pitch, 1.0f, 1.0f);
        } else {
            const float e = SkipEaseInOut(p);

            // B (front): pushed forward, recedes and fades out.
            const float bFade = 1.0f - SkipSmooth(p / 0.75f);
            fill(skipTriFull_.Get(), pairLeft + pitch + pitch * 1.15f * e, 1.0f - 0.2f * e, bFade);

            // A (back): glides into B's slot; its notch dissolves as it becomes the front.
            const float s = SkipSmooth((p - 0.1f) / 0.8f);
            fill(skipTriNotch_.Get(), pairLeft + pitch * e, 1.0f, 1.0f - s);
            fill(skipTriFull_.Get(), pairLeft + pitch * e, 1.0f, s);

            // A' (new back): grows in behind, where A used to be.
            const float cIn = SkipSmooth((p - 0.3f) / 0.7f);
            fill(skipTriNotch_.Get(), pairLeft - pitch * 0.45f * (1.0f - cIn), 0.7f + 0.3f * cIn, cIn);
        }

        target_->SetTransform(oldTransform);
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawMediaControls(bool playing, D2D1_POINT_2F prev, D2D1_POINT_2F play, D2D1_POINT_2F next, double now) {
        UpdateMediaButtonAnimations(now, playing);
        DrawMediaButton(prev, MediaLayout::kNavButtonRadius, 0, false);
        DrawMediaButton(play, MediaLayout::kPlayButtonRadius, playing ? 1 : 2, true);
        DrawMediaButton(next, MediaLayout::kNavButtonRadius, 3, false);
    }

    void DrawMediaButton(D2D1_POINT_2F center, float radius, int kind, bool primary) {
        int buttonCmd = (kind == 0) ? 0 : ((kind == 1 || kind == 2) ? 1 : 2);
        bool isHovered = (g_hoveredMediaButton.load() == buttonCmd);
        const float press = (buttonCmd >= 0 && buttonCmd < 3) ? mediaBtnPress_[buttonCmd] : 0.0f;

        const float r = radius * (1.0f - 0.13f * press);

        const D2D1_COLOR_F restingBg = D2D1::ColorF(
            1.0f, 1.0f, 1.0f,
            primary ? (isHovered ? 0.16f : 0.080f) : (isHovered ? 0.09f : 0.040f)
        );
        const D2D1_COLOR_F pressedBg = D2D1::ColorF(
            currentAccent_.r, currentAccent_.g, currentAccent_.b,
            primary ? 0.28f : 0.18f
        );

        const D2D1_COLOR_F currentBg = D2D1::ColorF(
            restingBg.r + (pressedBg.r - restingBg.r) * press,
            restingBg.g + (pressedBg.g - restingBg.g) * press,
            restingBg.b + (pressedBg.b - restingBg.b) * press,
            restingBg.a + (pressedBg.a - restingBg.a) * press
        );

        if (ID2D1SolidColorBrush* bg = AcquireBrush(currentBg)) {
            target_->FillEllipse(D2D1::Ellipse(center, r, r), bg);
        }

        const float baseOpacity = primary ? (isHovered ? 1.0f : 0.88f) : (isHovered ? 0.92f : 0.62f);
        const float iconOpacity = Clamp(baseOpacity + (1.0f - baseOpacity) * press, 0.0f, 1.0f);
        accentBrush_->SetOpacity(iconOpacity);

        if (kind == 0 || kind == 3) {
            DrawSkipIcon(center, kind == 3, skipProgress_[kind == 3 ? 1 : 0], iconOpacity, press);
            accentBrush_->SetOpacity(1.0f);
            return;
        }

        if (kind == 1 || kind == 2) {
            DrawPlayPauseIcon(center, iconOpacity, press);
            accentBrush_->SetOpacity(1.0f);
            return;
        }

        const wchar_t* glyph = nullptr;
        IDWriteTextFormat* fmt = nullptr;
        if (usingFluentIcons_) {
            if (kind == 0) {
                glyph = L"\uF8AC";
                fmt = mediaNavIconFormat_.Get();
            } else if (kind == 1) {
                glyph = L"\uF8AE";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 2) {
                glyph = L"\uF5B0";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 3) {
                glyph = L"\uF8AD";
                fmt = mediaNavIconFormat_.Get();
            }
        } else {
            if (kind == 0) {
                glyph = L"\uE100";
                fmt = mediaNavIconFormat_.Get();
            } else if (kind == 1) {
                glyph = L"\uE103";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 2) {
                glyph = L"\uE102";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 3) {
                glyph = L"\uE101";
                fmt = mediaNavIconFormat_.Get();
            }
        }

        if (glyph && fmt) {
            const float offsetX = (kind == 2) ? 1.0f : 0.0f;
            D2D1_RECT_F glyphRect = D2D1::RectF(
                center.x - radius + offsetX,
                center.y - radius,
                center.x + radius + offsetX,
                center.y + radius
            );

            if (press > 0.001f) {
                D2D1_MATRIX_3X2_F oldTransform;
                target_->GetTransform(&oldTransform);
                const float scaleFactor = 1.0f - 0.13f * press;
                target_->SetTransform(D2D1::Matrix3x2F::Scale(scaleFactor, scaleFactor, center) * oldTransform);
                target_->DrawTextW(glyph, 1, fmt, glyphRect, accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                target_->SetTransform(oldTransform);
            } else {
                target_->DrawTextW(glyph, 1, fmt, glyphRect, accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        accentBrush_->SetOpacity(1.0f);
    }

    // ── Play/pause morph ─────────────────────────────────────────────────────
    // Both icons are two quads with matching vertices: the pause bars become the
    // two halves of the play triangle (the right half collapses into the tip).
    static constexpr float kPlayPauseCorner = 1.5f;

    struct PlayPauseShapes {
        D2D1_POINT_2F pause[8];
        D2D1_POINT_2F play[8];
    };

    static const PlayPauseShapes& GetPlayPauseShapes() {
        static const PlayPauseShapes shapes = [] {
            PlayPauseShapes s = {};
            const float rr = kPlayPauseCorner;
            const float bx0 = 2.1f, bx1 = 6.5f, by = 7.5f;
            // Left bar, then right bar: TL, TR, BR, BL.
            s.pause[0] = D2D1::Point2F(-bx1 + rr, -by + rr);
            s.pause[1] = D2D1::Point2F(-bx0 - rr, -by + rr);
            s.pause[2] = D2D1::Point2F(-bx0 - rr,  by - rr);
            s.pause[3] = D2D1::Point2F(-bx1 + rr,  by - rr);
            s.pause[4] = D2D1::Point2F( bx0 + rr, -by + rr);
            s.pause[5] = D2D1::Point2F( bx1 - rr, -by + rr);
            s.pause[6] = D2D1::Point2F( bx1 - rr,  by - rr);
            s.pause[7] = D2D1::Point2F( bx0 + rr,  by - rr);

            // Triangle (optically centred), split by a shared edge T-U.
            const D2D1_POINT_2F A = D2D1::Point2F(-5.5f, -7.5f);
            const D2D1_POINT_2F B = D2D1::Point2F(-5.5f,  7.5f);
            const D2D1_POINT_2F C = D2D1::Point2F( 7.5f,  0.0f);
            const D2D1_POINT_2F T = D2D1::Point2F( 1.0f, -3.75f);
            const D2D1_POINT_2F U = D2D1::Point2F( 1.0f,  3.75f);
            const float la = std::hypot(B.x - C.x, B.y - C.y);
            const float lb = std::hypot(C.x - A.x, C.y - A.y);
            const float lc = std::hypot(A.x - B.x, A.y - B.y);
            const float per = la + lb + lc;
            const float ix = (la * A.x + lb * B.x + lc * C.x) / per;
            const float iy = (la * A.y + lb * B.y + lc * C.y) / per;
            const float area = 0.5f * std::fabs((B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x));
            const float inr = 2.0f * area / per;
            const float k = (inr - rr) / inr;
            auto shrink = [&](D2D1_POINT_2F p) {
                return D2D1::Point2F(ix + k * (p.x - ix), iy + k * (p.y - iy));
            };
            s.play[0] = shrink(A); s.play[1] = shrink(T); s.play[2] = shrink(U); s.play[3] = shrink(B);
            s.play[4] = shrink(T); s.play[5] = shrink(C); s.play[6] = shrink(C); s.play[7] = shrink(U);
            return s;
        }();
        return shapes;
    }

    bool EnsureRoundJoinStyle() {
        if (roundJoinStyle_) return true;
        if (!d2dFactory_) return false;
        return SUCCEEDED(d2dFactory_->CreateStrokeStyle(
                   D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                                               D2D1_DASH_STYLE_SOLID, 0.0f),
                   nullptr, 0, &roundJoinStyle_)) && roundJoinStyle_;
    }

    void DrawPlayPauseIcon(D2D1_POINT_2F center, float opacity, float press) {
        if (!d2dFactory_) return;
        EnsureRoundJoinStyle();
        const PlayPauseShapes& sh = GetPlayPauseShapes();
        const float m = Clamp(playMorph_, 0.0f, 1.0f);

        D2D1_POINT_2F p[8];
        for (int i = 0; i < 8; ++i) {
            p[i] = D2D1::Point2F(sh.pause[i].x + (sh.play[i].x - sh.pause[i].x) * m,
                                 sh.pause[i].y + (sh.play[i].y - sh.pause[i].y) * m);
        }

        ComPtr<ID2D1PathGeometry> quad[2];
        for (int q = 0; q < 2; ++q) {
            if (FAILED(d2dFactory_->CreatePathGeometry(&quad[q])) || !quad[q]) return;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(quad[q]->Open(&sink)) || !sink) return;
            sink->BeginFigure(p[q * 4], D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(p[q * 4 + 1]);
            sink->AddLine(p[q * 4 + 2]);
            sink->AddLine(p[q * 4 + 3]);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return;
        }

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);

        // Everything is drawn opaque inside a layer that carries the icon opacity,
        // so the overlap between fill, stroke and the two halves never double-blends.
        ComPtr<ID2D1Layer> layer;
        const bool layered = SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && layer;
        if (layered) {
            target_->PushLayer(
                D2D1::LayerParameters(D2D1::RectF(center.x - 18.0f, center.y - 18.0f,
                                                  center.x + 18.0f, center.y + 18.0f),
                                      nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), opacity, nullptr, D2D1_LAYER_OPTIONS_NONE),
                layer.Get());
        }
        accentBrush_->SetOpacity(layered ? 1.0f : opacity);

        const float sf = 1.0f - 0.13f * press;
        target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y) *
                              D2D1::Matrix3x2F::Scale(sf, sf, center) * oldTransform);
        for (int q = 0; q < 2; ++q) {
            target_->FillGeometry(quad[q].Get(), accentBrush_.Get());
            target_->DrawGeometry(quad[q].Get(), accentBrush_.Get(), kPlayPauseCorner * 2.0f,
                                  roundJoinStyle_.Get());
        }
        target_->SetTransform(oldTransform);
        if (layered) target_->PopLayer();
        accentBrush_->SetOpacity(1.0f);
    }

