// Hotkeys that turn DLSS 5 and DLAA off and back on while playing, to compare (README.md, "Turn it
// off and on while playing"). [DLSS5] ToggleKey and [DLAA] ToggleKey, only for what the
// session started with: without DLSS 5 (Export and Helper, or SelfTest) or DLAA Enable there is
// no key to read. The keys are read once a frame at Present (D5FrameEnd), on the press, while the
// game's window is in front.
//  - Off skips the feature's evaluate and its write-back. The helper and the feature stay, so on
//    is instant, and the first frame back goes with RESET (D5MakeFrame: the feature skipped the
//    frame before). Nothing is sent meanwhile: the frame numbers continue where they stopped.
//  - DLAA off also stops the jitter from the next frame and lets the game's MLAA run; back on, the
//    first DLAA frame is not jittered and the jitter starts after it (dlaa.inl), as at startup.
//  - ToggleIndicator: "DLSS 5 ON" and the like for 2 s in the top left corner, over the finished
//    frame (the HUD included), drawn with Clear rectangles from a built-in 5x7 font. Green is on,
//    red off, yellow on but not running (the helper or the feature is off for the session).

bool g_d5_nr_on = true;       // DLSS 5 runs (its hotkey turns it off)
bool g_dlaa_on = true;        // DLAA runs
bool g_toggle_indicator = true;

constexpr DWORD kToggleIndicatorMs = 2000;

struct ToggleKey
{
    const char* feature = "";
    int vk = 0;                    // 0: no key
    char name[16] = {};            // as in the ini, for the log
    bool* on = nullptr;
    bool down = false;
    bool showing = false;          // the indicator line is up
    DWORD shown_at = 0;            // GetTickCount at the press
};

ToggleKey g_toggle_keys[2];

// A key name (F1-F24, A-Z, 0-9, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock)
// or a virtual-key code (decimal, or hex with 0x). Empty or 0: none.
int ToggleParseKey(const char* value)
{
    if (value[0] == '\0' || std::strcmp(value, "0") == 0) {
        return 0;
    }
    if (value[1] == '\0') {
        const char c = value[0] >= 'a' && value[0] <= 'z' ? static_cast<char>(value[0] - 'a' + 'A') : value[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return c;
        }
    }
    if ((value[0] == 'F' || value[0] == 'f') && value[1] >= '1' && value[1] <= '9') {
        char* end = nullptr;
        const long n = std::strtol(value + 1, &end, 10);
        if (*end == '\0' && n >= 1 && n <= 24) {
            return VK_F1 + static_cast<int>(n) - 1;
        }
    }
    static const struct
    {
        const char* name;
        int vk;
    } kNames[] = {{"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR},
        {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL}};
    for (const auto& key : kNames) {
        if (_stricmp(value, key.name) == 0) {
            return key.vk;
        }
    }
    char* end = nullptr;
    const long code = std::strtol(value, &end, 0);
    if (end != value && *end == '\0' && code > 0 && code < 256) {
        return static_cast<int>(code);
    }
    return -1;
}

void ToggleReadKey(ToggleKey& key, const char* section, const char* feature, bool* on, const char* fallback, const char* ini_path)
{
    key.feature = feature;
    key.on = on;
    GetPrivateProfileStringA(section, "ToggleKey", fallback, key.name, sizeof(key.name), ini_path);
    const int vk = ToggleParseKey(key.name);
    if (vk < 0) {
        Log("Toggle: [%s] ToggleKey=%s is not a key name or code; no hotkey for %s", section, key.name, feature);
    }
    key.vk = std::max(vk, 0);
}

// After LoadDlss5Config and LoadDlaaConfig.
void LoadToggleConfig(const char* ini_path)
{
    g_toggle_indicator = GetPrivateProfileIntA("DLSS5", "ToggleIndicator", 1, ini_path) != 0;
    if (g_d5_nr) {
        ToggleReadKey(g_toggle_keys[0], "DLSS5", "DLSS 5", &g_d5_nr_on, "F10", ini_path);
    }
    if (g_dlaa_enable) {
        ToggleReadKey(g_toggle_keys[1], "DLAA", "DLAA", &g_dlaa_on, "F11", ini_path);
    }
    for (const ToggleKey& key : g_toggle_keys) {
        if (key.vk != 0) {
            Log("Toggle: %s on and off with %s (virtual key 0x%02X), indicator %s", key.feature, key.name, key.vk,
                g_toggle_indicator ? "on" : "off");
        }
    }
}

// The window in front belongs to the game (any of its windows: the device's, a borderless one).
bool ToggleGameInFront()
{
    const HWND window = GetForegroundWindow();
    DWORD pid = 0;
    return window != nullptr && GetWindowThreadProcessId(window, &pid) != 0 && pid == GetCurrentProcessId();
}

// The feature can't run this session (yellow in the indicator): the helper failed, or gave up on it.
bool ToggleFeatureGone(const ToggleKey& key)
{
    if (g_d5_self_test) {
        return false;
    }
    const D5Feature& f = g_d5_features[&key == &g_toggle_keys[1] ? DSF_DLSS5_FEATURE_DLAA : DSF_DLSS5_FEATURE_NR];
    return g_d5_bridge_state == D5_BRIDGE_FAILED || (g_d5_bridge_state == D5_BRIDGE_READY && f.state == D5_FEATURE_OFF);
}

// ---------------------------------------------------------------------------------------
// The indicator: 5x7 glyphs, one row of 5 bits per byte (bit 4 is the left column), for the
// characters the indicator writes. Anything else is blank.

struct ToggleGlyph
{
    char c;
    std::uint8_t rows[7];
};

const ToggleGlyph kToggleFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
};

const std::uint8_t* ToggleGlyphRows(char c)
{
    for (const ToggleGlyph& glyph : kToggleFont) {
        if (glyph.c == c) {
            return glyph.rows;
        }
    }
    return nullptr;
}

// Appends text's lit pixels as rectangles of scale x scale screen pixels, one per run in a row.
int ToggleTextRects(const char* text, LONG x, LONG y, LONG scale, D3DRECT* rects, int count, int capacity)
{
    for (int i = 0; text[i] != '\0'; ++i) {
        const std::uint8_t* rows = ToggleGlyphRows(text[i]);
        const LONG left = x + i * 6 * scale;
        for (int row = 0; rows != nullptr && row < 7; ++row) {
            for (int column = 0; column < 5;) {
                if ((rows[row] & (0x10 >> column)) == 0) {
                    ++column;
                    continue;
                }
                const int start = column;
                while (column < 5 && (rows[row] & (0x10 >> column)) != 0) {
                    ++column;
                }
                if (count < capacity) {
                    rects[count++] = {left + start * scale, y + row * scale, left + column * scale, y + (row + 1) * scale};
                }
            }
        }
    }
    return count;
}

// At Present: the lines whose time is not up, on the back buffer. Clear needs no shaders or
// vertex states, only the targets (all four saved and put back), the viewport, the scissor test
// and the colour write mask.
void ToggleDrawIndicator(IDirect3DDevice9* device, DWORD now)
{
    IDirect3DSurface9* back = nullptr;
    D3DSURFACE_DESC desc = {};
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || back == nullptr || FAILED(back->GetDesc(&desc))) {
        D5SafeRelease(back);
        return;
    }
    g_d5_internal = true;
    D5SavedTargets saved;
    D5SaveTargets(device, &saved);
    D3DVIEWPORT9 viewport = {};
    device->GetViewport(&viewport);
    DWORD scissor = FALSE;
    DWORD write_mask = 0xF;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &write_mask);

    device->SetRenderTarget(0, back);   // the viewport becomes the whole back buffer
    for (DWORD i = 1; i < 4; ++i) {
        device->SetRenderTarget(i, nullptr);   // Clear writes every bound target
    }
    device->SetDepthStencilSurface(nullptr);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

    const LONG scale = std::max<LONG>(2, static_cast<LONG>(desc.Height) / 240);
    LONG y = 8 * scale;
    for (const ToggleKey& key : g_toggle_keys) {
        if (!key.showing) {
            continue;
        }
        char text[16] = {};
        std::snprintf(text, sizeof(text), "%s %s", key.feature, *key.on ? "ON" : "OFF");
        const LONG x = 8 * scale;
        const LONG width = static_cast<LONG>(std::strlen(text)) * 6 * scale - scale;
        const D3DRECT box = {x - 2 * scale, y - 2 * scale, x + width + 2 * scale, y + 9 * scale};
        g_d5_clear(device, 1, &box, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        D3DRECT rects[256];
        const int count = ToggleTextRects(text, x, y, scale, rects, 0, 256);
        const D3DCOLOR colour = !*key.on ? D3DCOLOR_XRGB(255, 80, 64) :
            ToggleFeatureGone(key)       ? D3DCOLOR_XRGB(255, 220, 0) :
                                           D3DCOLOR_XRGB(64, 255, 64);
        g_d5_clear(device, static_cast<DWORD>(count), rects, D3DCLEAR_TARGET, colour, 1.0f, 0);
        y += 13 * scale;
    }

    D5RestoreTargets(device, &saved);
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, write_mask);
    g_d5_internal = false;
    back->Release();
}

// D5FrameEnd, before DlaaFrameEnd decides the next frame's jitter.
void TogglesFrameEnd(IDirect3DDevice9* device)
{
    if (g_toggle_keys[0].vk == 0 && g_toggle_keys[1].vk == 0) {
        return;
    }
    const DWORD now = GetTickCount();
    bool shown = false;
    for (ToggleKey& key : g_toggle_keys) {
        if (key.vk == 0) {
            continue;
        }
        const bool down = (GetAsyncKeyState(key.vk) & 0x8000) != 0;
        if (down && !key.down && ToggleGameInFront()) {
            *key.on = !*key.on;
            key.showing = true;
            key.shown_at = now;
            Log("Toggle: %s %s (%s) from frame %ld%s", key.feature, *key.on ? "on" : "off", key.name,
                static_cast<long>(g_d5_frame_index + 1),
                !*key.on ? "" : ToggleFeatureGone(key) ? ", but it is off for this session (see above)" : ", its first frame with RESET");
        }
        key.down = down;
        key.showing = key.showing && now - key.shown_at < kToggleIndicatorMs;
        shown = shown || key.showing;
    }
    if (shown && g_toggle_indicator) {
        ToggleDrawIndicator(device, now);
    }
}
