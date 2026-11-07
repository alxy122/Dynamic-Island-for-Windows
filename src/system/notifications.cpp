#if DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER
DWORD WINAPI NotificationThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using winrt::Windows::UI::Notifications::KnownNotificationBindings;
    using winrt::Windows::UI::Notifications::NotificationKinds;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListener;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;

    std::set<uint32_t> seenIds;
    bool firstPoll = true;
    bool accessLogged = false;

    // The Windows Notification Service (WNS) and UWP subsystem take time to initialize on boot.
    // If this runs too early, instantiating UserNotificationListener::Current()
    // can permanently bind to an uninitialized COM proxy, permanently breaking notifications for the process.
    // To prevent this, we enforce a strict 30-second delay from process creation before touching the API.
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER ct;
        ct.LowPart = creationTime.dwLowDateTime;
        ct.HighPart = creationTime.dwHighDateTime;

        FILETIME systemTime;
        GetSystemTimeAsFileTime(&systemTime);
        ULARGE_INTEGER st;
        st.LowPart = systemTime.dwLowDateTime;
        st.HighPart = systemTime.dwHighDateTime;

        uint64_t msSinceProcessStart = (st.QuadPart - ct.QuadPart) / 10000;
        if (msSinceProcessStart < 30000) {
            DWORD waitTime = 30000 - (DWORD)msSinceProcessStart;
            Wh_Log(L"Process started recently. Delaying UserNotificationListener init by %d ms to let UWP subsystem load...", waitTime);
            WaitForSingleObject(g_stopEvent, waitTime);
        }
    }

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        try {
            auto listener = UserNotificationListener::Current();
            auto access = listener.RequestAccessAsync().get();
            if (access != UserNotificationListenerAccessStatus::Allowed) {
                if (!accessLogged) {
                    if (access == UserNotificationListenerAccessStatus::Denied) {
                        // Actionable: this is a Windows privacy switch, not a
                        // mod setting, and it is the usual reason the module
                        // stays silent when everything else is configured.
                        Wh_Log(L"Notification listener permission DENIED by Windows. "
                               L"Enable Settings > Privacy & security > Notifications > "
                               L"\"Let apps access your notifications\", then restart the mod. "
                               L"Retrying meanwhile...");
                    } else {
                        Wh_Log(L"Notification listener permission not granted or UWP subsystem not ready on boot (status: %d); retrying connection loop...", (int)access);
                    }
                    accessLogged = true;
                }
                WaitForSingleObject(g_stopEvent, 3000);
                continue;
            }

            if (accessLogged) {
                Wh_Log(L"WinRT UserNotificationListener successfully connected.");
                accessLogged = false;
            }

            while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                try {
                    auto notifications = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                    std::set<uint32_t> currentIds;

                    for (uint32_t i = 0; i < notifications.Size(); ++i) {
                        currentIds.insert(notifications.GetAt(i).Id());
                    }

                    if (firstPoll) {
                        seenIds = std::move(currentIds);
                        firstPoll = false;
                        WaitForSingleObject(g_stopEvent, 1000);
                        continue;
                    }

                    for (uint32_t i = 0; i < notifications.Size(); ++i) {
                        try {
                            auto userNotification = notifications.GetAt(i);
                            const uint32_t id = userNotification.Id();

                            if (seenIds.count(id)) {
                                continue;
                            }

                            // Immediately mark as seen so we don't process it again
                            seenIds.insert(id);

                            if (g_settings.notificationRespectDnD && g_isDnDActive.load()) {
                                continue;
                            }

                            NotificationSnapshot snapshot;
                            snapshot.active = true;
                            snapshot.expiresAt = NowSeconds() + 4.0;
                            auto appInfo = userNotification.AppInfo();
                            auto displayInfo = appInfo.DisplayInfo();
                            snapshot.app = displayInfo.DisplayName().c_str();

                            // Filter out noisy background console/script engines like PowerShell that fire system toasts unexpectedly
                            const std::wstring lowerApp = ToLowerCopy(snapshot.app);
                            if (lowerApp.find(L"powershell") != std::wstring::npos ||
                                lowerApp.find(L"pwsh") != std::wstring::npos ||
                                lowerApp.find(L"cmd.exe") != std::wstring::npos) {
                                continue;
                            }
                            try {
                                auto logo = displayInfo.GetLogo({32.0f, 32.0f});
                                std::vector<uint8_t> logoBytes = ReadWinRtStreamBytes(logo);
                                if (!logoBytes.empty()) {
                                    BitmapPixels decoded;
                                    if (DecodeImageBytesToPixels(logoBytes, &decoded)) {
                                        snapshot.icon = PixelsRef(std::move(decoded));
                                    }
                                }
                            } catch (...) {
                            }

                            if (!snapshot.icon && !snapshot.app.empty()) {
                                snapshot.icon = FindAppIconByName(snapshot.app, 64);
                            }

                            auto notification = userNotification.Notification();
                            auto binding = notification.Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                            if (binding) {
                                auto textElements = binding.GetTextElements();
                                if (textElements.Size() > 0) {
                                    snapshot.title = textElements.GetAt(0).Text().c_str();
                                }
                                if (textElements.Size() > 1) {
                                    snapshot.body = textElements.GetAt(1).Text().c_str();
                                }
                            }

                            if (snapshot.title.empty()) {
                                snapshot.title = snapshot.app.empty() ? L"New notification" : snapshot.app;
                            }
                            if (!snapshot.body.empty()) {
                                snapshot.title += L" - " + snapshot.body;
                            }
                            if (snapshot.title.size() > 120) {
                                snapshot.title.resize(120);
                                snapshot.title += L"...";
                            }

                            {
                                std::lock_guard lock(g_stateMutex);
                                g_state.notification = std::move(snapshot);
                            }
                            TriggerNudge();
                        } catch (const winrt::hresult_error& nex) {
                            if (nex.to_abi() == static_cast<int32_t>(0x80004001) || nex.to_abi() == static_cast<int32_t>(0x80040154)) { // E_NOTIMPL or REGDB_E_CLASSNOTREG
                                // UWP subsystem not ready, skip without spamming logs
                            } else {
                                Wh_Log(L"Failed to parse a notification (0x%08X); skipping.", nex.to_abi());
                            }
                        } catch (...) {
                            Wh_Log(L"Failed to parse a notification; skipping.");
                        }
                    }

                    seenIds = std::move(currentIds);
                } catch (const winrt::hresult_error& ex) {
                    const HRESULT hr = ex.to_abi();
                    if (hr == static_cast<HRESULT>(0x80004001) || hr == static_cast<HRESULT>(0x80040154)) { // E_NOTIMPL or REGDB_E_CLASSNOTREG
                        if (!accessLogged) {
                            Wh_Log(L"Notification listener UWP subsystem not fully ready (0x%08X). Retrying in background...", hr);
                            accessLogged = true;
                        }
                    } else {
                        Wh_Log(L"NotificationThreadProc inner loop WinRT error: %s (0x%08X); reconnecting...", ex.message().c_str(), hr);
                    }
                    WaitForSingleObject(g_stopEvent, 3000);
                    break;
                } catch (...) {
                    Wh_Log(L"NotificationThreadProc inner loop unknown exception; reconnecting...");
                    WaitForSingleObject(g_stopEvent, 3000);
                    break;
                }

                WaitForSingleObject(g_stopEvent, 1000);
            }
        } catch (const winrt::hresult_error& ex) {
            if (!accessLogged) {
                Wh_Log(L"NotificationThreadProc connection error: %s (0x%08X). Retrying in 3s...", ex.message().c_str(), ex.to_abi());
                accessLogged = true;
            }
            WaitForSingleObject(g_stopEvent, 3000);
        } catch (...) {
            if (!accessLogged) {
                Wh_Log(L"NotificationThreadProc unknown connection exception. Retrying in 3s...");
                accessLogged = true;
            }
            WaitForSingleObject(g_stopEvent, 3000);
        }
    }

    winrt::uninit_apartment();
    return 0;
}
#endif

