    HWND hwnd_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    int bitmapWidth_ = 0;     // logical size of the frame being drawn
    int bitmapHeight_ = 0;
    int bitmapCapW_ = 0;      // allocated size of the DIB (>= logical size)
    int bitmapCapH_ = 0;

    // Fixed-stage bookkeeping (see Render)
    POINT lastOrigin_ = {0, 0};
    RECT lastBounds_ = {0, 0, 0, 0};
    BYTE lastAlpha_ = 0;
    bool stageValid_ = false;
    double lastFullPaint_ = -1e9;

    // Pills drawn this frame, in client px, for the DWM backdrop windows (#59).
    D2D1_RECT_F pillRects_[DwmBackdrop::kMaxPills] = {};
    float pillRadii_[DwmBackdrop::kMaxPills] = {};
    int pillCount_ = 0;

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> smallTextFormat_;
    ComPtr<IDWriteTextFormat> boldTextFormat_;
    ComPtr<IDWriteTextFormat> hugeTextFormat_;
    ComPtr<IDWriteTextFormat> clockFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    ComPtr<IDWriteTextFormat> mediaPlayIconFormat_;
    ComPtr<IDWriteTextFormat> mediaNavIconFormat_;
    bool usingFluentIcons_ = true;
    ComPtr<IDWriteTextFormat> idleTextFormat_;
    ComPtr<IDWriteTextFormat> calDayLargeFormat_;
    ComPtr<IDWriteTextFormat> calGridFormat_;
    ComPtr<IDWriteTextFormat> timeDashboardFormat_;
    ComPtr<IDWriteTextFormat> dateDashboardFormat_;
    ComPtr<ID2D1SolidColorBrush> accentBrush_;
    ComPtr<ID2D1SolidColorBrush> redBrush_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    ComPtr<ID2D1SolidColorBrush> tintBrush_;
    ComPtr<ID2D1SolidColorBrush> shadowBrush_;
    ComPtr<ID2D1SolidColorBrush> micDotBrush_;
    ComPtr<ID2D1SolidColorBrush> micGlowBrush_;
    ComPtr<ID2D1SolidColorBrush> camDotBrush_;
    ComPtr<ID2D1SolidColorBrush> camGlowBrush_;
    ComPtr<ID2D1SolidColorBrush> scratchColorBrush_;
    MarqueeLayoutCache marqueeTitleCache_;
    MarqueeLayoutCache marqueeArtistCache_;
    MarqueeLayoutCache marqueeAlbumCache_;
    MarqueeLayoutCache marqueeClipboardCache_;
    MarqueeLayoutCache marqueeNotificationCache_;
    ComPtr<ID2D1Bitmap> artBitmap_;
    ComPtr<ID2D1Bitmap> notificationIconBitmap_;
    ComPtr<ID2D1Bitmap> mediaSourceIconBitmap_;
    ComPtr<ID2D1Bitmap> clipboardIconBitmap_;
    ComPtr<ID2D1Bitmap> clipboardImageBitmap_;
    uint64_t artGeneration_ = 0;
    SYSTEMTIME calendarCachedDate_{};
    std::wstring calendarCachedMonthName_;
    std::wstring calendarCachedWeekdayName_;
    std::array<std::wstring, 7> calendarCachedInitials_;  // indexed 0 = Sunday
    int calendarCachedFirstDay_ = 0;                       // resolved, 0 = Sunday
    int calendarCachedFirstDaySetting_ = -2;               // -2 forces the first fill
    ComPtr<IDWriteTextFormat> weatherDescFormat_;
    float weatherDescFormatSize_ = -1.0f;
    uint64_t notificationIconGeneration_ = 0;
    uint64_t mediaSourceIconGeneration_ = 0;
    uint64_t clipboardIconGeneration_ = 0;
    uint64_t clipboardImageGeneration_ = 0;
    ComPtr<ID2D1Bitmap> fileTrayIconBitmap_;
    uint64_t fileTrayIconGeneration_ = 0;
    ComPtr<ID2D1Bitmap> songIdCoverBitmap_;
    uint64_t songIdCoverGeneration_ = 0;
    MarqueeLayoutCache marqueeSongTitleCache_;
    MarqueeLayoutCache marqueeSongArtistCache_;

    // ── Lyrics tab state ─────────────────────────────────────────────────────
    KaraokeLayoutCache karaokeCache_;

    // ── Collapsed lyrics state ───────────────────────────────────────────────
    CollapsedLyricLine collapsedLyricCur_;
    CollapsedLyricLine collapsedLyricPrev_;
    int collapsedLyricIdx_ = -2;            // -2 = nothing built yet (forces a snap)
    std::wstring collapsedLyricTitle_;
    std::wstring collapsedLyricArtist_;
    double collapsedLyricAnimStart_ = -10.0;

    // ── Media-tab lyrics panel state ─────────────────────────────────────────
    CollapsedLyricLine sideLyricCur_;
    CollapsedLyricLine sideLyricPrev_;
    int sideLyricIdx_ = -2;                 // -2 = nothing built yet (forces a snap)
    std::wstring sideLyricTitle_;
    std::wstring sideLyricArtist_;
    std::wstring sideLyricFamily_;
    float sideLyricScale_ = -1.0f;
    double sideLyricAnimStart_ = -10.0;
    double sideLyricLastDrawn_ = -1.0;

    // ── Immersive lyrics state ───────────────────────────────────────────────
    float immersiveMixLin_ = 0.0f;
    double immersiveMixLast_ = -1.0;
    double immLastDrawn_ = -1.0;
    float immScroll_ = 0.0f;
    float immVel_ = 0.0f;
    float immGlow_ = 0.0f;
    std::wstring immTrackKey_;
    std::unordered_map<int, CollapsedLyricLine> immCache_;
    ComPtr<IDWriteTextFormat> immFormat_;
    float immFmtScale_ = -1.0f;
    std::wstring immFmtFamily_;
    MarqueeLayoutCache marqueeImmersiveCache_;
    LinearGradientSlot immKaraokeGrad_;
    float lyricsScrollPos_ = 0.0f;
    float lyricsScrollAnimStartPos_ = 0.0f;
    float lyricsScrollAnimTargetPos_ = 0.0f;
    double lyricsScrollAnimStartTime_ = -1.0;
    std::wstring lyricsScrollTrackKey_;
    int lyricsHighlightIndex_ = -1;
    int lyricsHighlightPrevIndex_ = -1;
    double lyricsHighlightFadeStartTime_ = -1.0;
    ComPtr<IDWriteTextFormat> lyricsActiveTextFormat_;
    float lyricsActiveTextFormatSize_ = -1.0f;
    ComPtr<IDWriteTextFormat> lyricsActiveFitTextFormat_;
    float lyricsActiveFitTextFormatSize_ = -1.0f;
    float lyricsWaveformAlpha_ = 0.0f;
    double lyricsWaveformFadeTime_ = -1.0;
    double lyricsLastDrawn_ = -1.0;  // last frame the lyrics tab was painted
    float  hoverScale_ = 1.0f;       // pill scale set by Render(); gives the resting expanded size
    float  islandSizeLastW_ = 0.0f;  // island resize tracking (see DrawMedia)
    float  islandSizeLastH_ = 0.0f;
    double islandSizeMovedAt_ = -1e9;
    bool   islandSettled_ = true;    // false while the island is still springing to a new size
    float  lyricsFxMix_ = 1.0f;      // 0 while resizing, eases to 1 after; scales lyric effects
    double lyricsFxLast_ = -1.0;
    float settingsOpacity_ = 0.96f;
    D2D1_COLOR_F pillBgColor_ = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f);
    // Design tokens for the current frame, rebuilt by EnsureBrushes.
    MaterialTokens material_{};
    // Accent color lerp: smoothly transition between successive sampled accents
    // so track changes don't produce a jarring instant color pop.
    D2D1_COLOR_F currentAccent_ = D2D1::ColorF(0x4cc9f0);
    double       lastAccentTime_ = -1.0;  // -1 = not yet set (will snap on first frame)
    float        mediaBtnPress_[3] = {0.0f, 0.0f, 0.0f};
    double       lastMediaBtnTime_ = -1.0;
    ComPtr<ID2D1PathGeometry> skipTriFull_;   // skip icons: rounded triangle, left edge at x=0
    ComPtr<ID2D1PathGeometry> skipTriNotch_;  // same, with the front triangle's gap cut out
    unsigned     skipSeenTrigger_[2] = {0, 0};
    double       skipAnimStart_[2] = {-1.0, -1.0};
    float        skipProgress_[2] = {-1.0f, -1.0f};  // index 0 = previous, 1 = next; <0 = idle, else 0..1
    double       lastSkipClickAt_ = -1.0;
    float        lastSkipDir_ = 1.0f;
    float        playMorph_ = 0.0f;  // play/pause morph: 0 = pause bars, 1 = play triangle
    float        playMorphFrom_ = 0.0f;
    float        playMorphTarget_ = 0.0f;
    double       playMorphStart_ = -1.0;
    double       playMorphLastDrawn_ = -1.0;
    bool         playMorphInit_ = false;
    ComPtr<ID2D1StrokeStyle> roundJoinStyle_;
    std::vector<TitleGlyph> titleOld_;  // title change: shared letters slide, the rest dissolve
    std::vector<TitleGlyph> titleNew_;
    std::wstring titleShown_;
    bool         titleShownPlaceholder_ = false;
    bool         titleAnimActive_ = false;
    double       titleAnimStart_ = 0.0;
    double       titleLastDrawn_ = -1.0;
    float        titleAnimDur_ = 1.0f;
    float        titleAvail_ = 0.0f;
    TitleScroll  titleOldScroll_;
    TitleScroll  titleNewScroll_;
    SwapTextState swapArtist_;
    SwapTextState swapAlbum_;
    ComPtr<ID2D1Bitmap> artFlipOld_;  // album art flip
    double       artFlipStart_ = -1.0;
    double       artFlipLastDrawn_ = -1.0;
    uint64_t     artFlipHash_ = 0;
    uint64_t     artHashGen_ = 0;
    uint64_t     artHashValue_ = 0;
    bool         artFlipInit_ = false;
    ComPtr<ID2D1Bitmap> pillArtOld_;    // pill cover change: outgoing disc during a drop
    ComPtr<ID2D1Bitmap> pillArtLast_;   // bitmap drawn in the previous pill frame
    double       pillArtStart_ = -1.0;  // -1 = idle
    double       pillArtLastDrawn_ = -1.0;
    double       pillArtFullAt_ = -1.0; // when the last full drop started
    double       artOuterLast_ = -1.0;  // shared art pass: last frame it ran
    float        artOuterAlpha_ = 1.0f; // shared art pass: eased opacity
    bool         upkeepPending_ = false;  // z-order upkeep requested, throttled in Render()
    double       lastUpkeepAt_ = -1e9;    // last time ApplyOverlayZOrder actually ran
    bool         islandHiding_ = false; // target size is 0 (auto-hide / fullscreen): art shrinks with the pill
    float        artMorphLin_ = 0.0f;   // art circle->square progress (linear, before easing)
    double       artMorphLast_ = -1.0;
    float        artBadgeAlpha_ = 1.0f; // source badge opacity on the expanded art
    double       pillTitlePendingAt_ = -1.0;
    uint64_t     pillArtHash_ = 0;
    std::wstring pillTitle_;
    float        pillArtDir_ = 1.0f;    // +1 clockwise (next), -1 counter-clockwise (previous)
    bool         pillArtPulse_ = false; // same cover, new track
    bool         pillArtInit_ = false;
    bool         expandedAnim_ = false;   // Settings::expandedMediaAnim, latched per frame
    bool         pillCoverAnim_ = false;  // Settings::pillCoverAnim, latched per frame
    bool         specInit_ = false;     // audio spectrum state
    double       specLastDrawn_ = -1.0;
    double       specStart_ = -1.0;     // -1 = no track-change animation running
    size_t       specKey_ = 0;
    float        specT_ = -1.0f;        // 0..1 animation progress, <0 when idle
    float        specDir_ = 1.0f;
    float        specPhase_ = 0.0f;
    float        specBass_ = 0.0f;
    float        specBassSlow_ = 0.0f;
    float        specKick_ = 0.0f;
    float        specPeak_[kSpectrumBands] = {};
    float        specPeakVel_[kSpectrumBands] = {};
    float        progressAmp_ = 1.0f;   // wavy/squiggle bar: wave strength, eases to 0 while paused
    double       progressLastTime_ = -1.0;

    // ── Source switch + dock ────────────────────────────────────────────────
    int          swapPhase_ = 0;        // kSwapNone
    double       swapStart_ = 0.0;
    double       swapLastUpdate_ = -1.0;
    float        swapDir_ = 1.0f;
    unsigned     swapSeenSeq_ = 0;
    std::wstring swapTarget_;
    ComPtr<ID2D1Layer> swapGroupLayer_;
    D2D1_MATRIX_3X2_F swapGroupOldTransform_ = D2D1::Matrix3x2F::Identity();
    bool         dockInit_ = false;
    int          dockSlotCount_ = 0;
    double       dockLastDrawn_ = -1.0;
    float        dockIndPos_ = 0.0f;
    float        dockIndFrom_ = 0.0f;
    float        dockIndTarget_ = 0.0f;
    double       dockIndStart_ = -1.0;
    float        dockHover_[MediaLayout::kDockMaxSlots] = {};
    float        dockPress_[MediaLayout::kDockMaxSlots] = {};
    ComPtr<ID2D1Bitmap> dockIconBitmap_[MediaLayout::kDockMaxSlots];
    uint64_t     dockIconGen_[MediaLayout::kDockMaxSlots] = {};
    MarqueeLayoutCache marqueeSourceCache_;

    // ── Tab transition state ────────────────────────────────────────────────
    int          tabContext_ = -1;      // 0 = media view, 1 = idle view (ids mean different things)
    int          tabCount_ = 0;
    bool         tabInit_ = false;      // false = next draw snaps to the selected tab
    bool         tabAnimating_ = false;
    float        tabPos_ = 0.0f;        // continuous position the spring moves
    float        tabTarget_ = 0.0f;
    float        tabVel_ = 0.0f;
    float        tabDir_ = 1.0f;        // +1 = last move went forward, -1 = backward
    double       tabLastDrawn_ = -1.0;
    ComPtr<ID2D1Layer> tabLayer_;       // reused layer for the tab fade

    // ── Cached resources (see "Cached Direct2D resources") ──────────────────
    std::vector<ComPtr<ID2D1SolidColorBrush>> brushPool_;
    size_t brushPoolUsed_ = 0;
    std::vector<ComPtr<ID2D1Layer>> layerPool_;
    size_t layerPoolUsed_ = 0;
    ComPtr<ID2D1SolidColorBrush> raisedBrush_;        // DrawCard fill
    ComPtr<ID2D1SolidColorBrush> raisedStrongBrush_;  // active card, badges, tracks
    LinearGradientSlot depthGrad_;
    LinearGradientSlot trackGrad_;
    RadialGradientSlot bloomGrad_;
    ShapeEntry shapeCache_[kShapeCacheSize];
    uint64_t shapeClock_ = 0;
};

