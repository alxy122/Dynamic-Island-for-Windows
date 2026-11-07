// ==USAGE-PARSE-BEGIN==
// -- Claude usage: reading the limits file (pure, unit-tested off-Windows) -------
// Claude Code hands its plan limits to the status line script and nowhere else.
// That script (claude-usage-statusline.ps1) mirrors them into a small JSON file:
//   {"updated_at":N,"five_hour":{"used_percentage":X,"resets_at":T},"seven_day":{...}}
// with epoch seconds. This reads that file; it never talks to Anthropic.
namespace usage {

struct Window {
    bool ok = false;
    double pct = 0.0;       // 0..100
    int64_t resetsAt = 0;   // epoch seconds
};

struct Data {
    Window five;
    Window seven;
    int64_t updatedAt = 0;
};

inline bool NumberIn(const std::string& s, size_t from, size_t to, const char* key, double& out) {
    const std::string k = std::string("\"") + key + "\"";
    const size_t p = s.find(k, from);
    if (p == std::string::npos || p >= to) return false;
    const size_t colon = s.find(':', p + k.size());
    if (colon == std::string::npos || colon >= to) return false;
    const char* begin = s.c_str() + colon + 1;
    char* end = nullptr;
    const double v = strtod(begin, &end);
    if (end == begin) return false;
    out = v;
    return true;
}

inline Window ParseWindow(const std::string& s, const char* name) {
    Window w;
    const std::string k = std::string("\"") + name + "\"";
    const size_t p = s.find(k);
    if (p == std::string::npos) return w;
    const size_t open = s.find('{', p);
    if (open == std::string::npos) return w;
    const size_t close = s.find('}', open);
    if (close == std::string::npos) return w;
    double pct = 0.0, reset = 0.0;
    if (!NumberIn(s, open, close, "used_percentage", pct) || !NumberIn(s, open, close, "resets_at", reset)) return w;
    w.ok = true;
    w.pct = pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
    w.resetsAt = static_cast<int64_t>(reset);
    return w;
}

// True if at least one window was found.
inline bool Parse(const std::string& json, Data& out) {
    out = Data();
    out.five = ParseWindow(json, "five_hour");
    out.seven = ParseWindow(json, "seven_day");
    double updated = 0.0;
    if (NumberIn(json, 0, json.size(), "updated_at", updated)) out.updatedAt = static_cast<int64_t>(updated);
    return out.five.ok || out.seven.ok;
}

// "45 min", "3 h 12 min", "2 d 4 h". Rounds up so it never shows 0 before a reset.
inline std::string Duration(int64_t secs) {
    if (secs <= 0) return "0 min";
    const int64_t m = (secs + 59) / 60;
    if (m < 60) return std::to_string(m) + " min";
    const int64_t h = m / 60, mm = m % 60;
    if (h < 48) return std::to_string(h) + " h" + (mm ? " " + std::to_string(mm) + " min" : std::string());
    const int64_t d = h / 24, hh = h % 24;
    return std::to_string(d) + " d" + (hh ? " " + std::to_string(hh) + " h" : std::string());
}

// 0 = normal, 1 = yellow warning (above 50 %), 2 = red warning (above 80 %).
inline int Level(double pct) { return pct > 80.0 ? 2 : (pct > 50.0 ? 1 : 0); }

}  // namespace usage
// ==USAGE-PARSE-END==

// -- Claude usage: file reader thread ------------------------------------------------
std::wstring ClaudeUsageFilePath(const std::wstring& configured) {
    const std::wstring raw = configured.empty() ? std::wstring(L"%USERPROFILE%\\.claude\\island-usage.json")
                                                : configured;
    wchar_t buf[2 * MAX_PATH] = {};
    const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), buf, ARRAYSIZE(buf));
    if (n == 0 || n > ARRAYSIZE(buf)) return raw;
    return buf;
}

bool ReadSmallFile(const std::wstring& path, std::string& out) {
    out.clear();
    // The status line script may be writing right now; share generously.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD got = 0;
    while (out.size() < 65536 && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return true;
}

bool SameUsage(const ClaudeUsageSnapshot& a, const ClaudeUsageSnapshot& b) {
    return a.hasData == b.hasData && a.fiveOk == b.fiveOk && a.sevenOk == b.sevenOk &&
           a.fivePct == b.fivePct && a.sevenPct == b.sevenPct &&
           a.fiveResetsAt == b.fiveResetsAt && a.sevenResetsAt == b.sevenResetsAt &&
           a.updatedAt == b.updatedAt;
}

DWORD WINAPI UsageThreadProc(void*) {
    WaitForSingleObject(g_stopEvent, 2000);

    ClaudeUsageSnapshot published;
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        bool enabled = false;
        std::wstring configuredFile;
        {
            std::lock_guard lock(g_settingsMutex);
            enabled = g_settings.claudeUsage;
            configuredFile = g_settings.claudeUsageFile;
        }

        ClaudeUsageSnapshot next;
        if (enabled) {
            std::string text;
            usage::Data d;
            if (ReadSmallFile(ClaudeUsageFilePath(configuredFile), text) && usage::Parse(text, d)) {
                next.hasData = true;
                next.fiveOk = d.five.ok;
                next.sevenOk = d.seven.ok;
                next.fivePct = d.five.pct;
                next.sevenPct = d.seven.pct;
                next.fiveResetsAt = d.five.resetsAt;
                next.sevenResetsAt = d.seven.resetsAt;
                next.updatedAt = d.updatedAt;
            }
        }

        if (!SameUsage(next, published)) {
            {
                std::lock_guard lock(g_stateMutex);
                g_state.claudeUsage = next;
            }
            published = next;
            g_layoutDirty = true;
        }

        WaitForSingleObject(g_stopEvent, 4000);
    }
    return 0;
}

