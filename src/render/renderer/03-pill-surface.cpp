    void DrawPill(const SharedState& state, const Settings& settings, const Activity& activity,
                  D2D1_RECT_F rect, float scale, double now) {
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float w = (rect.right - rect.left) * scale;
        const float h = (rect.bottom - rect.top) * scale;
        rect = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);

        float radius = settings.w11Style ? 8.0f * settings.sizeScale : (rect.bottom - rect.top) * 0.5f;
        if (settings.notchStyle) {
            radius = 16.0f * settings.sizeScale;
        } else if (!settings.w11Style) {
            radius = std::min(radius, 44.0f * settings.sizeScale);
        }
        // A radius larger than half the height breaks on a thin strip.
        radius = std::min(radius, std::min(w, h) * 0.5f);

        // Strip mode: content fades in as the island grows out of the strip.
        const float contentAlpha = settings.stripMode
            ? SmoothStep01((h / std::max(0.05f, settings.sizeScale) - StripModeLayout::kContentFadeStart) /
                           StripModeLayout::kContentFadeSpan)
            : 1.0f;
        if (pillCount_ < DwmBackdrop::kMaxPills) {
            pillRects_[pillCount_] = rect;
            pillRadii_[pillCount_] = radius;
            ++pillCount_;
        }
        if (contentAlpha > 0.01f) {
            const float savedOpacity = settingsOpacity_;
            settingsOpacity_ = savedOpacity * contentAlpha;
            DrawSoftShadow(rect, radius);
            settingsOpacity_ = savedOpacity;
        }

        DrawPillSurface(rect, radius, activity.kind, settings);

        if (activity.kind == IslandKind::Progress) {
            DrawProgressRing(rect, state.progress.percent);
        }

        if (activity.kind == IslandKind::BatteryLow) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 2.1));
            redBrush_->SetOpacity(0.45f + 0.45f * pulse);
            DrawIslandShape(rect, radius, settings.w11Style, settings.notchStyle, redBrush_.Get(), 2.0f);
            redBrush_->SetOpacity(1.0f);
        }

        // No inner highlight ring here. A second bright hairline just inside the
        // contour read as a light rim tracing the whole island, which is exactly
        // the edge glow this design drops. The contour stroke alone defines the
        // silhouette now.

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        D2D1_POINT_2F pillCenter = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        target_->SetTransform(D2D1::Matrix3x2F::Scale(settings.sizeScale, settings.sizeScale, pillCenter) * oldTransform);

        float invScale = 1.0f / settings.sizeScale;
        float unW = (rect.right - rect.left) * invScale;
        float unH = (rect.bottom - rect.top) * invScale;
        D2D1_RECT_F unscaledRect = D2D1::RectF(pillCenter.x - unW * 0.5f, pillCenter.y - unH * 0.5f, pillCenter.x + unW * 0.5f, pillCenter.y + unH * 0.5f);

        // The strip itself draws no content; it fades in while the island opens.
        const bool drawContent = contentAlpha > 0.01f;
        ID2D1Layer* stripFadeLayer = nullptr;
        if (drawContent && contentAlpha < 0.995f) {
            stripFadeLayer = AcquireLayer();
            if (stripFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), contentAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   stripFadeLayer);
            }
        }

        if (drawContent) switch (activity.kind) {
            case IslandKind::Media:
                DrawMedia(state, unscaledRect, settings, now);
                break;
            case IslandKind::Clipboard:
                DrawClipboard(state, unscaledRect);
                break;
            case IslandKind::Notification:
                DrawNotification(state, unscaledRect);
                break;
            case IslandKind::Volume:
                DrawVolume(state, unscaledRect);
                break;
            case IslandKind::Brightness:
                DrawBrightness(state, unscaledRect);
                break;
            case IslandKind::CapsLock:
                DrawCapsLock(state, unscaledRect);
                break;
            case IslandKind::Device:
                DrawDevice(state, unscaledRect);
                break;
            case IslandKind::Bluetooth:
                DrawBluetoothDevice(state, unscaledRect);
                break;
            case IslandKind::Timer:
                DrawTimer(state, unscaledRect);
                break;
            case IslandKind::BatteryLow:
                DrawBattery(state, unscaledRect);
                break;
            case IslandKind::Progress:
                DrawProgress(state, unscaledRect);
                break;
            case IslandKind::DoNotDisturb:
                DrawDoNotDisturb(state, unscaledRect);
                break;
            case IslandKind::Lookup:
                DrawLookup(state, unscaledRect, settings, now);
                break;
            case IslandKind::SongId:
                DrawSongId(state, unscaledRect, settings, now);
                break;
            case IslandKind::Idle:
            default:
                DrawIdleDashboard(state, unscaledRect, settings, now);
                break;
        }

        // ── Apple-style privacy indicator dots ───────────────────────────────
        // Green dot = camera in use, Orange dot = mic in use.
        // Drawn in top-right corner of pill, outside content area.
        if (drawContent) {
            DrawPrivacyDots(state, settings, unscaledRect, now);
        }
        if (stripFadeLayer) {
            target_->PopLayer();
        }

        target_->SetTransform(oldTransform);
    }

    // A real soft shadow, approximated by stacking concentric rounded shapes
    // with a quadratic alpha falloff. There is no GPU device here (this renders
    // to a DC-bound target for UpdateLayeredWindow), so a Gaussian blur effect
    // is not available -- but the render padding around the pill leaves room to
    // fake it convincingly and cheaply.
    void DrawSoftShadow(D2D1_RECT_F rect, float radius) {
        // The island sits on top of the backdrop, so a shadow here would darken the blur.
        if (!g_settings.dropShadow || g_settings.backdropMaterial != BackdropMaterial::None) {
            return;
        }

        const float spread = Clamp(14.0f * g_settings.sizeScale, 6.0f, kRenderPadY - 4.0f);
        const float yOffset = spread * 0.35f;
        constexpr int kSteps = 7;

        ID2D1SolidColorBrush* brush = AcquireBrush(material_.shadow);
        if (!brush) {
            return;
        }

        for (int i = kSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float grow = spread * t;
            // Quadratic falloff keeps the core dense and the outer edge feathered.
            const float alpha = material_.shadow.a * (1.0f - t) * (1.0f - t) * 0.55f * settingsOpacity_;
            if (alpha <= 0.002f) {
                continue;
            }
            brush->SetOpacity(alpha);

            const D2D1_RECT_F shadowRect = D2D1::RectF(
                rect.left - grow, rect.top - grow * 0.55f + yOffset,
                rect.right + grow, rect.bottom + grow + yOffset);
            FillIslandShape(shadowRect, radius + grow, g_settings.w11Style,
                            g_settings.notchStyle, brush);
        }
        brush->SetOpacity(1.0f);
    }

    // Vertical depth shading. This used to open with a white sheen across the
    // top, which on a dark island read as a lit strip along the upper edge --
    // the same edge-glow look the rest of this design removes. What is left is
    // a single downward shade that grounds the surface without lighting any
    // edge: neutral at the top, gradually deeper toward the bottom.
    void FillSurfaceDepth(D2D1_RECT_F rect, float radius, bool strong) {
        const float h = rect.bottom - rect.top;
        if (h <= 2.0f) {
            return;
        }

        // Light backgrounds need less of it: the same alpha over near-white
        // turns into a visible grey wash rather than subtle depth.
        const float botA = (strong ? 0.085f : 0.060f) *
                           (material_.onDark ? 1.0f : 0.55f) * settingsOpacity_;

        // Stops are constant; `botA` is applied as brush opacity (identical result,
        // since every stop alpha was just a multiple of botA). One brush forever.
        static const D2D1_GRADIENT_STOP kStops[3] = {
            {0.0f,  D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
            {0.45f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f)},
            {1.0f,  D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)},
        };
        ID2D1LinearGradientBrush* brush = UseLinearGradient(
            depthGrad_, kStops, 3,
            D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom));
        if (!brush) {
            return;
        }
        brush->SetOpacity(Clamp(botA, 0.0f, 1.0f));
        FillIslandShape(rect, radius, g_settings.w11Style, g_settings.notchStyle, brush);
    }

    // A wide, very soft accent wash bled in from the top of the surface. Driven
    // by the album-art accent, this is what makes the media surface feel alive
    // without tinting the whole pill.
    void FillAccentBloom(D2D1_RECT_F rect, float radius, float strength) {
        if (strength <= 0.01f) {
            return;
        }

        const float w = rect.right - rect.left;
        const float h = rect.bottom - rect.top;
        if (w <= 2.0f || h <= 2.0f) {
            return;
        }

        // Only the accent colour is baked into the stops. Strength is brush opacity, so
        // the gradient is rebuilt only while the accent is actually changing colour.
        const D2D1_COLOR_F a = material_.accent;
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
            {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
        };
        const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f, rect.top);
        ID2D1RadialGradientBrush* brush =
            UseRadialGradient(bloomGrad_, stops, 2, center, w * 0.75f, h * 1.15f);
        if (!brush) {
            return;
        }
        brush->SetOpacity(Clamp(strength * settingsOpacity_, 0.0f, 1.0f));
        FillIslandShape(rect, radius, g_settings.w11Style, g_settings.notchStyle, brush);
    }

    // Rounded inner card used by every dashboard, so panels across the media,
    // calendar, weather, hardware and file-tray surfaces match exactly.
    //
    // Fill only, no outline. Each card used to also get a hairline ring, and six
    // of those side by side in the hardware grid turned into a mesh of bright
    // edges competing with the content. The fill alpha in BuildMaterialTokens
    // was raised to carry the separation on its own.
    void DrawCard(D2D1_RECT_F rect, float radius, bool active = false) {
        ID2D1SolidColorBrush* fill = active ? raisedStrongBrush_.Get() : raisedBrush_.Get();
        if (fill) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), fill);
        }
    }

    // Accent-filled progress track shared by the scrubber, volume, battery and
    // timer, so "progress" looks the same everywhere.
    void DrawAccentTrack(D2D1_RECT_F track, float progress, float radius) {
        if (raisedStrongBrush_) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), raisedStrongBrush_.Get());
        }

        const float span = (track.right - track.left) * Clamp(progress, 0.0f, 1.0f);
        if (span <= 0.5f) {
            return;
        }

        const D2D1_RECT_F fillRect = D2D1::RectF(track.left, track.top, track.left + span, track.bottom);

        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, WithAlpha(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.30f), 0.95f)},
            {1.0f, WithAlpha(material_.accent, 1.0f)},
        };
        if (ID2D1LinearGradientBrush* grad = UseLinearGradient(
                trackGrad_, stops, 2,
                D2D1::Point2F(fillRect.left, fillRect.top),
                D2D1::Point2F(fillRect.right, fillRect.top))) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(fillRect, radius, radius), grad);
        }
    }

    void DrawPrivacyDots(const SharedState& state, const Settings& settings, D2D1_RECT_F rect, double now) {
        UNREFERENCED_PARAMETER(now);
        const float height = rect.bottom - rect.top;
        if (height > 55.0f) return;

        const bool mic = state.system.micActive && settings.privacyDots && settings.privacyDotsMic;
        const bool cam = state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam;
        if (!mic && !cam) return;

        const float dotR   = 4.0f;
        const float margin = 16.0f;
        const float dotY   = rect.top + (rect.bottom - rect.top) * 0.5f;

        const float x = rect.right - margin - dotR;

        if (cam) {
            D2D1_COLOR_F camColor = settings.privacyDotsCamHex;
            camColor.a = settingsOpacity_;
            if (!camDotBrush_) target_->CreateSolidColorBrush(camColor, &camDotBrush_);
            else camDotBrush_->SetColor(camColor);

            if (camDotBrush_) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dotY), dotR, dotR), camDotBrush_.Get());
            }
        } else if (mic) {
            D2D1_COLOR_F micColor = settings.privacyDotsMicHex;
            micColor.a = settingsOpacity_;
            if (!micDotBrush_) target_->CreateSolidColorBrush(micColor, &micDotBrush_);
            else micDotBrush_->SetColor(micColor);

            if (micDotBrush_) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dotY), dotR, dotR), micDotBrush_.Get());
            }
        }
    }

    ComPtr<ID2D1PathGeometry> CreateNotchGeometry(D2D1_RECT_F rect, float radius) {
        ComPtr<ID2D1PathGeometry> geom;
        if (FAILED(d2dFactory_->CreatePathGeometry(&geom))) return nullptr;

        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geom->Open(&sink))) return nullptr;

        float r = std::min({radius, (rect.right - rect.left) * 0.5f, (rect.bottom - rect.top) * 0.5f});
        if (r < 0.0f) r = 0.0f;

        sink->BeginFigure(D2D1::Point2F(rect.left, rect.top), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(rect.right, rect.top));
        sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - r));
        if (r > 0.0f) {
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rect.right - r, rect.bottom),
                D2D1::SizeF(r, r), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(rect.left + r, rect.bottom));
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rect.left, rect.bottom - r),
                D2D1::SizeF(r, r), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        } else {
            sink->AddLine(D2D1::Point2F(rect.left, rect.bottom));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();

        return geom;
    }

    ComPtr<ID2D1Geometry> CreateIslandMaskGeometry(D2D1_RECT_F rect, float radius, bool notchStyle) {
        return GetShapeGeometry(rect, radius, notchStyle);
    }

    void FillIslandShape(D2D1_RECT_F rect, float radius, bool w11Style, bool notchStyle, ID2D1Brush* brush) {
        if (!brush) return;
        if (notchStyle) {
            ComPtr<ID2D1Geometry> geom = GetShapeGeometry(rect, radius, true);
            if (geom) {
                target_->FillGeometry(geom.Get(), brush);
                return;
            }
        }
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    }

    void DrawIslandShape(D2D1_RECT_F rect, float radius, bool w11Style, bool notchStyle, ID2D1Brush* brush, float strokeWidth) {
        if (!brush) return;
        if (notchStyle) {
            auto geom = CreateNotchGeometry(rect, radius);
            if (geom) {
                target_->DrawGeometry(geom.Get(), brush, strokeWidth);
                return;
            }
        }
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, strokeWidth);
    }

    // The island's material, composed bottom-up:
    //   tint scrim -> base fill -> depth shading -> accent bloom -> contour
    // Each layer is individually subtle; together they give the pill depth
    // instead of the flat single-fill look it had before. Deliberately absent:
    // any bright rim, hairline or sheen tracing the island's edge.
    void DrawPillSurface(D2D1_RECT_F rect, float radius, IslandKind kind, const Settings& settings) {
        // The dark tint scrim exists to deepen an opaque background. With a real
        // backdrop enabled it would just mud up the blur, so it is skipped.
        if (tintBrush_ && settings.backdropMaterial == BackdropMaterial::None) {
            FillIslandShape(rect, radius, settings.w11Style, settings.notchStyle, tintBrush_.Get());
        }

        // Blur / acrylic and tint come from the DWM window below (see DwmBackdrop);
        // only the fill is drawn here.

        // User-defined pill background color. The authored alpha is combined
        // with the global pill transparency, so a plain 6-digit hex behaves
        // exactly as before (alpha 1.0) while #RRGGBBAA stays translucent.
        D2D1_COLOR_F bg = pillBgColor_;
        bg.a = Clamp(bg.a * settingsOpacity_, 0.0f, 1.0f);
        if (settings.backdropMaterial != BackdropMaterial::None) {
            // An opaque fill would hide the backdrop, so cap it.
            bg.a = std::min(bg.a, settings.backdropFillAlpha);
        }
        if (ID2D1SolidColorBrush* fillBrush = AcquireBrush(bg)) {
            FillIslandShape(rect, radius, settings.w11Style, settings.notchStyle, fillBrush);
        }

        if (settings.materialDepth) {
            FillSurfaceDepth(rect, radius, kind == IslandKind::Media || kind == IslandKind::Idle);

            // Media leans on the album-art accent; other surfaces get a whisper
            // of it so the whole UI still feels connected to what is playing.
            const float bloom = (kind == IslandKind::Media) ? 0.115f
                                : (kind == IslandKind::Idle) ? 0.055f
                                                             : 0.075f;
            FillAccentBloom(rect, radius, bloom * settings.accentBloom);
        }

        if (settings.contourBorderMode != ContourBorderMode::Borderless && settings.contourBorderEnabled) {
            D2D1_COLOR_F borderColor = settings.contourBorderColor;
            float strokeWidth = settings.w11Style ? 1.0f : 0.8f;

            if (settings.contourBorderMode == ContourBorderMode::Auto) {
                if (currentAccent_.a > 0.0f) {
                    borderColor = currentAccent_;
                    borderColor.a = std::min(1.0f, (kind == IslandKind::Idle ? 0.35f : 0.60f) * settingsOpacity_);
                    strokeWidth = 1.0f;
                } else {
                    borderColor.a = std::min(1.0f, borderColor.a * settingsOpacity_);
                }
            } else {
                borderColor.a = std::min(1.0f, borderColor.a * settingsOpacity_);
            }

            if (ID2D1SolidColorBrush* border = AcquireBrush(borderColor)) {
                D2D1_RECT_F borderRect = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f,
                                                     rect.right - 0.5f, rect.bottom - 0.5f);
                DrawIslandShape(borderRect, radius, settings.w11Style, settings.notchStyle, border, strokeWidth);
            }
        }
    }


