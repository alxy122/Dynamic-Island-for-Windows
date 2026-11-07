    // ── Tab transition (spring-driven carousel) ─────────────────────────────
    // tabPos_ is a continuous "which tab is in view" value that a damped spring
    // chases toward the selected index. Every visible tab is drawn from its
    // distance to that position (slide + shrink + fade), so the motion is
    // interruptible, keeps its momentum on fast scrolling, and never restarts.
    void ResetTabTransition() {
        tabInit_ = false;
        tabAnimating_ = false;
        g_tabAnimating.store(false, std::memory_order_relaxed);
        g_tabEdgeBump.store(0, std::memory_order_relaxed);
    }

    template <typename DrawFn>
    void DrawTabTransition(int context, int tabIdx, const std::vector<int>& tabIds,
                           D2D1_RECT_F rect, double now, DrawFn&& drawTab) {
        const int count = static_cast<int>(tabIds.size());
        if (count <= 0) {
            ResetTabTransition();
            return;
        }
        tabIdx = ClampInt(tabIdx, 0, count - 1);

        // ── Feel: tweak these ──
        constexpr float kStiffness = 250.0f;        // higher = faster (settles in ~0.29s)
        constexpr float kDamping = 31.0f;           // ~0.92 damping ratio: no visible overshoot
        constexpr float kTravelPx = 34.0f;          // vertical gap between neighbouring tabs
        constexpr float kMinScale = 0.965f;         // size of a tab that is fully out of view
        constexpr float kFadePower = 1.4f;          // >1 thins the mid-point slightly so text never ghosts

        float dt = static_cast<float>(now - tabLastDrawn_);
        tabLastDrawn_ = now;
        // First frame of a motion: the previous paint may be long ago, so don't
        // let a big gap swallow the start of the animation.
        dt = tabAnimating_ ? Clamp(dt, 0.001f, 0.05f) : (1.0f / 120.0f);

        // Snap (no animation) when the tab view was just opened, switched between
        // media/idle views, or the number of tabs changed.
        if (!tabInit_ || context != tabContext_ || count != tabCount_) {
            tabInit_ = true;
            tabContext_ = context;
            tabCount_ = count;
            tabPos_ = static_cast<float>(tabIdx);
            tabTarget_ = tabPos_;
            tabVel_ = 0.0f;
            tabDir_ = 1.0f;
            tabAnimating_ = false;
        }
        const float newTarget = static_cast<float>(tabIdx);
        if (newTarget != tabTarget_) {
            tabDir_ = newTarget > tabTarget_ ? 1.0f : -1.0f;  // remembered for the whole move
            tabTarget_ = newTarget;
        }

        // The first and last tab are hard stops: there is no edge bump any more.
        g_tabEdgeBump.store(0, std::memory_order_relaxed);

        if (tabPos_ != tabTarget_ || tabVel_ != 0.0f) {
            float remaining = dt;
            while (remaining > 0.0f) {
                const float h = std::min(remaining, 0.004f);
                tabVel_ += (-kStiffness * (tabPos_ - tabTarget_) - kDamping * tabVel_) * h;
                tabPos_ += tabVel_ * h;

                // Never travel past the first or last tab, and kill any velocity
                // pushing further out, so the edge tabs cannot overshoot or twitch.
                const float maxTabPos = static_cast<float>(count - 1);
                if (tabPos_ < 0.0f) {
                    tabPos_ = 0.0f;
                    if (tabVel_ < 0.0f) tabVel_ = 0.0f;
                } else if (tabPos_ > maxTabPos) {
                    tabPos_ = maxTabPos;
                    if (tabVel_ > 0.0f) tabVel_ = 0.0f;
                }
                remaining -= h;
            }
            if (std::fabs(tabPos_ - tabTarget_) < 0.0008f && std::fabs(tabVel_) < 0.02f) {
                tabPos_ = tabTarget_;
                tabVel_ = 0.0f;
            }
        }

        const bool moving = (tabPos_ != tabTarget_ || tabVel_ != 0.0f);
        tabAnimating_ = moving;
        g_tabAnimating.store(moving, std::memory_order_relaxed);

        if (!moving) {
            drawTab(tabIds[tabIdx]);
            return;
        }
        g_tabLastPaintAt.store(now, std::memory_order_relaxed);  // render loop keeps frames coming until it settles

        const D2D1_POINT_2F center =
            D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);

        if (!tabLayer_) {
            target_->CreateLayer(&tabLayer_);  // cached: re-creating it every frame is costly
        }

        // Every tab is a pure function of its distance to tabPos_, with no
        // "arriving" / "leaving" roles. Motion is therefore continuous even when you
        // reverse mid-scroll, and the tabs travel as one rigid strip (a fixed gap
        // apart), which is what reads as real scrolling rather than two fading cards.
        auto drawLayer = [&](int index) {
            if (index < 0 || index >= count) {
                return;
            }
            const float d = static_cast<float>(index) - tabPos_;  // <0 above, >0 below
            const float ad = Clamp(std::fabs(d), 0.0f, 1.0f);
            const float eased = SmoothStep01(ad);

            const float alpha = std::pow(1.0f - eased, kFadePower);
            if (alpha <= 0.004f) {
                return;
            }

            const float scale = 1.0f - (1.0f - kMinScale) * eased;
            const float dy = d * kTravelPx;

            target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) *
                                  D2D1::Matrix3x2F::Translation(0.0f, dy) * oldTransform);
            if (alpha < 0.995f && tabLayer_) {
                target_->PushLayer(
                    D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), alpha, nullptr,
                                          D2D1_LAYER_OPTIONS_NONE),
                    tabLayer_.Get());
                drawTab(tabIds[index]);
                target_->PopLayer();
            } else {
                drawTab(tabIds[index]);  // fully opaque: skip the layer, much cheaper
            }
            target_->SetTransform(oldTransform);
        };

        // Farther tab first, so the one closer to centre ends up on top.
        const int lo = static_cast<int>(std::floor(tabPos_));
        if ((tabPos_ - static_cast<float>(lo)) < 0.5f) {
            drawLayer(lo + 1);
            drawLayer(lo);
        } else {
            drawLayer(lo);
            drawLayer(lo + 1);
        }
    }

    // Vertical pagination dots on the right edge. The active dot stretches into a
    // pill and slides with the transition instead of jumping.
    void DrawTabPagination(D2D1_RECT_F rect, int tabCount) {
        if (tabCount <= 1) {
            return;
        }
        ID2D1SolidColorBrush* dotBrush = AcquireBrush(WithAlpha(material_.textPrimary, 1.0f));
        if (!dotBrush) {
            return;
        }

        const float dotX = rect.right - 10.0f;
        const float spacing = 9.0f;
        const float r = 2.5f;
        const float startY = (rect.top + rect.bottom) * 0.5f - spacing * (tabCount - 1) * 0.5f;

        // Resting dots. The one under the indicator fades out, so the pill reads
        // as travelling over the track instead of sitting on top of a dot.
        for (int i = 0; i < tabCount; ++i) {
            const float under = SmoothStep01(Clamp(1.0f - std::fabs(tabPos_ - static_cast<float>(i)), 0.0f, 1.0f));
            dotBrush->SetOpacity(0.22f * (1.0f - under) * settingsOpacity_);
            target_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(dotX, startY + spacing * static_cast<float>(i)), r, r),
                dotBrush);
        }

        // Travelling indicator: follows the spring exactly (so it also gives a little
        // at the ends) and stretches slightly with speed.
        const float pos = Clamp(tabPos_, -0.3f, static_cast<float>(tabCount) - 0.7f);
        const float stretch = Clamp(std::fabs(tabVel_) * 0.55f, 0.0f, 3.5f);
        const float halfH = r + 2.2f + stretch;
        const float cy = startY + spacing * pos;
        dotBrush->SetOpacity(0.90f * settingsOpacity_);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dotX - r, cy - halfH, dotX + r, cy + halfH), r, r),
            dotBrush);
    }

    // Publishes the content-space rect currently being painted so
    // OverlayWndProc hit-tests against the real geometry rather than assuming
    // the pill is centered in the client area. Seqlock write: bump to odd,
    // store, bump to even, so a reader can never mix two frames.
    void PublishContentGeometry(D2D1_RECT_F rect, float rightTrim = 0.0f) {
        const unsigned seq = g_mediaHitSeq.load(std::memory_order_relaxed);
        g_mediaHitSeq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        g_mediaHitLeft.store(rect.left, std::memory_order_relaxed);
        g_mediaHitTop.store(rect.top, std::memory_order_relaxed);
        g_mediaHitRight.store(rect.right, std::memory_order_relaxed);
        g_mediaHitBottom.store(rect.bottom, std::memory_order_relaxed);
        g_mediaHitScale.store(g_settings.sizeScale, std::memory_order_relaxed);
        g_mediaHitTrim.store(rightTrim, std::memory_order_relaxed);
        g_mediaHitStamp.store(GetTickCount64(), std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        g_mediaHitSeq.store(seq + 2, std::memory_order_relaxed);
    }

    // Corner radius for the island mask in *content* space -- the coordinate
    // system DrawMedia and DrawIdleDashboard are handed, which DrawPill scales
    // by sizeScale afterwards. Because that scale is applied later, these radii
    // must not be pre-multiplied by sizeScale. They previously were, so the
    // stadium corner arc grew with the size scale until it cut across the album
    // art's top-left corner (visible from roughly 2x upwards).
    float ContentIslandRadius(float contentHeight) const {
        if (g_settings.notchStyle) {
            return 16.0f;
        }
        if (g_settings.w11Style) {
            return 8.0f;
        }
        return std::min(contentHeight * 0.5f, 44.0f);
    }

