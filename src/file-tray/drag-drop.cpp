bool TrayModuleEnabled() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings.fileTrayModule;
}

void AddPathsToFileTrayCore(const std::vector<std::wstring>& paths) {
    int maxItems = 10;
    {
        std::lock_guard lock(g_settingsMutex);
        maxItems = std::max(1, g_settings.fileTrayMaxItems);
    }

    std::vector<FileTrayItem> added;
    added.reserve(paths.size());
    for (const std::wstring& path : paths) {
        FileTrayItem item;
        item.path = path;
        item.name = BaseNameFromPath(path);
        if (item.name.empty()) {
            item.name = path;
        }

        WIN32_FILE_ATTRIBUTE_DATA attr = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
            item.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.sizeBytes = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        }

        SHFILEINFOW info = {};
        if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
            item.icon = IconToPixelsRef(info.hIcon, 32);
            DestroyIcon(info.hIcon);
        }
        added.push_back(std::move(item));
    }

    if (added.empty()) {
        return;
    }

    g_fileTrayScroll.store(0, std::memory_order_relaxed);  // show the newest file

    std::lock_guard lock(g_stateMutex);
    for (auto& item : added) {
        // Re-dropping a file promotes the existing entry instead of duplicating it.
        auto existing = std::find_if(g_state.fileTrayItems.begin(), g_state.fileTrayItems.end(),
                                     [&](const FileTrayItem& other) {
                                         return _wcsicmp(other.path.c_str(), item.path.c_str()) == 0;
                                     });
        if (existing != g_state.fileTrayItems.end()) {
            g_state.fileTrayItems.erase(existing);
        }
        g_state.fileTrayItems.push_back(std::move(item));
    }
    while (g_state.fileTrayItems.size() > static_cast<size_t>(maxItems)) {
        g_state.fileTrayItems.erase(g_state.fileTrayItems.begin());
    }
}

// Adds files and persists the result. Callers use this; the Core version above
// only changes the in-memory list.
void AddPathsToFileTray(const std::vector<std::wstring>& paths) {
    AddPathsToFileTrayCore(paths);
    SaveFileTray();
}

void RemoveFileTrayItemByPath(const std::wstring& path) {
    {
        std::lock_guard lock(g_stateMutex);
        auto& items = g_state.fileTrayItems;
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const FileTrayItem& item) {
                                       return _wcsicmp(item.path.c_str(), path.c_str()) == 0;
                                   }),
                    items.end());
    }
    SaveFileTray();
}

bool ExtractDropPaths(IDataObject* data, std::vector<std::wstring>* out) {
    if (!data || !out) {
        return false;
    }
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&fmt, &medium))) {
        return false;
    }

    // An HDROP is just the HGLOBAL; DragQueryFile locks it itself.
    HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) {
            continue;
        }
        std::wstring path(len, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        out->push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return !out->empty();
}

void AddTextToFileTray(const std::wstring& rawText) {
    std::wstring text = rawText;
    while (!text.empty() && iswspace(text.back())) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    constexpr size_t kMaxChars = 100000;
    if (text.size() > kMaxChars) {
        text.resize(kMaxChars);
    }

    // One-line preview for the row: whitespace collapsed, capped.
    constexpr size_t kPreviewChars = 60;
    std::wstring preview;
    bool lastSpace = true;
    bool truncated = false;
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
        }
        if (ch == L' ') {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        if (preview.size() >= kPreviewChars) {
            truncated = true;
            break;
        }
        preview.push_back(ch);
    }
    while (!preview.empty() && preview.back() == L' ') {
        preview.pop_back();
    }
    if (truncated) {
        preview += L"...";
    }

    int maxItems = 10;
    {
        std::lock_guard lock(g_settingsMutex);
        maxItems = std::max(1, g_settings.fileTrayMaxItems);
    }

    FileTrayItem item;
    item.isText = true;
    item.text = std::move(text);
    item.name = preview.empty() ? std::wstring(L"Text") : preview;

    g_fileTrayScroll.store(0, std::memory_order_relaxed);  // show the newest snippet

    std::lock_guard lock(g_stateMutex);
    auto& items = g_state.fileTrayItems;
    // Pasting the same snippet again promotes it instead of duplicating it.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const FileTrayItem& other) {
                                   return other.isText && other.text == item.text;
                               }),
                items.end());
    items.push_back(std::move(item));
    while (items.size() > static_cast<size_t>(maxItems)) {
        items.erase(items.begin());
    }
}

void CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(hwnd)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) {
                GlobalFree(mem);
            }
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// Pastes whatever is on the clipboard into the tray: copied files first
// (as references), otherwise copied text.
void PasteIntoFileTray(HWND hwnd) {
    std::vector<std::wstring> paths;
    if (OpenClipboard(hwnd)) {
        HANDLE data = GetClipboardData(CF_HDROP);
        if (data) {
            HDROP drop = reinterpret_cast<HDROP>(data);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                if (len == 0) {
                    continue;
                }
                std::wstring path(len, L'\0');
                DragQueryFileW(drop, i, path.data(), len + 1);
                paths.push_back(std::move(path));
            }
        }
        CloseClipboard();
    }

    if (!paths.empty()) {
        AddPathsToFileTray(paths);
    } else {
        AddTextToFileTray(ReadClipboardText(hwnd));
        SaveFileTray();
    }

    const int tab = FileTrayTabIndex(GetSettingsCopy());
    if (tab >= 0) {
        g_idleTab = tab;
    }
    g_clickExpanded = true;
    g_layoutDirty = true;
    TriggerNudge();
}

DWORD PickTrayDropEffect(DWORD allowed) {
    // The tray only references files, so never ask the source to move them.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

// For non-file drags (browser images / links / text). Nothing is ever deleted from
// the source for these, so MOVE is harmless and some apps only offer it.
DWORD PickTrayDropEffectAny(DWORD allowed) {
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    return DROPEFFECT_NONE;
}

// ── Non-Explorer drops (Brave / Chrome / Edge / Firefox ...) ─────────────────
CLIPFORMAT TrayFmtFileDescriptorW() {
    static const CLIPFORMAT f = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    return f;
}
CLIPFORMAT TrayFmtFileContents() {
    static const CLIPFORMAT f = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    return f;
}
CLIPFORMAT TrayFmtInetUrlW() {
    static const CLIPFORMAT f = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
    return f;
}

// True when the drag carries something the tray can keep. *realFiles is set only
// for genuine CF_HDROP files (those are never allowed to be "moved").
bool TrayDataAcceptable(IDataObject* data, bool* realFiles) {
    *realFiles = false;
    if (!data) return false;

    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (data->QueryGetData(&fmt) == S_OK) {
        *realFiles = true;
        return true;
    }
    fmt.cfFormat = TrayFmtFileDescriptorW();      // images dragged out of a browser
    if (data->QueryGetData(&fmt) == S_OK) return true;
    fmt.cfFormat = TrayFmtInetUrlW();             // dragged links
    if (data->QueryGetData(&fmt) == S_OK) return true;
    fmt.cfFormat = CF_UNICODETEXT;                // dragged text selections
    if (data->QueryGetData(&fmt) == S_OK) return true;
    return false;
}

std::wstring SanitizeDroppedFileName(std::wstring name) {
    name = BaseNameFromPath(name);
    for (wchar_t& c : name) {
        if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c) != nullptr) c = L'_';
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    if (name.empty()) name = L"dropped-file";
    if (name.size() > 120) name.resize(120);
    return name;
}

HANDLE CreateUniqueDroppedFile(const std::wstring& dir, const std::wstring& name,
                               std::wstring* outPath) {
    const size_t dot = name.find_last_of(L'.');
    const bool hasExt = dot != std::wstring::npos && dot > 0;
    const std::wstring stem = hasExt ? name.substr(0, dot) : name;
    const std::wstring ext = hasExt ? name.substr(dot) : std::wstring();

    for (int n = 0; n < 1000; ++n) {
        const std::wstring suffix = n ? (L" (" + std::to_wstring(n) + L")") : std::wstring();
        const std::wstring candidate = dir + L"\\" + stem + suffix + ext;
        HANDLE h = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            *outPath = candidate;
            return h;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) break;
    }
    return INVALID_HANDLE_VALUE;
}

bool WriteMediumToFile(STGMEDIUM& medium, HANDLE file) {
    constexpr uint64_t kMaxBytes = 512ull * 1024 * 1024;
    uint64_t total = 0;

    if (medium.tymed == TYMED_ISTREAM && medium.pstm) {
        LARGE_INTEGER zero = {};
        medium.pstm->Seek(zero, STREAM_SEEK_SET, nullptr);
        std::vector<BYTE> buffer(64 * 1024);
        for (;;) {
            ULONG got = 0;
            const HRESULT hr = medium.pstm->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &got);
            if (got > 0) {
                DWORD written = 0;
                if (!WriteFile(file, buffer.data(), got, &written, nullptr) || written != got) return false;
                total += got;
                if (total > kMaxBytes) return false;
            }
            if (hr != S_OK || got == 0) break;
        }
        return total > 0;
    }

    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
        const SIZE_T size = GlobalSize(medium.hGlobal);
        if (size == 0 || size > kMaxBytes) return false;
        void* src = GlobalLock(medium.hGlobal);
        if (!src) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(file, src, static_cast<DWORD>(size), &written, nullptr);
        GlobalUnlock(medium.hGlobal);
        return ok && written == size;
    }
    return false;
}

// Browser images have no file on disk, so they are saved into the mod's storage
// folder (filetray\dropped) and the saved path goes on the shelf like any file.
bool ExtractVirtualFilePaths(IDataObject* data, std::vector<std::wstring>* out) {
    if (!data || !out) return false;

    FORMATETC descFmt = {TrayFmtFileDescriptorW(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM descMedium = {};
    if (FAILED(data->GetData(&descFmt, &descMedium))) return false;

    std::vector<std::wstring> names;
    std::vector<bool> skip;
    if (descMedium.hGlobal) {
        const SIZE_T size = GlobalSize(descMedium.hGlobal);
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(descMedium.hGlobal));
        if (group && size >= sizeof(UINT)) {
            const UINT maxFit = size >= sizeof(FILEGROUPDESCRIPTORW)
                ? static_cast<UINT>(1 + (size - sizeof(FILEGROUPDESCRIPTORW)) / sizeof(FILEDESCRIPTORW))
                : 0;
            const UINT count = std::min<UINT>(group->cItems, std::min<UINT>(maxFit, 16));
            for (UINT i = 0; i < count; ++i) {
                const FILEDESCRIPTORW& fd = group->fgd[i];
                wchar_t nameBuf[MAX_PATH + 1] = {};
                wcsncpy_s(nameBuf, fd.cFileName, MAX_PATH);
                names.push_back(nameBuf);
                skip.push_back((fd.dwFlags & FD_ATTRIBUTES) &&
                               (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
            }
        }
        if (group) GlobalUnlock(descMedium.hGlobal);
    }
    ReleaseStgMedium(&descMedium);
    if (names.empty()) return false;

    wchar_t storagePath[MAX_PATH] = {};
    if (!Wh_GetModStoragePath(storagePath, ARRAYSIZE(storagePath))) return false;
    const std::wstring dir = std::wstring(storagePath) + L"\\filetray\\dropped";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    for (size_t i = 0; i < names.size(); ++i) {
        if (skip[i]) continue;

        FORMATETC contentFmt = {TrayFmtFileContents(), nullptr, DVASPECT_CONTENT,
                                static_cast<LONG>(i), TYMED_ISTREAM | TYMED_HGLOBAL};
        STGMEDIUM content = {};
        if (FAILED(data->GetData(&contentFmt, &content))) continue;

        std::wstring savedPath;
        HANDLE file = CreateUniqueDroppedFile(dir, SanitizeDroppedFileName(names[i]), &savedPath);
        if (file != INVALID_HANDLE_VALUE) {
            const bool ok = WriteMediumToFile(content, file);
            CloseHandle(file);
            if (ok) {
                out->push_back(savedPath);
            } else {
                DeleteFileW(savedPath.c_str());
            }
        }
        ReleaseStgMedium(&content);
    }
    return !out->empty();
}

// Dragged link or selected text -> a text snippet on the shelf.
bool ExtractDropText(IDataObject* data, std::wstring* out) {
    if (!data || !out) return false;

    const CLIPFORMAT formats[2] = {TrayFmtInetUrlW(), CF_UNICODETEXT};
    for (CLIPFORMAT cf : formats) {
        FORMATETC fmt = {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        if (FAILED(data->GetData(&fmt, &medium))) continue;

        std::wstring text;
        if (medium.hGlobal) {
            const SIZE_T maxChars = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
            const auto* p = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
            if (p) {
                text.assign(p, wcsnlen_s(p, maxChars));
                GlobalUnlock(medium.hGlobal);
            }
        }
        ReleaseStgMedium(&medium);

        if (!text.empty()) {
            *out = std::move(text);
            return true;
        }
    }
    return false;
}

// Receives drags while they are still hovering, which is what lets the island
// expand onto the File Tray *before* the drop.
class TrayDropTarget final : public IDropTarget {
   public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropTarget))) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = --refs_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        accept_ = false;
        realFiles_ = false;
        if (!g_trayInternalDrag.load() && TrayModuleEnabled() && data) {
            // Explorer files, browser images (virtual files), links and text.
            accept_ = TrayDataAcceptable(data, &realFiles_);
        }

        if (accept_) {
            tab_ = FileTrayTabIndex(GetSettingsCopy());
            if (tab_ >= 0) {
                g_idleTab = tab_;
            }
            g_trayDragOver = true;
            g_clickExpanded = true;
            g_layoutDirty = true;
            TriggerNudge();
        }
        if (effect) {
            *effect = accept_ ? PickEffect(*effect) : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override {
        if (accept_ && tab_ >= 0) {
            g_idleTab = tab_;  // stay on the tray while the island expands
        }
        if (effect) {
            *effect = accept_ ? PickEffect(*effect) : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragLeave() override {
        accept_ = false;
        realFiles_ = false;
        g_trayDragOver = false;
        g_layoutDirty = true;
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        std::vector<std::wstring> paths;
        std::wstring text;
        bool gotFiles = false;
        bool gotText = false;

        if (accept_ && data) {
            gotFiles = ExtractDropPaths(data, &paths);               // Explorer
            if (!gotFiles) {
                paths.clear();
                gotFiles = ExtractVirtualFilePaths(data, &paths);    // browser images
            }
            if (!gotFiles) {
                gotText = ExtractDropText(data, &text);              // links / text
            }
        }
        accept_ = false;

        if (gotFiles || gotText) {
            if (gotFiles) {
                AddPathsToFileTray(paths);
            } else {
                AddTextToFileTray(text);
                SaveFileTray();
            }
            const int tab = FileTrayTabIndex(GetSettingsCopy());
            if (tab >= 0) {
                g_idleTab = tab;
            }
            g_clickExpanded = true;
            g_layoutDirty = true;
            TriggerNudge();
            if (effect) *effect = PickEffect(*effect);
        } else if (effect) {
            *effect = DROPEFFECT_NONE;
        }
        realFiles_ = false;
        g_trayDragOver = false;
        return S_OK;
    }

   private:
    // Real files are never "moved" (the source would delete them); browser
    // content has nothing to delete, so any effect is fine there.
    DWORD PickEffect(DWORD allowed) const {
        return realFiles_ ? PickTrayDropEffect(allowed) : PickTrayDropEffectAny(allowed);
    }

    std::atomic<LONG> refs_{1};
    bool accept_ = false;
    bool realFiles_ = false;
    int tab_ = -1;
};

class TrayDropSource final : public IDropSource {
   public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropSource))) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = --refs_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHODIMP QueryContinueDrag(BOOL escPressed, DWORD keyState) override {
        if (escPressed) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

   private:
    std::atomic<LONG> refs_{1};
};

// Builds the same data object Explorer would hand out for this file, so any
// app, folder or the desktop accepts it as a normal file drag.
bool CreateFileDataObject(const std::wstring& path, IDataObject** out) {
    *out = nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        return false;
    }

    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr)) {
        hr = parent->GetUIObjectOf(nullptr, 1, &child, __uuidof(IDataObject), nullptr,
                                   reinterpret_cast<void**>(out));
    }
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr) && *out != nullptr;
}

// Blocks (modal OLE loop) until the drag finishes.
void StartFileTrayDrag(const std::wstring& path) {
    ComPtr<IDataObject> dataObject;
    if (!CreateFileDataObject(path, dataObject.GetAddressOf())) {
        return;
    }

    ComPtr<IDropSource> source;
    source.Attach(new TrayDropSource());

    constexpr DWORD kOkEffects = DROPEFFECT_COPY | DROPEFFECT_LINK;
    DWORD effect = DROPEFFECT_NONE;

    // SHDoDragDrop adds the normal Explorer drag image (thumbnail + name).
    // Resolved dynamically; plain DoDragDrop is the fallback.
    using SHDoDragDrop_t = HRESULT(WINAPI*)(HWND, IDataObject*, IDropSource*, DWORD, DWORD*);
    static const auto pSHDoDragDrop = reinterpret_cast<SHDoDragDrop_t>(
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHDoDragDrop"));

    g_trayInternalDrag = true;
    if (pSHDoDragDrop) {
        pSHDoDragDrop(nullptr, dataObject.Get(), source.Get(), kOkEffects, &effect);
    } else {
        DoDragDrop(dataObject.Get(), source.Get(), kOkEffects, &effect);
    }
    g_trayInternalDrag = false;

    g_hoveredFileTrayRow = -1;
    g_hoveredFileTrayAction = -1;
    g_layoutDirty = true;

    // File was moved away by the target: its tray entry is now stale.
    if (effect != DROPEFFECT_NONE && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        RemoveFileTrayItemByPath(path);
    }
}

