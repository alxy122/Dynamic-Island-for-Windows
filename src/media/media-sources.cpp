std::wstring FriendlyMediaSourceName(std::wstring_view source) {
    const std::wstring lower = ToLowerCopy(std::wstring(source));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YouTube";
    if (lower.find(L"spotify") != std::wstring::npos) return L"Spotify";
    if (lower.find(L"chrome") != std::wstring::npos) return L"Chrome";
    if (lower.find(L"msedge") != std::wstring::npos || lower.find(L"edge") != std::wstring::npos) return L"Edge";
    if (lower.find(L"firefox") != std::wstring::npos) return L"Firefox";
    if (lower.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (lower.find(L"wmplayer") != std::wstring::npos) return L"Windows Media";
    if (lower.find(L"zune") != std::wstring::npos || lower.find(L"media") != std::wstring::npos) return L"Media Player";

    std::wstring text(source);
    const size_t bang = text.find(L'!');
    if (bang != std::wstring::npos && bang + 1 < text.size()) {
        text.erase(0, bang + 1);
    }
    const size_t dot = text.find(L'.');
    if (dot != std::wstring::npos) {
        text.resize(dot);
    }
    return text.empty() ? L"Media" : text;
}

std::wstring MediaSourceBadge(std::wstring_view sourceName) {
    const std::wstring lower = ToLowerCopy(std::wstring(sourceName));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YT";
    if (lower.find(L"spotify") != std::wstring::npos) return L"SP";
    if (lower.find(L"chrome") != std::wstring::npos) return L"CH";
    if (lower.find(L"edge") != std::wstring::npos) return L"ED";
    if (lower.find(L"firefox") != std::wstring::npos) return L"FF";
    if (lower.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (sourceName.empty()) return L"\u25b6";
    std::wstring badge;
    badge.push_back(static_cast<wchar_t>(towupper(sourceName[0])));
    return badge;
}

bool WindowLooksLikeMediaSource(HWND hwnd, const std::wstring& sourceLower) {
    if (!IsWindowVisible(hwnd) || hwnd == g_hwnd) {
        return false;
    }

    std::wstring image;
    if (!ProcessImageNameForWindow(hwnd, &image)) {
        return false;
    }

    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
    if (base.empty()) {
        return false;
    }

    const std::wstring stem = StripExtension(base);
    return sourceLower.find(base) != std::wstring::npos ||
           (stem.size() >= 4 && sourceLower.find(stem) != std::wstring::npos) ||
           (base.find(L"chrome") != std::wstring::npos && sourceLower.find(L"chrome") != std::wstring::npos) ||
           (base.find(L"msedge") != std::wstring::npos && sourceLower.find(L"edge") != std::wstring::npos) ||
           (base.find(L"vlc") != std::wstring::npos && sourceLower.find(L"vlc") != std::wstring::npos);
}

// #62 / #114: short-form video feeds and browser noise should not take over the
// island. A blocklist entry is matched loosely against the friendly source name,
// the raw AUMID and the title, so a single entry like "tiktok" or "firefox.exe"
// catches both the desktop app and a browser tab. Blocked sources are skipped
// for media mode entirely (and therefore never auto-expand).
//
// Reads the blocklist under g_settingsMutex rather than taking a Settings
// reference: the caller is the render loop, and walking a std::vector<std::wstring>
// that LoadSettings() may be reassigning is a use-after-free, not just a stale
// read. The haystack is built outside the lock because that part allocates.
bool MediaExpandBlocked(const MediaSnapshot& media) {
    {
        // Fast path for the overwhelmingly common empty-blocklist case: one
        // uncontended lock, no allocation, no string work.
        std::lock_guard lock(g_settingsMutex);
        if (g_settings.mediaExpandBlocklist.empty()) {
            return false;
        }
    }

    const std::wstring haystack = ToLowerCopy(
        media.sourceName + L"\n" + media.sourceAppUserModelId + L"\n" + media.title);

    std::lock_guard lock(g_settingsMutex);
    for (const std::wstring& needle : g_settings.mediaExpandBlocklist) {
        if (!needle.empty() && haystack.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool IsBrowserMediaSource(std::wstring_view source) {
    const std::wstring lower = ToLowerCopy(std::wstring(source));
    return lower.find(L"chrome") != std::wstring::npos ||
           lower.find(L"edge") != std::wstring::npos ||
           lower.find(L"msedge") != std::wstring::npos ||
           lower.find(L"firefox") != std::wstring::npos ||
           lower.find(L"youtube") != std::wstring::npos;
}

std::wstring SiteBadgeFromTitle(std::wstring_view title) {
    const std::wstring lower = ToLowerCopy(std::wstring(title));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YT";
    if (lower.find(L"netflix") != std::wstring::npos) return L"NF";
    if (lower.find(L"prime video") != std::wstring::npos || lower.find(L"amazon") != std::wstring::npos) return L"PV";
    if (lower.find(L"disney") != std::wstring::npos) return L"D+";
    if (lower.find(L"hotstar") != std::wstring::npos) return L"HS";
    if (lower.find(L"spotify") != std::wstring::npos) return L"SP";
    if (lower.find(L"soundcloud") != std::wstring::npos) return L"SC";
    return L"WEB";
}

struct IconCacheEntry {
    DWORD pid = 0;
    std::wstring exePath;
    PixelsRef pixels;
    uint64_t lastUsed = 0;
};

std::mutex g_iconCacheMutex;
std::unordered_map<DWORD, IconCacheEntry> g_iconCacheByPid;
uint64_t g_iconCacheClock = 0;

PixelsRef GetCachedProcessIconPixels(DWORD pid, UINT size) {
    std::wstring exePath;
    ProcessImageNameForPid(pid, &exePath);

    {
        std::lock_guard lock(g_iconCacheMutex);
        auto it = g_iconCacheByPid.find(pid);
        if (it != g_iconCacheByPid.end() && it->second.exePath == exePath &&
            it->second.pixels->width == size) {
            it->second.lastUsed = ++g_iconCacheClock;
            return it->second.pixels;
        }
    }

    PixelsRef pixels;
    HICON icon = getProcessIcon(pid);
    if (icon) {
        pixels = IconToPixelsRef(icon, size);
        DestroyIcon(icon);
    }

    if (pixels) {
        std::lock_guard lock(g_iconCacheMutex);
        g_iconCacheByPid[pid] = IconCacheEntry{pid, exePath, pixels, ++g_iconCacheClock};
        if (g_iconCacheByPid.size() > 32) {
            auto oldest = g_iconCacheByPid.begin();
            for (auto it = g_iconCacheByPid.begin(); it != g_iconCacheByPid.end(); ++it) {
                if (it->second.lastUsed < oldest->second.lastUsed) {
                    oldest = it;
                }
            }
            g_iconCacheByPid.erase(oldest);
        }
    }

    return pixels;
}

PixelsRef GetWindowIconPixels(HWND hwnd, UINT size) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    PixelsRef pixels = GetCachedProcessIconPixels(pid, size);
    if (pixels) {
        return pixels;
    }

    HICON icon = getWindowIcon(hwnd);
    if (icon) {
        pixels = IconToPixelsRef(icon, size);
        DestroyIcon(icon);
    }
    return pixels;
}

bool IsIgnorableForegroundWindow(HWND hwnd, const std::wstring& title) {
    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    const std::wstring cls = ToLowerCopy(className);
    const std::wstring lowerTitle = ToLowerCopy(title);

    std::wstring image;
    ProcessImageNameForWindow(hwnd, &image);
    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));

    return title.empty() ||
           hwnd == g_hwnd ||
           cls == L"shell_traywnd" ||
           cls == L"workerw" ||
           cls == L"progman" ||
           lowerTitle.find(L"windhawk") != std::wstring::npos ||
           base == L"explorer.exe" ||
           base == L"windhawk.exe";
}

BOOL CALLBACK FindMediaSourceWindowProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<std::pair<const std::wstring*, HWND*>*>(lParam);
    if (WindowLooksLikeMediaSource(hwnd, *data->first)) {
        *data->second = hwnd;
        return FALSE;
    }
    return TRUE;
}

std::wstring FindBrowserMediaSiteBadge(const std::wstring& sourceAppUserModelId) {
    const std::wstring sourceLower = ToLowerCopy(sourceAppUserModelId);
    HWND found = nullptr;
    std::pair<const std::wstring*, HWND*> data{&sourceLower, &found};
    EnumWindows(FindMediaSourceWindowProc, reinterpret_cast<LPARAM>(&data));
    if (found) {
        wchar_t title[192] = {};
        GetWindowTextW(found, title, ARRAYSIZE(title));
        return SiteBadgeFromTitle(title);
    }
    return L"WEB";
}

PixelsRef FindMediaSourceIcon(const std::wstring& sourceAppUserModelId) {
    PixelsRef pixels;
    if (IsBrowserMediaSource(sourceAppUserModelId)) {
        // Continue searching for the browser window anyway to retrieve
        // the browser's app icon or the PWA's app icon.
    }

    const std::wstring sourceLower = ToLowerCopy(sourceAppUserModelId);
    HWND found = nullptr;
    std::pair<const std::wstring*, HWND*> data{&sourceLower, &found};
    EnumWindows(FindMediaSourceWindowProc, reinterpret_cast<LPARAM>(&data));
    if (found) {
        pixels = GetWindowIconPixels(found, 32);
    }
    return pixels;
}

struct FindAppIconData {
    const std::wstring* targetName;
    HWND bestHwnd = nullptr;
    std::unordered_map<DWORD, std::wstring> pidToProcessName;
};

BOOL CALLBACK FindAppIconWindowProc(HWND hwnd, LPARAM lParam) {
    auto* d = reinterpret_cast<FindAppIconData*>(lParam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (hwnd == g_hwnd) {
        return TRUE;
    }

    // 1. Fast path: Check window title first (lightweight check without opening process handles)
    wchar_t title[128] = {};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    std::wstring titleLower = ToLowerCopy(title);
    if (!titleLower.empty() && titleLower.find(*d->targetName) != std::wstring::npos) {
        d->bestHwnd = hwnd;
        return FALSE; // Found via title, stop enumeration
    }

    // 2. Slow path fallback: Check process executable name
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return TRUE;
    }

    std::wstring baseNoExe;
    auto it = d->pidToProcessName.find(pid);
    if (it != d->pidToProcessName.end()) {
        baseNoExe = it->second;
    } else {
        std::wstring image;
        if (ProcessImageNameForPid(pid, &image)) {
            std::wstring base = ToLowerCopy(BaseNameFromPath(image));
            baseNoExe = base;
            if (baseNoExe.size() > 4 && baseNoExe.substr(baseNoExe.size() - 4) == L".exe") {
                baseNoExe = baseNoExe.substr(0, baseNoExe.size() - 4);
            }
            d->pidToProcessName[pid] = baseNoExe;
        } else {
            d->pidToProcessName[pid] = L"";
        }
    }

    if (!baseNoExe.empty()) {
        if (baseNoExe.find(*d->targetName) != std::wstring::npos ||
            d->targetName->find(baseNoExe) != std::wstring::npos) {
            d->bestHwnd = hwnd;
            return FALSE; // Found via process name, stop enumeration
        }
    }

    return TRUE;
}

PixelsRef FindAppIconByName(const std::wstring& appName, UINT size) {
    PixelsRef pixels;
    if (appName.empty()) {
        return pixels;
    }

    const std::wstring appNameLower = ToLowerCopy(appName);
    FindAppIconData data;
    data.targetName = &appNameLower;

    EnumWindows(FindAppIconWindowProc, reinterpret_cast<LPARAM>(&data));

    if (data.bestHwnd) {
        pixels = GetWindowIconPixels(data.bestHwnd, size);
    }
    return pixels;
}

std::vector<uint8_t> ReadWinRtStreamBytes(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& reference) {
    std::vector<uint8_t> bytes;
    if (!reference) {
        return bytes;
    }

    auto stream = reference.OpenReadAsync().get();
    if (!stream) {
        return bytes;
    }

    const uint64_t size64 = stream.Size();
    if (size64 == 0 || size64 > 8 * 1024 * 1024) {
        return bytes;
    }

    const uint32_t size = static_cast<uint32_t>(size64);
    winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
    reader.LoadAsync(size).get();
    bytes.resize(size);
    reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    return bytes;
}

void TriggerNudge() {
    // If we're parked (auto-hidden), any real event must wake us regardless
    // of the normal throttle, or it'd never be seen until the next hook/
    // hotkey/fullscreen-recheck wake.
    const bool wasParked = g_autoHiddenParked.exchange(false, std::memory_order_relaxed);
    const double now = NowSeconds();
    const double previous = g_lastNudgeTime.load();
    if (!wasParked && now - previous < 0.45) {
        return;
    }
    g_lastNudgeTime = now;
    HWND hwnd = g_hwnd;
    if (hwnd) {
        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
    }
}

// Chooses which SMTC session the island should follow.
//
// GetCurrentSession() returns whatever Windows last treated as the foreground
// media app, which is often a stale or paused session belonging to a different
// player. Following it blindly meant a player that was genuinely playing could
// be ignored entirely -- the reason VLC frequently produced no reaction at all
// while Spotify worked. Prefer the current session when it is actually playing,
// otherwise the first session that is, and only then fall back to the current
// one so paused/idle state still shows.
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession SelectActiveMediaSession(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    const std::wstring& preferredAumid) {
    using PlaybackStatus =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    auto sessionIsPlaying = [](auto const& candidate) -> bool {
        if (!candidate) {
            return false;
        }
        try {
            return candidate.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
        } catch (...) {
            return false;
        }
    };

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession current = nullptr;
    try {
        current = manager.GetCurrentSession();
    } catch (...) {
    }

    // The user picked a source (dock / menu): follow it, playing or not.
    if (!preferredAumid.empty()) {
        try {
            for (auto const& candidate : manager.GetSessions()) {
                if (preferredAumid == candidate.SourceAppUserModelId().c_str()) {
                    return candidate;
                }
            }
        } catch (...) {
        }
    }

    if (sessionIsPlaying(current)) {
        return current;
    }

    try {
        for (auto const& candidate : manager.GetSessions()) {
            if (sessionIsPlaying(candidate)) {
                return candidate;
            }
        }
    } catch (...) {
    }

    return current;
}

// Exe name for path-style AUMIDs (classic desktop apps), friendly name otherwise.
std::wstring MediaSourceDisplayName(const std::wstring& aumid) {
    std::wstring name = FriendlyMediaSourceName(aumid);
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos ||
        name.find(L':') != std::wstring::npos) {
        name = StripExtension(BaseNameFromPath(aumid));
        if (!name.empty()) {
            name[0] = static_cast<wchar_t>(towupper(name[0]));
        }
    }
    return name.empty() ? std::wstring(L"Media") : name;
}

struct MediaSourceTracker {
    // Auto mode / dock pick bookkeeping (see ChooseMediaSourceAumid).
    std::unordered_map<std::wstring, bool> wasPlaying;      // playing state at the previous poll
    std::unordered_map<std::wstring, uint64_t> startedSeq;  // when each source last STARTED playing (higher = newer)
    uint64_t seqCounter = 0;
    std::wstring dockOverride;                              // temporary dock pick; empty = none
    uint64_t dockOverrideBaseSeq = 0;                       // any start newer than this ends the dock pick
    unsigned dockOverrideRevSeen = 0;
    std::vector<std::wstring> order;                     // first seen = first in the dock
    std::unordered_map<std::wstring, PixelsRef> icons;
    std::unordered_map<std::wstring, int> iconRetry;     // polls before searching for an icon again
};

bool CollectMediaSources(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    MediaSourceTracker& tracker, std::vector<MediaSourceInfo>* out) {
    using PlaybackStatus =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    std::vector<std::pair<std::wstring, bool>> live;  // AUMID, playing
    try {
        for (auto const& session : manager.GetSessions()) {
            std::wstring aumid = session.SourceAppUserModelId().c_str();
            if (aumid.empty()) continue;
            bool playing = false;
            try {
                playing = session.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
            } catch (...) {
            }
            auto it = std::find_if(live.begin(), live.end(),
                                   [&](const auto& e) { return e.first == aumid; });
            if (it != live.end()) {
                it->second = it->second || playing;
            } else {
                live.emplace_back(std::move(aumid), playing);
            }
        }
    } catch (...) {
        return false;  // keep whatever the dock showed before
    }

    // Remember when each source last STARTED playing (not playing -> playing).
    // Auto mode follows the newest start.
    {
        std::wstring currentAumid;
        try {
            auto current = manager.GetCurrentSession();
            if (current) {
                currentAumid = current.SourceAppUserModelId().c_str();
            }
        } catch (...) {
        }

        // A source that disappeared forgets its history, so reopening it counts as a fresh start.
        for (auto it = tracker.wasPlaying.begin(); it != tracker.wasPlaying.end();) {
            const std::wstring& id = it->first;
            const bool stillLive = std::any_of(live.begin(), live.end(),
                                               [&](const auto& e) { return e.first == id; });
            if (stillLive) {
                ++it;
            } else {
                tracker.startedSeq.erase(id);
                it = tracker.wasPlaying.erase(it);
            }
        }

        std::vector<std::wstring> started;
        for (const auto& e : live) {
            bool& was = tracker.wasPlaying[e.first];
            if (e.second && !was) {
                started.push_back(e.first);
            }
            was = e.second;
        }
        // Several sources starting inside one poll: the one Windows calls "current" counts as newest.
        std::stable_sort(started.begin(), started.end(),
                         [&](const std::wstring& a, const std::wstring& b) {
                             return a != currentAumid && b == currentAumid;
                         });
        for (const std::wstring& id : started) {
            tracker.startedSeq[id] = ++tracker.seqCounter;
        }
    }

    // Stable order: new sources go on the end, vanished ones drop out.
    tracker.order.erase(
        std::remove_if(tracker.order.begin(), tracker.order.end(),
                       [&](const std::wstring& id) {
                           return std::none_of(live.begin(), live.end(),
                                               [&](const auto& e) { return e.first == id; });
                       }),
        tracker.order.end());
    for (const auto& e : live) {
        if (std::find(tracker.order.begin(), tracker.order.end(), e.first) == tracker.order.end()) {
            tracker.order.push_back(e.first);
        }
    }
    for (auto it = tracker.icons.begin(); it != tracker.icons.end();) {
        if (std::find(tracker.order.begin(), tracker.order.end(), it->first) == tracker.order.end()) {
            tracker.iconRetry.erase(it->first);
            it = tracker.icons.erase(it);
        } else {
            ++it;
        }
    }

    out->clear();
    for (const std::wstring& id : tracker.order) {
        MediaSourceInfo info;
        info.aumid = id;
        info.name = MediaSourceDisplayName(id);
        info.badge = MediaSourceBadge(info.name);
        for (const auto& e : live) {
            if (e.first == id) info.playing = e.second;
        }

        PixelsRef& icon = tracker.icons[id];
        if (!icon) {  // window enumeration is not free, so retry rarely
            int& wait = tracker.iconRetry[id];
            if (wait <= 0) {
                icon = FindMediaSourceIcon(id);
                wait = 8;
            } else {
                --wait;
            }
        }
        info.icon = icon;
        out->push_back(std::move(info));
    }
    return true;
}

// Which source should the island show?  Returns "" to fall back to the system's current session.
//   1. Dock pick: temporary. Ends when something NEW starts playing anywhere, or when
//      that app goes away. After that, the lock or Auto applies again.
//   2. Locked app (right-click menu): wins over Auto.
//   3. Auto: the playing app that started most recently. If it closes, the next most
//      recent playing app. With nothing playing: the app that played last.
std::wstring ChooseMediaSourceAumid(MediaSourceTracker& tracker, const std::wstring& lockedAumid) {
    auto exists = [&](const std::wstring& id) {
        return std::find(tracker.order.begin(), tracker.order.end(), id) != tracker.order.end();
    };
    auto seqOf = [&](const std::wstring& id) -> uint64_t {
        auto it = tracker.startedSeq.find(id);
        return it == tracker.startedSeq.end() ? 0 : it->second;
    };

    // Pick up a new dock request (or an Auto reset from the menu).
    const unsigned rev = g_dockOverrideRev.load();
    if (rev != tracker.dockOverrideRevSeen) {
        tracker.dockOverrideRevSeen = rev;
        {
            std::lock_guard lock(g_mediaSourceMutex);
            tracker.dockOverride = g_dockOverrideAumid;
        }
        tracker.dockOverrideBaseSeq = tracker.seqCounter;
    }

    if (!tracker.dockOverride.empty()) {
        bool somethingNewStarted = false;
        for (const std::wstring& id : tracker.order) {
            if (seqOf(id) > tracker.dockOverrideBaseSeq) {
                somethingNewStarted = true;
                break;
            }
        }
        if (!exists(tracker.dockOverride) || somethingNewStarted) {
            tracker.dockOverride.clear();  // back to the lock / Auto
        } else {
            return tracker.dockOverride;
        }
    }

    if (!lockedAumid.empty() && exists(lockedAumid)) {
        return lockedAumid;
    }

    // Auto. Playing sources beat paused ones; within each group the newest start wins.
    std::wstring best;
    bool bestPlaying = false;
    uint64_t bestSeq = 0;
    for (const std::wstring& id : tracker.order) {
        const auto pit = tracker.wasPlaying.find(id);
        const bool playing = pit != tracker.wasPlaying.end() && pit->second;
        const uint64_t seq = seqOf(id);
        if (best.empty() || (playing && !bestPlaying) || (playing == bestPlaying && seq > bestSeq)) {
            best = id;
            bestPlaying = playing;
            bestSeq = seq;
        }
    }
    if (!bestPlaying && bestSeq == 0) {
        return std::wstring();  // nothing has ever played: let Windows decide
    }
    return best;
}

