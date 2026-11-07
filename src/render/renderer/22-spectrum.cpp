    // ── Audio spectrum styles ────────────────────────────────────────────────
    // DrawSpectrum is the single entry point used by the collapsed pill, the
    // expanded player and the Lyrics tab. It owns the track-change detection and
    // hands the animation clock to the selected style via specT_/specDir_.
    float SpecBandRange(const SharedState& state, float from, float to) const {
        // Average of the analyzer bands covering [from, to) in 0..kSpectrumBands.
        int a = std::clamp(static_cast<int>(std::floor(from)), 0, kSpectrumBands - 1);
        int b = std::clamp(static_cast<int>(std::ceil(to)), a + 1, kSpectrumBands);
        float sum = 0.0f;
        for (int i = a; i < b; ++i) sum += state.bands[static_cast<size_t>(i)];
        return sum / static_cast<float>(b - a);
    }

    void DrawSpectrum(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        constexpr double kSpecDur = 1.15;
        const bool recentlyDrawn = specLastDrawn_ >= 0.0 && (now - specLastDrawn_) < 0.25;
        float dt = recentlyDrawn ? static_cast<float>(now - specLastDrawn_) : 0.016f;
        dt = Clamp(dt, 0.001f, 0.05f);
        specLastDrawn_ = now;

        const size_t key = std::hash<std::wstring>{}(state.media.title) * 31u +
                           std::hash<std::wstring>{}(state.media.artist);
        if (!specInit_ || !recentlyDrawn) {
            specInit_ = true;
            specKey_ = key;
            specStart_ = -1.0;
            if (!recentlyDrawn) {
                std::fill(std::begin(specPeak_), std::end(specPeak_), 0.0f);
                std::fill(std::begin(specPeakVel_), std::end(specPeakVel_), 0.0f);
                specBassSlow_ = 0.0f;
                specKick_ = 0.0f;
            }
        } else if (key != specKey_ && !state.media.title.empty()) {
            const bool running = specStart_ >= 0.0 && (now - specStart_) < 0.35;
            specKey_ = key;
            // Title and artist often land a few frames apart: absorb the second one.
            if (!running) {
                specStart_ = now;
                specDir_ = 1.0f;
                if (lastSkipClickAt_ >= 0.0 && (now - lastSkipClickAt_) < 6.0) specDir_ = lastSkipDir_;
                // Led: the old peaks rain down as the scanline reboots the matrix.
                for (float& v : specPeakVel_) v = 0.0f;
                for (int i = 0; i < kSpectrumBands; ++i) specPeak_[i] *= 0.6f;
            }
        }

        specT_ = -1.0f;
        if (specStart_ >= 0.0) {
            const float t = static_cast<float>((now - specStart_) / kSpecDur);
            if (t >= 1.0f) {
                specStart_ = -1.0;
            } else {
                specT_ = t;
                g_layoutDirty = true;
            }
        }

        // Low-end energy drives the orb's pulse and a beat "kick" envelope.
        const float bass = SpecBandRange(state, 0.0f, 4.0f);
        specBassSlow_ += (bass - specBassSlow_) * (1.0f - std::exp(-dt / 0.45f));
        const float beat = std::max(0.0f, bass - specBassSlow_ - 0.04f);
        specKick_ = std::max(specKick_ * std::exp(-dt / 0.18f), Clamp(beat * 3.2f, 0.0f, 1.0f));
        specBass_ += (bass - specBass_) * (1.0f - std::exp(-dt / 0.07f));
        specPhase_ += dt * (1.6f + 5.0f * specBass_ + 8.0f * specKick_);

        switch (settings.spectrumStyle) {
            case SpectrumStyle::Orb:    DrawSpectrumOrb(state, rect); break;
            case SpectrumStyle::Plasma: DrawSpectrumPlasma(state, rect); break;
            case SpectrumStyle::Led:    DrawSpectrumLed(state, rect, dt); break;
            default:                    DrawWaveform(state, rect); break;
        }
    }

    // ── Pulse Orb ────────────────────────────────────────────────────────────
    void DrawSpectrumOrb(const SharedState& state, D2D1_RECT_F rect) {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        const float R = std::min(w, h) * 0.5f + 1.0f;  // outer reach
        const float r0 = R * 0.40f;  // core radius at rest
        // Fewer, thinner spokes on a small orb so they never merge into a solid disc.
        const int kSpokes = R < 14.0f ? 16 : 24;
        const int half = kSpokes / 2;
        constexpr float kTwoPi = 6.28318531f;

        const float t = specT_;
        // Spokes are fixed (bass at the bottom, mirrored left/right) so the shape always
        // reads as a spectrum; only the track-change detonation spins the orb.
        float lenScale = 1.0f, coreScale = 1.0f, ringShrink = 0.0f, spin = 0.0f;
        float burstAlpha = 0.0f, burstR = 0.0f;
        if (t >= 0.0f) {
            if (t < 0.28f) {
                const float u = SkipSmooth(t / 0.28f);
                lenScale = 1.0f - u;
                coreScale = 1.0f - 0.35f * u;
                ringShrink = 0.30f * u;
            } else {
                const float u = Clamp((t - 0.28f) / 0.72f, 0.0f, 1.0f);
                const float spring = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
                lenScale = spring;
                coreScale = 1.0f + 0.55f * std::exp(-7.0f * u) * std::cos(6.0f * u);
                ringShrink = 0.30f * std::exp(-9.0f * u);
                spin += specDir_ * 4.2f * std::pow(1.0f - u, 3.0f);
                burstR = r0 + (R + 3.0f - r0) * SkipEaseInOut(u / 0.8f);
                burstAlpha = 0.75f * (1.0f - Clamp(u / 0.85f, 0.0f, 1.0f)) * (1.0f - Clamp(u / 0.85f, 0.0f, 1.0f));
            }
        }

        const float baseR = r0 * (1.0f - ringShrink);
        const float bassPulse = 0.18f * specBass_ + 0.22f * specKick_;

        EnsureRoundJoinStyle();
        const float strokeW = Clamp(kTwoPi * (baseR + 1.0f) / static_cast<float>(kSpokes) * 0.62f, 1.0f, R * 0.16f);

        // Thin shell ring just inside the spokes: breathes with the bass (no filled background).
        accentBrush_->SetOpacity(0.25f + 0.35f * specBass_ + 0.25f * specKick_);
        const float shellR = baseR * (0.90f + 0.18f * bassPulse);
        target_->DrawEllipse(D2D1::Ellipse(c, shellR, shellR), accentBrush_.Get(), 0.8f);

        for (int k = 0; k < kSpokes; ++k) {
            const int m = k < half ? k : kSpokes - 1 - k;  // mirrored 0..half-1
            const float lo = static_cast<float>(m) * kSpectrumBands / half;
            float v = SpecBandRange(state, lo, lo + static_cast<float>(kSpectrumBands) / half);
            v = Clamp(v * 1.1f, 0.04f, 1.0f);
            // Each spoke trails the detonation slightly so the burst reads as a wave.
            float ls = lenScale;
            if (t >= 0.28f) {
                const float u = Clamp((t - 0.28f - 0.012f * m) / 0.72f, 0.0f, 1.0f);
                ls = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
            }
            const float len = (R - baseR - 1.5f) * (0.10f + 0.90f * v) * Clamp(ls, 0.0f, 1.45f);
            // k = 0 and k = kSpokes-1 sit either side of the bottom (bass), highs meet at the top.
            const float a = spin + 1.5707963f + kTwoPi * (static_cast<float>(k) + 0.5f) / kSpokes;
            const float ca = std::cos(a), sa = std::sin(a);
            const float l = std::max(len, 0.6f);
            const D2D1_POINT_2F p0 = D2D1::Point2F(c.x + ca * (baseR + 1.0f), c.y + sa * (baseR + 1.0f));
            const D2D1_POINT_2F p1 = D2D1::Point2F(c.x + ca * (baseR + 1.0f + l), c.y + sa * (baseR + 1.0f + l));
            // Soft glow underlay, then the spoke itself.
            accentBrush_->SetOpacity(0.10f + 0.14f * v);
            target_->DrawLine(p0, p1, accentBrush_.Get(), strokeW * 2.0f, roundJoinStyle_.Get());
            accentBrush_->SetOpacity(0.45f + 0.55f * v);
            target_->DrawLine(p0, p1, accentBrush_.Get(), strokeW, roundJoinStyle_.Get());
        }

        // Core.
        accentBrush_->SetOpacity(0.95f);
        const float coreR = std::max(0.8f, baseR * 0.58f * coreScale * (1.0f + bassPulse));
        target_->FillEllipse(D2D1::Ellipse(c, coreR, coreR), accentBrush_.Get());
        ComPtr<ID2D1SolidColorBrush> hot;
        if (SUCCEEDED(target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.65f), &hot)) && hot) {
            hot->SetOpacity(0.55f + 0.35f * specKick_);
            target_->FillEllipse(D2D1::Ellipse(c, coreR * 0.45f, coreR * 0.45f), hot.Get());
        }

        if (burstAlpha > 0.01f) {
            accentBrush_->SetOpacity(burstAlpha);
            target_->DrawEllipse(D2D1::Ellipse(c, burstR, burstR), accentBrush_.Get(), 0.8f + 1.4f * burstAlpha);
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Plasma Thread ────────────────────────────────────────────────────────
    void DrawSpectrumPlasma(const SharedState& state, D2D1_RECT_F rect) {
        constexpr int kPts = 34;
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float maxA = h * 0.46f;
        const float t = specT_;
        const float dir = specDir_;

        EnsureRoundJoinStyle();
        ComPtr<ID2D1SolidColorBrush> hot;
        target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.6f), &hot);

        // Front position (normalised, already mirrored for "previous").
        const float front = t < 0.15f ? -0.4f : -0.4f + 1.9f * SkipEaseInOut((t - 0.15f) / 0.78f);

        for (int strand = 0; strand < 2; ++strand) {
            D2D1_POINT_2F pts[kPts];
            for (int i = 0; i < kPts; ++i) {
                const float xn = static_cast<float>(i) / (kPts - 1);
                const float xd = dir >= 0.0f ? xn : 1.0f - xn;  // distance along the sweep
                // max(): sin(pi) is slightly negative in float and pow(neg, 0.7) is NaN.
                float env = std::pow(std::max(0.0f, std::sin(3.14159265f * xn)), 0.7f);  // pinned at both ends
                float band = SpecBandRange(state, xn * (kSpectrumBands - 1), xn * (kSpectrumBands - 1) + 1.5f);
                band = Clamp(0.10f + 0.95f * band + 0.25f * specKick_, 0.0f, 1.25f);

                float ignite = 1.0f, boost = 0.0f;
                if (t >= 0.0f) {
                    if (t < 0.15f) {
                        ignite = 1.0f - SkipSmooth(t / 0.15f);
                    } else {
                        ignite = SkipSmooth((front - xd) / 0.22f + 0.15f);
                        const float g = (xd - front) / 0.16f;
                        boost = 0.95f * std::exp(-g * g);
                    }
                }
                const float ph = specPhase_ + (strand ? 3.14159265f : 0.0f);
                const float wave = std::sin(xn * (7.0f + 4.0f * band) - ph) * 0.65f +
                                   std::sin(xn * 13.0f + ph * 0.6f) * 0.35f;
                const float A = maxA * env * (band * ignite + boost * env);
                pts[i] = D2D1::Point2F(rect.left + xn * w, cy + wave * A * (strand ? -1.0f : 1.0f));
            }
            ComPtr<ID2D1PathGeometry> path;
            if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || !path) continue;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(path->Open(&sink)) || !sink) continue;
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(pts + 1, kPts - 1);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) continue;

            // Glow underlay, body, hot core.
            accentBrush_->SetOpacity(strand ? 0.16f : 0.22f);
            target_->DrawGeometry(path.Get(), accentBrush_.Get(), 4.2f, roundJoinStyle_.Get());
            accentBrush_->SetOpacity(strand ? 0.62f : 0.95f);
            target_->DrawGeometry(path.Get(), accentBrush_.Get(), strand ? 1.1f : 1.6f, roundJoinStyle_.Get());
            if (hot && !strand) {
                hot->SetOpacity(0.35f + 0.4f * specKick_);
                target_->DrawGeometry(path.Get(), hot.Get(), 0.6f, roundJoinStyle_.Get());
            }
        }

        // Spark riding the wave front during the re-ignite.
        if (t >= 0.15f && hot) {
            const float xd = Clamp(front, 0.0f, 1.0f);
            const float xn = dir >= 0.0f ? xd : 1.0f - xd;
            const float a = 1.0f - Clamp((t - 0.80f) / 0.2f, 0.0f, 1.0f);
            if (front > -0.1f && front < 1.15f) {
                hot->SetOpacity(0.85f * a);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 2.2f, 2.2f), hot.Get());
                accentBrush_->SetOpacity(0.28f * a);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 5.5f, 5.5f), accentBrush_.Get());
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Peak Matrix ──────────────────────────────────────────────────────────
    // Stacked LED cells per column with gravity-driven falling peak caps.
    void DrawSpectrumLed(const SharedState& state, D2D1_RECT_F rect, float dt) {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const float colGap = 1.8f, colW = 2.6f;
        const int cols = std::clamp(static_cast<int>((w + colGap) / (colW + colGap)), 3, 14);
        const float cw = (w - colGap * (cols - 1)) / cols;
        const int rows = std::clamp(static_cast<int>(h / 3.6f), 4, 8);
        const float cellGap = 1.0f;
        const float ch = (h - cellGap * (rows - 1)) / rows;
        const float t = specT_;
        const float dir = specDir_;

        ComPtr<ID2D1SolidColorBrush> hot;
        target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.7f), &hot);

        // Scan position in column units, mirrored for "previous".
        const float travel = static_cast<float>(cols) + 4.0f;
        const float scan = t < 0.0f ? -10.0f : -1.5f + travel * SkipEaseInOut(t / 0.82f);

        for (int c = 0; c < cols; ++c) {
            const int slot = dir >= 0.0f ? c : cols - 1 - c;  // order the scan reaches this column
            // Same source as Classic Bars: each column reads the level history, newest on the
            // right, so every column rises and falls with the music (no frequency tilt).
            const size_t wfSize = state.waveform.size();
            const size_t wfOffset = static_cast<size_t>(cols - c) * 3;
            const size_t wfSrc = (state.waveformWrite + wfSize * 2 - wfOffset) % wfSize;
            float v = Clamp(state.waveform[wfSrc], 0.03f, 1.0f);

            // Falling peak cap with gravity; a hit snaps it up.
            float& pk = specPeak_[c];
            float& pv = specPeakVel_[c];
            if (v >= pk) { pk = v; pv = 0.0f; }
            else { pv += 2.4f * dt; pk = std::max(v, pk - pv * dt); }

            float colAlpha = 1.0f, flash = 0.0f;
            if (t >= 0.0f) {
                const float d = scan - static_cast<float>(slot);  // >0: scanline already passed
                if (d < 0.0f) {
                    colAlpha = 0.35f;  // old song, about to be wiped
                } else {
                    flash = Clamp(1.0f - d / 3.2f, 0.0f, 1.0f);  // glowing tail behind the scanline
                    colAlpha = 0.35f + 0.65f * SkipSmooth(d / 1.2f);
                    if (d < 1.6f) { pk = std::min(pk, 0.0f + v); }      // peaks reset as the line passes
                }
            }

            const float x = rect.left + c * (cw + colGap);
            const int lit = static_cast<int>(std::round(v * rows));
            const int cap = std::clamp(static_cast<int>(std::round(pk * rows)) - 1, 0, rows - 1);
            for (int r = 0; r < rows; ++r) {
                const float yb = rect.bottom - r * (ch + cellGap);
                D2D1_RECT_F cell = D2D1::RectF(x, yb - ch, x + cw, yb);
                const float rr = std::min(cw, ch) * 0.35f;
                const float topness = rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.0f;
                if (flash > 0.0f) {
                    // Whole column ignites, fading from the bottom up as the tail dies.
                    ID2D1SolidColorBrush* b = (hot && flash > 0.7f && r >= rows / 2) ? hot.Get() : accentBrush_.Get();
                    b->SetOpacity(Clamp(0.25f + 0.75f * flash, 0.0f, 1.0f) * (0.6f + 0.4f * (1.0f - topness)));
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), b);
                } else if (r < lit) {
                    accentBrush_->SetOpacity((0.50f + 0.45f * topness) * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accentBrush_.Get());
                } else if (r == cap && cap >= lit && pk > 0.05f && hot) {
                    hot->SetOpacity(0.9f * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), hot.Get());
                } else {
                    accentBrush_->SetOpacity(0.09f * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accentBrush_.Get());
                }
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

