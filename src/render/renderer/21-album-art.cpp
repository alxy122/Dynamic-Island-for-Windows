    // ── Album art: flip ──────────────────────────────────────────────────────
    // The decoder assigns a fresh generation on every poll during the post-change
    // settle window even when the image is identical, so the flip is keyed on a
    // hash of the pixels instead and only fires when the picture really changed.
    uint64_t ArtContentHash(const MediaSnapshot& media) {
        if (!media.art) return 0;
        if (artHashValue_ != 0 && artHashGen_ == media.art->generation) return artHashValue_;
        uint64_t h = 1469598103934665603ull;
        h = (h ^ static_cast<uint64_t>(media.art->width)) * 1099511628211ull;
        h = (h ^ static_cast<uint64_t>(media.art->height)) * 1099511628211ull;
        const std::vector<uint8_t>& v = media.art->bgra;
        for (size_t i = 0; i < v.size(); i += 61) {
            h = (h ^ v[i]) * 1099511628211ull;
        }
        if (h == 0) h = 1;
        artHashGen_ = media.art->generation;
        artHashValue_ = h;
        return h;
    }

    void DrawArtFace(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius) {
        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        HRESULT hrMask = d2dFactory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &mask);
        ComPtr<ID2D1Layer> layer;
        HRESULT hrLayer = target_->CreateLayer(nullptr, &layer);
        const bool roundedClip = SUCCEEDED(hrMask) && SUCCEEDED(hrLayer) && mask && layer;
        if (roundedClip) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer.Get());
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        if (bitmap) {
            target_->DrawBitmap(bitmap, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            accentBrush_->SetOpacity(0.24f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
            accentBrush_->SetOpacity(1.0f);
        }
        if (roundedClip) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
    }

    void DrawAlbumArtFlip(const MediaSnapshot& media, D2D1_RECT_F rect, double now,
                          float radius, bool drawBadge) {
        constexpr double kFlipDuration = 0.62;
        const bool recentlyDrawn = artFlipLastDrawn_ >= 0.0 && (now - artFlipLastDrawn_) < 0.25;
        artFlipLastDrawn_ = now;

        const uint64_t hash = ArtContentHash(media);
        if (!artFlipInit_ || !recentlyDrawn) {
            artFlipInit_ = true;
            artFlipHash_ = hash;
            artFlipStart_ = -1.0;
            artFlipOld_.Reset();
        } else if (hash != artFlipHash_) {
            // Mid-flip changes just retarget the face shown after the turn.
            if (expandedAnim_ && artFlipStart_ < 0.0 && hash != 0) {
                artFlipOld_ = artBitmap_;  // still the previous cover: not rebuilt yet
                artFlipStart_ = now;
            }
            artFlipHash_ = hash;
        }

        float p = -1.0f;
        if (artFlipStart_ >= 0.0) {
            p = static_cast<float>((now - artFlipStart_) / kFlipDuration);
            if (p >= 1.0f) {
                artFlipStart_ = -1.0;
                artFlipOld_.Reset();
                p = -1.0f;
            }
        }
        if (p < 0.0f) {
            DrawAlbumArt(media, rect, now, radius, drawBadge);
            return;
        }

        g_layoutDirty = true;
        const float q = SkipEaseInOut(p);
        const float c = std::fabs(std::cos(3.14159265f * q));  // 1 = facing us, 0 = edge-on
        if (c < 0.02f) return;

        const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                                   (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        target_->SetTransform(D2D1::Matrix3x2F::Scale(c, 1.0f - 0.05f * (1.0f - c), center) * oldTransform);

        if (q < 0.5f) {
            DrawArtFace(artFlipOld_.Get(), rect, radius);
        } else {
            DrawAlbumArt(media, rect, now, radius, drawBadge);
        }

        // Darken the face as it turns away, for a sense of depth.
        ComPtr<ID2D1SolidColorBrush> shade;
        if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f * (1.0f - c)), &shade)) && shade) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), shade.Get());
        }
        target_->SetTransform(oldTransform);
    }

    // ── Album art (collapsed pill): record drop ──────────────────────────────
    // A track change plays as a record swap: the old disc spins up and is thrown
    // away while the new one drops in from above, spinning fast, then springs onto
    // the pill and slows to rest. Skipping backwards spins the other way. Same
    // cover, new track: a short press-and-rebound with the ring and glint.
    void EnsureArtBitmap(const MediaSnapshot& media) {
        if (!media.art) return;
        const BitmapPixels& art = *media.art;
        if (artGeneration_ != art.generation || !artBitmap_) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(art.width, art.height),
                                  art.bgra.data(), art.width * 4,
                                  &props, &artBitmap_);
            artGeneration_ = art.generation;
        }
    }

    // One disc: scaled about its centre, the picture spun inside a fixed round
    // mask. `lag` is how far the angle moved over the last few ms; it drives the
    // ghost copies drawn on top of the disc.
    void DrawArtDisc(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius,
                     float scale, float angle, float lag, float alpha) {
        if (alpha <= 0.004f || scale <= 0.02f) return;
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                              (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F base;
        target_->GetTransform(&base);
        const D2D1_MATRIX_3X2_F scaled = D2D1::Matrix3x2F::Scale(scale, scale, c) * base;
        target_->SetTransform(scaled);

        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        ComPtr<ID2D1Layer> layer;
        const bool clipped =
            SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(rect, radius, radius), &mask)) &&
            SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && mask && layer;
        if (clipped) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), Clamp(alpha, 0.0f, 1.0f)),
                               layer.Get());
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        const float speed = std::min(std::fabs(lag) / 16.0f, 1.0f);
        const int ghosts = (bitmap && speed > 0.05f) ? 3 : 0;
        for (int k = 0; k <= ghosts; ++k) {
            const float a = (k == 0) ? 1.0f : 0.30f * speed * (1.0f - 0.30f * static_cast<float>(k - 1));
            target_->SetTransform(
                D2D1::Matrix3x2F::Rotation(angle - lag * static_cast<float>(k), c) * scaled);
            if (bitmap) {
                target_->DrawBitmap(bitmap, rect, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                accentBrush_->SetOpacity(0.24f);
                target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
                accentBrush_->SetOpacity(1.0f);
            }
        }

        if (clipped) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
        target_->SetTransform(base);
    }

    // Accent ring leaving the rim + a soft glint sweeping across the disc.
    // ringP / sheenP: <0 or >1 = not running, otherwise progress 0..1.
    void DrawArtLandingFx(D2D1_RECT_F rect, float radius, float ringP, float sheenP, float dir) {
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                              (rect.top + rect.bottom) * 0.5f);
        const float r0 = std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f;

        if (sheenP > 0.0f && sheenP < 1.0f) {
            const float e = SkipEaseInOut(sheenP);
            const float pos = (e * 2.0f - 1.0f) * 1.3f * r0;
            const float half = 0.55f * r0;
            const float dx = 0.82f * dir, dy = -0.57f;
            const float peak = 0.34f * std::sin(3.14159265f * sheenP);
            D2D1_GRADIENT_STOP stops[3] = {
                {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, peak)},
                {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> coll;
            ComPtr<ID2D1LinearGradientBrush> glint;
            if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 3, &coll)) && coll &&
                SUCCEEDED(target_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(c.x + dx * (pos - half), c.y + dy * (pos - half)),
                        D2D1::Point2F(c.x + dx * (pos + half), c.y + dy * (pos + half))),
                    coll.Get(), &glint)) && glint) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), glint.Get());
            }
        }

        for (int i = 0; i < 2; ++i) {
            const float p = (ringP - 0.16f * static_cast<float>(i)) / (1.0f - 0.16f * static_cast<float>(i));
            if (p <= 0.0f || p >= 1.0f) continue;
            const float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
            const float a = (i == 0 ? 0.60f : 0.28f) * (1.0f - p) * (1.0f - p);
            const float rr = r0 + 0.8f + 4.2f * e;
            accentBrush_->SetOpacity(a);
            target_->DrawEllipse(D2D1::Ellipse(c, rr, rr), accentBrush_.Get(), 0.6f + 1.4f * (1.0f - p));
        }
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawAlbumArtDrop(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius) {
        constexpr double kFullDur = 0.95;
        constexpr double kPulseDur = 0.55;
        constexpr double kTitleWait = 0.9;  // how long to wait for a new cover after a title change
        const bool recentlyDrawn = pillArtLastDrawn_ >= 0.0 && (now - pillArtLastDrawn_) < 0.25;
        pillArtLastDrawn_ = now;

        const uint64_t hash = ArtContentHash(media);
        if (!pillArtInit_ || !recentlyDrawn || !pillCoverAnim_) {
            // First frame, the pill was off screen, or the animation is turned off: snap.
            pillArtInit_ = true;
            pillArtHash_ = hash;
            pillTitle_ = media.title;
            pillArtStart_ = -1.0;
            pillTitlePendingAt_ = -1.0;
            pillArtOld_.Reset();
            pillArtLast_.Reset();
        } else {
            float dir = 1.0f;
            if (lastSkipClickAt_ >= 0.0 && (now - lastSkipClickAt_) < 6.0) dir = lastSkipDir_;

            if (hash != pillArtHash_) {
                // The picture really changed (the decoder re-issues generations
                // for identical covers, hence the hash).
                if (pillArtStart_ < 0.0 && hash != 0) {
                    pillArtOld_ = pillArtLast_;  // what was on screen last frame; may be empty
                    pillArtStart_ = now;
                    pillArtPulse_ = false;
                    pillArtDir_ = dir;
                    pillArtFullAt_ = now;
                    pillTitlePendingAt_ = -1.0;
                }
                pillArtHash_ = hash;
            }

            if (media.title != pillTitle_) {
                const bool hadTitle = !pillTitle_.empty();
                pillTitle_ = media.title;
                // Only arm the pulse if no drop is running or just finished; the
                // metadata often arrives before the new cover does.
                if (hadTitle && !media.title.empty() && pillArtStart_ < 0.0 &&
                    (now - pillArtFullAt_) > 1.6) {
                    pillTitlePendingAt_ = now;
                }
            }
            if (pillTitlePendingAt_ >= 0.0) {
                if (pillArtStart_ >= 0.0) {
                    pillTitlePendingAt_ = -1.0;
                } else if (now - pillTitlePendingAt_ >= kTitleWait) {
                    // No new cover showed up: same album, so do the gentle pulse.
                    pillTitlePendingAt_ = -1.0;
                    pillArtOld_.Reset();
                    pillArtStart_ = now;
                    pillArtPulse_ = true;
                    pillArtDir_ = dir;
                } else {
                    g_layoutDirty = true;  // keep frames coming while we wait
                }
            }
        }

        EnsureArtBitmap(media);
        ID2D1Bitmap* fresh = !media.art ? nullptr : artBitmap_.Get();

        float p = -1.0f;
        if (pillArtStart_ >= 0.0) {
            p = static_cast<float>((now - pillArtStart_) / (pillArtPulse_ ? kPulseDur : kFullDur));
            if (p >= 1.0f) {
                pillArtStart_ = -1.0;
                pillArtOld_.Reset();
                p = -1.0f;
            }
        }

        if (p < 0.0f) {
            DrawAlbumArt(media, rect, now, radius, false);
        } else {
            g_layoutDirty = true;
            const float dir = pillArtDir_;

            if (!pillArtPulse_) {
                // Old disc: spins up and is thrown away behind the new one.
                if (pillArtOld_) {
                    const float u = Clamp(p / 0.40f, 0.0f, 1.0f);
                    if (u < 1.0f) {
                        const float up = std::max(u - 0.058f, 0.0f);
                        const float k = u * u * u, kp = up * up * up;
                        DrawArtDisc(pillArtOld_.Get(), rect, radius,
                                    1.0f - 0.26f * k,
                                    dir * 150.0f * k,
                                    dir * 150.0f * (k - kp),
                                    1.0f - SkipSmooth(u / 0.9f));
                    }
                }
                // New disc: drops in big and fast-spinning, springs down, slows to rest.
                if (p > 0.14f) {
                    const float u = Clamp((p - 0.14f) / 0.86f, 0.0f, 1.0f);
                    const float up = std::max(u - 0.027f, 0.0f);
                    const float settle = 1.0f - SkipSmooth((u - 0.8f) / 0.2f);
                    const float scale = 1.0f + 0.30f * std::exp(-5.2f * u) * std::cos(7.5f * u) * settle;
                    const float angle = -dir * 220.0f * std::pow(1.0f - u, 3.0f);
                    const float anglePrev = -dir * 220.0f * std::pow(1.0f - up, 3.0f);
                    DrawArtDisc(fresh, rect, radius, scale, angle, angle - anglePrev,
                                SkipSmooth(u / 0.28f));
                }
                DrawArtLandingFx(rect, radius, (p - 0.32f) / 0.60f, (p - 0.34f) / 0.46f, dir);
            } else {
                // Same cover: press-and-rebound in place.
                auto wob = [](float x) { return std::exp(-5.5f * x) * std::sin(11.0f * x); };
                const float w = wob(p);
                const float wp = wob(std::max(p - 0.04f, 0.0f));
                DrawArtDisc(fresh, rect, radius, 1.0f - 0.20f * w, dir * 18.0f * w,
                            dir * 18.0f * (w - wp), 1.0f);
                DrawArtLandingFx(rect, radius, (p - 0.08f) / 0.62f, (p - 0.12f) / 0.55f, dir);
            }
        }

        if (fresh) {
            pillArtLast_ = artBitmap_;
        } else {
            pillArtLast_.Reset();
        }
    }

