DWORD WINAPI MediaThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    Manager manager{nullptr};
    bool loggedUnavailable = false;
    int64_t pendingSeekBaseline = -1;  // guards against bogus "went backwards" readings
    MediaSourceTracker sourceTracker;
    ULONGLONG pendingSeekBaselineTickCount = 0;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MediaSnapshot next;
        std::vector<MediaSourceInfo> newSources;
        bool gotSources = false;

        try {
            if (!manager) {
                manager = Manager::RequestAsync().get();
            }

            if (manager) {
                gotSources = CollectMediaSources(manager, sourceTracker, &newSources);
                const std::wstring chosenAumid = gotSources
                    ? ChooseMediaSourceAumid(sourceTracker, GetPreferredMediaSource())
                    : GetPreferredMediaSource();
                auto session = SelectActiveMediaSession(manager, chosenAumid);
                if (session) {
                    auto properties = session.TryGetMediaPropertiesAsync().get();
                    auto playback = session.GetPlaybackInfo();
                    auto timeline = session.GetTimelineProperties();

                    next.available = true;
                    next.playing = playback.PlaybackStatus() == PlaybackStatus::Playing;
                    next.title = properties.Title().c_str();
                    next.artist = properties.Artist().c_str();
                    next.albumTitle = properties.AlbumTitle().c_str();

                    if (timeline) {
                        const int64_t rawPos = timeline.Position().count();
                        int64_t np = rawPos;
                        int64_t ne = timeline.EndTime().count();
                        bool npP = (playback.PlaybackStatus() == PlaybackStatus::Playing);
                        if (npP) {
                            // Position() is the position at LastUpdatedTime, not "now".
                            try {
                                const auto lastUpdated = timeline.LastUpdatedTime();
                                const int64_t ageTicks = (winrt::clock::now() - lastUpdated).count();
                                if (ageTicks > 0 && ageTicks < 10 * 10000000LL) {
                                    np = rawPos + ageTicks;
                                }
                            } catch (...) {
                            }
                        }

                        std::lock_guard lock(g_stateMutex);
                        // Some sources (notably the Spotify desktop app) refresh LastUpdatedTime
                        // without moving Position(), which makes the estimate jump backwards.
                        // Hold off until a second poll confirms it is a real seek.
                        constexpr int64_t kJitterToleranceTicks = 15000000LL;   // 1.5s
                        constexpr int64_t kConfirmToleranceTicks = 5000000LL;   // 0.5s
                        const bool sameTrackPlaying =
                            npP && g_state.media.playing && ne == g_state.media.endTicks;

                        if (sameTrackPlaying) {
                            const int64_t predictedNow = g_state.media.positionTicks +
                                (static_cast<int64_t>(GetTickCount64()) - g_state.media.lastUpdatedTicks) * 10000;

                            if (np < predictedNow - kJitterToleranceTicks) {
                                const ULONGLONG nowTicks64 = GetTickCount64();
                                bool confirmed = false;
                                if (pendingSeekBaseline >= 0) {
                                    const int64_t expectedIfRealSeek = pendingSeekBaseline +
                                        (static_cast<int64_t>(nowTicks64) -
                                         static_cast<int64_t>(pendingSeekBaselineTickCount)) * 10000;
                                    const int64_t diff = np - expectedIfRealSeek;
                                    if ((diff < 0 ? -diff : diff) < kConfirmToleranceTicks) {
                                        confirmed = true;
                                    }
                                }

                                if (confirmed) {
                                    pendingSeekBaseline = -1;
                                } else {
                                    pendingSeekBaseline = np;
                                    pendingSeekBaselineTickCount = nowTicks64;
                                    np = predictedNow;
                                }
                            } else {
                                pendingSeekBaseline = -1;
                            }
                        } else {
                            pendingSeekBaseline = -1;
                        }
                        if (np != g_state.media.positionTicks ||
                            ne != g_state.media.endTicks ||
                            npP != g_state.media.playing) {
                            next.positionTicks = np;
                            next.endTicks = ne;
                            next.lastUpdatedTicks = GetTickCount64();
                        } else {
                            next.positionTicks = g_state.media.positionTicks;
                            next.endTicks = g_state.media.endTicks;
                            next.lastUpdatedTicks = g_state.media.lastUpdatedTicks;
                        }
                    }

                    next.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
                    next.sourceName = MediaSourceDisplayName(next.sourceAppUserModelId);

                    // Fallback for VLC, which often exposes a session with no
                    // Title metadata at all.
                    //
                    // The marker must still be at the END of the caption, just
                    // without insisting on the exact " - " separator that skins
                    // and fullscreen do not reliably produce. Matching the
                    // marker anywhere would happily pick up a browser tab named
                    // "How to use VLC media player - YouTube". The owning
                    // process is checked too, so only a real VLC window counts.
                    if (next.title.empty() && next.sourceName == L"VLC") {
                        const std::wstring marker = L"VLC media player";
                        HWND hwnd = nullptr;
                        while ((hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr)) != nullptr) {
                            if (!IsWindowVisible(hwnd)) {
                                continue;
                            }

                            wchar_t windowTitle[512];
                            if (GetWindowTextW(hwnd, windowTitle, ARRAYSIZE(windowTitle)) <= 0) {
                                continue;
                            }

                            std::wstring caption(windowTitle);
                            while (!caption.empty() && caption.back() == L' ') {
                                caption.pop_back();
                            }
                            if (caption.size() <= marker.size() ||
                                caption.compare(caption.size() - marker.size(),
                                                marker.size(), marker) != 0) {
                                continue;
                            }

                            std::wstring image;
                            if (!ProcessImageNameForWindow(hwnd, &image) ||
                                ToLowerCopy(BaseNameFromPath(image)).find(L"vlc") == std::wstring::npos) {
                                continue;
                            }

                            // Everything before the app name is the media title,
                            // minus any trailing " - " separator.
                            std::wstring candidate = caption.substr(0, caption.size() - marker.size());
                            while (!candidate.empty() &&
                                   (candidate.back() == L' ' || candidate.back() == L'-')) {
                                candidate.pop_back();
                            }
                            if (!candidate.empty()) {
                                next.title = candidate;
                                break;
                            }
                        }
                    }

                    std::wstring prevSourceAppUserModelId;
                    bool hasPrevIcon = false;
                    PixelsRef prevIcon;
                    uint64_t prevIconGeneration = 0;
                    std::wstring prevBadge;

                    std::wstring prevTitle;
                    std::wstring prevArtist;
                    PixelsRef prevArt;
                    uint64_t prevArtGeneration = 0;
                    double prevArtChangedAt = 0.0;
                    double prevTitleChangedAt = 0.0;

                    bool prevPlaying = false;
                    {
                        std::lock_guard lock(g_stateMutex);
                        prevSourceAppUserModelId = g_state.media.sourceAppUserModelId;
                        hasPrevIcon = static_cast<bool>(g_state.media.sourceIcon);
                        prevIcon = g_state.media.sourceIcon;
                        prevIconGeneration = g_state.media.sourceIconGeneration;
                        prevBadge = g_state.media.sourceBadge;

                        prevTitle = g_state.media.title;
                        prevArtist = g_state.media.artist;
                        prevArt = g_state.media.art;
                        prevArtGeneration = g_state.media.artGeneration;
                        prevArtChangedAt = g_state.media.artChangedAt;
                        prevTitleChangedAt = g_state.media.titleChangedAt;
                        prevPlaying = g_state.media.playing;
                    }

                    if (next.sourceAppUserModelId == prevSourceAppUserModelId) {
                        next.sourceBadge = prevBadge;
                        next.sourceIcon = prevIcon;
                        next.sourceIconGeneration = prevIconGeneration;

                        if (!hasPrevIcon) {
                            next.sourceIcon = FindMediaSourceIcon(next.sourceAppUserModelId);
                            next.sourceIconGeneration = next.sourceIcon->generation;
                        }
                    } else {
                        next.sourceBadge = MediaSourceBadge(next.sourceName);
                        if (IsBrowserMediaSource(next.sourceAppUserModelId)) {
                            next.sourceBadge = FindBrowserMediaSiteBadge(next.sourceAppUserModelId);
                        }
                        next.sourceIcon = FindMediaSourceIcon(next.sourceAppUserModelId);
                        next.sourceIconGeneration = next.sourceIcon->generation;
                    }

                    // Track when title/artist last changed. Browsers often update
                    // the text metadata a beat before they swap the artwork bytes,
                    // so we keep retrying the art fetch for a short "settle window"
                    // after any track change instead of locking onto the first
                    // (possibly stale/empty) result forever.
                    const bool sourceChanged = (next.sourceAppUserModelId != prevSourceAppUserModelId);
                    const bool metadataChanged = (sourceChanged || next.title != prevTitle || next.artist != prevArtist || (!prevPlaying && next.playing));
                    next.titleChangedAt = (metadataChanged && !next.title.empty() && next.playing) ? NowSeconds() : prevTitleChangedAt;
                    constexpr double kArtSettleSeconds = 3.0;
                    const bool artSettled = !metadataChanged &&
                        (NowSeconds() - prevTitleChangedAt) > kArtSettleSeconds;

                    if (artSettled && prevArt) {
                        next.art = prevArt;
                        next.artGeneration = prevArtGeneration;
                        next.artChangedAt = prevArtChangedAt;
                    } else if (auto thumbnail = properties.Thumbnail()) {
                        std::vector<uint8_t> bytes = ReadWinRtStreamBytes(thumbnail);
                        if (!bytes.empty()) {
                            BitmapPixels decoded;
                            if (DecodeImageBytesToPixels(bytes, &decoded)) {
                                next.art = PixelsRef(std::move(decoded));
                                next.artGeneration = next.art->generation;
                                next.artChangedAt = NowSeconds();
                            }
                        }
                    }

                    // If this particular poll's fetch came back empty (thumbnail
                    // not ready yet), don't flash a blank cover for a track that
                    // hasn't actually changed — keep the old art until a fresh
                    // fetch succeeds. But never do this across an actual track
                    // change, or we're back to showing the wrong song's art.
                    if (!next.art && prevArt && !metadataChanged) {
                        next.art = prevArt;
                        next.artGeneration = prevArtGeneration;
                        next.artChangedAt = prevArtChangedAt;
                    }
                }
            }
        } catch (...) {
            if (!loggedUnavailable) {
                Wh_Log(L"WinRT media session unavailable; media module will fall back to idle.");
                loggedUnavailable = true;
            }
        }

        bool trackJustChanged = false;
        {
            std::lock_guard lock(g_stateMutex);

            // A switch the user asked for is not a "new track": it must not auto-expand
            // the island or open the 5s recentTrackChange window.
            const bool userSourceSwitch =
                !g_state.media.sourceAppUserModelId.empty() &&
                next.sourceAppUserModelId != g_state.media.sourceAppUserModelId &&
                NowSeconds() - g_userSourceSwitchAt.load() < 4.0;
            if (userSourceSwitch) {
                next.titleChangedAt = g_state.media.titleChangedAt;
            }

            const bool isDifferentTrack = !userSourceSwitch && (!next.title.empty() && next.playing) &&
                (next.title != g_state.media.title || next.artist != g_state.media.artist || (!g_state.media.playing && next.playing));

            if (isDifferentTrack) {
                next.titleChangedAt = NowSeconds();
                trackJustChanged = true;
                g_idleTab = 0;
            }

            if (g_state.media.art &&
                next.title == g_state.media.title && next.artist == g_state.media.artist &&
                next.positionTicks >= g_state.media.positionTicks) {
                next.art = g_state.media.art;
                next.artGeneration = g_state.media.artGeneration;
                next.artChangedAt = g_state.media.artChangedAt;
            }
            if (!next.sourceIcon &&
                next.sourceAppUserModelId == g_state.media.sourceAppUserModelId) {
                next.sourceIcon = g_state.media.sourceIcon;
                next.sourceIconGeneration = g_state.media.sourceIconGeneration;
            }

            g_state.media = std::move(next);
            if (gotSources) {
                g_state.mediaSources = std::move(newSources);
            }
        }

        if (trackJustChanged) {
            g_layoutDirty = true;
            if (g_settings.mediaAutoExpand) {
                TriggerNudge();
            }
        }

        HANDLE mediaWaits[2] = {g_stopEvent, g_mediaRefreshEvent};
        WaitForMultipleObjects(g_mediaRefreshEvent ? 2 : 1, mediaWaits, FALSE, 1500);
    }

    winrt::uninit_apartment();
    return 0;
}

