DWORD WINAPI WeatherThreadProc(void*) {
    // Initial delay to avoid slowing down startup
    WaitForSingleObject(g_stopEvent, 3000);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        std::wstring cityOverride;
        bool isFahrenheit = false;
        bool weatherEnabled = true;
        {
            // g_settingsMutex, not g_stateMutex: all three values below live in
            // g_settings, so the old g_stateMutex here guarded nothing and left
            // the weatherCity string copy racing against LoadSettings().
            std::lock_guard lock(g_settingsMutex);
            weatherEnabled = g_settings.weather;
            cityOverride = g_settings.weatherCity;
            isFahrenheit = g_settings.weatherFahrenheit;
        }

        if (!weatherEnabled) {
            WaitForSingleObject(g_stopEvent, 2000);
            continue;
        }

        std::wstring url = L"/?format=j1";
        if (!cityOverride.empty()) {
            const std::wstring encodedCity = PercentEncodeUtf8(cityOverride);
            if (!encodedCity.empty()) {
                url = L"/" + encodedCity + L"?format=j1";
            }
        }

        Wh_Log(L"Weather: Requesting weather from wttr.in/host: wttr.in, path: %s", url.c_str());
        std::string wRes = HttpGet(L"wttr.in", url.c_str(), true);

        bool weatherCommitted = false;

        if (!wRes.empty()) {
            Wh_Log(L"Weather: Received response from wttr.in (size: %zu bytes)", wRes.size());
            float temp = 0.0f;
            int code = 0;
            std::wstring desc = L"";
            std::wstring windSpeed = L"";
            std::wstring windDir = L"";
            std::wstring humidity = L"";
            std::wstring feelsLike = L"";
            std::wstring cityLabel = L"Local Weather";

            const char* areaStr = strstr(wRes.c_str(), "\"areaName\":");
            if (areaStr) {
                const char* valStr = strstr(areaStr, "\"value\":");
                if (valStr) {
                    valStr += 8;
                    while (*valStr == ' ' || *valStr == '\"') valStr++;
                    const char* end = strchr(valStr, '\"');
                    if (end) {
                        std::string cityA(valStr, end - valStr);
                        int wchars_num = MultiByteToWideChar(CP_UTF8, 0, cityA.c_str(), -1, NULL, 0);
                        if (wchars_num > 0) {
                            std::vector<wchar_t> wstr(wchars_num);
                            MultiByteToWideChar(CP_UTF8, 0, cityA.c_str(), -1, &wstr[0], wchars_num);
                            cityLabel = wstr.data();
                        }
                    }
                }
            }

            const char* currentStr = strstr(wRes.c_str(), "\"current_condition\":");
            if (currentStr) {
                auto ParseStringField = [&](const char* key, std::wstring& out) {
                    const char* kStr = strstr(currentStr, key);
                    if (kStr) {
                        kStr += strlen(key);
                        while (*kStr == ' ' || *kStr == '\"' || *kStr == ':') kStr++;
                        const char* end = strchr(kStr, '\"');
                        if (end) {
                            std::string valA(kStr, end - kStr);
                            int wchars_num = MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, NULL, 0);
                            if (wchars_num > 0) {
                                std::vector<wchar_t> wstr(wchars_num);
                                MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, &wstr[0], wchars_num);
                                out = wstr.data();
                            }
                        }
                    }
                };

                const char* tempStr = strstr(currentStr, isFahrenheit ? "\"temp_F\":" : "\"temp_C\":");
                if (tempStr) {
                    tempStr += 9;
                    while (*tempStr == ' ' || *tempStr == '\"') tempStr++;
                    sscanf(tempStr, "%f", &temp);
                }
                const char* codeStr = strstr(currentStr, "\"weatherCode\":");
                if (codeStr) {
                    codeStr += 14;
                    while (*codeStr == ' ' || *codeStr == '\"') codeStr++;
                    sscanf(codeStr, "%d", &code);
                }

                const char* descStr = strstr(currentStr, "\"weatherDesc\":");
                if (descStr) {
                    const char* valStr = strstr(descStr, "\"value\":");
                    if (valStr) {
                        valStr += 8;
                        while (*valStr == ' ' || *valStr == '\"') valStr++;
                        const char* end = strchr(valStr, '\"');
                        if (end) {
                            std::string valA(valStr, end - valStr);
                            int wchars_num = MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, NULL, 0);
                            if (wchars_num > 0) {
                                std::vector<wchar_t> wstr(wchars_num);
                                MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, &wstr[0], wchars_num);
                                desc = wstr.data();
                                while(!desc.empty() && desc.back() == L' ') desc.pop_back();
                            }
                        }
                    }
                }

                ParseStringField(isFahrenheit ? "\"windspeedMiles\"" : "\"windspeedKmph\"", windSpeed);
                ParseStringField("\"winddir16Point\"", windDir);
                ParseStringField("\"humidity\"", humidity);
                ParseStringField(isFahrenheit ? "\"FeelsLikeF\"" : "\"FeelsLikeC\"", feelsLike);

                // Treat the response as usable only if a real reading came out
                // of it. wttr.in answers rate limiting and unknown locations
                // with a page that contains no current_condition (or an empty
                // one); committing that regardless is what made the island
                // display a confident, wrong "0 degrees".
                // 0 is not a valid WWO weather code, so it doubles as a
                // "nothing was parsed" sentinel here.
                weatherCommitted = (code != 0) || !desc.empty();

                std::wstring finalCity = cityOverride.empty() ? cityLabel : cityOverride;
                if (weatherCommitted) {
                    Wh_Log(L"Weather parsed success: city=%s, temp=%.1f, feelsLike=%s, humidity=%s%%, desc=%s",
                           finalCity.c_str(), temp, feelsLike.c_str(), humidity.c_str(), desc.c_str());
                } else {
                    Wh_Log(L"Weather: response had no usable reading, keeping previous data.");
                }
            } else {
                Wh_Log(L"Weather: Failed to find \"current_condition\" in response.");
            }

            if (weatherCommitted) {
                std::lock_guard lock(g_stateMutex);
                g_state.weather.hasData = true;
                g_state.weather.temperature = temp;
                g_state.weather.weatherCode = code;
                if (!cityOverride.empty()) g_state.weather.city = cityOverride;
                else g_state.weather.city = cityLabel;
                g_state.weather.weatherDesc = desc;
                g_state.weather.windSpeed = windSpeed;
                g_state.weather.windDir = windDir;
                g_state.weather.humidity = humidity;
                g_state.weather.feelsLike = feelsLike;
                g_state.weather.lastUpdated = NowSeconds();
            }
        } else {
            Wh_Log(L"Weather: HttpGet returned empty response.");
        }

        // Retry soon after a failure instead of leaving the dashboard empty for
        // a full refresh interval, but back off on repeated failures. wttr.in is
        // a free community service and rate limiting is one of the failures being
        // recovered from here, so a flat fast retry would sustain the very
        // condition it is reacting to.
        static const DWORD kBackoffMs[] = {60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000};
        static size_t backoffIndex = 0;

        DWORD waitMs;
        if (weatherCommitted) {
            backoffIndex = 0;
            waitMs = 15 * 60 * 1000;
        } else {
            waitMs = kBackoffMs[backoffIndex];
            if (backoffIndex + 1 < ARRAYSIZE(kBackoffMs)) {
                ++backoffIndex;
            }
        }

        HANDLE events[] = {g_stopEvent, g_settingsChangedEvent};
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, waitMs);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            // The city changed, so retry immediately at full speed.
            backoffIndex = 0;
        }
    }
    return 0;
}

