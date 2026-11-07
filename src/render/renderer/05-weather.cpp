    // Fixes windhawk-mods#4352: wind direction was printed as compass
    // initialisms (WSW, NE), which is meteorologist shorthand rather than
    // something glanceable. These are flow arrows, not bearing arrows -- a wind
    // *from* the north-east is drawn as an arrow pointing south-west, matching
    // the convention weather apps use.
    std::wstring WindDirToArrow(const std::wstring& dir) {
        if (dir == L"N") return L"\x2193";
        if (dir == L"NNE" || dir == L"NE" || dir == L"ENE") return L"\x2199";
        if (dir == L"E") return L"\x2190";
        if (dir == L"ESE" || dir == L"SE" || dir == L"SSE") return L"\x2196";
        if (dir == L"S") return L"\x2191";
        if (dir == L"SSW" || dir == L"SW" || dir == L"WSW") return L"\x2197";
        if (dir == L"W") return L"\x2192";
        if (dir == L"WNW" || dir == L"NW" || dir == L"NNW") return L"\x2198";
        return dir;
    }

    // ── Vector weather icons ────────────────────────────────────────────────
    // All sized relative to `s` (the icon's box width) and centred on `c`, so a
    // single call site controls scale. Built from ellipses and rounded rects
    // rather than path geometry wherever possible, to keep per-frame allocation
    // down.

    void DrawCloudShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush) {
        const D2D1_RECT_F base = D2D1::RectF(c.x - 0.44f * s, c.y + 0.02f * s,
                                             c.x + 0.44f * s, c.y + 0.21f * s);
        target_->FillRoundedRectangle(D2D1::RoundedRect(base, 0.10f * s, 0.10f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.02f * s), 0.18f * s, 0.18f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.01f * s, c.y - 0.10f * s), 0.24f * s, 0.24f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + 0.25f * s, c.y + 0.03f * s), 0.17f * s, 0.17f * s), brush);
    }

    void DrawSunShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush, float coreRadius = 0.26f) {
        target_->FillEllipse(D2D1::Ellipse(c, coreRadius * s, coreRadius * s), brush);

        const float inner = (coreRadius + 0.09f) * s;
        const float outer = (coreRadius + 0.21f) * s;
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            target_->DrawLine(D2D1::Point2F(c.x + ca * inner, c.y + sa * inner),
                              D2D1::Point2F(c.x + ca * outer, c.y + sa * outer),
                              brush, std::max(1.2f, 0.055f * s));
        }
    }

    void DrawLightningShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush) {
        ComPtr<ID2D1PathGeometry> bolt;
        if (FAILED(d2dFactory_->CreatePathGeometry(&bolt)) || !bolt) {
            return;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(bolt->Open(&sink)) || !sink) {
            return;
        }
        sink->BeginFigure(D2D1::Point2F(c.x + 0.07f * s, c.y + 0.16f * s), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(c.x - 0.11f * s, c.y + 0.44f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.00f * s, c.y + 0.44f * s));
        sink->AddLine(D2D1::Point2F(c.x - 0.06f * s, c.y + 0.66f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.15f * s, c.y + 0.36f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.03f * s, c.y + 0.36f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.12f * s, c.y + 0.16f * s));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target_->FillGeometry(bolt.Get(), brush);
    }

    void DrawWeatherIcon(D2D1_POINT_2F center, float size, WeatherVisual visual,
                         ID2D1Brush* strong, ID2D1Brush* soft) {
        const float s = size;
        const float stroke = std::max(1.3f, 0.06f * s);

        switch (visual) {
            case WeatherVisual::Clear:
                DrawSunShape(center, s, strong, 0.28f);
                break;

            case WeatherVisual::PartlyCloudy: {
                // Sun peeking out behind the cloud's upper-left.
                DrawSunShape(D2D1::Point2F(center.x - 0.20f * s, center.y - 0.20f * s), s * 0.62f, soft, 0.30f);
                DrawCloudShape(D2D1::Point2F(center.x + 0.05f * s, center.y + 0.06f * s), s * 0.92f, strong);
                break;
            }

            case WeatherVisual::Cloudy:
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.06f * s), s, soft);
                DrawCloudShape(D2D1::Point2F(center.x + 0.04f * s, center.y + 0.06f * s), s * 0.86f, strong);
                break;

            case WeatherVisual::Fog: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.16f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const float y = center.y + (0.24f + 0.15f * static_cast<float>(i)) * s;
                    const float half = (0.34f - 0.05f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(center.x - half, y),
                                      D2D1::Point2F(center.x + half, y), soft, stroke);
                }
                break;
            }

            case WeatherVisual::Storm:
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.20f * s), s * 0.92f, soft);
                DrawLightningShape(center, s, strong);
                break;

            case WeatherVisual::Rain: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const float x = center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    const float y = center.y + 0.24f * s;
                    target_->DrawLine(D2D1::Point2F(x + 0.05f * s, y),
                                      D2D1::Point2F(x - 0.03f * s, y + 0.26f * s), soft, stroke);
                }
                break;
            }

            case WeatherVisual::Snow: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const D2D1_POINT_2F f = D2D1::Point2F(
                        center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s,
                        center.y + (0.34f + (i == 1 ? 0.06f : 0.0f)) * s);
                    const float r = 0.075f * s;
                    for (int k = 0; k < 3; ++k) {
                        const float a = static_cast<float>(k) * 3.14159265f / 3.0f;
                        target_->DrawLine(D2D1::Point2F(f.x - std::cos(a) * r, f.y - std::sin(a) * r),
                                          D2D1::Point2F(f.x + std::cos(a) * r, f.y + std::sin(a) * r),
                                          soft, std::max(1.0f, 0.035f * s));
                    }
                }
                break;
            }

            case WeatherVisual::Unknown:
            default: {
                // Thermometer: stem, bulb, and a couple of gradation ticks.
                const float stemW = 0.11f * s;
                const D2D1_RECT_F stem = D2D1::RectF(center.x - stemW, center.y - 0.40f * s,
                                                     center.x + stemW, center.y + 0.18f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, stemW, stemW), soft);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + 0.26f * s), 0.19f * s, 0.19f * s), strong);
                const D2D1_RECT_F mercury = D2D1::RectF(center.x - stemW * 0.55f, center.y - 0.12f * s,
                                                        center.x + stemW * 0.55f, center.y + 0.20f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(mercury, stemW * 0.55f, stemW * 0.55f), strong);
                break;
            }
        }
    }

    // Small vector glyphs shared by the weather, hardware and game-overlay cards.
    //   0 wind   1 thermometer   2 droplet
    //   3 cpu    4 memory        5 gpu       6 upload   7 download   8 disk
    //   9 gauge (frame rate)
    //
    // The game overlay used to carry its own icon set (DrawGameIcon) drawn at
    // different stroke weights and proportions, so the same CPU appeared as two
    // different symbols depending on which surface you were looking at. Every
    // surface now draws from this one family.
    void DrawMetricGlyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush) {
        const float stroke = std::max(1.1f, 0.11f * s);

        if (kind == 9) {  // gauge: dial arc, ticks and a needle
            const float r = 0.40f * s;
            target_->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke);
            for (int i = 0; i < 5; ++i) {
                const float a = -3.14159265f * 0.8f + static_cast<float>(i) * 3.14159265f * 0.4f;
                const float ca = std::cos(a);
                const float sa = std::sin(a);
                target_->DrawLine(D2D1::Point2F(c.x + ca * r, c.y + sa * r),
                                  D2D1::Point2F(c.x + ca * (r - 0.10f * s), c.y + sa * (r - 0.10f * s)),
                                  brush, stroke * 0.7f);
            }
            target_->FillEllipse(D2D1::Ellipse(c, 0.075f * s, 0.075f * s), brush);
            const float na = -3.14159265f * 0.25f;
            target_->DrawLine(c, D2D1::Point2F(c.x + std::cos(na) * r * 0.82f,
                                               c.y + std::sin(na) * r * 0.82f),
                              brush, stroke);
            return;
        }

        // Arrow used by the upload/download glyphs; `dir` is -1 up, +1 down.
        auto drawArrow = [&](float dir) {
            target_->DrawLine(D2D1::Point2F(c.x, c.y - 0.38f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.34f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x + 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
        };

        switch (kind) {
            case 3: {  // cpu: chip body with pins on all four sides
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.28f * s, c.y - 0.28f * s,
                                                     c.x + 0.28f * s, c.y + 0.28f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.07f * s, 0.07f * s), brush, stroke);
                const D2D1_RECT_F core = D2D1::RectF(c.x - 0.11f * s, c.y - 0.11f * s,
                                                     c.x + 0.11f * s, c.y + 0.11f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(core, 0.03f * s, 0.03f * s), brush);
                for (int i = -1; i <= 1; ++i) {
                    const float o = static_cast<float>(i) * 0.15f * s;
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y - 0.28f * s), D2D1::Point2F(c.x + o, c.y - 0.42f * s), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y + 0.28f * s), D2D1::Point2F(c.x + o, c.y + 0.42f * s), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x - 0.28f * s, c.y + o), D2D1::Point2F(c.x - 0.42f * s, c.y + o), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + 0.28f * s, c.y + o), D2D1::Point2F(c.x + 0.42f * s, c.y + o), brush, stroke * 0.8f);
                }
                break;
            }
            case 4: {  // memory: module with contact notches along the bottom
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.42f * s, c.y - 0.26f * s,
                                                     c.x + 0.42f * s, c.y + 0.22f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                for (int i = -2; i <= 2; ++i) {
                    const float x = c.x + static_cast<float>(i) * 0.16f * s;
                    target_->DrawLine(D2D1::Point2F(x, c.y - 0.10f * s), D2D1::Point2F(x, c.y + 0.06f * s), brush, stroke * 0.85f);
                }
                target_->DrawLine(D2D1::Point2F(c.x - 0.20f * s, c.y + 0.34f * s),
                                  D2D1::Point2F(c.x + 0.20f * s, c.y + 0.34f * s), brush, stroke);
                break;
            }
            case 5: {  // gpu: board with a fan
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.44f * s, c.y - 0.24f * s,
                                                     c.x + 0.44f * s, c.y + 0.26f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.15f * s, 0.15f * s), brush, stroke * 0.9f);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.045f * s, 0.045f * s), brush);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y - 0.10f * s), D2D1::Point2F(c.x + 0.32f * s, c.y - 0.10f * s), brush, stroke * 0.8f);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y + 0.04f * s), D2D1::Point2F(c.x + 0.32f * s, c.y + 0.04f * s), brush, stroke * 0.8f);
                break;
            }
            case 6:  // upload
                drawArrow(-1.0f);
                break;
            case 7:  // download
                drawArrow(1.0f);
                break;
            case 8: {  // disk: stacked platters
                for (int i = 0; i < 3; ++i) {
                    const float y = c.y - 0.22f * s + static_cast<float>(i) * 0.22f * s;
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, y), 0.36f * s, 0.12f * s), brush, stroke * 0.9f);
                }
                break;
            }
            default:
                break;
        }
        if (kind >= 3) {
            return;
        }

        switch (kind) {
            case 0: {  // wind: three streaming lines
                const float lens[3] = {0.46f, 0.30f, 0.38f};
                for (int i = 0; i < 3; ++i) {
                    const float y = c.y + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(c.x - 0.44f * s, y),
                                      D2D1::Point2F(c.x - 0.44f * s + lens[i] * s * 1.9f, y),
                                      brush, stroke);
                }
                break;
            }
            case 1: {  // thermometer
                const float w = 0.13f * s;
                const D2D1_RECT_F stem = D2D1::RectF(c.x - w, c.y - 0.42f * s, c.x + w, c.y + 0.12f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, w, w), brush);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 0.24f * s), 0.22f * s, 0.22f * s), brush);
                break;
            }
            case 2:
            default: {  // droplet: circle body with a tapered tip
                ComPtr<ID2D1PathGeometry> drop;
                if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&drop)) && drop) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(drop->Open(&sink)) && sink) {
                        sink->BeginFigure(D2D1::Point2F(c.x, c.y - 0.44f * s), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(c.x + 0.34f * s, c.y - 0.02f * s),
                            D2D1::Point2F(c.x + 0.30f * s, c.y + 0.36f * s),
                            D2D1::Point2F(c.x, c.y + 0.38f * s)));
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(c.x - 0.30f * s, c.y + 0.36f * s),
                            D2D1::Point2F(c.x - 0.34f * s, c.y - 0.02f * s),
                            D2D1::Point2F(c.x, c.y - 0.44f * s)));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        target_->FillGeometry(drop.Get(), brush);
                    }
                }
                break;
            }
        }
    }

    void DrawWeatherDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, bool hasWeather, const std::wstring& wIcon, const std::wstring& wText) {
        wchar_t wTemp[32] = {};
        if (hasWeather) swprintf_s(wTemp, L"%.0f\x00B0", state.weather.temperature);
        else wcscpy_s(wTemp, L"--\x00B0");

        std::wstring city = hasWeather ? state.weather.city : std::wstring(Loc(L"Locating..."));
        std::wstring desc = wText;

        // ── Layout ───────────────────────────────────────────────────────────
        // Left is an editorial hero block (place / reading / condition), left
        // aligned so the city, temperature and description share one optical
        // margin. Right is a stack of metric cards using the same card primitive
        // as every other dashboard. The old version centred each element in its
        // own box, which is what left the icon and the temperature floating apart
        // with a dead gap between them.
        const float heroLeft = rect.left + 26.0f * scale;
        const float heroRight = rect.left + 188.0f * scale;
        const float dividerX = rect.left + 200.0f * scale;
        const float metricLeft = rect.left + 214.0f * scale;
        const float metricRight = rect.right - 22.0f * scale;

        // Accent-tinted icon brushes: `strong` carries the shape, `soft` the
        // secondary detail (rain, rays, fog bands).
        ComPtr<ID2D1SolidColorBrush> iconStrong;
        ComPtr<ID2D1SolidColorBrush> iconSoft;
        target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.95f * settingsOpacity_), &iconStrong);
        target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.42f * settingsOpacity_), &iconSoft);

        // Place label.
        textBrush_->SetOpacity(0.55f);
        if (smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(city.c_str(), static_cast<UINT32>(city.length()), smallTextFormat_.Get(),
                               D2D1::RectF(heroLeft, rect.top + 26.0f * scale, heroRight, rect.top + 44.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Icon and temperature share one band and sit adjacent, so they read as
        // a single unit instead of two centred items.
        const float readingCenterY = rect.top + 82.0f * scale;
        if (hasWeather && iconStrong && iconSoft) {
            DrawWeatherIcon(D2D1::Point2F(heroLeft + 22.0f * scale, readingCenterY), 44.0f * scale,
                            WeatherVisualFromCode(state.weather.weatherCode),
                            iconStrong.Get(), iconSoft.Get());
        } else if (iconStrong && iconSoft) {
            DrawWeatherIcon(D2D1::Point2F(heroLeft + 22.0f * scale, readingCenterY), 44.0f * scale,
                            WeatherVisual::Unknown, iconStrong.Get(), iconSoft.Get());
        }

        textBrush_->SetOpacity(0.98f);
        if (hugeTextFormat_) {
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(wTemp, static_cast<UINT32>(wcslen(wTemp)), hugeTextFormat_.Get(),
                               D2D1::RectF(heroLeft + 52.0f * scale, readingCenterY - 32.0f * scale,
                                           heroRight, readingCenterY + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float descTop = rect.top + 120.0f * scale;
        const float descBottom = rect.top + 162.0f * scale;

        // Description
        const size_t descLength = desc.length();
        float descFontSize = 13.5f;
        if (descLength > 58) descFontSize = 9.8f;
        else if (descLength > 44) descFontSize = 10.5f;
        else if (descLength > 32) descFontSize = 11.5f;
        else if (descLength > 22) descFontSize = 12.5f;
        descFontSize *= scale;

        if (std::fabs(weatherDescFormatSize_ - descFontSize) > 0.01f || !weatherDescFormat_) {
            weatherDescFormat_.Reset();
            if (dwriteFactory_) {
                bool created = false;
                if (!settings.fontFamily.empty()) {
                    HRESULT hr = dwriteFactory_->CreateTextFormat(
                        settings.fontFamily.c_str(), nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        descFontSize, L"", &weatherDescFormat_);
                    if (SUCCEEDED(hr)) created = true;
                }
                if (!created) {
                    HRESULT hr = dwriteFactory_->CreateTextFormat(
                        L"Segoe UI Variable Display", nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        descFontSize, L"", &weatherDescFormat_);
                    if (FAILED(hr)) {
                        dwriteFactory_->CreateTextFormat(
                            L"Segoe UI", nullptr,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL,
                            descFontSize, L"", &weatherDescFormat_);
                    }
                }
            }
            if (weatherDescFormat_) {
                weatherDescFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                // Left aligned to share the hero column's margin with the city
                // and temperature.
                weatherDescFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                weatherDescFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            weatherDescFormatSize_ = descFontSize;
        }

        textBrush_->SetOpacity(0.82f);
        target_->DrawTextW(desc.c_str(), static_cast<UINT32>(desc.length()),
                           weatherDescFormat_ ? weatherDescFormat_.Get() : textFormat_.Get(),
                           D2D1::RectF(heroLeft, descTop, heroRight, descBottom),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        textBrush_->SetOpacity(0.96f);

        // Hairline divider, inset from both ends so it reads as a separator
        // rather than a full-height rule.
        ComPtr<ID2D1SolidColorBrush> divider;
        target_->CreateSolidColorBrush(WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_), &divider);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dividerX, rect.top + 34.0f * scale,
                                          dividerX + 1.0f * scale, rect.bottom - 34.0f * scale),
                              0.5f * scale, 0.5f * scale), divider.Get());

        // ── Metric cards ─────────────────────────────────────────────────────
        // Each card is icon + label on one line with the value beneath, which
        // keeps long localized labels from colliding with the value the way a
        // single "Label: value" line did.
        struct Metric {
            int glyph;
            const wchar_t* label;
            std::wstring value;
        };

        const std::wstring windUnit = settings.weatherFahrenheit ? L" mph" : L" km/h";
        const Metric metrics[3] = {
            {0, Loc(L"Wind"),
             hasWeather ? state.weather.windSpeed + windUnit + L" " + WindDirToArrow(state.weather.windDir)
                        : std::wstring(L"--")},
            {1, Loc(L"Feels Like"),
             hasWeather ? state.weather.feelsLike + L"\x00B0" : std::wstring(L"--")},
            {2, Loc(L"Humidity"),
             hasWeather ? state.weather.humidity + L"%" : std::wstring(L"--")},
        };

        const float cardH = 40.0f * scale;
        const float cardGap = 7.0f * scale;
        const float stackH = cardH * 3.0f + cardGap * 2.0f;
        float cardTop = (rect.top + rect.bottom) * 0.5f - stackH * 0.5f;

        for (const Metric& m : metrics) {
            const D2D1_RECT_F card = D2D1::RectF(metricLeft, cardTop, metricRight, cardTop + cardH);
            DrawCard(card, 10.0f * scale);

            if (iconSoft) {
                DrawMetricGlyph(D2D1::Point2F(card.left + 17.0f * scale, card.top + 14.0f * scale),
                                14.0f * scale, m.glyph, iconSoft.Get());
            }

            const float textLeft = card.left + 30.0f * scale;
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.62f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 4.0f * scale,
                                               card.right - 8.0f * scale, card.top + 20.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                target_->DrawTextW(m.value.c_str(), static_cast<UINT32>(m.value.size()), textFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 19.0f * scale,
                                               card.right - 8.0f * scale, card.bottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            cardTop += cardH + cardGap;
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

