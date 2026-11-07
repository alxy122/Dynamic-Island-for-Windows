// ── Weather iconography ──────────────────────────────────────────────────────
// The dashboard used colour emoji (☀️ ⛅ 🌡️) rendered with ENABLE_COLOR_FONT,
// which clashed badly with the island's monochrome, accent-tinted UI and varied
// in size and baseline between glyphs. These are drawn as vectors instead: they
// inherit the accent colour, scale cleanly, sit on a predictable baseline, and
// can never fall back to a missing-glyph box the way a font-dependent icon can.
enum class WeatherVisual {
    Clear,
    PartlyCloudy,
    Cloudy,
    Fog,
    Storm,
    Rain,
    Snow,
    Unknown,
};

// Groupings mirror GetWeatherIconAndText so both stay keyed off the same WWO
// weather codes.
WeatherVisual WeatherVisualFromCode(int code) {
    switch (code) {
        case 113:
            return WeatherVisual::Clear;
        case 116:
            return WeatherVisual::PartlyCloudy;
        case 119: case 122:
            return WeatherVisual::Cloudy;
        case 143: case 248: case 260:
            return WeatherVisual::Fog;
        case 200: case 386: case 389: case 392: case 395:
            return WeatherVisual::Storm;
        case 176: case 263: case 266: case 281: case 284: case 293: case 296:
        case 299: case 302: case 305: case 308: case 311: case 314: case 353:
        case 356: case 359:
            return WeatherVisual::Rain;
        case 179: case 182: case 185: case 227: case 230: case 317: case 320:
        case 323: case 326: case 329: case 332: case 335: case 338: case 350:
        case 362: case 365: case 368: case 371:
            return WeatherVisual::Snow;
        default:
            return WeatherVisual::Unknown;
    }
}

// Cache for DrawKaraokeLetterPop: the layout and per-cluster hit rects only
// depend on the active lyric line's text/format/box size, never on time, so
// they are rebuilt only when the line changes. Rects are stored relative to a
// (0,0) layout origin.
struct KaraokeLayoutCache {
    struct ClusterInfo {
        UINT32 textStart = 0;
        UINT32 textLength = 0;
        bool isWhitespace = true;
        D2D1_RECT_F rect{};
    };

    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float layoutWidth = 0.0f;
    float layoutHeight = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    std::vector<ClusterInfo> clusters;
    bool valid = false;
};

struct MarqueeLayoutCache {
    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float wrapWidth = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
};

// Measured geometry of the collapsed idle strip, produced by
// Renderer::MeasureIdleStrip. The render loop uses totalWidth to size the
// island; DrawIdleDashboard uses the per-slot widths to place the clock, divider
// and weather reading inside it.
struct IdleStripMetrics {
    float clockWidth = 0.0f;
    float weatherWidth = 0.0f;
    bool hasWeather = false;
    bool hasPrivacy = false;
    float totalWidth = IdleStripLayout::kMinWidth;
};

// One visible character of the song title, used by the title-change animation.
struct TitleGlyph {
    wchar_t ch0 = 0;       // first UTF-16 unit (used to match letters between titles)
    wchar_t ch1 = 0;       // second unit of a surrogate pair, otherwise 0
    float x = 0.0f;        // left edge inside the full title layout
    float sx = 0.0f;       // on-screen left edge (relative to the title rect) when the animation started
    bool  eligible = true; // visible when the animation started (only these get paired)
    float w = 0.0f;
    float h = 0.0f;
    float seed = 0.0f;     // stable 0..1 value so each letter's smoke is unique
    int match = -1;        // index of the paired glyph in the other title, or -1
    ComPtr<IDWriteTextLayout> layout;  // this character alone, same format as the title
};

// Marquee state of one title at the moment the animation starts, mirroring what
// DrawMarqueeText does (offset = fmod(now * speed, cycle)).
struct TitleScroll {
    bool scrolls = false;
    float width = 0.0f;
    float cycle = 0.0f;
    float offset0 = 0.0f;
};

// State for the gentle swap animation of the artist / album lines.
struct SwapTextState {
    std::wstring shown;               // text currently on screen
    std::wstring prev;                // text that is fading out
    double start = -1.0;              // animation start, -1 = idle
    double lastDrawn = -1.0;          // last frame this row was drawn
    MarqueeLayoutCache prevCache;     // layout cache for the outgoing text
};

struct MediaPillMetrics {
    float clockWidth = 0.0f;
    float sectionWidth = 0.0f;                       // clock + divider block on the left
    float totalWidth = MediaPillLayout::kBaseWidth;  // content space, before sizeScale
};

// Substitutes '0' for every decimal digit before measuring.
//
// Proportional faces give '1' a visibly narrower advance than '0', so measuring
// the live clock would change the pill's width as the time changed -- with
// seconds enabled that is a layered-window resize and reposition every second,
// and a visible twitch. Normalising to the widest digit makes the width stable
// for a given digit count, and since '0' is never narrower than the digit it
// replaces, the real string is guaranteed to fit the box we reserve.
std::wstring WidestDigitForm(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'0' && c <= L'9') {
            c = L'0';
        }
    }
    return text;
}

// ── Collapsed-pill lyrics ────────────────────────────────────────────────────
// Width of the collapsed media pill while it carries a lyric line (content px).
constexpr float kCollapsedLyricsWidth = 320.0f;

// Synced lyrics that belong to the track playing right now (same test as
// DrawCollapsedLyrics uses before it shows a lyric line).
bool CollapsedLyricsUsable(const SharedState& state) {
    return state.lyrics.hasData && state.lyrics.synced && !state.lyrics.lines.empty() &&
           state.lyrics.matchedTitle == state.media.title &&
           state.lyrics.matchedArtist == state.media.artist;
}

// Did the last track that was decided either way have lyrics? Bridges the
// look-up after a track change, see CollapsedLyricsActive. Render thread only.
std::atomic<bool> g_collapsedLyricsLastHad{false};

// True when the collapsed pill should be widened for lyrics.
//
// Only while there really are synced lyrics for the current track. A browser
// stream, a video or a podcast has none, and used to get a 320px pill that just
// repeated the page title while the clock, weather and the rest were hidden.
// While the lyrics thread is still looking up a new track, the pill keeps the
// width the previous track had, so an album does not shrink and regrow at every
// song change, and a video does not flash wide for a second.
bool CollapsedLyricsActive(const SharedState& state, const Settings& settings) {
    if (!settings.collapsedLyrics || !settings.media || !state.media.available) {
        return false;
    }
    if (CollapsedLyricsUsable(state)) {
        g_collapsedLyricsLastHad.store(true, std::memory_order_relaxed);
        return true;
    }
    const bool forThisTrack = state.lyrics.matchedTitle == state.media.title &&
                              state.lyrics.matchedArtist == state.media.artist;
    if (forThisTrack && !state.lyrics.fetching) {
        // Decided: not found, unsynced, or the request gave up.
        g_collapsedLyricsLastHad.store(false, std::memory_order_relaxed);
        return false;
    }
    // Still looking (or the lyrics thread has not seen the new track yet).
    return g_collapsedLyricsLastHad.load(std::memory_order_relaxed);
}

// One pre-built lyric line. Built once when the line changes, then reused every
// frame, so animating it costs a DrawTextLayout and nothing else.
struct CollapsedLyricLine {
    std::wstring text;
    ComPtr<IDWriteTextLayout> layout;
    float width = 0.0f;
    float height = 0.0f;
    bool isTitle = false;  // true when this is the media-title fallback, not a lyric line
};

