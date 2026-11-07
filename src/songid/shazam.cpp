// ═════════════════════════════════════════════════════════════════════════════
// Song recognition (Shazam)
//
//   hotkey -> StartSongRecognition() -> worker thread:
//     1. capture N seconds (WASAPI loopback or microphone), resample to 16 kHz mono
//     2. build a Shazam signature (spectral peaks) locally
//     3. POST the signature to amp.shazam.com, parse the track, fetch the cover
//     4. publish the result to g_state.songId (the island draws it)
//
// Only the fingerprint (frequency peaks) leaves the PC, never raw audio.
// The endpoint is unofficial and may change or rate-limit without notice.
// ═════════════════════════════════════════════════════════════════════════════
std::atomic<uint64_t> g_songIdSeq{0};
std::atomic<bool> g_songIdRunning{false};
std::atomic<bool> g_songIdCancel{false};
std::atomic<double> g_songIdLastEnd{-100.0};
bool g_songIdHotkeyRegistered = false;

template <typename Fn>
void SongIdUpdate(uint64_t id, Fn&& fn, bool dirty = true) {
    {
        std::lock_guard lock(g_stateMutex);
        if (g_songIdSeq.load() != id) {
            return;  // superseded or dismissed
        }
        fn(g_state.songId);
    }
    if (dirty) {
        g_layoutDirty = true;
    }
}

namespace ShazamId {

constexpr wchar_t kUserAgent[] = L"Dalvik/2.1.0 (Linux; U; Android 9; SM-G960F Build/PPR1.180610.011)";
constexpr int kRate = 16000;
constexpr int kFftSize = 2048;
constexpr int kHop = 128;
constexpr int kBins = 1025;
constexpr int kHistory = 256;

inline void Put16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
inline void Put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

uint32_t Crc32(const uint8_t* data, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string Base64(const std::string& in) {
    static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    const size_t n = in.size();
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < n) v |= static_cast<uint8_t>(in[i + 1]) << 8;
        if (i + 2 < n) v |= static_cast<uint8_t>(in[i + 2]);
        out.push_back(kTbl[(v >> 18) & 63]);
        out.push_back(kTbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kTbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kTbl[v & 63] : '=');
    }
    return out;
}

void AppendHex(std::string& s, uint32_t v, int digits) {
    static const char* kHex = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; --i) {
        s.push_back(kHex[(v >> (i * 4)) & 0xF]);
    }
}

// Random (version 4) UUID, upper-case, hyphenated.
std::string NewUuid() {
    GUID g = {};
    if (FAILED(CoCreateGuid(&g))) {
        g.Data1 = static_cast<unsigned long>(GetTickCount64());
    }
    std::string s;
    AppendHex(s, g.Data1, 8);
    s.push_back('-');
    AppendHex(s, g.Data2, 4);
    s.push_back('-');
    AppendHex(s, g.Data3, 4);
    s.push_back('-');
    AppendHex(s, g.Data4[0], 2);
    AppendHex(s, g.Data4[1], 2);
    s.push_back('-');
    for (int i = 2; i < 8; ++i) {
        AppendHex(s, g.Data4[i], 2);
    }
    return s;
}

// ── Signature generator ──────────────────────────────────────────────────────
// 16 kHz mono int16 in. Every 128 samples: 2048-point FFT of a Hann window,
// peak spreading in frequency/time, then local-maximum picking 46 frames in the
// past. Peaks are grouped into four frequency bands and packed in Shazam's
// binary "sig" layout.
class SignatureGenerator {
   public:
    SignatureGenerator() {
        constexpr double kPi = 3.14159265358979323846;
        fftOut_.assign(static_cast<size_t>(kHistory) * kBins, 0.0f);
        spreadOut_.assign(static_cast<size_t>(kHistory) * kBins, 0.0f);
        window_.resize(kFftSize);
        twCos_.resize(kFftSize / 2);
        twSin_.resize(kFftSize / 2);
        bitRev_.resize(kFftSize);
        ring_.fill(0);
        for (int i = 0; i < kFftSize; ++i) {
            window_[i] = static_cast<float>(1024.0 * (0.5 - 0.5 * std::cos(2.0 * kPi * (i + 1) / 2049.0)));
        }
        for (int i = 0; i < kFftSize / 2; ++i) {
            const double a = -2.0 * kPi * i / kFftSize;
            twCos_[i] = static_cast<float>(std::cos(a));
            twSin_[i] = static_cast<float>(std::sin(a));
        }
        for (int i = 0; i < kFftSize; ++i) {
            int r = 0;
            for (int b = 0; b < 11; ++b) {
                if (i & (1 << b)) r |= 1 << (10 - b);
            }
            bitRev_[i] = r;
        }
    }

    void Feed(const int16_t* samples, size_t count) {
        size_t i = 0;
        for (; i + kHop <= count; i += kHop) {
            ProcessChunk(samples + i);
        }
        totalSamples_ += static_cast<uint32_t>(i);
    }

    size_t PeakCount() const {
        size_t n = 0;
        for (const auto& band : peaks_) n += band.size();
        return n;
    }
    uint32_t SampleCount() const { return totalSamples_; }

    std::string EncodeUri() const {
        std::string contents;
        for (int band = 0; band < 4; ++band) {
            const std::vector<Peak>& list = peaks_[band];
            if (list.empty()) continue;
            std::string pb;
            uint32_t pass = 0;
            for (const Peak& p : list) {
                if (p.pass - pass >= 255) {
                    pb.push_back(static_cast<char>(0xFF));
                    Put32(pb, p.pass);
                    pass = p.pass;
                }
                pb.push_back(static_cast<char>(p.pass - pass));
                Put16(pb, p.magnitude);
                Put16(pb, p.bin);
                pass = p.pass;
            }
            Put32(contents, 0x60030040u + static_cast<uint32_t>(band));
            Put32(contents, static_cast<uint32_t>(pb.size()));
            contents += pb;
            while (contents.size() % 4) contents.push_back('\0');
        }

        std::string out;
        Put32(out, 0xCAFE2580u);                                   // magic1
        Put32(out, 0);                                             // crc32 (patched below)
        Put32(out, static_cast<uint32_t>(contents.size() + 8));    // size minus header
        Put32(out, 0x94119C00u);                                   // magic2
        Put32(out, 0); Put32(out, 0); Put32(out, 0);               // void1
        Put32(out, 3u << 27);                                      // sample-rate id (16 kHz) << 27
        Put32(out, 0); Put32(out, 0);                              // void2
        Put32(out, totalSamples_ + static_cast<uint32_t>(kRate * 0.24));
        Put32(out, (15u << 19) + 0x40000u);                        // fixed value
        Put32(out, 0x40000000u);
        Put32(out, static_cast<uint32_t>(contents.size() + 8));
        out += contents;

        const uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(out.data()) + 8, out.size() - 8);
        for (int i = 0; i < 4; ++i) {
            out[4 + i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
        }
        return "data:audio/vnd.shazam.sig;base64," + Base64(out);
    }

   private:
    struct Peak {
        uint32_t pass;
        uint16_t magnitude;
        uint16_t bin;
    };

    static int Idx(uint32_t pos, int offset) {
        return ((static_cast<int>(pos % kHistory) + offset) % kHistory + kHistory) % kHistory;
    }

    void ProcessChunk(const int16_t* chunk) {
        for (int i = 0; i < kHop; ++i) {
            ring_[ringPos_] = chunk[i];
            ringPos_ = (ringPos_ + 1) % kFftSize;
        }

        float re[kFftSize];
        float im[kFftSize];
        for (int i = 0; i < kFftSize; ++i) {
            const int dst = bitRev_[i];
            re[dst] = static_cast<float>(ring_[(ringPos_ + i) % kFftSize]) * window_[i];
            im[dst] = 0.0f;
        }
        for (int len = 2; len <= kFftSize; len <<= 1) {
            const int half = len >> 1;
            const int step = kFftSize / len;
            for (int i = 0; i < kFftSize; i += len) {
                for (int k = 0; k < half; ++k) {
                    const float wr = twCos_[k * step];
                    const float wi = twSin_[k * step];
                    const int a = i + k;
                    const int b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }

        float* cur = &fftOut_[static_cast<size_t>(pos_) * kBins];
        for (int k = 0; k < kBins; ++k) {
            const float p = (re[k] * re[k] + im[k] * im[k]) / 131072.0f;
            cur[k] = p < 1e-10f ? 1e-10f : p;
        }

        // Peak spreading: neighbouring bins, then backwards in time.
        float spread[kBins];
        std::memcpy(spread, cur, sizeof(spread));
        static const int kFormer[3] = {-1, -3, -6};
        for (int pos = 0; pos < kBins; ++pos) {
            if (pos < 1023) {
                spread[pos] = std::max({spread[pos], spread[pos + 1], spread[pos + 2]});
            }
            float maxValue = spread[pos];
            for (int f : kFormer) {
                float* row = &spreadOut_[static_cast<size_t>(Idx(pos_, f)) * kBins];
                maxValue = std::max(row[pos], maxValue);
                row[pos] = maxValue;
            }
        }
        std::memcpy(&spreadOut_[static_cast<size_t>(pos_) * kBins], spread, sizeof(spread));

        pos_ = (pos_ + 1) % kHistory;
        ++numSpread_;
        if (numSpread_ >= 46) {
            Recognize();
        }
    }

    void Recognize() {
        static const int kNear[8] = {-10, -7, -4, -3, 1, 2, 5, 8};
        static const int kFar[14] = {-53, -45, 165, 172, 179, 186, 193, 200,
                                     214, 221, 228, 235, 242, 249};

        const float* f46 = &fftOut_[static_cast<size_t>(Idx(pos_, -46)) * kBins];
        const float* s49 = &spreadOut_[static_cast<size_t>(Idx(pos_, -49)) * kBins];
        auto mag = [](float x) { return std::log(std::max(x, 1.0f / 64.0f)) * 1477.3f + 6144.0f; };

        for (int bin = 10; bin < 1015; ++bin) {
            const float v = f46[bin];
            if (v < 1.0f / 64.0f || v < s49[bin - 1]) continue;

            float maxN = 0.0f;
            for (int off : kNear) maxN = std::max(maxN, s49[bin + off]);
            if (v <= maxN) continue;

            float maxO = maxN;
            for (int off : kFar) {
                const float* row = &spreadOut_[static_cast<size_t>(Idx(pos_, off)) * kBins];
                maxO = std::max(maxO, row[bin - 1]);
            }
            if (v <= maxO) continue;

            const float m0 = mag(v);
            const float mb = mag(f46[bin - 1]);
            const float ma = mag(f46[bin + 1]);
            const float var1 = m0 * 2.0f - mb - ma;
            if (var1 <= 0.0f) continue;
            const float var2 = (ma - mb) * 32.0f / var1;
            const int corrected = static_cast<int>(static_cast<float>(bin) * 64.0f + var2);
            if (corrected < 0 || corrected > 65535) continue;

            const float hz = static_cast<float>(corrected) * (16000.0f / 2.0f / 1024.0f / 64.0f);
            int band;
            if (hz < 250.0f) continue;
            else if (hz < 520.0f) band = 0;
            else if (hz < 1450.0f) band = 1;
            else if (hz < 3500.0f) band = 2;
            else if (hz <= 5500.0f) band = 3;
            else continue;

            Peak p;
            p.pass = numSpread_ - 46;
            p.magnitude = static_cast<uint16_t>(std::clamp(static_cast<int>(m0), 0, 65535));
            p.bin = static_cast<uint16_t>(corrected);
            peaks_[band].push_back(p);
        }
    }

    std::vector<float> fftOut_;
    std::vector<float> spreadOut_;
    std::vector<float> window_;
    std::vector<float> twCos_;
    std::vector<float> twSin_;
    std::vector<int> bitRev_;
    std::array<int16_t, kFftSize> ring_{};
    int ringPos_ = 0;
    uint32_t pos_ = 0;          // next write slot in both history rings
    uint32_t numSpread_ = 0;
    uint32_t totalSamples_ = 0;
    std::vector<Peak> peaks_[4];
};

// ── Audio capture ────────────────────────────────────────────────────────────
struct CaptureOutcome {
    std::vector<int16_t> samples;  // 16 kHz mono
    bool cancelled = false;
    bool failed = false;
    int peak = 0;
};

std::vector<int16_t> To16kMono(const std::vector<float>& in, double srcRate) {
    std::vector<int16_t> out;
    if (in.empty() || srcRate < 8000.0) return out;
    constexpr double kPi = 3.14159265358979323846;

    auto toS16 = [](double v) {
        v *= 32767.0;
        return static_cast<int16_t>(v > 32767.0 ? 32767 : (v < -32768.0 ? -32768 : static_cast<int>(v)));
    };

    if (std::fabs(srcRate - 16000.0) < 1.0) {
        out.reserve(in.size());
        for (float v : in) out.push_back(toS16(v));
        return out;
    }

    // Windowed-sinc low-pass + decimation, so 48 kHz / 44.1 kHz material does
    // not alias into the 250-5500 Hz bands the fingerprint looks at.
    const double ratio = srcRate / 16000.0;
    const double cutoff = std::min(1.0, 1.0 / ratio) * 0.92;
    const double halfWidth = 10.0 * std::max(1.0, ratio);
    const size_t outCount = static_cast<size_t>(static_cast<double>(in.size()) / ratio);
    out.resize(outCount);
    const long last = static_cast<long>(in.size()) - 1;
    for (size_t i = 0; i < outCount; ++i) {
        const double center = static_cast<double>(i) * ratio;
        const long lo = std::max(0L, static_cast<long>(std::ceil(center - halfWidth)));
        const long hi = std::min(last, static_cast<long>(std::floor(center + halfWidth)));
        double acc = 0.0, norm = 0.0;
        for (long n = lo; n <= hi; ++n) {
            const double t = static_cast<double>(n) - center;
            const double a = kPi * cutoff * t;
            const double sinc = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
            const double w = 0.5 + 0.5 * std::cos(kPi * t / halfWidth);
            const double k = cutoff * sinc * w;
            acc += static_cast<double>(in[static_cast<size_t>(n)]) * k;
            norm += k;
        }
        out[i] = toS16(norm != 0.0 ? acc / norm : 0.0);
    }
    return out;
}

CaptureOutcome CaptureAudio(bool useMic, int seconds, uint64_t id) {
    CaptureOutcome out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mix = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(useMic ? eCapture : eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        hr = client->GetMixFormat(&mix);
    }
    if (SUCCEEDED(hr)) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                useMic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK,
                                10000000, 0, mix, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = client->GetService(IID_PPV_ARGS(&capture));
    }
    if (SUCCEEDED(hr)) {
        hr = client->Start();
    }
    if (FAILED(hr) || !mix) {
        if (mix) CoTaskMemFree(mix);
        out.failed = true;
        return out;
    }

    const UINT32 channels = std::max<UINT32>(1, mix->nChannels);
    const UINT32 bits = mix->wBitsPerSample;
    const UINT32 blockAlign = mix->nBlockAlign;
    const double rate = static_cast<double>(mix->nSamplesPerSec);
    const bool isFloat =
        mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat, kSubTypeIeeeFloat));

    std::vector<float> mono;
    const size_t target = static_cast<size_t>(rate * seconds);
    mono.reserve(target + static_cast<size_t>(rate));

    const double t0 = NowSeconds();
    double nextPublish = 0.0;
    float level = 0.0f;

    while (mono.size() < target) {
        if (g_songIdCancel.load() || !g_running.load() ||
            WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
            out.cancelled = true;
            break;
        }
        const double now = NowSeconds();
        // Loopback delivers nothing while the output is silent, so wall-clock
        // time is the backstop.
        if (now - t0 > seconds + 3.0) break;

        double sumSq = 0.0;
        size_t count = 0;
        UINT32 packet = 0;
        if (SUCCEEDED(capture->GetNextPacketSize(&packet))) {
            while (packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bufFlags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &bufFlags, nullptr, nullptr))) break;
                if ((bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    mono.insert(mono.end(), frames, 0.0f);
                    count += frames;
                } else {
                    for (UINT32 f = 0; f < frames; ++f) {
                        const BYTE* p = data + static_cast<size_t>(f) * blockAlign;
                        float sum = 0.0f;
                        for (UINT32 c = 0; c < channels; ++c) {
                            float v = 0.0f;
                            if (isFloat && bits == 32) {
                                v = reinterpret_cast<const float*>(p)[c];
                            } else if (bits == 16) {
                                v = reinterpret_cast<const int16_t*>(p)[c] / 32768.0f;
                            } else if (bits == 24) {
                                const BYTE* q = p + c * 3;
                                int32_t s = q[0] | (q[1] << 8) | (q[2] << 16);
                                if (s & 0x800000) s |= static_cast<int32_t>(0xFF000000u);
                                v = static_cast<float>(s) / 8388608.0f;
                            } else if (bits == 32) {
                                v = static_cast<float>(reinterpret_cast<const int32_t*>(p)[c]) / 2147483648.0f;
                            }
                            sum += v;
                        }
                        const float m = sum / static_cast<float>(channels);
                        mono.push_back(m);
                        sumSq += static_cast<double>(m) * m;
                        ++count;
                    }
                }
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&packet))) packet = 0;
            }
        }

        if (count > 0) {
            const float rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(count)));
            level += (Clamp(rms * 5.0f, 0.0f, 1.0f) - level) * 0.5f;
        } else {
            level *= 0.85f;
        }
        if (now >= nextPublish) {
            nextPublish = now + 0.05;
            const float lv = level;
            SongIdUpdate(id, [lv](SongIdSnapshot& s) { s.level = lv; }, false);
        }
        WaitForSingleObject(g_stopEvent, 15);
    }

    client->Stop();
    CoTaskMemFree(mix);
    if (out.cancelled) return out;

    if (mono.size() > target) mono.resize(target);
    out.samples = To16kMono(mono, rate);
    int peak = 0;
    for (int16_t v : out.samples) {
        const int a = v < 0 ? -static_cast<int>(v) : static_cast<int>(v);
        peak = std::max(peak, a);
    }
    out.peak = peak;
    return out;
}

// ── HTTP + response parsing ──────────────────────────────────────────────────
HttpResult HttpPostJson(const wchar_t* host, const std::wstring& path, const std::string& body,
                        const wchar_t* userAgent, const wchar_t* headers) {
    HttpResult result;
    HINTERNET hSession = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    WinHttpSetTimeouts(hSession, 8000, 8000, 15000, 20000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1L),
                                   const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                if (WinHttpQueryHeaders(hRequest,
                                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    bool readOk = true;
                    for (;;) {
                        DWORD size = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &size)) { readOk = false; break; }
                        if (size == 0) break;
                        std::vector<char> buffer(size);
                        DWORD downloaded = 0;
                        if (!WinHttpReadData(hRequest, buffer.data(), size, &downloaded)) { readOk = false; break; }
                        result.body.append(buffer.data(), downloaded);
                        if (result.body.size() > 4u * 1024u * 1024u) { readOk = false; break; }
                    }
                    if (readOk) {
                        result.ok = true;
                        result.status = status;
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// Index where the value of a key that sits directly inside `obj` starts
// (depth 1), or npos. Nested keys with the same name are ignored.
size_t FindTopLevelValue(const std::string& obj, const char* key) {
    const size_t keyLen = std::strlen(key);
    int depth = 0;
    bool inString = false, escaped = false;
    size_t strStart = 0;
    auto isSpace = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
    for (size_t i = 0; i < obj.size(); ++i) {
        const char c = obj[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
                if (depth == 1 && i - strStart == keyLen && obj.compare(strStart, keyLen, key) == 0) {
                    size_t j = i + 1;
                    while (j < obj.size() && isSpace(obj[j])) ++j;
                    if (j < obj.size() && obj[j] == ':') {
                        ++j;
                        while (j < obj.size() && isSpace(obj[j])) ++j;
                        return j < obj.size() ? j : std::string::npos;
                    }
                }
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            strStart = i + 1;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return std::string::npos;
}

std::wstring TopLevelString(const std::string& obj, const char* key) {
    const size_t v = FindTopLevelValue(obj, key);
    if (v == std::string::npos || obj[v] != '"') return std::wstring();
    std::string raw;
    bool escaped = false;
    for (size_t i = v + 1; i < obj.size(); ++i) {
        const char c = obj[i];
        if (escaped) { raw.push_back(c); escaped = false; continue; }
        if (c == '\\') { raw.push_back(c); escaped = true; continue; }
        if (c == '"') break;
        raw.push_back(c);
    }
    return JsonStringUnescape(raw);
}

std::string TopLevelObject(const std::string& obj, const char* key) {
    const size_t v = FindTopLevelValue(obj, key);
    if (v == std::string::npos || obj[v] != '{') return std::string();
    return QuickLookup::BalancedObjectAt(obj, v);
}

struct TrackMatch {
    std::wstring title;
    std::wstring artist;
    std::wstring url;
    std::wstring coverUrl;
};

enum class ParseOutcome { Match, NoMatch, Invalid };

ParseOutcome ParseShazamResponse(const std::string& body, TrackMatch* out) {
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || body[first] != '{') return ParseOutcome::Invalid;

    const size_t mv = FindTopLevelValue(body, "matches");
    if (mv != std::string::npos && body[mv] == '[') {
        size_t j = mv + 1;
        while (j < body.size() && (body[j] == ' ' || body[j] == '\n' || body[j] == '\r' || body[j] == '\t')) ++j;
        if (j < body.size() && body[j] == ']') return ParseOutcome::NoMatch;
    }

    const std::string track = TopLevelObject(body, "track");
    if (track.empty()) return (mv == std::string::npos) ? ParseOutcome::NoMatch : ParseOutcome::Invalid;

    TrackMatch m;
    m.title = TopLevelString(track, "title");
    m.artist = TopLevelString(track, "subtitle");
    if (m.title.empty()) return ParseOutcome::Invalid;

    const std::string images = TopLevelObject(track, "images");
    m.coverUrl = TopLevelString(images, "coverarthq");
    if (m.coverUrl.empty()) m.coverUrl = TopLevelString(images, "coverart");

    const std::string share = TopLevelObject(track, "share");
    m.url = TopLevelString(share, "href");
    if (m.url.empty()) m.url = TopLevelString(track, "url");

    *out = std::move(m);
    return ParseOutcome::Match;
}

PixelsRef FetchCover(const std::wstring& url) {
    if (url.rfind(L"https://", 0) != 0) return PixelsRef();
    const size_t hostEnd = url.find(L'/', 8);
    if (hostEnd == std::wstring::npos) return PixelsRef();
    const std::wstring host = url.substr(8, hostEnd - 8);
    const std::wstring path = url.substr(hostEnd);
    if (host.empty()) return PixelsRef();

    const HttpResult r = HttpGetEx(host.c_str(), path.c_str(), true, kUserAgent);
    if (!r.ok || r.status != 200 || r.body.empty()) return PixelsRef();
    BitmapPixels px;
    const std::vector<uint8_t> bytes(r.body.begin(), r.body.end());
    if (!DecodeImageBytesToPixels(bytes, &px)) return PixelsRef();
    return PixelsRef(std::move(px));
}

std::string BuildRequestBody(const std::string& uri, uint32_t sampleCount) {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = {};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const unsigned long long ms = (u.QuadPart - 116444736000000000ULL) / 10000ULL;
    const std::string stamp = std::to_string(ms);

    std::string body =
        "{\"geolocation\":{\"altitude\":300.0,\"latitude\":45.0,\"longitude\":2.0},"
        "\"signature\":{\"samplems\":";
    body += std::to_string(static_cast<unsigned long long>(sampleCount) * 1000ull / kRate);
    body += ",\"timestamp\":" + stamp + ",\"uri\":\"" + uri + "\"},\"timestamp\":" + stamp +
            ",\"timezone\":\"Europe/Paris\"}";
    return body;
}

// ── Worker ───────────────────────────────────────────────────────────────────
void RunRecognition(uint64_t id, bool useMic, int seconds) {
    auto dismiss = [&]() {
        SongIdUpdate(id, [](SongIdSnapshot& s) { s.active = false; });
    };
    auto fail = [&](SongIdStatus st, const wchar_t* msg, double ttl) {
        const std::wstring text = msg;
        SongIdUpdate(id, [&](SongIdSnapshot& s) {
            s.status = st;
            s.message = text;
            s.level = 0.0f;
            s.expiresAt = NowSeconds() + ttl;
        });
    };

    CaptureOutcome cap = CaptureAudio(useMic, seconds, id);
    if (cap.cancelled) { dismiss(); return; }
    if (cap.failed) { fail(SongIdStatus::Error, L"Couldn't open the audio device", 6.0); return; }
    if (cap.samples.size() < static_cast<size_t>(kRate) * 2 || cap.peak < 20) {
        fail(SongIdStatus::NoAudio, L"No audio detected", 5.0);
        return;
    }

    SongIdUpdate(id, [](SongIdSnapshot& s) {
        s.status = SongIdStatus::Matching;
        s.level = 0.0f;
        s.expiresAt = NowSeconds() + 40.0;
    });

    SignatureGenerator gen;
    gen.Feed(cap.samples.data(), cap.samples.size());
    Wh_Log(L"Song recognition: %zu peaks from %zu samples.", gen.PeakCount(), cap.samples.size());
    if (gen.PeakCount() < 8) {
        fail(SongIdStatus::NoMatch, L"Not enough detail in the audio", 5.0);
        return;
    }

    const std::string body = BuildRequestBody(gen.EncodeUri(), gen.SampleCount());
    const std::wstring path =
        L"/discovery/v5/en/US/android/-/tag/" + AsciiToWide(NewUuid()) + L"/" + AsciiToWide(NewUuid()) +
        L"?sync=true&webv3=true&sampling=true&connected=&shazamapiversion=v3&sharehub=true"
        L"&hubv5minorversion=v5.1&hidelb=true&video=v3";

    const HttpResult res = HttpPostJson(
        L"amp.shazam.com", path, body, kUserAgent,
        L"Content-Type: application/json\r\nContent-Language: en_US\r\n");

    if (g_songIdCancel.load()) { dismiss(); return; }
    if (!res.ok) { fail(SongIdStatus::Error, L"Couldn't reach Shazam", 6.0); return; }
    if (res.status == 429) { fail(SongIdStatus::Error, L"Shazam is rate limiting - try again soon", 7.0); return; }
    if (res.status != 200) {
        Wh_Log(L"Song recognition: HTTP %lu from Shazam.", res.status);
        fail(SongIdStatus::Error, L"Shazam returned an error", 6.0);
        return;
    }

    TrackMatch match;
    switch (ParseShazamResponse(res.body, &match)) {
        case ParseOutcome::NoMatch:
            fail(SongIdStatus::NoMatch, L"No match found", 6.0);
            return;
        case ParseOutcome::Invalid:
            fail(SongIdStatus::Error, L"Unexpected response from Shazam", 6.0);
            return;
        case ParseOutcome::Match:
            break;
    }

    PixelsRef cover = FetchCover(match.coverUrl);
    if (g_songIdCancel.load()) { dismiss(); return; }

    SongIdUpdate(id, [&](SongIdSnapshot& s) {
        s.status = SongIdStatus::Found;
        s.title = match.title;
        s.artist = match.artist;
        s.url = match.url;
        s.cover = cover;
        s.level = 0.0f;
        s.expiresAt = NowSeconds() + 12.0;
    });
    TriggerNudge();
}

void Worker(uint64_t id, bool useMic, int seconds) {
    struct Guard {
        ~Guard() {
            g_songIdLastEnd.store(NowSeconds());
            g_songIdRunning.store(false);
        }
    } guard;

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        RunRecognition(id, useMic, seconds);
    } catch (...) {
        Wh_Log(L"Song recognition: unexpected exception.");
        SongIdUpdate(id, [](SongIdSnapshot& s) {
            s.status = SongIdStatus::Error;
            s.message = L"Song recognition failed";
            s.expiresAt = NowSeconds() + 5.0;
        });
    }
    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }
}

}  // namespace ShazamId

// Hotkey press (render thread). Pressing it again while listening cancels.
void StartSongRecognition(HWND hwnd) {
    const Settings s = GetSettingsCopy();
    if (!s.songRecognition || g_manuallyHidden.load()) {
        return;
    }
    if (g_songIdRunning.load()) {
        g_songIdCancel.store(true);
        return;
    }
    if (NowSeconds() - g_songIdLastEnd.load() < 1.0) {
        return;
    }
    if (g_lookupUi.open) {
        CloseLookupPanel(hwnd);
    }

    g_songIdRunning = true;
    g_songIdCancel = false;
    const uint64_t id = ++g_songIdSeq;
    const double now = NowSeconds();
    {
        std::lock_guard lock(g_stateMutex);
        SongIdSnapshot sid;
        sid.active = true;
        sid.status = SongIdStatus::Listening;
        sid.micSource = s.songIdUseMic;
        sid.listenSeconds = static_cast<float>(s.songIdSeconds);
        sid.startedAt = now;
        sid.expiresAt = now + s.songIdSeconds + 25.0;
        g_state.songId = std::move(sid);
    }
    g_layoutDirty = true;
    TriggerNudge();

    std::thread(ShazamId::Worker, id, s.songIdUseMic, s.songIdSeconds).detach();
}

// Click on the card: open the track page when found, otherwise just dismiss
// (and stop listening if it is still running).
void HandleSongIdClick() {
    std::wstring url;
    {
        std::lock_guard lock(g_stateMutex);
        if (g_state.songId.status == SongIdStatus::Found) {
            url = g_state.songId.url;
        }
        g_state.songId.active = false;
    }
    if (g_songIdRunning.load()) {
        g_songIdCancel = true;
    }
    if (url.rfind(L"https://", 0) == 0) {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    g_layoutDirty = true;
}

// Registers (or releases) the song recognition hotkey. Render thread only.
void ApplySongIdHotkey() {
    HWND hwnd = g_hwnd;
    if (!hwnd) {
        return;
    }
    if (g_songIdHotkeyRegistered) {
        UnregisterHotKey(hwnd, ID_SONGID_HOTKEY);
        g_songIdHotkeyRegistered = false;
    }
    const Settings settings = GetSettingsCopy();
    if (!settings.songRecognition) {
        return;
    }
    if (RegisterHotKey(hwnd, ID_SONGID_HOTKEY, settings.songIdModifiers, settings.songIdVk)) {
        g_songIdHotkeyRegistered = true;
    } else {
        Wh_Log(L"Failed to register song recognition hotkey (error %lu).", GetLastError());
    }
}

