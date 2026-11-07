uint64_t PublishNewLookup(const LookupSnapshot& snap, bool nudge = true) {
    uint64_t id;
    {
        std::lock_guard lock(g_stateMutex);
        g_state.lookup = snap;
        id = ++g_lookupRequestSeq;
    }
    g_layoutDirty = true;
    if (nudge) {
        TriggerNudge();  // typing and searching stay quiet; only opening the panel bounces
    }
    return id;
}

// ── Quick Lookup panel: session + input ──────────────────────────────────────
// All of this runs on the render thread. g_state.lookup is the only part worker
// threads also touch, so it only changes under g_stateMutex.

// Keeps the panel alive while the user is interacting with it.
void TouchLookupPanel() {
    std::lock_guard lock(g_stateMutex);
    if (g_state.lookup.active) {
        g_state.lookup.expiresAt =
            std::max(g_state.lookup.expiresAt, NowSeconds() + QuickLookup::kPanelIdleSeconds);
    }
}

std::vector<LookupRow> CurrentLookupRows(LookupStatus* status) {
    std::lock_guard lock(g_stateMutex);
    if (status) *status = g_state.lookup.status;
    return BuildLookupRows(g_state.lookupRecent, g_lookupUi);
}

void ForceForeground(HWND hwnd) {
    INPUT input{};
    SendInput(1, &input, sizeof(INPUT));
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    DwmBackdrop::EnsureBehind(hwnd);
    SetFocus(hwnd);
}

// The island is WS_EX_NOACTIVATE so it never steals focus. While the search box is
// open it has to be able to receive keys, so the flag is dropped for the session.
void BeginLookupSession(HWND hwnd) {
    HWND previous = GetForegroundWindow();
    g_lookupUi.prevForeground = (previous && previous != hwnd) ? previous : nullptr;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    }
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    ForceForeground(hwnd);
}

void EndLookupSession(HWND hwnd) {
    if (!g_lookupUi.open) return;
    HWND previous = g_lookupUi.prevForeground;
    const bool hadFocus = GetForegroundWindow() == hwnd;
    g_lookupUi = LookupUiState{};  // open = false first, so the focus change below can't re-enter

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    if (hadFocus && previous && IsWindow(previous) && IsWindowVisible(previous)) {
        SetForegroundWindow(previous);
    }
}

void CloseLookupPanel(HWND hwnd) {
    {
        std::lock_guard lock(g_stateMutex);
        g_state.lookup.active = false;
        ++g_lookupRequestSeq;  // a reply still in flight is now ignored
    }
    g_layoutDirty = true;
    EndLookupSession(hwnd);
}

// Runs a lookup and shows the outcome in the panel.
void SubmitLookup(const std::wstring& rawText) {
    const std::wstring query = QuickLookup::NormalizeQuery(rawText);
    if (query.empty() || query.size() > QuickLookup::kMaxQueryChars) {
        return;
    }

    const double now = NowSeconds();
    LookupUiState& ui = g_lookupUi;
    ui.text = query;
    ui.caret = query.size();
    ui.selectAll = false;
    ui.selected = 0;
    ui.caretResetAt = now;

    LookupSnapshot snap;
    snap.active = true;
    snap.query = query;

    const std::wstring key = ToLowerCopy(query);
    bool cachedFound = false;
    QuickLookup::Result cached;
    if (QuickLookup::CacheLookup(key, &cachedFound, &cached)) {
        QuickLookup::FillSnapshot(snap, query, cachedFound, &cached, now);
        if (cachedFound) {
            std::lock_guard lock(g_stateMutex);
            QuickLookup::PushRecentLocked(g_state, query, cached);  // bump it to the top
        }
        PublishNewLookup(snap, false);
        return;
    }

    snap.status = LookupStatus::Loading;
    snap.expiresAt = now + QuickLookup::kLoadingTimeoutSeconds;
    const uint64_t id = PublishNewLookup(snap, false);

    // The query text is deliberately not logged.
    Wh_Log(L"Quick Lookup: request started.");
    std::thread([id, query, key]() {
        try {
            QuickLookup::Result result;
            const QuickLookup::Outcome outcome = QuickLookup::LookupText(query, &result);
            if (outcome != QuickLookup::Outcome::Failed) {
                QuickLookup::CacheStore(key, outcome == QuickLookup::Outcome::Found, result);
            }
            QuickLookup::ApplyResult(id, query, outcome == QuickLookup::Outcome::Found, result);
            Wh_Log(L"Quick Lookup: finished (outcome %d).", static_cast<int>(outcome));
        } catch (...) {
            Wh_Log(L"Quick Lookup: lookup threw an exception.");
        }
    }).detach();
}

void SubmitLookupSelection() {
    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = CurrentLookupRows(&status);
    if (status == LookupStatus::Search && !rows.empty()) {
        const int idx = ClampInt(g_lookupUi.selected, 0, static_cast<int>(rows.size()) - 1);
        SubmitLookup(rows[static_cast<size_t>(idx)].query);
    } else {
        SubmitLookup(g_lookupUi.text);
    }
}

// Call after any change to the text. Editing leaves the result view and goes back
// to the list, and drops any reply still in flight.
void LookupUiEdited() {
    LookupUiState& ui = g_lookupUi;
    const double now = NowSeconds();
    ui.selected = 0;
    ui.selectAll = false;
    ui.caretResetAt = now;

    bool leaveResult = false;
    {
        std::lock_guard lock(g_stateMutex);
        leaveResult = g_state.lookup.active && g_state.lookup.status != LookupStatus::Search;
    }
    if (leaveResult) {
        LookupSnapshot snap;
        snap.active = true;
        snap.status = LookupStatus::Search;
        snap.expiresAt = now + QuickLookup::kPanelIdleSeconds;
        PublishNewLookup(snap, false);
    } else {
        TouchLookupPanel();
    }
    g_layoutDirty = true;
}

void InsertIntoLookupField(const std::wstring& raw) {
    LookupUiState& ui = g_lookupUi;
    std::wstring s;
    s.reserve(raw.size());
    for (wchar_t c : raw) {
        s.push_back((c < 0x20 || c == 0x7F) ? L' ' : c);  // single-line field
    }
    if (s.empty()) return;

    if (ui.selectAll) {
        ui.text.clear();
        ui.caret = 0;
        ui.selectAll = false;
    }
    ui.caret = std::min(ui.caret, ui.text.size());

    const size_t maxChars = QuickLookup::kMaxQueryChars;
    const size_t room = maxChars > ui.text.size() ? maxChars - ui.text.size() : 0;
    if (room == 0) return;
    if (s.size() > room) {
        s.resize(room);
        if (!s.empty() && IS_HIGH_SURROGATE(s.back())) s.pop_back();
    }
    ui.text.insert(ui.caret, s);
    ui.caret += s.size();
    LookupUiEdited();
}

size_t LookupStepBack(const std::wstring& t, size_t i) {
    if (i == 0) return 0;
    --i;
    if (i > 0 && IS_LOW_SURROGATE(t[i]) && IS_HIGH_SURROGATE(t[i - 1])) --i;
    return i;
}

size_t LookupStepForward(const std::wstring& t, size_t i) {
    if (i >= t.size()) return t.size();
    return (IS_HIGH_SURROGATE(t[i]) && i + 1 < t.size()) ? i + 2 : i + 1;
}

size_t LookupPrevWord(const std::wstring& t, size_t i) {
    while (i > 0 && iswspace(t[i - 1])) --i;
    while (i > 0 && !iswspace(t[i - 1])) --i;
    return i;
}

size_t LookupNextWord(const std::wstring& t, size_t i) {
    while (i < t.size() && !iswspace(t[i])) ++i;
    while (i < t.size() && iswspace(t[i])) ++i;
    return i;
}

// Called on the render thread when the hotkey fires.
void StartQuickLookup(HWND hwnd) {
    bool enabled = false;
    {
        std::lock_guard lock(g_settingsMutex);
        enabled = g_settings.quickLookup;
    }
    // A manually hidden island has nothing to show the panel on.
    if (!enabled || g_manuallyHidden.load()) {
        return;
    }

    // Hotkey again = close.
    if (g_lookupUi.open) {
        CloseLookupPanel(hwnd);
        return;
    }

    // Clipboard text is only offered as a suggestion row; nothing is sent anywhere yet.
    std::wstring clip = QuickLookup::NormalizeQuery(ReadClipboardText(hwnd));
    if (clip.size() > QuickLookup::kMaxQueryChars) {
        clip.clear();
    }

    const double now = NowSeconds();
    g_lookupUi = LookupUiState{};
    g_lookupUi.open = true;
    g_lookupUi.clipboardQuery = clip;
    g_lookupUi.caretResetAt = now;

    LookupSnapshot snap;
    snap.active = true;
    snap.status = LookupStatus::Search;
    snap.expiresAt = now + QuickLookup::kPanelIdleSeconds;
    PublishNewLookup(snap);

    BeginLookupSession(hwnd);
}

// WM_KEYDOWN. Printable characters arrive separately through WM_CHAR.
bool HandleLookupKeyDown(HWND hwnd, WPARAM vk) {
    LookupUiState& ui = g_lookupUi;
    if (!ui.open) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool moved = false;

    switch (vk) {
        case VK_ESCAPE:
            CloseLookupPanel(hwnd);
            return true;
        case VK_RETURN:
            SubmitLookupSelection();
            return true;
        case VK_UP:
        case VK_DOWN: {
            LookupStatus status = LookupStatus::Search;
            const std::vector<LookupRow> rows = CurrentLookupRows(&status);
            if (status == LookupStatus::Search && !rows.empty()) {
                const int n = static_cast<int>(rows.size());
                ui.selected = (ClampInt(ui.selected, 0, n - 1) + (vk == VK_DOWN ? 1 : n - 1)) % n;
            }
            moved = true;
            break;
        }
        case VK_LEFT:
            ui.caret = ui.selectAll ? 0 : (ctrl ? LookupPrevWord(ui.text, ui.caret)
                                                : LookupStepBack(ui.text, ui.caret));
            ui.selectAll = false;
            moved = true;
            break;
        case VK_RIGHT:
            ui.caret = ui.selectAll ? ui.text.size() : (ctrl ? LookupNextWord(ui.text, ui.caret)
                                                             : LookupStepForward(ui.text, ui.caret));
            ui.selectAll = false;
            moved = true;
            break;
        case VK_HOME:
            ui.caret = 0;
            ui.selectAll = false;
            moved = true;
            break;
        case VK_END:
            ui.caret = ui.text.size();
            ui.selectAll = false;
            moved = true;
            break;
        case VK_BACK:
            if (ui.selectAll) {
                ui.text.clear();
                ui.caret = 0;
            } else if (ui.caret > 0) {
                const size_t from = ctrl ? LookupPrevWord(ui.text, ui.caret)
                                         : LookupStepBack(ui.text, ui.caret);
                ui.text.erase(from, ui.caret - from);
                ui.caret = from;
            } else {
                return true;
            }
            LookupUiEdited();
            return true;
        case VK_DELETE:
            if (ui.selectAll) {
                ui.text.clear();
                ui.caret = 0;
            } else if (ui.caret < ui.text.size()) {
                const size_t to = LookupStepForward(ui.text, ui.caret);
                ui.text.erase(ui.caret, to - ui.caret);
            } else {
                return true;
            }
            LookupUiEdited();
            return true;
        case 'A':
            if (!ctrl) return false;
            ui.selectAll = !ui.text.empty();
            moved = true;
            break;
        case 'V':
            if (!ctrl) return false;
            InsertIntoLookupField(QuickLookup::CollapseWhitespace(
                ReadClipboardText(hwnd), QuickLookup::kMaxQueryChars));
            return true;
        default:
            return false;
    }

    if (moved) {
        ui.caretResetAt = NowSeconds();
        TouchLookupPanel();
        g_layoutDirty = true;
    }
    return true;
}

bool HandleLookupChar(wchar_t ch) {
    if (!g_lookupUi.open) return false;
    if (ch < 0x20 || ch == 0x7F) return true;  // control keys are handled in WM_KEYDOWN
    InsertIntoLookupField(std::wstring(1, ch));
    return true;
}

// Returns true when the click belonged to the panel (so it must not fall through
// to "open the media app" and friends).
bool HandleLookupClick(int x, int y) {
    LookupUiState& ui = g_lookupUi;
    if (!ui.open) return false;

    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = CurrentLookupRows(&status);
    const MediaContentPoint pt = MediaContentFromClient(x, y);
    TouchLookupPanel();
    if (!pt.valid) return true;

    if (status == LookupStatus::Search) {
        const bool header = LookupHasHeader(ui, rows);
        if (header && LookupHasRecentRows(rows) && LookupClearHitTest(pt)) {
            {
                std::lock_guard lock(g_stateMutex);
                g_state.lookupRecent.clear();
            }
            ui.selected = 0;
            g_layoutDirty = true;
            return true;
        }
        const int row = LookupRowAtContentPoint(pt, static_cast<int>(rows.size()), header);
        if (row >= 0) {
            SubmitLookup(rows[static_cast<size_t>(row)].query);
            return true;
        }
    }
    if (LookupFieldHitTest(pt)) {
        ui.caret = ui.text.size();
        ui.selectAll = false;
        ui.caretResetAt = NowSeconds();
        g_layoutDirty = true;
    }
    return true;
}

void HandleLookupMouseMove(int x, int y) {
    LookupUiState& ui = g_lookupUi;
    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = CurrentLookupRows(&status);
    TouchLookupPanel();
    if (status != LookupStatus::Search) return;

    const int row = LookupRowAtContentPoint(MediaContentFromClient(x, y),
                                            static_cast<int>(rows.size()),
                                            LookupHasHeader(ui, rows));
    if (row >= 0 && row != ui.selected) {
        ui.selected = row;
        g_layoutDirty = true;
    }
}

LPCWSTR LookupCursorAt(int x, int y) {
    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = CurrentLookupRows(&status);
    const MediaContentPoint pt = MediaContentFromClient(x, y);
    if (status == LookupStatus::Search) {
        const bool header = LookupHasHeader(g_lookupUi, rows);
        if ((header && LookupHasRecentRows(rows) && LookupClearHitTest(pt)) ||
            LookupRowAtContentPoint(pt, static_cast<int>(rows.size()), header) >= 0) {
            return IDC_HAND;
        }
    }
    return LookupFieldHitTest(pt) ? IDC_IBEAM : IDC_ARROW;
}

// Registers (or releases) the Quick Lookup hotkey. Render thread only, because
// RegisterHotKey must run on the thread that owns the window.
void ApplyLookupHotkey() {
    HWND hwnd = g_hwnd;
    if (!hwnd) {
        return;
    }
    if (g_lookupHotkeyRegistered) {
        UnregisterHotKey(hwnd, ID_LOOKUP_HOTKEY);
        g_lookupHotkeyRegistered = false;
    }

    const Settings settings = GetSettingsCopy();
    if (!settings.quickLookup) {
        return;
    }
    if (RegisterHotKey(hwnd, ID_LOOKUP_HOTKEY, settings.lookupModifiers, settings.lookupVk)) {
        g_lookupHotkeyRegistered = true;
    } else {
        Wh_Log(L"Failed to register Quick Lookup hotkey (error %lu).", GetLastError());
    }
}

