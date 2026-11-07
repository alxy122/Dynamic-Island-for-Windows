class Renderer {
   public:
    bool Initialize(HWND hwnd) {
        hwnd_ = hwnd;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory),
                                       reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

        hr = d2dFactory_->CreateDCRenderTarget(&props, &target_);
        if (FAILED(hr)) {
            return false;
        }

        const Settings initialSettings = GetSettingsCopy();
        EnsureTextFormats(initialSettings.sizeScale, initialSettings.fontFamily,
                          initialSettings.textScale);

        return CreateBackingBitmap(520, 140);
    }

    bool Render(const SharedState& state, const Settings& settings, const Activity& primary,
                const std::optional<Activity>& secondary, float width, float height,
                float nudge, bool hover, bool pinned, double now) {
        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);

        width = std::max(width, 0.0f);
        if (UsesFixedStage(settings)) {
            // A pill can never be bigger than the stage it lives in.
            width = std::min(width, kStageContentWidth * settings.sizeScale);
            height = std::min(height, kStageContentHeight * settings.sizeScale);
        }

        const StageLayout stage = ComputeStageLayout(settings, width, height);
        const int pixelWidth = stage.width;
        const int pixelHeight = stage.height;

        const bool flushTop = settings.notchStyle || settings.borderMergedMode;
        const float top = stage.pillTop + (flushTop ? std::max(0.0f, nudge) : nudge);
        const float left = stage.pillLeft;

        const BYTE alpha = static_cast<BYTE>(Clamp(settings.pillOpacity, 0.35f, 1.0f) * 255.0f);
        const bool sizeChanged = pixelWidth != bitmapWidth_ || pixelHeight != bitmapHeight_;
        const bool moved = stage.origin.x != lastOrigin_.x || stage.origin.y != lastOrigin_.y;

        // Full repaint when anything about the window itself changed, plus once a
        // second as a safety net (sleep/resume, display changes).
        const bool fullPaint = !kUseDirtyRect || !stageValid_ || sizeChanged || moved ||
                               alpha != lastAlpha_ || (now - lastFullPaint_) > 1.0;

        if (sizeChanged) {
            if (!CreateBackingBitmap(pixelWidth, pixelHeight)) {
                return false;
            }
        } else if (g_layoutDirty.exchange(false)) {
            upkeepPending_ = true;
        }
        if (upkeepPending_) {
            if (now - lastUpkeepAt_ > 0.25) {
                upkeepPending_ = false;
                lastUpkeepAt_ = now;
                ApplyOverlayZOrder(hwnd_, pixelWidth, pixelHeight);
            } else {
                g_layoutDirty = true;  // retry once the throttle expires
            }
        }

        // Region that can contain pixels this frame: the pill plus its shadow and
        // nudge slack, unioned with where it was last frame so the old pixels get erased.
        RECT bounds;
        bounds.left = std::max(0, static_cast<int>(std::floor(left - kRenderPadX - 4.0f)));
        bounds.top = std::max(0, static_cast<int>(std::floor(top - kRenderPadY - 12.0f)));
        bounds.right = std::min(pixelWidth, static_cast<int>(std::ceil(left + width + kRenderPadX + 4.0f)));
        bounds.bottom = std::min(pixelHeight, static_cast<int>(std::ceil(top + height + kRenderPadY + 12.0f)));

        RECT dirty = {0, 0, pixelWidth, pixelHeight};
        if (!fullPaint) {
            UnionRect(&dirty, &lastBounds_, &bounds);
        }

        if (IsRectEmpty(&dirty)) {
            dirty = RECT{0, 0, pixelWidth, pixelHeight};
        }

        // Bind only the region being repainted. Binding the whole stage made D2D copy
        // all of it in at BeginDraw and out at EndDraw every frame, even for a small pill.
        // The bound rect's top-left becomes (0,0); the translation restores stage
        // coordinates, and every draw routine composes with the current transform.
        RECT rc = dirty;
        HRESULT hr = target_->BindDC(memDc_, &rc);
        if (FAILED(hr)) {
            return false;
        }

        pillCount_ = 0;  // DrawPill records each pill it draws for the backdrop windows

        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(dirty.left),
                                                            -static_cast<float>(dirty.top)));
        // Clear() honours the active clip, so this only wipes the dirty region.
        target_->PushAxisAlignedClip(
            D2D1::RectF(static_cast<float>(dirty.left), static_cast<float>(dirty.top),
                        static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        target_->Clear(D2D1::ColorF(0, 0.0f));
        BeginFrameResources();

        EnsureBrushes(settings, state, now);
        settingsOpacity_ = settings.pillOpacity;

        const bool gameMetricsPresent = primary.kind == IslandKind::Idle &&
            (settings.gameOverlay || g_gameOverlayPinned.load());
        // True while the island is collapsing to nothing (auto-hide / fullscreen).
        islandHiding_ = primary.width < 1.0f && primary.height < 1.0f && !secondary;

        const float hoverScale = (settings.expandOnHover && ((hover && !gameMetricsPresent) || pinned)) ? 1.025f : 1.0f;
        const float scale = hoverScale;
        hoverScale_ = scale;


        if (width >= 2.0f && height >= 2.0f) {
            if (secondary) {
                const float gap = 12.0f * settings.sizeScale;
                const float maxH = std::max(primary.height, secondary->height);
                // Notch / border-merged islands hang from the top edge, so the
                // shorter pill stays flush with it instead of being centred
                // inside the taller one's height (which pushed it down).
                const bool hangFromTop = settings.notchStyle || settings.borderMergedMode;
                const float pTop = hangFromTop ? top : top + (maxH - primary.height) * 0.5f;
                const float sTop = hangFromTop ? top : top + (maxH - secondary->height) * 0.5f;

                DrawPill(state, settings, primary,
                         D2D1::RectF(left, pTop, left + primary.width, pTop + primary.height),
                         scale, now);
                drawingSecondary_ = true;
                DrawPill(state, settings, *secondary,
                         D2D1::RectF(left + primary.width + gap, sTop,
                                      left + primary.width + gap + secondary->width,
                                      sTop + secondary->height),
                         scale, now);
                drawingSecondary_ = false;
            } else {
                DrawPill(state, settings, primary,
                         D2D1::RectF(left, top, left + width, top + height), scale, now);
            }
        }

        target_->PopAxisAlignedClip();
        hr = target_->EndDraw();
        if (FAILED(hr)) {
            stageValid_ = false;
            return false;
        }

        POINT src = {0, 0};
        SIZE size = {bitmapWidth_, bitmapHeight_};
        POINT dst = stage.origin;  // constant while the pill animates

        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = alpha;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UPDATELAYEREDWINDOWINFO info = {};
        info.cbSize = sizeof(info);
        info.pptDst = &dst;
        info.psize = &size;
        info.hdcSrc = memDc_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = fullPaint ? nullptr : &dirty;  // only this rect is re-composited

        const bool presented = UpdateLayeredWindowIndirect(hwnd_, &info) != FALSE;
        if (presented) {
            stageValid_ = true;
            lastOrigin_ = stage.origin;
            lastBounds_ = bounds;
            lastAlpha_ = alpha;
            if (fullPaint) lastFullPaint_ = now;
            g_pillClientRect = PillClientRect{left, top, left + width, top + height, true};
        } else {
            stageValid_ = false;
        }

        if (presented) {
            DwmBackdrop::Sync(hwnd_, stage.origin, pillRects_, pillRadii_, pillCount_,
                              settings.notchStyle, settings.backdropMaterial, pillBgColor_,
                              settings.backdropTint);
        }
        return presented;
    }

    void Shutdown() {
        ReleaseCachedResources();
        marqueeTitleCache_.layout.Reset();
        marqueeArtistCache_.layout.Reset();
        marqueeAlbumCache_.layout.Reset();
        marqueeClipboardCache_.layout.Reset();
        marqueeNotificationCache_.layout.Reset();
        scratchColorBrush_.Reset();
        lyricsActiveTextFormat_.Reset();
        lyricsActiveFitTextFormat_.Reset();
        karaokeCache_.layout.Reset();

        artBitmap_.Reset();
        notificationIconBitmap_.Reset();
        mediaSourceIconBitmap_.Reset();
        clipboardIconBitmap_.Reset();
        clipboardImageBitmap_.Reset();
        accentBrush_.Reset();
        redBrush_.Reset();
        textBrush_.Reset();
        mutedBrush_.Reset();
        tintBrush_.Reset();
        shadowBrush_.Reset();
        micDotBrush_.Reset();
        micGlowBrush_.Reset();
        camDotBrush_.Reset();
        camGlowBrush_.Reset();
        weatherDescFormat_.Reset();
        micGlowBrush_.Reset();
        camDotBrush_.Reset();
        camGlowBrush_.Reset();
        target_.Reset();
        textFormat_.Reset();
        smallTextFormat_.Reset();
        boldTextFormat_.Reset();
        hugeTextFormat_.Reset();
        clockFormat_.Reset();
        iconFormat_.Reset();
        mediaPlayIconFormat_.Reset();
        mediaNavIconFormat_.Reset();
        idleTextFormat_.Reset();
        calDayLargeFormat_.Reset();
        calGridFormat_.Reset();
        dwriteFactory_.Reset();
        skipTriFull_.Reset();
        skipTriNotch_.Reset();
        titleOld_.clear();
        titleNew_.clear();
        titleAnimActive_ = false;
        roundJoinStyle_.Reset();
        artFlipOld_.Reset();
        pillArtOld_.Reset();
        pillArtLast_.Reset();
        pillArtStart_ = -1.0;
        pillArtInit_ = false;
        swapArtist_.prevCache.layout.Reset();
        swapAlbum_.prevCache.layout.Reset();
        d2dFactory_.Reset();

        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (memDc_) {
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
    }

    // Measures the collapsed idle strip so the render loop can size the island to
    // the text it is actually about to paint, instead of the old fixed 96/170px.
    // DrawIdleDashboard calls this too and lays the slots out from the same
    // numbers, so the pill can never be sized for a clock width the painter is
    // not using.
    IdleStripMetrics MeasureIdleStrip(const SharedState& state, const Settings& settings,
                                      double now) {
        // Idempotent and cheap when nothing changed, but necessary here: textScale
        // drives the idle font size, so measuring before the formats are rebuilt
        // would size the pill for the previous Text size setting.
        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);

        IdleStripMetrics metrics;
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring clock = FormatIslandTime(local, settings.clockFollowSystem,
                                                    settings.use24HourClock, settings.showSeconds);
        metrics.clockWidth =
            MeasureTextWidthCached(WidestDigitForm(clock), fmt, idleClockWidthCache_);

        metrics.hasWeather = settings.weather;
        if (metrics.hasWeather) {
            // Mirrors the label DrawIdleDashboard builds, including the no-data
            // placeholder, so the reserved slot matches what gets drawn.
            const bool hasData = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
            wchar_t label[32] = {};
            if (hasData) {
                std::wstring icon = L"\U0001F321\uFE0F";
                std::wstring desc = state.weather.weatherDesc;
                GetWeatherIconAndText(state.weather.weatherCode, icon, desc);
                swprintf_s(label, L"%s %.0f\x00B0", icon.c_str(), state.weather.temperature);
            } else {
                wcscpy_s(label, ARRAYSIZE(label), L"\U0001F321\uFE0F --\x00B0");
            }
            metrics.weatherWidth =
                MeasureTextWidthCached(WidestDigitForm(label), fmt, idleWeatherWidthCache_);
        }

        metrics.hasPrivacy =
            (state.system.micActive && settings.privacyDots && settings.privacyDotsMic) ||
            (state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam);

        float total = IdleStripLayout::kPadX * 2.0f + metrics.clockWidth;
        if (metrics.hasWeather) {
            total += IdleStripLayout::kSlotGap * 2.0f + IdleStripLayout::kDividerWidth +
                     metrics.weatherWidth;
        }
        if (metrics.hasPrivacy) {
            total += IdleStripLayout::kPrivacyReserve;
        }

        total = std::ceil(total / IdleStripLayout::kWidthQuantum) * IdleStripLayout::kWidthQuantum;
        metrics.totalWidth = Clamp(total, IdleStripLayout::kMinWidth, IdleStripLayout::kMaxWidth);
        return metrics;
    }

    // Collapsed media pill, with the optional clock. Returns content-space
    // sizes; the render loop multiplies by sizeScale like every other collapsed
    // width. Measured in the widest-digit form so the width does not wobble as
    // the digits change.
    //
    // Which readings go on the left:
    //  - no lyric line on screen and MediaPillInfo on: clock and weather, the same
    //    readings as the idle strip, so playing a video does not hide them;
    //  - otherwise, with MediaPillClock on: the clock alone (room for the lyrics);
    //  - otherwise nothing, and the pill is exactly the old 150px.
    // As the second, small pill next to a notification it shows the sound waves
    // only (no readings, no cover), see drawingSecondary_.
    MediaPillMetrics MeasureMediaPill(const SharedState& state, const Settings& settings) {
        MediaPillMetrics metrics;
        const float privacyShift = (state.system.micActive || state.system.cameraActive)
                                       ? MediaPillLayout::kPrivacyShift : 0.0f;
        if (drawingSecondary_) {
            metrics.showArt = false;
            metrics.totalWidth = MediaPillLayout::kWavesOnlyWidth + privacyShift;
            return metrics;
        }
        const bool infoMode = settings.mediaPillInfo && !CollapsedLyricsActive(state, settings);
        if (!infoMode && !settings.mediaPillClock) {
            return metrics;
        }

        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

        // The idle strip is never on screen together with the media pill, and the
        // caches are keyed on the text, so sharing them costs nothing.
        SYSTEMTIME local = {};
        GetLocalTime(&local);
        MediaPillSlot clock;
        clock.text = FormatIslandTime(local, settings.clockFollowSystem,
                                      settings.use24HourClock, settings.showSeconds);
        clock.width = MeasureTextWidthCached(WidestDigitForm(clock.text), fmt, idleClockWidthCache_);
        metrics.slots.push_back(std::move(clock));

        if (infoMode && settings.weather) {
            MediaPillSlot weather;
            weather.text = IdleWeatherLabel(state, NowSeconds());
            weather.width = MeasureTextWidthCached(WidestDigitForm(weather.text), fmt, idleWeatherWidthCache_);
            weather.colorFont = true;
            metrics.slots.push_back(std::move(weather));
        }

        float section = MediaPillLayout::kClockPadLeft;
        for (size_t i = 0; i < metrics.slots.size(); ++i) {
            if (i > 0) section += MediaPillLayout::kSlotGap;
            section += metrics.slots[i].width + MediaPillLayout::kSlotGap + MediaPillLayout::kDividerWidth;
        }
        section = std::ceil(section / MediaPillLayout::kWidthQuantum) * MediaPillLayout::kWidthQuantum;
        metrics.sectionWidth = section;
        if (infoMode) {
            // No title here: only the cover (if the player has one) and the waves.
            // A browser usually sends no cover, and the placeholder just looked odd.
            metrics.showArt = static_cast<bool>(state.media.art);
            metrics.totalWidth = section + privacyShift +
                                 (metrics.showArt ? MediaPillLayout::kInfoArtWidth
                                                  : MediaPillLayout::kInfoWavesWidth);
        } else {
            metrics.totalWidth = MediaPillLayout::kBaseWidth + section;
        }
        return metrics;
    }

    // Set by DrawPill while it paints the second pill of a split layout.
    bool drawingSecondary_ = false;

    // "☀️ 12°", or the placeholder while there is no fresh reading. Same text as
    // the idle strip shows.
    std::wstring IdleWeatherLabel(const SharedState& state, double now) {
        const bool hasData = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
        wchar_t label[32] = {};
        if (hasData) {
            std::wstring icon = L"\U0001F321️";
            std::wstring desc = state.weather.weatherDesc;
            GetWeatherIconAndText(state.weather.weatherCode, icon, desc);
            swprintf_s(label, L"%s %.0f\x00B0", icon.c_str(), state.weather.temperature);
        } else {
            wcscpy_s(label, ARRAYSIZE(label), L"\U0001F321️ --\x00B0");
        }
        return label;
    }

   private:
    // Keyed on the string alone. EnsureTextFormats invalidates both caches
    // whenever it rebuilds the formats, so a cached width can never outlive the
    // font size it was measured at -- comparing the IDWriteTextFormat pointer
    // would not be enough, since a rebuilt format can land on the freed address.
    struct TextWidthCache {
        std::wstring key;
        float width = 0.0f;
        bool valid = false;
    };
    TextWidthCache idleClockWidthCache_;
    TextWidthCache idleWeatherWidthCache_;

    float MeasureTextWidthCached(const std::wstring& text, IDWriteTextFormat* fmt,
                                 TextWidthCache& cache) {
        if (cache.valid && cache.key == text) {
            return cache.width;
        }

        float width = 0.0f;
        if (fmt && dwriteFactory_ && !text.empty()) {
            ComPtr<IDWriteTextLayout> layout;
            // Effectively unbounded wrap width: the idle formats are NO_WRAP, and
            // we want the natural advance, not a wrapped block.
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    text.c_str(), static_cast<UINT32>(text.size()), fmt,
                    4096.0f, IdleStripLayout::kHeight, &layout)) &&
                layout) {
                DWRITE_TEXT_METRICS tm = {};
                if (SUCCEEDED(layout->GetMetrics(&tm))) {
                    width = std::max(tm.width, tm.widthIncludingTrailingWhitespace);
                }
            }
        }

        cache.key = text;
        cache.width = width;
        cache.valid = true;
        return width;
    }

    bool CreateBackingBitmap(int width, int height) {
        const int logicalW = width;
        const int logicalH = height;
        // Expand/collapse changes the needed size almost every frame. Reuse the
        // existing DIB whenever it is big enough; UpdateLayeredWindow only reads
        // the (0,0,width,height) region, so a larger DIB is fine.
        if (dib_ && width <= bitmapCapW_ && height <= bitmapCapH_) {
            bitmapWidth_ = width;
            bitmapHeight_ = height;
            return true;
        }

        // Round up with headroom so growing animations rarely reallocate.
        width = ((width + 63) / 64) * 64 + 64;
        height = ((height + 31) / 32) * 32 + 32;

        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (!memDc_) {
            HDC screen = GetDC(nullptr);
            memDc_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!memDc_) {
                return false;
            }
        }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib_) {
            return false;
        }

        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dib_));
        bitmapCapW_ = width;
        bitmapCapH_ = height;
        bitmapWidth_ = logicalW;
        bitmapHeight_ = logicalH;
        return true;
    }

    float lastFontScale_ = 0.0f;
    float lastTextScale_ = 0.0f;
    std::wstring lastFontFamily_;
    // textScale is an independent typography multiplier (the Text size setting):
    // sizeScale magnifies the whole island via a transform, whereas this changes
    // only the type, so the island can stay compact while the clock and labels
    // get bigger.
    void EnsureTextFormats(float scale, const std::wstring& fontFamily, float textScale) {
        if (std::abs(scale - lastFontScale_) < 0.001f && fontFamily == lastFontFamily_ &&
            std::abs(textScale - lastTextScale_) < 0.001f) {
            return;
        }
        lastTextScale_ = textScale;
        const float ts = Clamp(textScale, 0.7f, 1.6f);

        // Every format below is about to be recreated at a new size, so any
        // width measured against the old ones is stale.
        idleClockWidthCache_.valid = false;
        idleWeatherWidthCache_.valid = false;

        textFormat_ = nullptr;
        smallTextFormat_ = nullptr;
        clockFormat_ = nullptr;
        boldTextFormat_ = nullptr;
        hugeTextFormat_ = nullptr;
        iconFormat_ = nullptr;
        mediaPlayIconFormat_ = nullptr;
        mediaNavIconFormat_ = nullptr;
        idleTextFormat_ = nullptr;
        calDayLargeFormat_ = nullptr;
        calGridFormat_ = nullptr;
        timeDashboardFormat_ = nullptr;
        dateDashboardFormat_ = nullptr;

        const wchar_t* defaultDisplay = L"Segoe UI Variable Display";
        const wchar_t* defaultSmall = L"Segoe UI Variable Small";
        const wchar_t* mainFamily = fontFamily.empty() ? defaultDisplay : fontFamily.c_str();
        const wchar_t* smallFamily = fontFamily.empty() ? defaultSmall : fontFamily.c_str();

        auto createFormat = [&](const wchar_t* family, const wchar_t* fallback,
                                DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                DWRITE_FONT_STRETCH stretch, float size,
                                ComPtr<IDWriteTextFormat>& out) {
            HRESULT hr = dwriteFactory_->CreateTextFormat(
                family, nullptr, weight, style, stretch, size, L"", &out);
            if (FAILED(hr) || !out) {
                if (fallback && wcscmp(family, fallback) != 0) {
                    hr = dwriteFactory_->CreateTextFormat(
                        fallback, nullptr, weight, style, stretch, size, L"", &out);
                }
                if (FAILED(hr) || !out) {
                    dwriteFactory_->CreateTextFormat(
                        L"Segoe UI", nullptr, weight, style, stretch, size, L"", &out);
                }
            }
        };

        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 13.5f * ts, textFormat_);
        createFormat(smallFamily, defaultSmall,
                     DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 11.0f * ts, smallTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 18.0f * ts, clockFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 12.0f * ts, boldTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 42.0f * ts, hugeTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 13.0f * ts, idleTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 63.0f * ts, calDayLargeFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 14.4f * ts, calGridFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 56.0f * ts, timeDashboardFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 16.0f * ts, dateDashboardFormat_);

        usingFluentIcons_ = false;
        ComPtr<IDWriteFontCollection> sysFonts;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->GetSystemFontCollection(&sysFonts, FALSE)) && sysFonts) {
            UINT32 fontIdx = 0;
            BOOL fontFound = FALSE;
            if (SUCCEEDED(sysFonts->FindFamilyName(L"Segoe Fluent Icons", &fontIdx, &fontFound)) && fontFound) {
                usingFluentIcons_ = true;
            }
        }

        const wchar_t* iconFontFamily = usingFluentIcons_ ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

        HRESULT hrIcon = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &iconFormat_);
        if (FAILED(hrIcon) || !iconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &iconFormat_);
        }

        HRESULT hrPlay = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &mediaPlayIconFormat_);
        if (FAILED(hrPlay) || !mediaPlayIconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &mediaPlayIconFormat_);
        }

        HRESULT hrNav = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &mediaNavIconFormat_);
        if (FAILED(hrNav) || !mediaNavIconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &mediaNavIconFormat_);
        }

        if (textFormat_) {
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (smallTextFormat_) {
            smallTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (boldTextFormat_) {
            boldTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            // Match every other centered format: without this the text hugs the
            // top of its layout rect, which left the weather dashboard's city /
            // icon / temperature visually "stuck" near the top of the island.
            boldTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (hugeTextFormat_) {
            hugeTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            hugeTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (clockFormat_) {
            clockFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            clockFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            clockFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (idleTextFormat_) {
            idleTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            idleTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            idleTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (calDayLargeFormat_) {
            calDayLargeFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            calDayLargeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            calDayLargeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (calGridFormat_) {
            calGridFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            calGridFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            calGridFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (timeDashboardFormat_) {
            timeDashboardFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            timeDashboardFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            timeDashboardFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (dateDashboardFormat_) {
            dateDashboardFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            dateDashboardFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dateDashboardFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (iconFormat_) {
            iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (mediaPlayIconFormat_) {
            mediaPlayIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            mediaPlayIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mediaPlayIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (mediaNavIconFormat_) {
            mediaNavIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            mediaNavIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mediaNavIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        if (fontFamily != lastFontFamily_) {
            marqueeTitleCache_.layout.Reset();
            marqueeArtistCache_.layout.Reset();
            marqueeAlbumCache_.layout.Reset();
            marqueeClipboardCache_.layout.Reset();
            marqueeNotificationCache_.layout.Reset();
            weatherDescFormat_.Reset();
            weatherDescFormatSize_ = -1.0f;
        }

        // The lyrics formats are built lazily in DrawLyricsDashboard; throw them
        // away so they pick up a new font / text size.
        lyricsActiveTextFormat_.Reset();
        lyricsActiveTextFormatSize_ = -1.0f;
        lyricsActiveFitTextFormat_.Reset();
        lyricsActiveFitTextFormatSize_ = -1.0f;
        karaokeCache_.layout.Reset();
        karaokeCache_.valid = false;

        // Collapsed lyric layouts were built from the old font; rebuild them.
        collapsedLyricCur_ = CollapsedLyricLine{};
        collapsedLyricPrev_ = CollapsedLyricLine{};
        collapsedLyricIdx_ = -2;

        lastFontScale_ = scale;
        lastFontFamily_ = fontFamily;
    }

