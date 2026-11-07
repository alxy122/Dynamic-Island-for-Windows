double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

// ── Lyrics sync offset ───────────────────────────────────────────────────────
// Convention: positive offset = lyrics appear EARLIER, negative = LATER.
// Draw code only reads the atomics, so nothing here takes a lock or polls anything.
constexpr double kLyricsBaseLookaheadSec = 0.49;  // built-in baseline; the user offset is added on top
constexpr int kLyricsOffsetLimitMs = 3000;

// Effective offset = Mod Settings value + right-click nudge, clamped. Called from
// LoadSettings() and from the right-click menu.
void PublishLyricsOffsets(int settingMs, int settingBtMs) {
    g_lyricsEffectiveOffsetMs.store(
        ClampInt(settingMs + Wh_GetIntValue(L"LyricsNudgeMs", 0),
                 -kLyricsOffsetLimitMs, kLyricsOffsetLimitMs),
        std::memory_order_relaxed);
    g_lyricsEffectiveOffsetBtMs.store(
        ClampInt(settingBtMs + Wh_GetIntValue(L"LyricsNudgeBtMs", 0),
                 -kLyricsOffsetLimitMs, kLyricsOffsetLimitMs),
        std::memory_order_relaxed);
}

// Seconds added to the reported playback position for every lyric surface.
double LyricsLookaheadSec() {
    const int offsetMs = g_defaultOutputIsBluetooth.load(std::memory_order_relaxed)
                             ? g_lyricsEffectiveOffsetBtMs.load(std::memory_order_relaxed)
                             : g_lyricsEffectiveOffsetMs.load(std::memory_order_relaxed);
    return kLyricsBaseLookaheadSec + static_cast<double>(offsetMs) / 1000.0;
}

// Smoothstep easing (ease in / ease out). Used by the Lyrics tab.
float SmoothStep01(float t) {
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ── Media source selection ───────────────────────────────────────────────────
int ActiveMediaSourceIndex(const SharedState& state) {
    for (size_t i = 0; i < state.mediaSources.size(); ++i) {
        if (state.mediaSources[i].aumid == state.media.sourceAppUserModelId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring GetPreferredMediaSource() {
    std::lock_guard lock(g_mediaSourceMutex);
    return g_preferredMediaSource;
}

// Instant switch. Empty AUMID = Auto. The timestamp tells MediaThreadProc this title
// change is not a "new track": no auto-expand, no 5s recentTrackChange window.
void RequestMediaSource(const std::wstring& aumid) {
    {
        std::lock_guard lock(g_mediaSourceMutex);
        // Dock pick: a TEMPORARY override, not a lock. MediaThreadProc drops it as soon as
        // something new starts playing or that app goes away, so Auto keeps working.
        g_dockOverrideAumid = aumid;
        g_dockOverrideRev.fetch_add(1);
    }
    g_userSourceSwitchAt.store(NowSeconds());
    if (g_mediaRefreshEvent) {
        SetEvent(g_mediaRefreshEvent);
    }
    g_layoutDirty = true;
}

// Right-click menu pick. Empty AUMID = Auto. A real AUMID LOCKS the island to that app
// until Auto or another app is picked; what starts playing elsewhere is ignored.
// Any temporary dock pick is cleared, so "Auto" really means "newest playing app".
void LockMediaSource(const std::wstring& aumid) {
    {
        std::lock_guard lock(g_mediaSourceMutex);
        g_preferredMediaSource.assign(aumid);
        g_dockOverrideAumid.clear();
        g_dockOverrideRev.fetch_add(1);
    }
    g_userSourceSwitchAt.store(NowSeconds());
    if (g_mediaRefreshEvent) {
        SetEvent(g_mediaRefreshEvent);
    }
    g_layoutDirty = true;
}

// Dock click. Animated when "Expanded player transitions" is on: the renderer slides
// the old content out first, then calls RequestMediaSource().
void RequestMediaSourceSwitch(const std::wstring& aumid) {
    int direction = 1;
    bool sameAsActive = false;
    {
        std::lock_guard lock(g_stateMutex);
        const int active = ActiveMediaSourceIndex(g_state);
        int target = -1;
        for (size_t i = 0; i < g_state.mediaSources.size(); ++i) {
            if (g_state.mediaSources[i].aumid == aumid) {
                target = static_cast<int>(i);
                break;
            }
        }
        sameAsActive = (target >= 0 && target == active);
        if (target >= 0 && active >= 0) {
            direction = target >= active ? 1 : -1;
        }
    }

    if (sameAsActive || !GetSettingsCopy().expandedMediaAnim) {
        RequestMediaSource(aumid);
        return;
    }
    {
        std::lock_guard lock(g_mediaSourceMutex);
        g_switchTarget = aumid;
    }
    g_switchDir.store(direction);
    g_switchSeq.fetch_add(1);
    g_layoutDirty = true;
}

// "Playing from %s" + "Brave" -> "Playing from Brave"
std::wstring FormatWithName(const wchar_t* format, const std::wstring& name) {
    std::wstring out = format;
    const size_t pos = out.find(L"%s");
    if (pos != std::wstring::npos) {
        out.replace(pos, 2, name);
    }
    return out;
}

// ── Dashboard tab loop ───────────────────────────────────────────────────────
// Single source of truth for how many tabs are in the mouse-wheel scroll loop.
// The first tab's meaning depends on context -- the media view when something is
// playing, the clock when idle -- but the *count* is identical, which is why the
// renderer, the scroll handler and the hit tests can all share this. That
// arithmetic used to be copy-pasted in seven places, which is exactly how they
// drifted out of sync.
// The appointments list tab sits right after the calendar. It exists once the
// feature is on and at least one calendar feed is configured.
bool AppointmentsTabEnabled(const Settings& settings) {
    return settings.appointments && !settings.appointmentCalendars.empty();
}

int ActiveTabCount(const Settings& settings) {
    int count = 2;  // primary (media or clock) + calendar
    if (AppointmentsTabEnabled(settings)) ++count;
    if (settings.weather) ++count;
    if (settings.hardwareMonitorModule) ++count;
    if (settings.fileTrayModule) ++count;
    if (settings.lyrics && g_lyricsTabAvailable.load(std::memory_order_relaxed)) ++count;
    return count;
}

int NormalizedTabIndex(const Settings& settings) {
    const int total = std::max(1, ActiveTabCount(settings));
    const int raw = g_idleTab.load(std::memory_order_relaxed);
    return ((raw % total) + total) % total;
}

// Tab order is always: primary, calendar, appointments?, weather?, hardware?, file tray?
// The expensive GPU / network counters are only sampled while the hardware card
// is actually on screen, so that check needs the card's real index rather than
// assuming it is the last tab -- which stopped being true once the File Tray
// was added after it.
// With "Pin lyrics as first tab" on (and a Lyrics tab present), the media view's
// order is: lyrics, media, calendar, ... so the media tab and every tab after it
// sit one position later. Returns the media tab's index: 0 normally, 1 when pinned.
int MediaTabIndex(const Settings& settings) {
    return (settings.lyricsPinnedFirst && settings.lyrics &&
            g_lyricsTabAvailable.load(std::memory_order_relaxed)) ? 1 : 0;
}

int HardwareMonitorTabIndex(const Settings& settings) {
    if (!settings.hardwareMonitorModule) {
        return -1;
    }
    return 2 + (AppointmentsTabEnabled(settings) ? 1 : 0) +
           (settings.weather ? 1 : 0) + MediaTabIndex(settings);
}

int FileTrayTabIndex(const Settings& settings) {
    if (!settings.fileTrayModule) {
        return -1;
    }
    return 2 + (AppointmentsTabEnabled(settings) ? 1 : 0) +
           (settings.weather ? 1 : 0) + (settings.hardwareMonitorModule ? 1 : 0) + MediaTabIndex(settings);
}

int AppointmentsTabIndex(const Settings& settings) {
    return AppointmentsTabEnabled(settings) ? 2 + MediaTabIndex(settings) : -1;
}

// Is the point (content space) on the Appointments tab's "+N more" line?
bool ApptMoreHitTest(const MediaContentPoint& cp) {
    if (!cp.valid || !g_apptMoreValid.load(std::memory_order_relaxed)) return false;
    return cp.x >= g_apptMoreL.load(std::memory_order_relaxed) &&
           cp.x <= g_apptMoreR.load(std::memory_order_relaxed) &&
           cp.y >= g_apptMoreT.load(std::memory_order_relaxed) &&
           cp.y <= g_apptMoreB.load(std::memory_order_relaxed);
}

// Moves the dashboard one tab (+1 = next, -1 = previous). Clamped at both ends:
// pushing past the first or last tab is ignored completely (no wrap, no bounce),
// so a free-spinning wheel or touchpad inertia can never make the edge tabs twitch.
void StepIdleTab(int direction) {
    const Settings settings = GetSettingsCopy();
    const int count = std::max(1, ActiveTabCount(settings));
    const int next = NormalizedTabIndex(settings) + direction;
    if (next < 0 || next >= count) {
        return;
    }
    g_idleTab = next;
    g_layoutDirty = true;
}

