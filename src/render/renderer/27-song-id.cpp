    // ── Song recognition card ────────────────────────────────────────────────
    void DrawSongId(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        (void)settings;
        const SongIdSnapshot& sid = state.songId;
        const float h = rect.bottom - rect.top;
        if (h < 40.0f || rect.right - rect.left < 200.0f) return;

        const bool busy = sid.status == SongIdStatus::Listening || sid.status == SongIdStatus::Matching;
        const bool found = sid.status == SongIdStatus::Found;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = h - 16.0f;
        const D2D1_RECT_F badge = D2D1::RectF(rect.left + 14.0f, cy - badgeSz * 0.5f,
                                              rect.left + 14.0f + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.30f;
        const D2D1_POINT_2F bc = D2D1::Point2F((badge.left + badge.right) * 0.5f, cy);

        if (raisedStrongBrush_) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), raisedStrongBrush_.Get());
        }

        if (found && sid.cover) {
            DrawRoundedBitmapPixels(*sid.cover, badge, br, songIdCoverBitmap_, songIdCoverGeneration_, 1.0f);
        } else if (busy) {
            // Pulsing rings that breathe with the live input level.
            const float level = Clamp(sid.level, 0.0f, 1.0f);
            target_->PushAxisAlignedClip(badge, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = 0; i < 3; ++i) {
                const float phase = static_cast<float>(std::fmod(now * 0.85 + i / 3.0, 1.0));
                const float r = badgeSz * (0.16f + 0.40f * phase);
                accentBrush_->SetOpacity(Clamp((1.0f - phase) * (0.30f + 0.60f * level), 0.0f, 1.0f));
                target_->DrawEllipse(D2D1::Ellipse(bc, r, r), accentBrush_.Get(), 1.6f);
            }
            const float dotR = badgeSz * (0.14f + 0.06f * level);
            accentBrush_->SetOpacity(0.95f);
            target_->FillEllipse(D2D1::Ellipse(bc, dotR, dotR), accentBrush_.Get());
            accentBrush_->SetOpacity(1.0f);
            target_->PopAxisAlignedClip();
        } else if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity(found ? 0.95f : 0.50f);
            target_->DrawTextW(L"\uE8D6", 1, iconFormat_.Get(), badge, textBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            textBrush_->SetOpacity(0.90f);
        }

        const float tx = badge.right + 14.0f;
        const float tr = rect.right - 14.0f;

        if (found) {
            const wchar_t* hint = sid.url.empty() ? L"Shazam" : L"Shazam  \u00b7  Tap to open";
            mutedBrush_->SetOpacity(0.50f);
            target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), smallTextFormat_.Get(),
                               D2D1::RectF(tx, cy - 27.0f, tr, cy - 12.0f), mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textBrush_->SetOpacity(0.96f);
            DrawMarqueeText(sid.title, D2D1::RectF(tx, cy - 12.0f, tr, cy + 6.0f), textFormat_.Get(),
                            textBrush_.Get(), now, 34.0f, marqueeSongTitleCache_);
            mutedBrush_->SetOpacity(0.80f);
            DrawMarqueeText(sid.artist, D2D1::RectF(tx, cy + 6.0f, tr, cy + 22.0f),
                            smallTextFormat_.Get(), mutedBrush_.Get(), now, 28.0f,
                            marqueeSongArtistCache_);
        } else {
            std::wstring label = L"Song recognition";
            std::wstring text;
            if (busy) {
                label += sid.micSource ? L"  \u00b7  Microphone" : L"  \u00b7  System audio";
                if (sid.status == SongIdStatus::Listening) {
                    const int remaining = std::max(
                        0, static_cast<int>(std::ceil(sid.listenSeconds - (now - sid.startedAt))));
                    text = remaining > 0 ? L"Listening\u2026  " + std::to_wstring(remaining) + L"s"
                                         : std::wstring(L"Listening\u2026");
                } else {
                    text = L"Matching\u2026";
                }
            } else {
                text = sid.message;
            }

            mutedBrush_->SetOpacity(0.50f);
            target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), smallTextFormat_.Get(),
                               D2D1::RectF(tx, cy - 18.0f, tr, cy - 4.0f), mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textBrush_->SetOpacity(0.95f);
            DrawMarqueeText(text, D2D1::RectF(tx, cy - 4.0f, tr, cy + 12.0f), textFormat_.Get(),
                            textBrush_.Get(), now, 34.0f, marqueeSongTitleCache_);

            if (busy) {
                const D2D1_RECT_F track = D2D1::RectF(tx, cy + 15.0f, tr, cy + 19.0f);
                if (sid.status == SongIdStatus::Listening) {
                    const float progress = sid.listenSeconds > 0.1f
                        ? Clamp(static_cast<float>((now - sid.startedAt) / sid.listenSeconds), 0.0f, 1.0f)
                        : 0.0f;
                    DrawAccentTrack(track, progress, 2.0f);
                } else {
                    if (raisedStrongBrush_) {
                        target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.0f, 2.0f),
                                                      raisedStrongBrush_.Get());
                    }
                    const float tw = tr - tx;
                    const float seg = tw * 0.32f;
                    const float ph = static_cast<float>(std::fmod(now, 1.0));
                    const float x0 = tx - seg + (tw + seg) * ph;
                    const D2D1_RECT_F part = D2D1::RectF(std::max(tx, x0), track.top,
                                                         std::min(tr, x0 + seg), track.bottom);
                    if (part.right > part.left) {
                        if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(material_.accent, 0.95f))) {
                            target_->FillRoundedRectangle(D2D1::RoundedRect(part, 2.0f, 2.0f), b);
                        }
                    }
                }
            }
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

