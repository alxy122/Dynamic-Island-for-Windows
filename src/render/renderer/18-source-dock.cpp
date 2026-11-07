    // ── Source switch animation + source dock ────────────────────────────────
    enum { kSwapNone = 0, kSwapOut, kSwapHold, kSwapIn };

    // Out: old content slides away and fades. Hold: nothing drawn while the media
    // thread switches sessions. In: new content slides in. The request is only sent
    // once Out has finished, so old and new content are never visible together.
    void UpdateSourceSwap(const SharedState& state, double now, float* alpha, float* dx) {
        *alpha = 1.0f;
        *dx = 0.0f;

        // Media tab was off screen for a while: don't resume mid-animation.
        if (swapPhase_ != kSwapNone && swapLastUpdate_ >= 0.0 && now - swapLastUpdate_ > 0.5) {
            if (swapPhase_ == kSwapOut) RequestMediaSource(swapTarget_);
            swapPhase_ = kSwapNone;
        }
        swapLastUpdate_ = now;

        if (!expandedAnim_ && swapPhase_ != kSwapNone) {  // animation switched off mid-swap
            if (swapPhase_ == kSwapOut) RequestMediaSource(swapTarget_);
            swapPhase_ = kSwapNone;
        }

        // New request. One that arrives during slide-in waits for it to finish.
        const unsigned seq = g_switchSeq.load();
        if (seq != swapSeenSeq_ && swapPhase_ != kSwapIn) {
            swapSeenSeq_ = seq;
            {
                std::lock_guard lock(g_mediaSourceMutex);
                swapTarget_ = g_switchTarget;
            }
            swapDir_ = g_switchDir.load() >= 0 ? 1.0f : -1.0f;
            if (!expandedAnim_) {
                RequestMediaSource(swapTarget_);
            } else if (swapPhase_ == kSwapNone) {
                swapPhase_ = kSwapOut;
                swapStart_ = now;
            } else if (swapPhase_ == kSwapHold) {
                RequestMediaSource(swapTarget_);  // retarget while nothing is visible
                swapStart_ = now;
            }                                     // kSwapOut: just retargeted
        }

        switch (swapPhase_) {
            case kSwapOut: {
                const float t = Clamp(static_cast<float>((now - swapStart_) / MediaLayout::kSwitchOutSec), 0.0f, 1.0f);
                const float e = t * t;  // ease-in
                *alpha = 1.0f - e;
                *dx = -swapDir_ * MediaLayout::kSwitchShift * e;  // opposite to the switch direction
                if (t >= 1.0f) {
                    RequestMediaSource(swapTarget_);
                    swapPhase_ = kSwapHold;
                    swapStart_ = now;
                    *alpha = 0.0f;
                }
                break;
            }
            case kSwapHold:
                *alpha = 0.0f;
                *dx = -swapDir_ * MediaLayout::kSwitchShift;
                if (state.media.sourceAppUserModelId == swapTarget_ ||
                    now - swapStart_ > MediaLayout::kSwitchHoldTimeoutSec) {
                    CancelMediaChangeAnims();
                    swapPhase_ = kSwapIn;
                    swapStart_ = now;
                }
                break;
            case kSwapIn: {
                const float t = Clamp(static_cast<float>((now - swapStart_) / MediaLayout::kSwitchInSec), 0.0f, 1.0f);
                const float inv = 1.0f - t;
                const float e = 1.0f - inv * inv * inv;  // ease-out cubic
                *alpha = e;
                *dx = swapDir_ * MediaLayout::kSwitchShift * (1.0f - e);
                if (t >= 1.0f) {
                    swapPhase_ = kSwapNone;
                    *alpha = 1.0f;
                    *dx = 0.0f;
                }
                break;
            }
            default:
                break;
        }
        if (swapPhase_ != kSwapNone) g_layoutDirty = true;
    }

    // The title smoke, art flip, artist/album swap and spectrum ripple all key off "was
    // this drawn a moment ago". Forget that so the new source simply appears.
    void CancelMediaChangeAnims() {
        titleAnimActive_ = false;
        titleOld_.clear();
        titleNew_.clear();
        titleLastDrawn_ = -1.0;
        artFlipStart_ = -1.0;
        artFlipOld_.Reset();
        artFlipLastDrawn_ = -1.0;
        swapArtist_.start = -1.0;
        swapArtist_.lastDrawn = -1.0;
        swapAlbum_.start = -1.0;
        swapAlbum_.lastDrawn = -1.0;
        specStart_ = -1.0;
        specLastDrawn_ = -1.0;
    }

    // Fades (and optionally shifts) everything drawn until EndSwapGroup.
    bool BeginSwapGroup(float alpha, float dx) {
        if (alpha >= 0.999f && std::fabs(dx) < 0.01f) return false;
        if (!swapGroupLayer_ &&
            (FAILED(target_->CreateLayer(&swapGroupLayer_)) || !swapGroupLayer_)) return false;
        target_->GetTransform(&swapGroupOldTransform_);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0.0f) * swapGroupOldTransform_);
        target_->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::IdentityMatrix(), Clamp(alpha, 0.0f, 1.0f), nullptr,
                                  D2D1_LAYER_OPTIONS_NONE),
            swapGroupLayer_.Get());
        return true;
    }

    void EndSwapGroup(bool began) {
        if (!began) return;
        target_->PopLayer();
        target_->SetTransform(swapGroupOldTransform_);
        swapGroupLayer_.Reset();
    }

    // "Playing from Brave" / "Paused in Spotify", 11px, tertiary text.
    void DrawSourceLine(const MediaSnapshot& media, D2D1_RECT_F area, double now) {
        if (media.sourceName.empty() || !smallTextFormat_) return;
        const std::wstring text =
            FormatWithName(Loc(media.playing ? L"Playing from %s" : L"Paused in %s"), media.sourceName);
        if (!scratchColorBrush_) {
            target_->CreateSolidColorBrush(material_.textTertiary, &scratchColorBrush_);
        } else {
            scratchColorBrush_->SetColor(material_.textTertiary);
        }
        if (!scratchColorBrush_) return;
        DrawMarqueeText(text, area, smallTextFormat_.Get(), scratchColorBrush_.Get(), now, 26.0f,
                        marqueeSourceCache_);
    }

    // One rounded pill, one circular slot per source. Shown only with 2+ sources.
    void DrawSourceDock(const SharedState& state, D2D1_RECT_F rect, double now) {
        const int sourceCount = static_cast<int>(state.mediaSources.size());
        if (sourceCount < 2) {
            dockInit_ = false;
            return;
        }

        const int liveIdx = ActiveMediaSourceIndex(state);
        const DockSlots slots = ComputeDockSlots(sourceCount, liveIdx);  // same inputs as the hit test
        if (slots.count < 2) return;

        // During a switch the ring already points at the source being switched to.
        int shownIdx = liveIdx;
        if (swapPhase_ != kSwapNone && !swapTarget_.empty()) {
            for (int i = 0; i < sourceCount; ++i) {
                if (state.mediaSources[i].aumid == swapTarget_) {
                    shownIdx = i;
                    break;
                }
            }
        }
        auto slotOf = [&](int sourceIdx) {
            for (int s = 0; s < slots.count; ++s) {
                if (slots.sourceIndex[s] == sourceIdx) return s;
            }
            return -1;
        };
        int targetSlot = slotOf(shownIdx);
        if (targetSlot < 0) targetSlot = slotOf(liveIdx);
        if (targetSlot < 0) targetSlot = 0;

        // ── Animation state ──────────────────────────────────────────────────
        const bool fresh = dockInit_ && dockLastDrawn_ >= 0.0 && (now - dockLastDrawn_) < 0.25 &&
                           dockSlotCount_ == slots.count;
        const float dt = fresh ? Clamp(static_cast<float>(now - dockLastDrawn_), 0.001f, 0.05f) : 0.016f;
        dockLastDrawn_ = now;

        if (!fresh) {
            dockInit_ = true;
            dockSlotCount_ = slots.count;
            dockIndPos_ = dockIndFrom_ = dockIndTarget_ = static_cast<float>(targetSlot);
            dockIndStart_ = -1.0;
            for (int i = 0; i < MediaLayout::kDockMaxSlots; ++i) {
                dockHover_[i] = 0.0f;
                dockPress_[i] = 0.0f;
            }
        } else if (static_cast<float>(targetSlot) != dockIndTarget_) {
            dockIndFrom_ = dockIndPos_;
            dockIndTarget_ = static_cast<float>(targetSlot);
            dockIndStart_ = now;
        }
        if (dockIndStart_ >= 0.0) {
            const float p = Clamp(static_cast<float>((now - dockIndStart_) / MediaLayout::kDockSlideSec), 0.0f, 1.0f);
            dockIndPos_ = dockIndFrom_ + (dockIndTarget_ - dockIndFrom_) * SkipEaseInOut(p);
            if (p >= 1.0f) {
                dockIndPos_ = dockIndTarget_;
                dockIndStart_ = -1.0;
            }
            g_layoutDirty = true;
        }

        const int hovered = g_hoveredSourceSlot.load(std::memory_order_relaxed);
        const int pressed = g_pressedSourceSlot.load(std::memory_order_relaxed);
        bool moving = false;
        for (int i = 0; i < slots.count; ++i) {
            const float hoverTarget = (i == hovered) ? 1.0f : 0.0f;
            dockHover_[i] += (hoverTarget - dockHover_[i]) * (1.0f - std::exp(-dt / 0.09f));
            if (std::fabs(hoverTarget - dockHover_[i]) < 0.004f) dockHover_[i] = hoverTarget; else moving = true;

            const float pressTarget = (i == pressed) ? 1.0f : 0.0f;
            const float tau = pressTarget > dockPress_[i] ? 0.025f : 0.075f;  // same feel as mediaBtnPress_
            dockPress_[i] += (pressTarget - dockPress_[i]) * (1.0f - std::exp(-dt / tau));
            if (std::fabs(pressTarget - dockPress_[i]) < 0.004f) dockPress_[i] = pressTarget; else moving = true;
        }
        if (moving) g_layoutDirty = true;

        // ── Drawing ──────────────────────────────────────────────────────────
        const float cy = rect.top + MediaLayout::kControlsY;
        const float x0 = rect.left + MediaLayout::kDockLeft;
        const D2D1_RECT_F pill = D2D1::RectF(x0, cy - MediaLayout::kDockHeight * 0.5f,
                                             x0 + MediaLayout::DockWidth(slots.count),
                                             cy + MediaLayout::kDockHeight * 0.5f);
        DrawCard(pill, MediaLayout::kDockHeight * 0.5f);

        // The pill's real colour (raised over the island base), used for the dot's ring.
        const D2D1_COLOR_F pillBg = MixColor(WithAlpha(material_.base, 1.0f),
                                             WithAlpha(material_.raised, 1.0f), material_.raised.a);

        // Active indicator: slides between slots.
        const float stride = MediaLayout::kDockSlot + MediaLayout::kDockGap;
        const float indCx = rect.left + MediaLayout::DockSlotLeft(0) + dockIndPos_ * stride +
                            MediaLayout::kDockSlot * 0.5f;
        const float indR = MediaLayout::kDockSlot * 0.5f;
        ComPtr<ID2D1SolidColorBrush> indFill;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &indFill)) && indFill) {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(indCx, cy), indR, indR), indFill.Get());
        }
        accentBrush_->SetOpacity(1.0f);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(indCx, cy), indR - 0.75f, indR - 0.75f),
                             accentBrush_.Get(), 1.5f);

        for (int i = 0; i < slots.count; ++i) {
            const D2D1_POINT_2F c = D2D1::Point2F(
                rect.left + MediaLayout::DockSlotLeft(i) + MediaLayout::kDockSlot * 0.5f, cy);
            const float active = Clamp(1.0f - std::fabs(dockIndPos_ - static_cast<float>(i)), 0.0f, 1.0f);
            const float lit = std::max(active, dockHover_[i]);
            const float opacity = 0.5f + 0.5f * lit;  // 50% idle, 100% active / hovered
            const float s = 1.0f - 0.10f * Clamp(dockPress_[i], 0.0f, 1.0f);
            const D2D1_RECT_F slotRect = D2D1::RectF(c.x - 13.0f, c.y - 13.0f, c.x + 13.0f, c.y + 13.0f);

            D2D1_MATRIX_3X2_F oldTransform;
            target_->GetTransform(&oldTransform);
            target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * oldTransform);

            const int srcIdx = slots.sourceIndex[i];
            if (srcIdx < 0) {
                wchar_t more[16] = {};
                swprintf_s(more, L"+%d", slots.overflow);
                if (boldTextFormat_) {
                    mutedBrush_->SetOpacity(0.55f + 0.45f * lit);
                    target_->DrawTextW(more, static_cast<UINT32>(wcslen(more)), boldTextFormat_.Get(),
                                       slotRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    mutedBrush_->SetOpacity(0.75f);
                }
            } else {
                const MediaSourceInfo& src = state.mediaSources[srcIdx];
                if (src.icon) {
                    DrawCircularBitmapPixels(*src.icon, c, MediaLayout::kDockIcon * 0.5f,
                                             dockIconBitmap_[i], dockIconGen_[i], opacity);
                } else if (boldTextFormat_ && !src.badge.empty()) {
                    textBrush_->SetOpacity(opacity);
                    target_->DrawTextW(src.badge.c_str(), static_cast<UINT32>(src.badge.size()),
                                       boldTextFormat_.Get(), slotRect, textBrush_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    textBrush_->SetOpacity(0.96f);
                }

                if (src.playing) {  // 7px green dot, bottom-right, 1.5px ring in the pill colour
                    const D2D1_POINT_2F d = D2D1::Point2F(c.x + 9.5f, c.y + 9.5f);
                    ComPtr<ID2D1SolidColorBrush> ring, dot;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(pillBg, &ring)) && ring) {
                        target_->FillEllipse(D2D1::Ellipse(d, 5.0f, 5.0f), ring.Get());
                    }
                    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &dot)) && dot) {
                        target_->FillEllipse(D2D1::Ellipse(d, 3.5f, 3.5f), dot.Get());
                    }
                }
            }
            target_->SetTransform(oldTransform);
        }
    }

