// ── Lyrics (LRCLIB, https://lrclib.net) ──────────────────────────────────────
// User-Agent sent to LRCLIB.
constexpr wchar_t kLyricsUserAgent[] =
    L"DynamicIslandForWindows/" WH_MOD_VERSION L" (https://github.com/devcode90/Dynamic-Island-for-Windows)";

bool IsUrlSafeChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string UrlEncodeComponent(const std::wstring& value) {
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (len > 0) {
        utf8.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    }

    std::string out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : utf8) {
        if (IsUrlSafeChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::wstring AsciiToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return w;
}

// Unescapes a raw JSON string value (still containing \n, \", \uXXXX etc.)
// and decodes it from UTF-8 into a wide string.
std::wstring JsonStringUnescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            switch (next) {
                case 'n': out.push_back('\n'); ++i; break;
                case 'r': out.push_back('\r'); ++i; break;
                case 't': out.push_back('\t'); ++i; break;
                case '"': out.push_back('"'); ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                case '/': out.push_back('/'); ++i; break;
                case 'u': {
                    if (i + 5 < raw.size()) {
                        std::string hexNarrow = raw.substr(i + 2, 4);
                        std::wstring hexWide = AsciiToWide(hexNarrow);
                        wchar_t code = static_cast<wchar_t>(wcstoul(hexWide.c_str(), nullptr, 16));
                        char utf8buf[4] = {};
                        int outLen = WideCharToMultiByte(CP_UTF8, 0, &code, 1, utf8buf,
                                                         sizeof(utf8buf), nullptr, nullptr);
                        if (outLen > 0) {
                            out.append(utf8buf, outLen);
                        }
                        i += 5;
                    }
                    break;
                }
                default:
                    out.push_back(next);
                    ++i;
                    break;
            }
        } else {
            out.push_back(c);
        }
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
    std::wstring wout;
    if (wlen > 0) {
        wout.resize(wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, wout.data(), wlen);
    }
    return wout;
}

// Finds "fieldName":"....." in a JSON blob and returns the raw (still
// escaped) contents between the quotes. Returns false if missing or null.
bool ExtractJsonStringField(const std::string& json, const char* fieldKey, std::string* outRaw) {
    const std::string key = std::string("\"") + fieldKey + "\":\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return false;
    }
    pos += key.size();

    std::string result;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            result.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            result.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '"') {
            *outRaw = result;
            return true;
        }
        result.push_back(c);
    }
    return false;
}

// Splits a top-level JSON array "[ {...}, {...} ]" into the substrings for
// each object, respecting nested braces and quoted strings.
std::vector<std::string> SplitJsonObjectArray(const std::string& json) {
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
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
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }

    return objects;
}

std::vector<LyricsLine> ParseSyncedLyrics(const std::wstring& text) {
    std::vector<LyricsLine> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);

        if (line.size() >= 3 && line[0] == L'[') {
            const size_t closeBracket = line.find(L']');
            if (closeBracket != std::wstring::npos) {
                int minutes = 0, seconds = 0, hundredths = 0;
                const std::wstring ts = line.substr(1, closeBracket - 1);
                if (swscanf_s(ts.c_str(), L"%d:%d.%d", &minutes, &seconds, &hundredths) >= 2) {
                    std::wstring lyricText = line.substr(closeBracket + 1);
                    while (!lyricText.empty() && lyricText.front() == L' ') {
                        lyricText.erase(lyricText.begin());
                    }
                    LyricsLine ll;
                    ll.timeMs = static_cast<int64_t>(minutes) * 60000 +
                               static_cast<int64_t>(seconds) * 1000 +
                               static_cast<int64_t>(hundredths) * 10;
                    ll.text = std::move(lyricText);
                    lines.push_back(std::move(ll));
                }
            }
        }

        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

std::vector<LyricsLine> ParsePlainLyrics(const std::wstring& text) {
    std::vector<LyricsLine> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty()) {
            LyricsLine ll;
            ll.timeMs = -1;
            ll.text = std::move(line);
            lines.push_back(std::move(ll));
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

