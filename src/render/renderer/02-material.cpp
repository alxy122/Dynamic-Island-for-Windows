    void EnsureBrushes(const Settings& settings, const SharedState& state, double now) {
        D2D1_COLOR_F targetAccent = settings.customAccent;
        if (settings.accentMode == AccentMode::System) {
            targetAccent = GetSystemAccentColor();
        } else if (settings.accentMode == AccentMode::Auto && state.media.art) {
            // Already contrast-corrected when the art was decoded.
            targetAccent = state.media.art->sampledAccent;
        } else {
            // A custom or system accent was never checked against the island's
            // own background. That did not matter while every theme was dark,
            // but it does now that one is light: the default cyan sits at about
            // 1.6:1 on Porcelain. Corrected before the smoothing lerp so the
            // value is stable rather than re-derived every frame.
            targetAccent = EnsureContrastAgainstBackground(targetAccent, settings.pillBgColor);
        }

        // Smooth accent transition: exponential lerp toward target, ~300ms half-life.
        // On the very first frame (lastAccentTime_ < 0) snap immediately so there's
        // no fade-in from the default color on startup.
        if (lastAccentTime_ < 0.0) {
            currentAccent_ = targetAccent;
            lastAccentTime_ = now;
        } else {
            const double dt = std::max(0.0, std::min(now - lastAccentTime_, 0.1));  // cap at 100ms
            lastAccentTime_ = now;
            // k = 1 - exp(-dt / tau), tau ≈ 0.18s → reaches 95% in ~350ms
            const float k = 1.0f - static_cast<float>(std::exp(-dt / 0.18));
            currentAccent_.r += (targetAccent.r - currentAccent_.r) * k;
            currentAccent_.g += (targetAccent.g - currentAccent_.g) * k;
            currentAccent_.b += (targetAccent.b - currentAccent_.b) * k;
            currentAccent_.a = 1.0f;
        }

        const D2D1_COLOR_F accent = currentAccent_;

        if (!accentBrush_) target_->CreateSolidColorBrush(accent, &accentBrush_);
        else accentBrush_->SetColor(accent);

        if (!redBrush_) target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.27f, 0.27f, 1.0f), &redBrush_);

        // Use user-configured text colors.
        D2D1_COLOR_F primary = settings.textPrimaryColor;
        primary.a = 0.98f;
        if (!textBrush_) target_->CreateSolidColorBrush(primary, &textBrush_);
        else textBrush_->SetColor(primary);

        D2D1_COLOR_F secondary = settings.textSecondaryColor;
        secondary.a = 0.90f;
        if (!mutedBrush_) target_->CreateSolidColorBrush(secondary, &mutedBrush_);
        else mutedBrush_->SetColor(secondary);

        // Keep the authored alpha. An 8-digit #RRGGBBAA background is how a
        // translucent island is requested independently of the global pill
        // transparency slider; DrawPillSurface combines the two.
        pillBgColor_ = settings.pillBgColor;

        D2D1_COLOR_F tintColor = D2D1::ColorF(0.010f, 0.010f, 0.012f, settings.tintOpacity);
        if (!tintBrush_) target_->CreateSolidColorBrush(tintColor, &tintBrush_);
        else tintBrush_->SetColor(tintColor);

        if (!shadowBrush_) target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.70f), &shadowBrush_);

        BuildMaterialTokens(settings);
    }

    // ── Premium material system ─────────────────────────────────────────────
    // Every surface in the island is built from one small token set so the whole
    // UI shares a single visual language. Tokens are recomputed once per frame
    // from the resolved theme, and adapt to background luminance so a light
    // custom background gets dark separators instead of washed-out white ones.
    struct MaterialTokens {
        D2D1_COLOR_F base{};          // the pill's own fill
        D2D1_COLOR_F raised{};        // inner cards, chips, wells
        D2D1_COLOR_F raisedStrong{};  // pressed / active chips
        D2D1_COLOR_F hairline{};      // 1px separators between content
        D2D1_COLOR_F stroke{};        // outer contour
        D2D1_COLOR_F shadow{};        // drop shadow tint
        D2D1_COLOR_F textPrimary{};
        D2D1_COLOR_F textSecondary{};
        D2D1_COLOR_F textTertiary{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accentSoft{};
        bool onDark = true;
        float opacity = 1.0f;
    };

    static D2D1_COLOR_F MixColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
        return D2D1::ColorF(a.r + (b.r - a.r) * t,
                            a.g + (b.g - a.g) * t,
                            a.b + (b.b - a.b) * t,
                            a.a + (b.a - a.a) * t);
    }

    static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float alpha) {
        c.a = Clamp(alpha, 0.0f, 1.0f);
        return c;
    }

    // ── Cached Direct2D resources ───────────────────────────────────────────
    // Nothing below is created per frame once it has warmed up. Render thread only.
    struct LinearGradientSlot { ComPtr<ID2D1LinearGradientBrush> brush; uint64_t key = 0; };
    struct RadialGradientSlot { ComPtr<ID2D1RadialGradientBrush> brush; uint64_t key = 0; };

    struct ShapeKey {
        float l = 0, t = 0, r = 0, b = 0, radius = 0;
        bool notch = false;
        bool operator==(const ShapeKey& o) const {
            return l == o.l && t == o.t && r == o.r && b == o.b &&
                   radius == o.radius && notch == o.notch;
        }
    };
    struct ShapeEntry {
        ShapeKey key;
        ComPtr<ID2D1Geometry> geom;
        uint64_t lastUsed = 0;
    };
    static constexpr size_t kShapeCacheSize = 32;

    static uint32_t PackColor8(const D2D1_COLOR_F& c) {
        auto q = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
    }

    static uint64_t HashStops(const D2D1_GRADIENT_STOP* s, UINT32 n) {
        uint64_t h = 1469598103934665603ull;
        for (UINT32 i = 0; i < n; ++i) {
            h = (h ^ PackColor8(s[i].color)) * 1099511628211ull;
            h = (h ^ static_cast<uint64_t>(s[i].position * 1000.0f + 0.5f)) * 1099511628211ull;
        }
        return h;
    }

    // Call once per frame, right after BeginDraw().
    void BeginFrameResources() {
        brushPoolUsed_ = 0;
        layerPoolUsed_ = 0;
    }

    // Returns a brush that is unique for the rest of this frame. Colour and opacity are
    // reset, so a previous user's SetOpacity() can never leak into the next one.
    ID2D1SolidColorBrush* AcquireBrush(const D2D1_COLOR_F& color) {
        if (brushPoolUsed_ >= brushPool_.size()) {
            ComPtr<ID2D1SolidColorBrush> b;
            if (FAILED(target_->CreateSolidColorBrush(color, &b)) || !b) {
                return nullptr;
            }
            brushPool_.push_back(std::move(b));
        }
        ID2D1SolidColorBrush* brush = brushPool_[brushPoolUsed_++].Get();
        brush->SetColor(color);
        brush->SetOpacity(1.0f);
        return brush;
    }

    // Same idea for layers. Each Acquire is distinct within a frame, so nested
    // PushLayer calls are safe, and the layer's backing store is kept between frames.
    ID2D1Layer* AcquireLayer() {
        if (layerPoolUsed_ >= layerPool_.size()) {
            ComPtr<ID2D1Layer> l;
            if (FAILED(target_->CreateLayer(&l)) || !l) {
                return nullptr;
            }
            layerPool_.push_back(std::move(l));
        }
        return layerPool_[layerPoolUsed_++].Get();
    }

    void SetPersistentBrush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& c) {
        if (!brush) {
            target_->CreateSolidColorBrush(c, &brush);
        } else {
            brush->SetColor(c);
        }
    }

    // The collection + brush are rebuilt only when the stops change; start/end move per call.
    ID2D1LinearGradientBrush* UseLinearGradient(LinearGradientSlot& slot,
                                                const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                D2D1_POINT_2F start, D2D1_POINT_2F end) {
        const uint64_t key = HashStops(stops, count);
        if (!slot.brush || slot.key != key) {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                             D2D1_EXTEND_MODE_CLAMP, &coll)) || !coll) {
                return nullptr;
            }
            if (FAILED(target_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(start, end), coll.Get(), &slot.brush)) ||
                !slot.brush) {
                slot.brush.Reset();
                return nullptr;
            }
            slot.key = key;
        }
        slot.brush->SetStartPoint(start);
        slot.brush->SetEndPoint(end);
        return slot.brush.Get();
    }

    ID2D1RadialGradientBrush* UseRadialGradient(RadialGradientSlot& slot,
                                                const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                D2D1_POINT_2F center, float rx, float ry) {
        const uint64_t key = HashStops(stops, count);
        if (!slot.brush || slot.key != key) {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                             D2D1_EXTEND_MODE_CLAMP, &coll)) || !coll) {
                return nullptr;
            }
            if (FAILED(target_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry),
                    coll.Get(), &slot.brush)) ||
                !slot.brush) {
                slot.brush.Reset();
                return nullptr;
            }
            slot.key = key;
        }
        slot.brush->SetCenter(center);
        slot.brush->SetGradientOriginOffset(D2D1::Point2F(0, 0));
        slot.brush->SetRadiusX(rx);
        slot.brush->SetRadiusY(ry);
        return slot.brush.Get();
    }

    // Rounded-rect or notch geometry, cached by rect + radius. Returns a ref-counted
    // pointer, so a caller holding it is safe even if the entry is evicted meanwhile.
    ComPtr<ID2D1Geometry> GetShapeGeometry(D2D1_RECT_F rect, float radius, bool notch) {
        const ShapeKey key{rect.left, rect.top, rect.right, rect.bottom, radius, notch};
        ShapeEntry* victim = &shapeCache_[0];
        for (ShapeEntry& e : shapeCache_) {
            if (e.geom && e.key == key) {
                e.lastUsed = ++shapeClock_;
                return e.geom;
            }
            if (e.lastUsed < victim->lastUsed) {
                victim = &e;  // empty entries have lastUsed == 0, so they go first
            }
        }

        ComPtr<ID2D1Geometry> geom;
        if (notch) {
            ComPtr<ID2D1PathGeometry> path = CreateNotchGeometry(rect, radius);
            if (path) path.As(&geom);
        }
        if (!geom) {  // not a notch, or the notch failed: same fallback as before
            ComPtr<ID2D1RoundedRectangleGeometry> rr;
            if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(rect, radius, radius), &rr)) && rr) {
                rr.As(&geom);
            }
        }
        if (!geom) return nullptr;

        victim->key = key;
        victim->geom = geom;
        victim->lastUsed = ++shapeClock_;
        return geom;
    }

    void ReleaseCachedResources() {
        brushPool_.clear();
        layerPool_.clear();
        brushPoolUsed_ = 0;
        layerPoolUsed_ = 0;
        raisedBrush_.Reset();
        raisedStrongBrush_.Reset();
        depthGrad_ = LinearGradientSlot{};
        trackGrad_ = LinearGradientSlot{};
        bloomGrad_ = RadialGradientSlot{};
        swapGroupLayer_.Reset();
        for (ShapeEntry& e : shapeCache_) e = ShapeEntry{};
    }

    void BuildMaterialTokens(const Settings& settings) {
        MaterialTokens t;
        t.opacity = settingsOpacity_;
        t.base = pillBgColor_;

        // Light surfaces need dark scrims/edges, dark surfaces need light ones.
        const bool onDark = RelativeLuminance(t.base) < 0.45;
        t.onDark = onDark;

        const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        const D2D1_COLOR_F black = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
        const D2D1_COLOR_F lift = onDark ? white : black;

        // Cards carry a slightly stronger fill than before. They used to be
        // outlined with a hairline ring, and losing that ring is what the fill
        // now has to compensate for so a card still reads as a distinct surface.
        t.raised = WithAlpha(lift, onDark ? 0.085f : 0.062f);
        t.raisedStrong = WithAlpha(lift, onDark ? 0.150f : 0.105f);
        t.hairline = WithAlpha(lift, onDark ? 0.100f : 0.085f);
        t.shadow = WithAlpha(black, 0.55f);

        t.stroke = settings.contourBorderColor;
        t.accent = currentAccent_;
        t.accentSoft = WithAlpha(currentAccent_, 0.18f);

        t.textPrimary = WithAlpha(settings.textPrimaryColor, 0.98f);
        t.textSecondary = WithAlpha(settings.textSecondaryColor, 0.88f);
        // Derived rather than configured, so a third tier always reads as
        // quieter than "secondary" whatever the user picked.
        t.textTertiary = WithAlpha(MixColor(settings.textSecondaryColor, t.base, 0.35f), 0.72f);

        material_ = t;
        SetPersistentBrush(raisedBrush_, t.raised);
        SetPersistentBrush(raisedStrongBrush_, t.raisedStrong);
    }

