// ==ICS-ENGINE-BEGIN==
// -- Appointments: iCal (ICS) parsing ------------------------------------------
// Self-contained and free of Win32 calls so it can be unit-tested off-Windows.
// The two hooks below are the only link to the OS (floating / unknown time zones
// fall back to the system time zone).
int64_t SystemLocalToUtc(int64_t naiveLocal);   // wall clock -> UTC (epoch seconds)
int64_t SystemUtcToLocalNaive(int64_t utc);     // UTC -> wall clock ("naive" seconds)

namespace ics {

// "Naive" time = wall clock as seconds since 1970-01-01 00:00, with no zone.
inline int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline void CivilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

inline int64_t FloorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

inline int DayOfWeek(int64_t days) {  // 0 = Sunday
    int64_t r = (days + 4) % 7;
    if (r < 0) r += 7;
    return static_cast<int>(r);
}

inline int DaysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return kDays[m - 1];
}

// Day-of-month of the n-th (n>0) or last-ish (n<0) given weekday in a month.
inline int NthWeekdayOfMonth(int y, int m, int dow, int n) {
    if (n > 0) {
        const int firstDow = DayOfWeek(DaysFromCivil(y, m, 1));
        const int day = 1 + ((dow - firstDow + 7) % 7) + (n - 1) * 7;
        return day <= DaysInMonth(y, m) ? day : 0;
    }
    const int last = DaysInMonth(y, m);
    const int lastDow = DayOfWeek(DaysFromCivil(y, m, last));
    const int day = last - ((lastDow - dow + 7) % 7) + (n + 1) * 7;
    return day >= 1 ? day : 0;
}

struct TimeVal {
    bool ok = false;
    bool utc = false;       // trailing Z
    bool dateOnly = false;  // VALUE=DATE
    int64_t naive = 0;
};

inline bool ParseDigits(const std::string& s, size_t pos, size_t n, int& out) {
    if (pos + n > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

inline TimeVal ParseTime(const std::string& s) {
    TimeVal t;
    int y, mo, d;
    if (!ParseDigits(s, 0, 4, y) || !ParseDigits(s, 4, 2, mo) || !ParseDigits(s, 6, 2, d)) return t;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return t;
    int hh = 0, mi = 0, ss = 0;
    if (s.size() >= 15 && s[8] == 'T') {
        if (!ParseDigits(s, 9, 2, hh) || !ParseDigits(s, 11, 2, mi) || !ParseDigits(s, 13, 2, ss)) return t;
        t.utc = s.size() >= 16 && s[15] == 'Z';
    } else {
        t.dateOnly = true;
    }
    t.naive = DaysFromCivil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
    t.ok = true;
    return t;
}

// ---- time zones (from the feed's own VTIMEZONE blocks) ------------------------
struct TzRule {
    int month = 0;   // 1..12
    int week = 0;    // 1..4, -1 = last
    int dow = 0;     // 0 = Sunday
    int secs = 0;    // local time of day of the switch
    bool valid = false;
};

struct Tz {
    bool hasRules = false;
    int stdOffset = 0;  // seconds east of UTC
    int dstOffset = 0;
    bool hasDst = false;
    TzRule dstStart;    // wall clock before the switch (standard time)
    TzRule dstEnd;      // wall clock before the switch (daylight time)
};

inline int64_t RuleNaive(int year, const TzRule& r) {
    const int day = NthWeekdayOfMonth(year, r.month, r.dow, r.week);
    if (day <= 0) return INT64_MAX;
    return DaysFromCivil(year, r.month, day) * 86400 + r.secs;
}

inline int OffsetAtNaive(const Tz& tz, int64_t naive) {
    if (!tz.hasDst || !tz.dstStart.valid || !tz.dstEnd.valid) return tz.stdOffset;
    int y;
    unsigned m, d;
    CivilFromDays(FloorDiv(naive, 86400), y, m, d);
    const int64_t start = RuleNaive(y, tz.dstStart);
    const int64_t end = RuleNaive(y, tz.dstEnd);
    bool dst;
    if (start < end) dst = naive >= start && naive < end;       // northern hemisphere
    else             dst = naive >= start || naive < end;       // southern hemisphere
    return dst ? tz.dstOffset : tz.stdOffset;
}

struct TzRef {
    enum Kind { Utc, Rules, System } kind = System;
    const Tz* tz = nullptr;
};

inline int64_t ToUtc(const TzRef& ref, int64_t naive) {
    switch (ref.kind) {
        case TzRef::Utc: return naive;
        case TzRef::Rules: return naive - OffsetAtNaive(*ref.tz, naive);
        default: return SystemLocalToUtc(naive);
    }
}

// ---- line-level parsing -------------------------------------------------------
struct Prop {
    std::string name;
    std::vector<std::pair<std::string, std::string>> params;
    std::string value;
    std::string Param(const char* key) const {
        for (const auto& p : params) if (p.first == key) return p.second;
        return std::string();
    }
};

inline std::string Upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return s;
}

inline bool ParseProp(const std::string& line, Prop& out) {
    out = Prop();
    bool inQuote = false;
    size_t colon = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') inQuote = !inQuote;
        else if (line[i] == ':' && !inQuote) { colon = i; break; }
    }
    if (colon == std::string::npos) return false;
    out.value = line.substr(colon + 1);

    std::vector<std::string> parts;
    std::string cur;
    inQuote = false;
    for (size_t i = 0; i < colon; ++i) {
        const char c = line[i];
        if (c == '"') { inQuote = !inQuote; cur.push_back(c); }
        else if (c == ';' && !inQuote) { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    out.name = Upper(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        const size_t eq = parts[i].find('=');
        if (eq == std::string::npos) continue;
        std::string v = parts[i].substr(eq + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        out.params.emplace_back(Upper(parts[i].substr(0, eq)), v);
    }
    return true;
}

inline std::string Unescape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == 'n' || n == 'N') r.push_back(' ');
            else r.push_back(n);
            ++i;
        } else {
            r.push_back(s[i]);
        }
    }
    return r;
}

inline std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == sep) { v.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    v.push_back(cur);
    return v;
}

inline int ParseOffset(const std::string& s) {  // +0200 / -0530 / +020000
    if (s.size() < 5) return 0;
    const int sign = s[0] == '-' ? -1 : 1;
    int hh = 0, mm = 0, ss = 0;
    ParseDigits(s, 1, 2, hh);
    ParseDigits(s, 3, 2, mm);
    if (s.size() >= 7) ParseDigits(s, 5, 2, ss);
    return sign * (hh * 3600 + mm * 60 + ss);
}

inline int WeekdayFromCode(const std::string& c) {
    static const char* kCodes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; ++i) if (c == kCodes[i]) return i;
    return -1;
}

struct ByDay { int ordinal = 0; int dow = 0; };

struct RRule {
    bool valid = false;
    std::string freq;
    int interval = 1;
    int count = 0;  // 0 = unlimited
    TimeVal until;
    std::vector<ByDay> byDay;
    std::vector<int> byMonthDay;
    std::vector<int> byMonth;
    int wkst = 1;   // Monday
};

inline RRule ParseRRule(const std::string& value) {
    RRule r;
    for (const std::string& kv : Split(value, ';')) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Upper(kv.substr(0, eq));
        const std::string v = kv.substr(eq + 1);
        if (k == "FREQ") r.freq = Upper(v);
        else if (k == "INTERVAL") r.interval = std::max(1, atoi(v.c_str()));
        else if (k == "COUNT") r.count = std::max(0, atoi(v.c_str()));
        else if (k == "UNTIL") r.until = ParseTime(v);
        else if (k == "WKST") { const int w = WeekdayFromCode(Upper(v)); if (w >= 0) r.wkst = w; }
        else if (k == "BYMONTH") for (const auto& x : Split(v, ',')) r.byMonth.push_back(atoi(x.c_str()));
        else if (k == "BYMONTHDAY") for (const auto& x : Split(v, ',')) r.byMonthDay.push_back(atoi(x.c_str()));
        else if (k == "BYDAY") {
            for (const auto& x : Split(v, ',')) {
                if (x.size() < 2) continue;
                ByDay b;
                const std::string code = Upper(x.substr(x.size() - 2));
                b.dow = WeekdayFromCode(code);
                if (b.dow < 0) continue;
                const std::string ord = x.substr(0, x.size() - 2);
                b.ordinal = ord.empty() ? 0 : atoi(ord.c_str());
                r.byDay.push_back(b);
            }
        }
    }
    r.valid = !r.freq.empty();
    return r;
}

inline TzRule RuleFromRRule(const RRule& rr, int secsOfDay) {
    TzRule t;
    if (rr.byMonth.empty() || rr.byDay.empty()) return t;
    t.month = rr.byMonth[0];
    t.week = rr.byDay[0].ordinal != 0 ? rr.byDay[0].ordinal : 1;
    t.dow = rr.byDay[0].dow;
    t.secs = secsOfDay;
    t.valid = true;
    return t;
}

// ---- events -------------------------------------------------------------------
struct Event {
    int64_t startUtc = 0;
    int64_t endUtc = 0;
    bool allDay = false;
    std::string title;  // UTF-8
};

struct RawEvent {
    std::string uid;
    std::string summary;
    TimeVal start, end, recurrenceId;
    std::string startTzid, endTzid, recurTzid;
    bool hasEnd = false;
    int64_t durationSecs = -1;
    std::string rrule;
    std::vector<std::pair<TimeVal, std::string>> exdates;  // value + TZID
    bool cancelled = false;
};

inline int64_t ParseDuration(const std::string& s) {  // P1DT2H30M, PT45M, P1W
    if (s.empty()) return -1;
    size_t i = 0;
    int sign = 1;
    if (s[i] == '-') { sign = -1; ++i; } else if (s[i] == '+') ++i;
    if (i >= s.size() || s[i] != 'P') return -1;
    ++i;
    bool inTime = false;
    int64_t total = 0;
    int64_t num = 0;
    bool haveNum = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == 'T') { inTime = true; continue; }
        if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); haveNum = true; continue; }
        if (!haveNum) return -1;
        if (c == 'W') total += num * 7 * 86400;
        else if (c == 'D') total += num * 86400;
        else if (c == 'H' && inTime) total += num * 3600;
        else if (c == 'M' && inTime) total += num * 60;
        else if (c == 'S' && inTime) total += num;
        num = 0;
        haveNum = false;
    }
    return sign * total;
}

inline std::string Unfold(const std::string& text, std::vector<std::string>& lines) {
    std::string cur;
    bool have = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        size_t len = e - i;
        if (len > 0 && text[i + len - 1] == '\r') --len;
        if (len > 0 && (text[i] == ' ' || text[i] == '\t') && have) {
            cur.append(text, i + 1, len - 1);
        } else {
            if (have) lines.push_back(cur);
            cur.assign(text, i, len);
            have = true;
        }
        i = e + 1;
    }
    if (have) lines.push_back(cur);
    return std::string();
}

class Calendar {
public:
    // Parses a whole .ics file; occurrences overlapping [winStartUtc, winEndUtc)
    // are appended to `out`.
    void Parse(const std::string& text, int64_t winStartUtc, int64_t winEndUtc, std::vector<Event>& out) {
        std::vector<std::string> lines;
        Unfold(text, lines);

        std::map<std::string, Tz> zones;
        std::vector<RawEvent> events;

        std::string tzid;
        Tz curTz;
        bool inTz = false, inSub = false, subIsDaylight = false;
        int subOffsetTo = 0, subSecs = 0;
        RRule subRule;
        bool inEvent = false;
        RawEvent ev;

        for (const std::string& line : lines) {
            Prop p;
            if (!ParseProp(line, p)) continue;

            if (p.name == "BEGIN") {
                const std::string v = Upper(p.value);
                if (v == "VTIMEZONE") { inTz = true; tzid.clear(); curTz = Tz(); }
                else if (v == "DAYLIGHT" || v == "STANDARD") {
                    if (inTz) { inSub = true; subIsDaylight = v == "DAYLIGHT"; subOffsetTo = 0; subSecs = 0; subRule = RRule(); }
                } else if (v == "VEVENT") { inEvent = true; ev = RawEvent(); }
                continue;
            }
            if (p.name == "END") {
                const std::string v = Upper(p.value);
                if (v == "VTIMEZONE") {
                    if (inTz && !tzid.empty()) { curTz.hasRules = true; zones[tzid] = curTz; }
                    inTz = false;
                } else if (v == "DAYLIGHT" || v == "STANDARD") {
                    if (inTz && inSub) {
                        const TzRule rule = RuleFromRRule(subRule, subSecs);
                        if (subIsDaylight) {
                            curTz.dstOffset = subOffsetTo;
                            if (rule.valid) { curTz.dstStart = rule; curTz.hasDst = true; }
                        } else {
                            curTz.stdOffset = subOffsetTo;
                            if (rule.valid) curTz.dstEnd = rule;
                        }
                    }
                    inSub = false;
                } else if (v == "VEVENT") {
                    if (inEvent && ev.start.ok && !ev.cancelled) events.push_back(ev);
                    inEvent = false;
                }
                continue;
            }

            if (inTz && !inSub) {
                if (p.name == "TZID") tzid = p.value;
            } else if (inTz && inSub) {
                if (p.name == "TZOFFSETTO") subOffsetTo = ParseOffset(p.value);
                else if (p.name == "DTSTART") { const TimeVal t = ParseTime(p.value); if (t.ok) subSecs = static_cast<int>(((t.naive % 86400) + 86400) % 86400); }
                else if (p.name == "RRULE") subRule = ParseRRule(p.value);
            } else if (inEvent) {
                if (p.name == "UID") ev.uid = p.value;
                else if (p.name == "SUMMARY") ev.summary = Unescape(p.value);
                else if (p.name == "DTSTART") { ev.start = ParseTime(p.value); ev.startTzid = p.Param("TZID"); }
                else if (p.name == "DTEND") { ev.end = ParseTime(p.value); ev.endTzid = p.Param("TZID"); ev.hasEnd = ev.end.ok; }
                else if (p.name == "DURATION") ev.durationSecs = ParseDuration(p.value);
                else if (p.name == "RRULE") ev.rrule = p.value;
                else if (p.name == "RECURRENCE-ID") { ev.recurrenceId = ParseTime(p.value); ev.recurTzid = p.Param("TZID"); }
                else if (p.name == "STATUS") ev.cancelled = Upper(p.value) == "CANCELLED";
                else if (p.name == "EXDATE") {
                    const std::string z = p.Param("TZID");
                    for (const auto& x : Split(p.value, ',')) {
                        const TimeVal t = ParseTime(x);
                        if (t.ok) ev.exdates.emplace_back(t, z);
                    }
                }
            }
        }

        // Pass 1: which recurrence instances were moved or deleted individually?
        std::set<std::pair<std::string, int64_t>> overridden;
        for (const RawEvent& e : events) {
            if (!e.recurrenceId.ok) continue;
            const TzRef ref = Resolve(e.recurrenceId.utc, e.recurTzid.empty() ? e.startTzid : e.recurTzid, zones);
            overridden.insert({e.uid, ToUtc(ref, e.recurrenceId.naive)});
        }

        // Pass 2: emit.
        const int64_t naiveHi = winEndUtc + 2 * 86400;
        for (const RawEvent& e : events) {
            const TzRef ref = Resolve(e.start.utc, e.startTzid, zones);
            const TzRef endRef = e.hasEnd ? Resolve(e.end.utc, e.endTzid, zones) : ref;
            const bool allDay = e.start.dateOnly;

            int64_t lenNaive;  // in the start's wall clock
            if (e.hasEnd && endRef.kind == ref.kind && endRef.tz == ref.tz) {
                lenNaive = e.end.naive - e.start.naive;
            } else if (e.hasEnd) {
                lenNaive = ToUtc(endRef, e.end.naive) - ToUtc(ref, e.start.naive);
            } else if (e.durationSecs >= 0) {
                lenNaive = e.durationSecs;
            } else {
                lenNaive = allDay ? 86400 : 0;
            }
            if (lenNaive < 0) lenNaive = 0;

            auto emit = [&](int64_t startNaive) {
                const int64_t s = ToUtc(ref, startNaive);
                int64_t en = ToUtc(ref, startNaive + lenNaive);
                if (en < s) en = s;
                if (en <= winStartUtc && !(s == en && s >= winStartUtc)) return;
                if (s >= winEndUtc) return;
                Event o;
                o.startUtc = s;
                o.endUtc = en;
                o.allDay = allDay;
                o.title = e.summary.empty() ? std::string("(no title)") : e.summary;
                out.push_back(std::move(o));
            };

            const RRule rr = e.rrule.empty() ? RRule() : ParseRRule(e.rrule);
            if (!rr.valid || e.recurrenceId.ok) {
                emit(e.start.naive);
                continue;
            }

            std::set<int64_t> exUtc;
            for (const auto& ex : e.exdates) {
                const TzRef xr = Resolve(ex.first.utc, ex.second.empty() ? e.startTzid : ex.second, zones);
                if (ex.first.dateOnly) exUtc.insert(ToUtc(ref, ex.first.naive));
                else exUtc.insert(ToUtc(xr, ex.first.naive));
            }
            int64_t untilUtc = INT64_MAX;
            if (rr.until.ok) {
                if (rr.until.utc) untilUtc = rr.until.naive;
                else untilUtc = ToUtc(ref, rr.until.naive + (rr.until.dateOnly ? 86399 : 0));
            }

            Expand(e, rr, naiveHi, [&](int64_t startNaive) {
                const int64_t s = ToUtc(ref, startNaive);
                if (s > untilUtc) return;
                if (exUtc.count(s)) return;
                if (overridden.count({e.uid, s})) return;
                emit(startNaive);
            });
        }
    }

private:
    static TzRef Resolve(bool utc, const std::string& tzid, const std::map<std::string, Tz>& zones) {
        TzRef r;
        if (utc) { r.kind = TzRef::Utc; return r; }
        if (!tzid.empty()) {
            const auto it = zones.find(tzid);
            if (it != zones.end()) { r.kind = TzRef::Rules; r.tz = &it->second; return r; }
            if (tzid == "UTC" || tzid == "GMT" || tzid == "Etc/UTC") { r.kind = TzRef::Utc; return r; }
        }
        r.kind = TzRef::System;
        return r;
    }

    template <typename F>
    static void Expand(const RawEvent& e, const RRule& rr, int64_t naiveHi, F&& visit) {
        const int64_t startDay = FloorDiv(e.start.naive, 86400);
        const int secsOfDay = static_cast<int>(e.start.naive - startDay * 86400);
        int sy; unsigned sm, sd;
        CivilFromDays(startDay, sy, sm, sd);
        const int64_t endDay = FloorDiv(naiveHi, 86400) + 1;

        int produced = 0;
        bool stop = false;
        auto emitDay = [&](int64_t day) {
            if (stop || day < startDay) return;
            ++produced;
            if (rr.count > 0 && produced > rr.count) { stop = true; return; }
            visit(day * 86400 + secsOfDay);
        };
        auto monthFilter = [&](int month) {
            if (rr.byMonth.empty()) return true;
            for (int m : rr.byMonth) if (m == month) return true;
            return false;
        };
        auto weekdayAllowed = [&](int dow) {
            if (rr.byDay.empty()) return true;
            for (const auto& b : rr.byDay) if (b.dow == dow) return true;
            return false;
        };
        // All days in (y, m) selected by BYMONTHDAY / BYDAY; falls back to `dflt`.
        auto monthDays = [&](int y, int m, int dflt) {
            std::vector<int> days;
            const int dim = DaysInMonth(y, m);
            if (!rr.byMonthDay.empty()) {
                for (int md : rr.byMonthDay) {
                    const int d = md > 0 ? md : dim + md + 1;
                    if (d >= 1 && d <= dim) days.push_back(d);
                }
            } else if (!rr.byDay.empty()) {
                for (const auto& b : rr.byDay) {
                    if (b.ordinal != 0) {
                        const int d = NthWeekdayOfMonth(y, m, b.dow, b.ordinal);
                        if (d > 0) days.push_back(d);
                    } else {
                        for (int d = 1; d <= dim; ++d)
                            if (DayOfWeek(DaysFromCivil(y, m, d)) == b.dow) days.push_back(d);
                    }
                }
            } else if (dflt <= dim) {
                days.push_back(dflt);
            }
            std::sort(days.begin(), days.end());
            days.erase(std::unique(days.begin(), days.end()), days.end());
            return days;
        };

        const int kMaxIter = 200000;
        if (rr.freq == "DAILY") {
            for (int64_t k = 0; k < kMaxIter && !stop; ++k) {
                const int64_t day = startDay + k * rr.interval;
                if (day > endDay) break;
                int y; unsigned m, d;
                CivilFromDays(day, y, m, d);
                if (!monthFilter(static_cast<int>(m))) continue;
                if (!weekdayAllowed(DayOfWeek(day))) continue;
                if (!rr.byMonthDay.empty()) {
                    bool ok = false;
                    const int dim = DaysInMonth(y, m);
                    for (int md : rr.byMonthDay) if ((md > 0 ? md : dim + md + 1) == static_cast<int>(d)) ok = true;
                    if (!ok) continue;
                }
                emitDay(day);
            }
        } else if (rr.freq == "WEEKLY") {
            const int startDow = DayOfWeek(startDay);
            const int64_t week0 = startDay - ((startDow - rr.wkst + 7) % 7);
            std::vector<int> dows;
            if (rr.byDay.empty()) dows.push_back(startDow);
            else for (const auto& b : rr.byDay) dows.push_back(b.dow);
            std::vector<int> offs;
            for (int dw : dows) offs.push_back((dw - rr.wkst + 7) % 7);
            std::sort(offs.begin(), offs.end());
            offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
            for (int64_t k = 0; k < kMaxIter && !stop; ++k) {
                const int64_t base = week0 + k * 7 * rr.interval;
                if (base > endDay) break;
                for (int off : offs) {
                    const int64_t day = base + off;
                    int y; unsigned m, d;
                    CivilFromDays(day, y, m, d);
                    if (!monthFilter(static_cast<int>(m))) continue;
                    emitDay(day);
                }
            }
        } else if (rr.freq == "MONTHLY") {
            for (int64_t k = 0; k < kMaxIter && !stop; ++k) {
                const int64_t mi = static_cast<int64_t>(sy) * 12 + (sm - 1) + k * rr.interval;
                const int y = static_cast<int>(FloorDiv(mi, 12));
                const int m = static_cast<int>(mi - static_cast<int64_t>(y) * 12) + 1;
                if (DaysFromCivil(y, m, 1) > endDay) break;
                if (!monthFilter(m)) continue;
                for (int d : monthDays(y, m, static_cast<int>(sd))) emitDay(DaysFromCivil(y, m, d));
            }
        } else if (rr.freq == "YEARLY") {
            for (int64_t k = 0; k < 5000 && !stop; ++k) {
                const int y = sy + static_cast<int>(k * rr.interval);
                if (DaysFromCivil(y, 1, 1) > endDay) break;
                std::vector<int> months = rr.byMonth;
                if (months.empty()) months.push_back(static_cast<int>(sm));
                std::sort(months.begin(), months.end());
                const bool selectorsPresent = !rr.byMonthDay.empty() || !rr.byDay.empty();
                for (int m : months) {
                    if (m < 1 || m > 12) continue;
                    if (selectorsPresent) {
                        for (int d : monthDays(y, m, static_cast<int>(sd))) emitDay(DaysFromCivil(y, m, d));
                    } else if (static_cast<int>(sd) <= DaysInMonth(y, m)) {
                        emitDay(DaysFromCivil(y, m, static_cast<int>(sd)));
                    }
                }
            }
        } else {
            visit(e.start.naive);  // unsupported FREQ: show the first instance only
        }
    }
};

// ---- appointment selection and labels (template: works on any event type) --------
constexpr int64_t kNowGraceSecs = 5 * 60;  // "now" stays up this long after the start

// Index of the next timed event that starts before `endOfDayUtc`, or -1.
template <typename Ev>
inline int FindNextToday(const std::vector<Ev>& events, int64_t nowUtc, int64_t endOfDayUtc) {
    int best = -1;
    for (size_t i = 0; i < events.size(); ++i) {
        const Ev& e = events[i];
        if (e.allDay) continue;
        if (e.startUtc < nowUtc - kNowGraceSecs) continue;
        if (e.startUtc >= endOfDayUtc) continue;
        if (best < 0 || e.startUtc < events[best].startUtc) best = static_cast<int>(i);
    }
    return best;
}

// Collapsed-island label. Empty = nothing to show (more than 5 h away).
inline const char* BucketLabel(int64_t secsUntil) {
    if (secsUntil <= 0) return "now";
    static const struct { int64_t limit; const char* text; } kBuckets[] = {
        {60, "<1m"},    {120, "<2m"},   {180, "<3m"},   {240, "<4m"},
        {300, "<5m"},   {600, "<10m"},  {900, "<15m"},  {1800, "<30m"},
        {2700, "<45m"}, {3600, "<1h"},  {7200, "<2h"},  {10800, "<3h"},
        {14400, "<4h"}, {18000, "<5h"},
    };
    for (const auto& b : kBuckets) if (secsUntil < b.limit) return b.text;
    return "";
}

// Start-page line: "now", "in 25m", "in 2h 15m".
inline std::string RelativeText(int64_t secsUntil) {
    if (secsUntil <= 0) return "now";
    const int64_t mins = (secsUntil + 59) / 60;  // round up: 61 s -> 2 m
    if (mins < 60) return "in " + std::to_string(mins) + "m";
    const int64_t h = mins / 60, m = mins % 60;
    return "in " + std::to_string(h) + "h" + (m ? " " + std::to_string(m) + "m" : std::string());
}

}  // namespace ics
// ==ICS-ENGINE-END==

