// ── Quick Lookup ─────────────────────────────────────────────────────────────
// Press the Quick Lookup hotkey to look up whatever text is on the clipboard.

// ── Quick Lookup ─────────────────────────────────────────────────────────────
// Press the Quick Lookup hotkey to look up whatever text is on the clipboard.
//
//   hotkey (render thread) -> StartQuickLookup()
//        1. read + normalise the clipboard text
//        2. in-memory cache hit?  -> publish the result at once
//        3. otherwise publish a "Looking up..." card and spawn ONE worker thread
//   worker thread          -> LookupText() -> publish the result if still wanted
//
// Privacy: nothing is looked up when the clipboard changes. Text is only sent over
// the network when the hotkey is pressed, only if it is a short word or phrase, and
// it is never written to the log or to disk.
//
// Adding a source later = write one function with the signature
//     Outcome LookupXxx(const std::wstring& query, Result* out)
// and add one line to LookupText().

// Wikimedia asks API clients to send a descriptive User-Agent with contact info.
constexpr wchar_t kLookupUserAgent[] =
    L"DynamicIslandForWindows/" WH_MOD_VERSION L" (https://github.com/devcode90/Dynamic-Island-for-Windows) QuickLookup";

// Bumped for every new request AND when the card is dismissed. A worker only
// publishes if its id is still current, so a slow reply can never resurrect a card
// the user already closed. Only changed or compared while holding g_stateMutex.
std::atomic<uint64_t> g_lookupRequestSeq{0};
bool g_lookupHotkeyRegistered = false;

namespace QuickLookup {

constexpr double kPanelIdleSeconds = 25.0;       // panel closes this long after the last key / mouse move
constexpr double kResultSeconds = kPanelIdleSeconds;
constexpr double kMessageSeconds = kPanelIdleSeconds;
constexpr size_t kRecentCapacity = 12;
constexpr double kLoadingTimeoutSeconds = 30.0;  // upper bound for "Looking up..."
constexpr size_t kMaxQueryChars = 80;            // longer text is never sent anywhere
constexpr size_t kMaxBodyChars = 360;            // pre-cap on a definition
constexpr size_t kMaxExtractSentences = 3;
constexpr size_t kMaxExtractChars = 320;
constexpr size_t kCacheCapacity = 40;

struct Result {
    std::wstring title;
    std::wstring subtitle;
    std::wstring body;
    std::wstring example;
    std::wstring source;
};

// Failed means "could not get a real answer" (timeout, 429, 5xx, no network).
// Only Found and NotFound are cached, so a flaky connection is never remembered.
enum class Outcome { Found, NotFound, Failed };

// ---- text helpers -------------------------------------------------------------
// Collapses runs of whitespace (including newlines and NBSP) into single spaces.
// Stops once the result is longer than maxChars, so a huge clipboard stays cheap.
std::wstring CollapseWhitespace(const std::wstring& in, size_t maxChars = std::wstring::npos) {
    std::wstring out;
    bool pendingSpace = false;
    for (wchar_t ch : in) {
        if (iswspace(ch) || ch == 0x00A0 || ch == L'\0') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(ch);
        if (out.size() > maxChars) {
            break;
        }
    }
    return out;
}

bool IsEdgePunct(wchar_t c) {
    return c != 0 && wcschr(L".,;:!?\"'()[]{}<>\u201c\u201d\u2018\u2019\u00ab\u00bb", c) != nullptr;
}

// Clipboard text -> lookup query. Too-long text is returned still too long (one
// char over the limit) so the caller can reject it instead of silently truncating.
std::wstring NormalizeQuery(const std::wstring& raw) {
    std::wstring text = CollapseWhitespace(raw, kMaxQueryChars);
    if (text.size() > kMaxQueryChars) {
        return text;
    }
    // "hello," / (hello) / “hello” -> hello
    while (!text.empty() && (IsEdgePunct(text.front()) || text.front() == L' ')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (IsEdgePunct(text.back()) || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

// Cuts to maxChars on a word boundary and appends an ellipsis.
std::wstring ClipToChars(std::wstring text, size_t maxChars) {
    if (text.size() <= maxChars) {
        return text;
    }
    text.resize(maxChars);
    const size_t space = text.find_last_of(L' ');
    if (space != std::wstring::npos && space > maxChars / 2) {
        text.resize(space);
    }
    while (!text.empty() && (iswspace(text.back()) || text.back() == L',' ||
                             text.back() == L';' || text.back() == L':' || text.back() == L'-')) {
        text.pop_back();
    }
    if (!text.empty() && IS_HIGH_SURROGATE(text.back())) {
        text.pop_back();
    }
    text += L"\u2026";
    return text;
}

std::wstring TitleCase(std::wstring text) {
    bool wordStart = true;
    for (wchar_t& c : text) {
        if (c == L' ') {
            wordStart = true;
        } else {
            if (wordStart) {
                c = static_cast<wchar_t>(towupper(c));
            }
            wordStart = false;
        }
    }
    return text;
}

bool IsSentenceCloser(wchar_t c) {
    return c == L'"' || c == L'\'' || c == L')' || c == L'\u201d' || c == L'\u2019';
}

// A '.', '!' or '?' ends a sentence only when whitespace and then a capital, digit
// or opening quote follow, and (for '.') the word before it isn't an initial or a
// common abbreviation. Good enough for encyclopedia text, not a full tokenizer.
bool IsSentenceEnd(const std::wstring& t, size_t i) {
    const wchar_t c = t[i];
    if (c != L'.' && c != L'!' && c != L'?') {
        return false;
    }
    size_t j = i + 1;
    while (j < t.size() && IsSentenceCloser(t[j])) {
        ++j;
    }
    if (j >= t.size()) {
        return true;
    }
    if (!iswspace(t[j])) {
        return false;
    }
    while (j < t.size() && iswspace(t[j])) {
        ++j;
    }
    if (j >= t.size()) {
        return true;
    }
    if (!(iswupper(t[j]) || iswdigit(t[j]) || t[j] == L'"' || t[j] == L'\u201c' || t[j] == L'(')) {
        return false;
    }
    if (c != L'.') {
        return true;
    }

    size_t s = i;
    while (s > 0 && iswalpha(t[s - 1])) {
        --s;
    }
    const std::wstring word = ToLowerCopy(t.substr(s, i - s));
    if (word.size() == 1) {
        return false;  // initials: "J. R. R. Tolkien"
    }
    static const wchar_t* const kAbbrev[] = {
        L"mr", L"mrs", L"ms", L"dr", L"prof", L"st", L"jr", L"sr", L"vs", L"inc", L"ltd",
        L"co", L"no", L"ca", L"approx", L"mt", L"gen", L"col", L"lt", L"sgt", L"rev", L"fig", L"vol"};
    for (const wchar_t* abbrev : kAbbrev) {
        if (word == abbrev) {
            return false;
        }
    }
    return true;
}

// First `maxSentences` sentences, but stops early rather than exceed maxChars
// (the first sentence is always kept, clipped if it alone is too long).
std::wstring FirstSentences(const std::wstring& text, size_t maxSentences, size_t maxChars) {
    size_t sentences = 0;
    size_t acceptedEnd = 0;
    for (size_t i = 0; i < text.size() && sentences < maxSentences; ++i) {
        if (!IsSentenceEnd(text, i)) {
            continue;
        }
        size_t end = i + 1;
        while (end < text.size() && IsSentenceCloser(text[end])) {
            ++end;
        }
        if (sentences > 0 && end > maxChars) {
            break;
        }
        acceptedEnd = end;
        ++sentences;
        i = end - 1;
    }
    if (acceptedEnd == 0) {
        acceptedEnd = text.size();  // no sentence boundary found: use it all, clipped
    }
    std::wstring out = text.substr(0, acceptedEnd);
    while (!out.empty() && iswspace(out.back())) {
        out.pop_back();
    }
    return ClipToChars(out, maxChars);
}

// ---- JSON helper (same string-find style as the lyrics code) --------------------
// Returns the balanced {...} object that starts at `open`, string- and escape-aware.
std::string BalancedObjectAt(const std::string& json, size_t open) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = open; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                return json.substr(open, i - open + 1);
            }
        }
    }
    return std::string();
}

// ---- in-memory cache (session only, MRU at the back) ----------------------------
struct CacheEntry {
    std::wstring key;
    bool found = false;
    Result result;
};

std::mutex g_cacheMutex;

std::vector<CacheEntry>& Cache() {
    static std::vector<CacheEntry> cache;
    return cache;
}

bool CacheLookup(const std::wstring& key, bool* found, Result* out) {
    std::lock_guard lock(g_cacheMutex);
    auto& cache = Cache();
    for (size_t i = cache.size(); i-- > 0;) {
        if (cache[i].key == key) {
            *found = cache[i].found;
            *out = cache[i].result;
            std::rotate(cache.begin() + static_cast<std::ptrdiff_t>(i),
                        cache.begin() + static_cast<std::ptrdiff_t>(i) + 1, cache.end());
            return true;
        }
    }
    return false;
}

void CacheStore(const std::wstring& key, bool found, const Result& result) {
    std::lock_guard lock(g_cacheMutex);
    auto& cache = Cache();
    cache.erase(std::remove_if(cache.begin(), cache.end(),
                               [&](const CacheEntry& e) { return e.key == key; }),
                cache.end());
    CacheEntry entry;
    entry.key = key;
    entry.found = found;
    entry.result = result;
    cache.push_back(std::move(entry));
    while (cache.size() > kCacheCapacity) {
        cache.erase(cache.begin());
    }
}

// ---- source 1: dictionary (single words) ----------------------------------------
// https://api.dictionaryapi.dev/api/v2/entries/en/<word>
// Shape: [{"word":..,"phonetic":..,"phonetics":[{"text":..}],
//          "meanings":[{"partOfSpeech":..,"definitions":[{"definition":..,"example":..}]}]}]
Outcome LookupDictionary(const std::wstring& word, Result* out) {
    const std::wstring path =
        L"/api/v2/entries/en/" + AsciiToWide(UrlEncodeComponent(ToLowerCopy(word)));
    const HttpResult res = HttpGetEx(L"api.dictionaryapi.dev", path.c_str(), true, kLookupUserAgent);
    if (!res.IsDefinitive()) {
        return Outcome::Failed;
    }
    if (res.status != 200) {
        return Outcome::NotFound;
    }

    const std::vector<std::string> entries = SplitJsonObjectArray(res.body);
    if (entries.empty()) {
        return Outcome::NotFound;
    }
    const std::string& entry = entries.front();

    std::string wordRaw, phoneticRaw;
    ExtractJsonStringField(entry, "word", &wordRaw);
    if (!ExtractJsonStringField(entry, "phonetic", &phoneticRaw) || phoneticRaw.empty()) {
        phoneticRaw.clear();
        ExtractJsonStringField(entry, "text", &phoneticRaw);  // first phonetics[].text
    }

    // First meaning's part of speech, then the first definition object after it.
    const size_t meaningsPos = entry.find("\"meanings\":[");
    if (meaningsPos == std::string::npos) {
        return Outcome::NotFound;
    }
    std::string posRaw;
    const size_t posKey = entry.find("\"partOfSpeech\":\"", meaningsPos);
    if (posKey != std::string::npos) {
        ExtractJsonStringField(entry.substr(posKey), "partOfSpeech", &posRaw);
    }
    const size_t defKey =
        entry.find("\"definition\":\"", posKey != std::string::npos ? posKey : meaningsPos);
    if (defKey == std::string::npos) {
        return Outcome::NotFound;
    }

    // Bound the search to this one definition object so a later definition's
    // "example" is never attributed to the first one.
    const size_t defOpen = entry.rfind('{', defKey);
    const std::string defObject = defOpen != std::string::npos ? BalancedObjectAt(entry, defOpen)
                                                               : entry.substr(defKey);
    std::string defRaw, exampleRaw;
    ExtractJsonStringField(defObject, "definition", &defRaw);
    ExtractJsonStringField(defObject, "example", &exampleRaw);

    const std::wstring definition = CollapseWhitespace(JsonStringUnescape(defRaw));
    if (definition.empty()) {
        return Outcome::NotFound;
    }

    Result r;
    r.title = CollapseWhitespace(JsonStringUnescape(wordRaw));
    if (r.title.empty()) {
        r.title = word;
    }
    r.subtitle = CollapseWhitespace(JsonStringUnescape(phoneticRaw));
    const std::wstring pos = CollapseWhitespace(JsonStringUnescape(posRaw));
    if (!pos.empty()) {
        if (!r.subtitle.empty()) {
            r.subtitle += L"  \u00b7  ";
        }
        r.subtitle += pos;
    }
    r.body = ClipToChars(definition, kMaxBodyChars);
    const std::wstring example = CollapseWhitespace(JsonStringUnescape(exampleRaw));
    if (!example.empty()) {
        r.example = L"\u201c" + ClipToChars(example, 200) + L"\u201d";
    }
    r.source = L"Dictionary";
    *out = std::move(r);
    return Outcome::Found;
}

// ---- source 2: Wikipedia summary (phrases, or words the dictionary lacks) ---------
// https://en.wikipedia.org/api/rest_v1/page/summary/<title>
// Titles are case-sensitive after the first letter, so a miss is retried once in
// Title Case ("machine learning" -> "Machine Learning").
Outcome LookupWikipedia(const std::wstring& query, Result* out) {
    const std::wstring candidates[2] = {query, TitleCase(query)};
    Outcome outcome = Outcome::NotFound;

    for (int i = 0; i < 2; ++i) {
        if (i == 1 && candidates[1] == candidates[0]) {
            break;
        }
        std::wstring title = candidates[i];
        std::replace(title.begin(), title.end(), L' ', L'_');
        const std::wstring path =
            L"/api/rest_v1/page/summary/" + AsciiToWide(UrlEncodeComponent(title));

        const HttpResult res = HttpGetEx(L"en.wikipedia.org", path.c_str(), true, kLookupUserAgent);
        if (!res.IsDefinitive()) {
            return Outcome::Failed;
        }
        if (res.status != 200) {
            continue;  // 404: try the next spelling
        }

        std::string extractRaw, titleRaw, descRaw;
        if (!ExtractJsonStringField(res.body, "extract", &extractRaw) || extractRaw.empty()) {
            continue;
        }
        ExtractJsonStringField(res.body, "title", &titleRaw);
        ExtractJsonStringField(res.body, "description", &descRaw);

        const std::wstring extract = CollapseWhitespace(JsonStringUnescape(extractRaw));
        if (extract.empty()) {
            continue;
        }

        Result r;
        r.title = CollapseWhitespace(JsonStringUnescape(titleRaw));
        if (r.title.empty()) {
            r.title = query;
        }
        r.subtitle = CollapseWhitespace(JsonStringUnescape(descRaw));
        r.body = FirstSentences(extract, kMaxExtractSentences, kMaxExtractChars);
        r.source = L"Wikipedia";
        *out = std::move(r);
        return Outcome::Found;
    }
    return outcome;
}

// ---- source 3: Wiktionary (primary dictionary) --------------------------------------
// https://en.wiktionary.org/api/rest_v1/page/definition/<word>
// Shape: {"en":[{"partOfSpeech":"Noun","language":"English","definitions":[
//          {"definition":"<html>","parsedExamples":[{"example":"<html>"}]}, ...]}, ...],
//         "fr":[...], ...}
// Definitions come back as HTML, so tags are stripped and entities decoded.

// Removes <tags>, decodes common HTML entities, collapses whitespace.
std::wstring StripHtml(const std::wstring& in) {
    std::wstring noTags;
    noTags.reserve(in.size());
    bool inTag = false;
    for (wchar_t c : in) {
        if (c == L'<') {
            inTag = true;
            continue;
        }
        if (c == L'>' && inTag) {
            inTag = false;
            continue;
        }
        if (!inTag) {
            noTags.push_back(c);
        }
    }

    std::wstring decoded;
    decoded.reserve(noTags.size());
    for (size_t i = 0; i < noTags.size(); ++i) {
        if (noTags[i] == L'&') {
            const size_t semi = noTags.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 9) {
                const std::wstring ent = noTags.substr(i + 1, semi - i - 1);
                wchar_t rep = 0;
                if (ent == L"amp") rep = L'&';
                else if (ent == L"lt") rep = L'<';
                else if (ent == L"gt") rep = L'>';
                else if (ent == L"quot") rep = L'"';
                else if (ent == L"apos") rep = L'\'';
                else if (ent == L"nbsp") rep = L' ';
                else if (ent.size() > 1 && ent[0] == L'#') {
                    const unsigned long code =
                        (ent[1] == L'x' || ent[1] == L'X')
                            ? wcstoul(ent.c_str() + 2, nullptr, 16)
                            : wcstoul(ent.c_str() + 1, nullptr, 10);
                    if (code > 0 && code < 0x10000 && !(code >= 0xD800 && code <= 0xDFFF)) {
                        rep = static_cast<wchar_t>(code);
                    }
                }
                if (rep) {
                    decoded.push_back(rep);
                    i = semi;
                    continue;
                }
            }
        }
        decoded.push_back(noTags[i]);
    }
    return CollapseWhitespace(decoded);
}

// Reads the "en" section of a Wiktionary definition response.
bool ParseWiktionaryEnglish(const std::string& body, const std::wstring& title, Result* out) {
    const size_t enKey = body.find("\"en\":[");
    if (enKey == std::string::npos) {
        return false;  // the word exists, but only in other languages
    }

    auto skipSeparators = [](const std::string& s, size_t p) {
        while (p < s.size() && (s[p] == ' ' || s[p] == ',' || s[p] == '\n' ||
                                s[p] == '\r' || s[p] == '\t')) {
            ++p;
        }
        return p;
    };

    std::vector<std::wstring> partsOfSpeech;
    std::vector<std::wstring> definitions;
    std::wstring example;

    size_t p = enKey + 6;  // just past  "en":[
    while (p < body.size()) {
        p = skipSeparators(body, p);
        if (p >= body.size() || body[p] != '{') {
            break;
        }
        const std::string block = BalancedObjectAt(body, p);
        if (block.empty()) {
            break;
        }
        p += block.size();

        std::string posRaw;
        ExtractJsonStringField(block, "partOfSpeech", &posRaw);
        const std::wstring pos = ToLowerCopy(CollapseWhitespace(JsonStringUnescape(posRaw)));
        if (!pos.empty() && partsOfSpeech.size() < 3 &&
            std::find(partsOfSpeech.begin(), partsOfSpeech.end(), pos) == partsOfSpeech.end()) {
            partsOfSpeech.push_back(pos);
        }

        // Definitions are taken from the first part of speech only.
        if (!definitions.empty()) {
            continue;
        }
        const size_t defsKey = block.find("\"definitions\":[");
        if (defsKey == std::string::npos) {
            continue;
        }
        size_t q = defsKey + 15;  // just past  "definitions":[
        while (q < block.size() && definitions.size() < 3) {
            q = skipSeparators(block, q);
            if (q >= block.size() || block[q] != '{') {
                break;
            }
            const std::string defObj = BalancedObjectAt(block, q);
            if (defObj.empty()) {
                break;
            }
            q += defObj.size();

            std::string defRaw;
            if (!ExtractJsonStringField(defObj, "definition", &defRaw)) {
                continue;
            }
            const std::wstring text = StripHtml(JsonStringUnescape(defRaw));
            if (text.empty()) {
                continue;
            }
            definitions.push_back(text);

            // Only the first definition's example is shown.
            if (definitions.size() == 1) {
                std::string exRaw;
                if (ExtractJsonStringField(defObj, "example", &exRaw)) {
                    example = StripHtml(JsonStringUnescape(exRaw));
                }
            }
        }
    }

    if (definitions.empty()) {
        return false;
    }

    Result r;
    r.title = title;
    for (size_t i = 0; i < partsOfSpeech.size(); ++i) {
        if (i > 0) {
            r.subtitle += L"  \u00b7  ";
        }
        r.subtitle += partsOfSpeech[i];
    }

    std::wstring bodyText;
    for (size_t i = 0; i < definitions.size(); ++i) {
        if (i > 0) {
            bodyText += L"\n";
        }
        if (definitions.size() > 1) {
            bodyText += std::to_wstring(i + 1) + L". ";
        }
        bodyText += ClipToChars(definitions[i], 170);
    }
    r.body = ClipToChars(bodyText, kMaxBodyChars);

    if (!example.empty()) {
        r.example = L"\u201c" + ClipToChars(example, 200) + L"\u201d";
    }
    r.source = L"Wiktionary";
    *out = std::move(r);
    return true;
}

// Titles are case-sensitive, so lowercase is tried first, then the text as typed.
Outcome LookupWiktionary(const std::wstring& word, Result* out) {
    const std::wstring lower = ToLowerCopy(word);
    const std::wstring candidates[2] = {lower, word};

    for (int i = 0; i < 2; ++i) {
        if (i == 1 && candidates[1] == candidates[0]) {
            break;
        }
        const std::wstring path = L"/api/rest_v1/page/definition/" +
                                  AsciiToWide(UrlEncodeComponent(candidates[i]));
        const HttpResult res = HttpGetEx(L"en.wiktionary.org", path.c_str(), true, kLookupUserAgent);
        if (!res.IsDefinitive()) {
            return Outcome::Failed;  // timeout / 429 / 5xx: never cached as "not found"
        }
        if (res.status != 200) {
            continue;  // 404: try the next spelling
        }
        if (ParseWiktionaryEnglish(res.body, candidates[i], out)) {
            return Outcome::Found;
        }
    }
    return Outcome::NotFound;
}

// ---- router -----------------------------------------------------------------------
// Single words:  Wiktionary -> dictionaryapi.dev (backup) -> Wikipedia
// Phrases:       Wikipedia  -> Wiktionary (idioms like "kick the bucket")
// To add a source: write LookupXxx() above and add one attempt() line below.
Outcome LookupText(const std::wstring& query, Result* out) {
    const bool singleWord = query.find(L' ') == std::wstring::npos;

    bool anyFailed = false;
    auto attempt = [&](Outcome outcome) -> bool {
        if (outcome == Outcome::Found) return true;
        if (outcome == Outcome::Failed) anyFailed = true;
        return false;
    };

    if (singleWord) {
        if (attempt(LookupWiktionary(query, out))) return Outcome::Found;
        if (attempt(LookupDictionary(query, out))) return Outcome::Found;
        if (attempt(LookupWikipedia(query, out))) return Outcome::Found;
    } else {
        if (attempt(LookupWikipedia(query, out))) return Outcome::Found;
        if (attempt(LookupWiktionary(query, out))) return Outcome::Found;
    }

    // "Nothing there" is only cacheable if every source gave a definite answer.
    return anyFailed ? Outcome::Failed : Outcome::NotFound;
}

// ---- publishing ---------------------------------------------------------------------
void FillSnapshot(LookupSnapshot& lk, const std::wstring& query, bool found, const Result* r,
                  double now) {
    lk = LookupSnapshot{};
    lk.active = true;
    lk.query = query;
    if (found && r) {
        lk.status = LookupStatus::Found;
        lk.title = r->title;
        lk.subtitle = r->subtitle;
        lk.body = r->body;
        lk.example = r->example;
        lk.source = r->source;
        lk.expiresAt = now + kResultSeconds;
    } else {
        lk.status = LookupStatus::NoResult;
        lk.expiresAt = now + kMessageSeconds;
    }
}

// Moves `query` to the front of the recent list. Caller must hold g_stateMutex.
void PushRecentLocked(SharedState& st, const std::wstring& query, const Result& r) {
    LookupRecent entry;
    entry.query = query;
    entry.title = r.title;
    entry.source = r.source;

    std::wstring detail = r.body;
    std::replace(detail.begin(), detail.end(), L'\n', L' ');
    if (detail.rfind(L"1. ", 0) == 0) {
        detail.erase(0, 3);
    }
    entry.detail = ClipToChars(CollapseWhitespace(detail), 60);

    auto& list = st.lookupRecent;
    const std::wstring key = ToLowerCopy(query);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const LookupRecent& e) { return ToLowerCopy(e.query) == key; }),
               list.end());
    list.insert(list.begin(), std::move(entry));
    if (list.size() > kRecentCapacity) {
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(kRecentCapacity), list.end());
    }
}

// Worker thread -> island. Dropped if the request was superseded or dismissed, or
// if its "Looking up..." card has already timed out.
void ApplyResult(uint64_t id, const std::wstring& query, bool found, const Result& r) {
    if (!g_running.load()) {
        return;
    }
    const double now = NowSeconds();
    {
        std::lock_guard lock(g_stateMutex);
        LookupSnapshot& lk = g_state.lookup;
        if (g_lookupRequestSeq.load() != id || !lk.active || now >= lk.expiresAt) {
            return;
        }
        FillSnapshot(lk, query, found, &r, now);
        if (found) {
            PushRecentLocked(g_state, query, r);
        }
    }
    g_layoutDirty = true;  // repaint; the springs resize the island to the new card
}

}  // namespace QuickLookup

