// -- Appointments: Windows glue -------------------------------------------------
SYSTEMTIME ApptNaiveToSystemTime(int64_t naive) {
    int y;
    unsigned m, d;
    const int64_t days = ics::FloorDiv(naive, 86400);
    ics::CivilFromDays(days, y, m, d);
    const int64_t sod = naive - days * 86400;
    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(sod / 3600);
    st.wMinute = static_cast<WORD>((sod % 3600) / 60);
    st.wSecond = static_cast<WORD>(sod % 60);
    return st;
}

int64_t ApptSystemTimeToNaive(const SYSTEMTIME& st) {
    return ics::DaysFromCivil(st.wYear, st.wMonth, st.wDay) * 86400 +
           st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
}

int64_t SystemLocalToUtc(int64_t naiveLocal) {
    const SYSTEMTIME lt = ApptNaiveToSystemTime(naiveLocal);
    SYSTEMTIME ut = {};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut)) {
        return naiveLocal;
    }
    return ApptSystemTimeToNaive(ut);
}

int64_t SystemUtcToLocalNaive(int64_t utc) {
    const SYSTEMTIME ut = ApptNaiveToSystemTime(utc);
    SYSTEMTIME lt = {};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &ut, &lt)) {
        return utc;
    }
    return ApptSystemTimeToNaive(lt);
}

int64_t ApptNowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::wstring ApptUtf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// What the collapsed strip and the start page need to know about "the next one".
struct AppointmentInfo {
    bool valid = false;
    int64_t secsUntil = 0;  // <= 0 while inside the "now" grace period
    int64_t startUtc = 0;
    std::wstring title;
    uint32_t color = 0x4285F4;
};

// Next timed appointment that starts later today (all-day events are skipped).
bool GetNextAppointment(const SharedState& state, const Settings& settings, AppointmentInfo& out) {
    out = AppointmentInfo();
    if (!settings.appointments || !state.calendar.events) {
        return false;
    }
    const int64_t nowUtc = ApptNowUtc();
    const int64_t localNow = SystemUtcToLocalNaive(nowUtc);
    const int64_t endOfDayUtc = SystemLocalToUtc((ics::FloorDiv(localNow, 86400) + 1) * 86400);
    const std::vector<CalEvent>& events = *state.calendar.events;
    const int idx = ics::FindNextToday(events, nowUtc, endOfDayUtc);
    if (idx < 0) {
        return false;
    }
    const CalEvent& e = events[static_cast<size_t>(idx)];
    out.valid = true;
    out.startUtc = e.startUtc;
    out.secsUntil = e.startUtc - nowUtc;
    out.title = e.title;
    out.color = e.color;
    return true;
}

// -- Appointments: feed thread -----------------------------------------------------
struct CalSource {
    std::wstring name;
    std::wstring url;
    uint32_t color = 0x4285F4;
};

std::wstring ApptTrim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool ApptParseHexColor(const std::wstring& text, uint32_t& out) {
    std::wstring t = text;
    if (!t.empty() && t[0] == L'#') t.erase(0, 1);
    if (t.size() != 6) return false;
    uint32_t v = 0;
    for (wchar_t c : t) {
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') v |= static_cast<uint32_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v |= static_cast<uint32_t>(c - L'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

// "Name|#RRGGBB|URL", "Name|URL" or just "URL".
std::vector<CalSource> ParseCalendarSources(const std::vector<std::wstring>& entries) {
    static const uint32_t kPalette[] = {0x4285F4, 0x0F9D58, 0xF4B400, 0xDB4437,
                                        0xAB47BC, 0x00ACC1, 0xFF7043, 0x9E9D24};
    std::vector<CalSource> result;
    for (const std::wstring& raw : entries) {
        std::vector<std::wstring> parts;
        size_t pos = 0;
        while (parts.size() < 2) {
            const size_t bar = raw.find(L'|', pos);
            if (bar == std::wstring::npos) break;
            parts.push_back(raw.substr(pos, bar - pos));
            pos = bar + 1;
        }
        parts.push_back(raw.substr(pos));  // the rest is the URL

        CalSource src;
        src.color = kPalette[result.size() % (sizeof(kPalette) / sizeof(kPalette[0]))];
        src.url = ApptTrim(parts.back());
        if (parts.size() >= 2) src.name = ApptTrim(parts[0]);
        if (parts.size() >= 3) {
            uint32_t c;
            if (ApptParseHexColor(ApptTrim(parts[1]), c)) src.color = c;
        }
        if (src.url.empty()) continue;
        if (src.name.empty()) src.name = L"Calendar " + std::to_wstring(result.size() + 1);
        result.push_back(std::move(src));
    }
    return result;
}

// https only. webcal:// is the same thing under another name.
bool ApptSplitUrl(std::wstring url, std::wstring& host, std::wstring& path) {
    if (url.size() > 9 && _wcsnicmp(url.c_str(), L"webcal://", 9) == 0) {
        url = L"https://" + url.substr(9);
    }
    if (url.size() <= 8 || _wcsnicmp(url.c_str(), L"https://", 8) != 0) return false;
    const std::wstring rest = url.substr(8);
    const size_t slash = rest.find(L'/');
    host = slash == std::wstring::npos ? rest : rest.substr(0, slash);
    path = slash == std::wstring::npos ? L"/" : rest.substr(slash);
    return !host.empty() && host.find(L'@') == std::wstring::npos && host.find(L':') == std::wstring::npos;
}

DWORD WINAPI CalendarThreadProc(void*) {
    // Let startup settle first, like the weather thread.
    WaitForSingleObject(g_stopEvent, 4000);

    static const DWORD kBackoffMs[] = {60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000};
    size_t backoff = 0;
    // url -> last good events. A feed that fails keeps showing its old data.
    std::map<std::wstring, std::vector<CalEvent>> cache;
    bool published = false;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        bool enabled = false;
        std::vector<std::wstring> entries;
        {
            std::lock_guard lock(g_settingsMutex);
            enabled = g_settings.appointments;
            entries = g_settings.appointmentCalendars;
        }

        DWORD waitMs = 5000;
        if (!enabled) {
            if (published) {
                std::lock_guard lock(g_stateMutex);
                g_state.calendar = CalendarSnapshot();
                published = false;
                g_layoutDirty = true;
            }
            cache.clear();
        } else {
            const std::vector<CalSource> sources = ParseCalendarSources(entries);
            bool anyFailed = false;

            std::set<std::wstring> wanted;
            for (const CalSource& src : sources) wanted.insert(src.url);
            for (auto it = cache.begin(); it != cache.end();) {
                it = wanted.count(it->first) ? std::next(it) : cache.erase(it);
            }

            const int64_t nowUtc = ApptNowUtc();
            for (const CalSource& src : sources) {
                if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) break;
                std::wstring host, path;
                if (!ApptSplitUrl(src.url, host, path)) {
                    // Never log the URL itself: the secret is part of it.
                    Wh_Log(L"Appointments: \"%s\" is not an https:// or webcal:// address.", src.name.c_str());
                    continue;
                }
                const HttpResult res = HttpGetEx(host.c_str(), path.c_str(), true, L"DynamicIsland/1.0");
                if (!res.ok || res.status != 200 || res.body.find("BEGIN:VCALENDAR") == std::string::npos) {
                    Wh_Log(L"Appointments: fetching \"%s\" failed (ok=%d, HTTP %lu).", src.name.c_str(),
                           res.ok ? 1 : 0, res.status);
                    anyFailed = true;
                    continue;
                }

                std::vector<ics::Event> parsed;
                ics::Calendar cal;
                cal.Parse(res.body, nowUtc - 86400, nowUtc + 8 * 86400, parsed);
                if (parsed.size() > 500) parsed.resize(500);

                std::vector<CalEvent>& dst = cache[src.url];
                dst.clear();
                for (const ics::Event& pe : parsed) {
                    CalEvent ce;
                    ce.startUtc = pe.startUtc;
                    ce.endUtc = pe.endUtc;
                    ce.allDay = pe.allDay;
                    ce.title = ApptUtf8ToWide(pe.title);
                    dst.push_back(std::move(ce));
                }
                Wh_Log(L"Appointments: \"%s\" -> %zu events in the next 8 days.", src.name.c_str(), dst.size());
            }

            auto merged = std::make_shared<std::vector<CalEvent>>();
            for (const CalSource& src : sources) {
                const auto it = cache.find(src.url);
                if (it == cache.end()) continue;
                for (CalEvent ce : it->second) {
                    ce.color = src.color;
                    merged->push_back(std::move(ce));
                }
            }
            std::stable_sort(merged->begin(), merged->end(),
                             [](const CalEvent& a, const CalEvent& b) { return a.startUtc < b.startUtc; });
            {
                std::lock_guard lock(g_stateMutex);
                g_state.calendar.events = merged;
                g_state.calendar.hasData = true;
                published = true;
                g_layoutDirty = true;
            }

            if (sources.empty()) {
                waitMs = 30 * 1000;
            } else if (anyFailed) {
                waitMs = kBackoffMs[backoff];
                if (backoff + 1 < ARRAYSIZE(kBackoffMs)) ++backoff;
            } else {
                backoff = 0;
                waitMs = 10 * 60 * 1000;
            }
        }

        HANDLE waits[2] = {g_stopEvent, g_calendarRefreshEvent};
        const DWORD r = WaitForMultipleObjects(g_calendarRefreshEvent ? 2 : 1, waits, FALSE, waitMs);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) backoff = 0;  // settings changed: refetch right away
    }
    return 0;
}

