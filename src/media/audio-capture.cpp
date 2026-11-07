DWORD WINAPI AudioThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        // Don't touch the audio engine at all unless a playing waveform is
        // actually going to be shown right now. Cheap poll, no capture open.
        if (!g_audioCaptureNeeded.load(std::memory_order_relaxed)) {
            WaitForSingleObject(g_stopEvent, 250);
            continue;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mixFormat = nullptr;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(hr)) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetMixFormat(&mixFormat);
        }
        if (SUCCEEDED(hr)) {
            REFERENCE_TIME bufferDuration = 10000000 / 5;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufferDuration, 0, mixFormat, nullptr);
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&capture));
        }
        if (SUCCEEDED(hr)) {
            hr = client->Start();
        }

        if (FAILED(hr)) {
            if (mixFormat) {
                CoTaskMemFree(mixFormat);
            }
            WaitForSingleObject(g_stopEvent, 2500);
            continue;
        }

        // Watchdog state — reset every time a capture session (re)starts so we
        // don't flag a stall before the first packet has even arrived.
        double lastPacketTime = NowSeconds();
        double nextWatchdogCheck = lastPacketTime + 1.0;

        while (WaitForSingleObject(g_stopEvent, 16) == WAIT_TIMEOUT) {
            if (!g_audioCaptureNeeded.load(std::memory_order_relaxed)) {
                break;
            }
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                break;
            }

            float amplitude = 0.0f;
            int packets = 0;
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    break;
                }

                lastPacketTime = NowSeconds();

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    amplitude = std::max(amplitude, SampleAudioAmplitude(data, frames, mixFormat));
                    PushAudioChunks(data, frames, mixFormat);
                } else {
                    PushWaveformSample(0.0f);
                    DecayAndPublishSpectrum();
                }
                capture->ReleaseBuffer(frames);
                ++packets;

                if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                    packetFrames = 0;
                }
            }

            if (packets > 0) {
                if (amplitude <= 0.001f) {
                    PushWaveformSample(0.0f);
                    DecayAndPublishSpectrum();
                }
            } else {
                PushWaveformSample(0.0f);
                DecayAndPublishSpectrum();
            }

            // --- Stall watchdog -------------------------------------------
            // Checked at most once a second (cheap: one time comparison plus
            // a mutex-protected bool read, same cost as work already done
            // elsewhere in this loop). Only forces a reconnect if SMTC says
            // media is playing but we've had zero packets for 5+ seconds —
            // that combination almost never happens unless the loopback
            // engine has stalled, so it won't fire during normal silence.
            const double now = NowSeconds();
            if (now >= nextWatchdogCheck) {
                nextWatchdogCheck = now + 1.0;
                bool mediaPlayingNow = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaPlayingNow = g_state.media.playing;
                }
                if (mediaPlayingNow && (now - lastPacketTime) > 5.0) {
                    Wh_Log(L"Audio loopback capture appears stalled while media is playing; reconnecting...");
                    break;
                }
            }
        }

        client->Stop();
        if (mixFormat) {
            CoTaskMemFree(mixFormat);
        }
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}
