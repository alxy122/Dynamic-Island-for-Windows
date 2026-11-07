// ---- Endpoint volume notifications ----
// Event-driven rather than polled: a 1s poll turned a held volume key into
// one visible step per second.
void ApplyVolumeReading(float level, bool muted, bool canSurface) {
    const int percent = ClampInt(static_cast<int>(level * 100.0f + 0.5f), 0, 100);

    bool flyoutEnabled = false;
    {
        std::lock_guard settingsLock(g_settingsMutex);
        flyoutEnabled = g_settings.volume;
    }

    bool surfaced = false;
    {
        std::lock_guard lock(g_stateMutex);
        const bool changed = percent != g_state.system.volumePercent ||
                             muted != g_state.system.volumeMuted;
        g_state.system.volumePercent = percent;
        g_state.system.volumeMuted = muted;
        g_state.muted = muted;

        if (canSurface && changed && flyoutEnabled) {
            surfaced = !g_state.volume.active;
            g_state.volume.active = true;
            g_state.volume.percent = percent;
            g_state.volume.muted = muted;
            g_state.volume.deviceName = L"System audio";
            g_state.volume.expiresAt = NowSeconds() + 1.8;
        }
    }

    // Only the first step nudges; nudging on every step made the pill bounce.
    if (surfaced) {
        TriggerNudge();
    }
}

// g_volumeEndpointStale is declared with the other globals near the top of the file.

// Called on audio service threads, not the render thread.
class VolumeNotifyClient final : public IAudioEndpointVolumeCallback, public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
        } else if (riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (data) {
            ApplyVolumeReading(data->fMasterVolume, data->bMuted != FALSE, true);
        }
        return S_OK;
    }

    // MMDevice API calls are not allowed from inside these callbacks, so the
    // render thread rebinds; the posted message wakes it even while parked.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            g_volumeEndpointStale = true;
            if (HWND hwnd = g_hwnd) {
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
};

// Owned by the render thread.
ComPtr<IMMDeviceEnumerator> g_volumeEnumerator;
ComPtr<IAudioEndpointVolume> g_endpointVolume;
VolumeNotifyClient* g_volumeNotifyClient = nullptr;

// Event-driven: runs only when BindVolumeEndpoint() rebinds (startup, default output
// changed, Bluetooth connect/disconnect). Never from a timer or a draw path.
// "BTH*" enumerator names (BTHENUM, BTHHFENUM, BTHLEENUM) mean a Bluetooth endpoint.
void UpdateDefaultOutputBluetoothFlag(IMMDevice* device) {
    // PKEY_Device_EnumeratorName {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 24
    static const PROPERTYKEY kPkeyEnumeratorName = {
        {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 24};

    bool isBluetooth = false;
    bool readFailed = false;
    if (device) {
        readFailed = true;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(kPkeyEnumeratorName, &value))) {
                readFailed = false;
                isBluetooth = value.vt == VT_LPWSTR && value.pwszVal &&
                              _wcsnicmp(value.pwszVal, L"BTH", 3) == 0;
            }
            PropVariantClear(&value);
        }
    }

    if (readFailed) {
        static bool s_loggedReadFailure = false;
        if (!s_loggedReadFailure) {
            s_loggedReadFailure = true;
            Wh_Log(L"Lyrics sync: could not read the output's enumerator name; treating it as non-Bluetooth.");
        }
    }

    static bool s_logged = false;
    const bool changed = g_defaultOutputIsBluetooth.exchange(isBluetooth, std::memory_order_relaxed) != isBluetooth;
    if (changed || !s_logged) {
        s_logged = true;
        Wh_Log(L"Lyrics sync: default audio output is %s.", isBluetooth ? L"Bluetooth" : L"not Bluetooth");
    }
    if (changed) {
        g_layoutDirty = true;
    }
}

void BindVolumeEndpoint() {
    if (!g_volumeEndpointStale.exchange(false)) {
        return;
    }

    if (!g_volumeNotifyClient) {
        g_volumeNotifyClient = new VolumeNotifyClient();
    }
    if (!g_volumeEnumerator) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&g_volumeEnumerator)))) {
            g_volumeEnumerator.Reset();
            return;
        }
        g_volumeEnumerator->RegisterEndpointNotificationCallback(g_volumeNotifyClient);
    }

    if (g_endpointVolume) {
        g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        g_endpointVolume.Reset();
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = g_volumeEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    UpdateDefaultOutputBluetoothFlag(SUCCEEDED(hr) ? device.Get() : nullptr);
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(g_endpointVolume.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        hr = g_endpointVolume->RegisterControlChangeNotify(g_volumeNotifyClient);
    }

    float level = 0.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(hr) && SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&level)) &&
        SUCCEEDED(g_endpointVolume->GetMute(&muted))) {
        // Baseline only: switching device must not pop the flyout.
        ApplyVolumeReading(level, muted != FALSE, false);
    } else if (g_endpointVolume) {
        g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        g_endpointVolume.Reset();
    }
}

void UnbindVolumeEndpoint() {
    if (g_volumeNotifyClient) {
        if (g_endpointVolume) {
            g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        }
        if (g_volumeEnumerator) {
            g_volumeEnumerator->UnregisterEndpointNotificationCallback(g_volumeNotifyClient);
        }
    }
    g_endpointVolume.Reset();
    g_volumeEnumerator.Reset();
    if (g_volumeNotifyClient) {
        g_volumeNotifyClient->Release();
        g_volumeNotifyClient = nullptr;
    }
    g_volumeEndpointStale = true;
}

