// ── Quick Lookup panel: rows + hit testing ───────────────────────────────────
// Shared by the renderer (what to draw) and the window procedure (what a click or
// Enter means) so the two can never disagree about which row is which.
enum class LookupRowKind { Typed, Clipboard, Recent };

struct LookupRow {
    LookupRowKind kind = LookupRowKind::Recent;
    std::wstring query;   // what Enter / a click looks up
    std::wstring label;
    std::wstring detail;
};

std::wstring LookupTypedQuery(const LookupUiState& ui) {
    const size_t a = ui.text.find_first_not_of(L" \t");
    if (a == std::wstring::npos) {
        return std::wstring();
    }
    const size_t b = ui.text.find_last_not_of(L" \t");
    return ui.text.substr(a, b - a + 1);
}

std::vector<LookupRow> BuildLookupRows(const std::vector<LookupRecent>& recents,
                                       const LookupUiState& ui) {
    std::vector<LookupRow> rows;
    const size_t cap = static_cast<size_t>(LookupLayout::kMaxRows);
    const std::wstring typed = LookupTypedQuery(ui);

    if (typed.empty()) {
        const std::wstring clipKey = ToLowerCopy(ui.clipboardQuery);
        if (!ui.clipboardQuery.empty()) {
            rows.push_back({LookupRowKind::Clipboard, ui.clipboardQuery, ui.clipboardQuery,
                            Loc(L"Clipboard")});
        }
        for (const LookupRecent& r : recents) {
            if (rows.size() >= cap) break;
            if (!clipKey.empty() && ToLowerCopy(r.query) == clipKey) continue;
            rows.push_back({LookupRowKind::Recent, r.query,
                            r.title.empty() ? r.query : r.title, r.detail});
        }
        return rows;
    }

    rows.push_back({LookupRowKind::Typed, typed, typed, std::wstring()});
    const std::wstring needle = ToLowerCopy(typed);
    for (const LookupRecent& r : recents) {
        if (rows.size() >= cap) break;
        const std::wstring q = ToLowerCopy(r.query);
        if (q == needle) continue;  // the Typed row already covers it
        if (q.find(needle) == std::wstring::npos &&
            ToLowerCopy(r.title).find(needle) == std::wstring::npos) {
            continue;
        }
        rows.push_back({LookupRowKind::Recent, r.query,
                        r.title.empty() ? r.query : r.title, r.detail});
    }
    return rows;
}

// The "Recent / Clear" header only exists while the box is empty.
inline bool LookupHasHeader(const LookupUiState& ui, const std::vector<LookupRow>& rows) {
    return !rows.empty() && LookupTypedQuery(ui).empty();
}

inline bool LookupHasRecentRows(const std::vector<LookupRow>& rows) {
    return std::any_of(rows.begin(), rows.end(),
                       [](const LookupRow& r) { return r.kind == LookupRowKind::Recent; });
}

inline bool LookupCaretVisible(double now, double resetAt) {
    return std::fmod(std::max(0.0, now - resetAt), LookupLayout::kCaretPeriod) <
           LookupLayout::kCaretOn;
}

inline bool LookupFieldHitTest(const MediaContentPoint& pt) {
    return pt.valid && pt.x >= LookupLayout::kPadX && pt.x <= pt.width - LookupLayout::kPadX &&
           pt.y >= LookupLayout::kFieldTop &&
           pt.y <= LookupLayout::kFieldTop + LookupLayout::kFieldH;
}

inline bool LookupClearHitTest(const MediaContentPoint& pt) {
    return pt.valid && pt.x >= pt.width - LookupLayout::kPadX - 64.0f &&
           pt.x <= pt.width - LookupLayout::kPadX &&
           pt.y >= LookupLayout::kBelowField - 3.0f &&
           pt.y <= LookupLayout::kBelowField + LookupLayout::kSectionH + 3.0f;
}

int LookupRowAtContentPoint(const MediaContentPoint& pt, int rowCount, bool header) {
    if (!pt.valid || rowCount <= 0) return -1;
    if (pt.x < LookupLayout::kPadX || pt.x > pt.width - LookupLayout::kPadX) return -1;
    const float y = pt.y - LookupLayout::RowsTop(header);
    if (y < 0.0f) return -1;
    const float stride = LookupLayout::kRowH + LookupLayout::kRowGap;
    const int index = static_cast<int>(y / stride);
    if (index >= rowCount) return -1;
    if (y - static_cast<float>(index) * stride > LookupLayout::kRowH) return -1;
    return index;
}

