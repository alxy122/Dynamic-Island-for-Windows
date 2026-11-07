// ── File Tray drag & drop (#33) ──────────────────────────────────────────────
// ── File Tray persistence ────────────────────────────────────────────────────
// The shelf survives restarts. Only *references* are stored (a path per file,
// the text for pasted snippets) -- never copies of the files themselves.
//
//   %LOCALAPPDATA%\DynamicIslandForWindows\filetray\items.dat
//
// Layout (UTF-8, one item per line, oldest first):
//   DIWFT 1
//   F<TAB><path>
//   T<TAB><preview name><TAB><full text>
// Backslash, newline, CR and TAB inside a value are escaped, so one item is
// always exactly one line. Size, folder flag and the Explorer icon are re-read
// on load instead of being stored.
namespace FileTrayStore {

std::mutex g_ioMutex;  // serialises the load/save file access

inline std::wstring GetCurrentUserSid() {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
        if (len > 0) {
            std::vector<BYTE> buffer(len);
            if (GetTokenInformation(hToken, TokenUser, buffer.data(), len, &len)) {
                auto* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
                LPWSTR sidString = nullptr;
                if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidString)) {
                    std::wstring result = sidString;
                    LocalFree(sidString);
                    CloseHandle(hToken);
                    return result;
                }
            }
        }
        CloseHandle(hToken);
    }
    wchar_t userBuf[256] = {};
    DWORD un = ARRAYSIZE(userBuf);
    if (GetUserNameW(userBuf, &un) && un > 0) {
        return userBuf;
    }
    return L"default";
}

bool ResolvePaths(std::wstring* dir, std::wstring* file) {
    wchar_t storagePath[MAX_PATH] = {};
    if (!Wh_GetModStoragePath(storagePath, ARRAYSIZE(storagePath))) {
        return false;
    }
    const std::wstring userKey = GetCurrentUserSid();
    *dir = std::wstring(storagePath) + L"\\filetray\\" + userKey;
    *file = *dir + L"\\items.dat";
    return true;
}

std::string Escape(const std::wstring& value) {
    std::string out;
    for (const char c : LyricsCache::WideToUtf8(value)) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::wstring Unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[++i];
            switch (next) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(next); break;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return LyricsCache::Utf8ToWide(out);
}

}  // namespace FileTrayStore

// Writes the current tray to disk. Safe to call from any thread, and never call
// it while holding g_stateMutex. With persistence off, or an empty tray, the
// saved file is removed instead.
void SaveFileTray() {
    bool persist = true;
    {
        std::lock_guard lock(g_settingsMutex);
        persist = g_settings.fileTrayPersist;
    }

    struct Row {
        bool isText;
        std::wstring name;
        std::wstring value;
    };
    std::vector<Row> rows;
    {
        std::lock_guard lock(g_stateMutex);
        rows.reserve(g_state.fileTrayItems.size());
        for (const FileTrayItem& item : g_state.fileTrayItems) {
            rows.push_back(Row{item.isText, item.name, item.isText ? item.text : item.path});
        }
    }

    std::lock_guard io(FileTrayStore::g_ioMutex);
    std::wstring dir, file;
    if (!FileTrayStore::ResolvePaths(&dir, &file)) {
        return;
    }

    if (!persist || rows.empty()) {
        DeleteFileW(file.c_str());
        return;
    }

    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    std::string data = "DIWFT 1\n";
    for (const Row& row : rows) {
        if (row.isText) {
            data += "T\t" + FileTrayStore::Escape(row.name) + "\t" +
                    FileTrayStore::Escape(row.value) + "\n";
        } else {
            data += "F\t" + FileTrayStore::Escape(row.value) + "\n";
        }
    }

    if (!LyricsCache::WriteFileAtomic(file, data)) {
        Wh_Log(L"File Tray: could not save %s (error %lu).", file.c_str(), GetLastError());
    }
}

// Reads the saved tray back. Runs once on the render thread at startup (it needs
// COM for the shell icons). Files that have since moved or been deleted are kept,
// since you asked for items to stay until you remove them; they just show a
// generic icon.
void LoadFileTray() {
    bool persist = true;
    int maxItems = 10;
    {
        std::lock_guard lock(g_settingsMutex);
        persist = g_settings.fileTrayPersist;
        maxItems = std::max(1, g_settings.fileTrayMaxItems);
    }

    std::lock_guard io(FileTrayStore::g_ioMutex);
    std::wstring dir, file;
    if (!FileTrayStore::ResolvePaths(&dir, &file)) {
        return;
    }
    if (!persist) {
        DeleteFileW(file.c_str());
        return;
    }

    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    constexpr LONGLONG kMaxBytes = 16ll * 1024 * 1024;
    std::string data;
    LARGE_INTEGER size = {};
    bool readOk = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxBytes;
    if (readOk) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        readOk = ReadFile(h, data.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                 got == static_cast<DWORD>(size.QuadPart);
    }
    CloseHandle(h);

    static const char kHeader[] = "DIWFT 1\n";
    if (!readOk || data.compare(0, sizeof(kHeader) - 1, kHeader) != 0) {
        Wh_Log(L"File Tray: saved list is unreadable; starting empty.");
        return;
    }

    std::vector<FileTrayItem> loaded;
    size_t pos = sizeof(kHeader) - 1;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        const std::string line = data.substr(pos, end - pos);
        pos = end + 1;

        if (line.size() < 3 || line[1] != '\t') {
            continue;
        }

        FileTrayItem item;
        if (line[0] == 'T') {
            const std::string rest = line.substr(2);
            const size_t tab = rest.find('\t');
            if (tab == std::string::npos) continue;
            item.isText = true;
            item.name = FileTrayStore::Unescape(rest.substr(0, tab));
            item.text = FileTrayStore::Unescape(rest.substr(tab + 1));
            if (item.text.empty()) continue;
            if (item.name.empty()) item.name = L"Text";
        } else if (line[0] == 'F') {
            item.path = FileTrayStore::Unescape(line.substr(2));
            if (item.path.empty()) continue;
            item.name = BaseNameFromPath(item.path);
            if (item.name.empty()) item.name = item.path;

            WIN32_FILE_ATTRIBUTE_DATA attr = {};
            const bool exists =
                GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &attr) != 0;
            if (exists) {
                item.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                item.sizeBytes =
                    (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            }

            // A missing file gets its type icon from the extension alone, which
            // never touches the disk (so an unplugged drive can't stall startup).
            SHFILEINFOW info = {};
            UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
            if (!exists) flags |= SHGFI_USEFILEATTRIBUTES;
            if (SHGetFileInfoW(item.path.c_str(), exists ? 0 : FILE_ATTRIBUTE_NORMAL, &info,
                               sizeof(info), flags) &&
                info.hIcon) {
                item.icon = IconToPixelsRef(info.hIcon, 32);
                DestroyIcon(info.hIcon);
            }
        } else {
            continue;
        }
        loaded.push_back(std::move(item));
    }

    if (loaded.empty()) {
        return;
    }

    std::lock_guard lock(g_stateMutex);
    auto& items = g_state.fileTrayItems;
    // Saved items are older than anything dropped since startup, so they go in front.
    items.insert(items.begin(), std::make_move_iterator(loaded.begin()),
                 std::make_move_iterator(loaded.end()));
    while (items.size() > static_cast<size_t>(maxItems)) {
        items.erase(items.begin());
    }
    Wh_Log(L"File Tray: restored %zu item(s).", items.size());
}

