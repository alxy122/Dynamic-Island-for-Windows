void UpdateSystemSnapshot(bool includeGpuStats, bool includeNetStats) {
    SystemSnapshot next;
    {
        std::lock_guard lock(g_stateMutex);
        next = g_state.system;
        next.charging = g_state.system.charging;
    }

    // GPU/network sampling is comparatively expensive (the GPU counter in
    // particular enumerates every GPU engine instance across every process
    // using the GPU), so only sample them when something on screen is
    // actually displaying them. Otherwise just carry the last sampled
    // value forward (already done via `next = g_state.system` above).
    if (includeGpuStats) {
        next.gpuPercent = GetGpuUsage();
    }
    if (includeNetStats) {
        GetNetworkUsage(next.netUpMbps, next.netDownMbps);
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        next.memoryPercent = static_cast<int>(memory.dwMemoryLoad);
        next.memoryTotalGB = static_cast<float>(memory.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f);
        next.memoryUsedGB = next.memoryTotalGB - static_cast<float>(memory.ullAvailPhys) / (1024.0f * 1024.0f * 1024.0f);
    }

    ULARGE_INTEGER freeBytesAvailable = {};
    ULARGE_INTEGER totalBytes = {};
    ULARGE_INTEGER totalFreeBytes = {};
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvailable, &totalBytes, &totalFreeBytes) &&
        totalBytes.QuadPart > 0) {
        next.diskFreePercent = ClampInt(
            static_cast<int>(totalFreeBytes.QuadPart * 100 / totalBytes.QuadPart), 0, 100);
    }

    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_hwnd) {
        wchar_t title[96] = {};
        GetWindowTextW(foreground, title, ARRAYSIZE(title));
        if (!IsIgnorableForegroundWindow(foreground, title)) {
            next.foregroundTitle = title;
            if (next.foregroundTitle.size() > 42) {
                next.foregroundTitle.resize(42);
                next.foregroundTitle += L"...";
            }

        } else {
            next.foregroundTitle.clear();
        }
    }

    FILETIME idle = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const ULONGLONG idleNow = FileTimeToUInt64(idle);
        const ULONGLONG kernelNow = FileTimeToUInt64(kernel);
        const ULONGLONG userNow = FileTimeToUInt64(user);
        const ULONGLONG idlePrev = FileTimeToUInt64(g_prevIdleTime);
        const ULONGLONG kernelPrev = FileTimeToUInt64(g_prevKernelTime);
        const ULONGLONG userPrev = FileTimeToUInt64(g_prevUserTime);

        const ULONGLONG total = (kernelNow - kernelPrev) + (userNow - userPrev);
        const ULONGLONG idleDelta = idleNow - idlePrev;
        if (total > 0 && kernelPrev != 0) {
            next.cpuPercent = ClampInt(static_cast<int>((total - idleDelta) * 100 / total), 0, 100);
        }

        g_prevIdleTime = idle;
        g_prevKernelTime = kernel;
        g_prevUserTime = user;
    }

    std::lock_guard lock(g_stateMutex);
    // Volume is owned by ApplyVolumeReading and may have moved since `next` was copied.
    next.volumePercent = g_state.system.volumePercent;
    next.volumeMuted = g_state.system.volumeMuted;
    g_state.system = next;
    g_state.muted = next.volumeMuted;
}

// Built-in panel brightness (0-100), or -1 when there is no such panel (desktops)
// or the query fails. Same IOCTL the Windows brightness slider uses, so no WMI.
int ReadPanelBrightness(bool onAc) {
    constexpr DWORD kIoctlQueryDisplayBrightness = 0x230498;  // CTL_CODE(FILE_DEVICE_VIDEO, 0x126, METHOD_BUFFERED, FILE_ANY_ACCESS)
    struct DisplayBrightness {  // DISPLAY_BRIGHTNESS from ntddvdeo.h
        UCHAR policy;           // bit 0 = AC level valid, bit 1 = DC level valid
        UCHAR acBrightness;
        UCHAR dcBrightness;
    };

    static double s_retryAt = 0.0;  // desktops have no LCD device, so do not retry every poll
    const double now = NowSeconds();
    if (now < s_retryAt) {
        return -1;
    }

    HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (lcd == INVALID_HANDLE_VALUE) {
        lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    }
    if (lcd == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Brightness: cannot open the LCD device (error %lu), retrying in 30s", GetLastError());
        s_retryAt = now + 30.0;
        return -1;
    }

    DisplayBrightness db = {};
    DWORD returned = 0;
    const bool ok = DeviceIoControl(lcd, kIoctlQueryDisplayBrightness, nullptr, 0, &db, sizeof(db),
                                    &returned, nullptr) && returned >= sizeof(db);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(lcd);
    if (!ok) {
        Wh_Log(L"Brightness: query failed (error %lu, %lu bytes), retrying in 30s", error, returned);
        s_retryAt = now + 30.0;
        return -1;
    }

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        s_loggedFirst = true;
        Wh_Log(L"Brightness: policy=%d ac=%d dc=%d", db.policy, db.acBrightness, db.dcBrightness);
    }
    // Pick the level for the current power source, falling back to the other one
    // when the driver only reports one of them.
    const bool hasAc = (db.policy & 1) != 0;
    const bool hasDc = (db.policy & 2) != 0;
    int level = onAc ? db.acBrightness : db.dcBrightness;
    if (onAc && !hasAc && hasDc) {
        level = db.dcBrightness;
    } else if (!onAc && !hasDc && hasAc) {
        level = db.acBrightness;
    }
    return ClampInt(level, 0, 100);
}

void UpdateBrightnessSnapshot() {
    SYSTEM_POWER_STATUS power = {};
    GetSystemPowerStatus(&power);
    const int level = ReadPanelBrightness(power.ACLineStatus != 0);
    if (level < 0) {
        return;
    }

    std::lock_guard lock(g_stateMutex);
    // Plugging in or unplugging swaps between the AC and battery levels, which is
    // not the user moving the slider, so that sample only resets the baseline.
    const bool changed = g_brightnessInitialized && level != g_lastBrightness &&
                         power.ACLineStatus == g_brightnessPowerSource;
    if (changed && g_settings.brightness) {
        g_state.brightness.active = true;
        g_state.brightness.percent = level;
        g_state.brightness.expiresAt = NowSeconds() + 1.8;
        TriggerNudge();
    }
    g_lastBrightness = level;
    g_brightnessPowerSource = power.ACLineStatus;
    g_brightnessInitialized = true;
}

