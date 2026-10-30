// [Replay] DisableFilmEffect: no "old film" look while Film Director plays a replay back.
//
// The look is not a post-process. The tone map (PostProcessing.fx) gets the same colour grade,
// vignette and grain constants as in normal play. It is Film Director's own GUI page
// (Media\GUI_FilmDirector.fchunk, Pages\grain_and_scratches, scene "Replays"), drawn in the
// composite + HUD pass (render-queue target set 0) over the finished frame:
//  - a full-screen quad in an 8x8 pale cyan texture ("Img.Replay_tint") right after the
//    composite, which lifts the blacks and washes the colours grey-green;
//  - film damage sprites near the end of the pass, after the playback menu: a long vertical
//    scratch (8x800), a dust speck (32x32), a hair (64x64) and an atlas of scratches and specks
//    (256x512).
// Hiding the playback menu (X) leaves them, and the scene's element lookup only searches its
// current page, so they are skipped here by texture: while the replay system plays back (mode 2),
// a set-0 draw whose stage-0 texture is one of these five is not drawn. Textures are recognised by
// size, format and a hash of their block rows (no pitch padding, so DXVK and native D3D9 agree),
// computed once per texture and cached until playback starts or stops. The tint's texture is also
// bound by the menu's text draws, which use another pixel shader, so it only counts with the
// sprite shader. The game's HUD in normal play, the playback menu, Bink movies and the plugin's own
// indicator are untouched.

bool g_replay_disable_film = false;

constexpr DWORD kReplaySystemOffset = 0x22C;        // engine: the replay system (Replay::System)
constexpr DWORD kReplayModeOffset = 0x4E588;        // 0 off, 1 recording, 2 playing back, 3 saving
constexpr DWORD kReplaySpriteShaderHash = 0xB3A294A7;   // the GUI's sprite pixel shader (D5ShaderHash)

struct ReplayFilmTexture
{
    UINT width;
    UINT height;
    D3DFORMAT format;
    DWORD hash;
    bool sprite_only;   // also bound by draws that aren't the overlay (text): match with the sprite shader only
    const char* what;
};

const ReplayFilmTexture kReplayFilmTextures[] = {
    {8, 8, D3DFMT_DXT5, 0xF6604F7E, true, "tint"},
    {8, 800, D3DFMT_DXT5, 0x72456D1A, false, "scratch"},
    {32, 32, D3DFMT_DXT5, 0xB29D6DF1, false, "speck"},
    {64, 64, D3DFMT_DXT5, 0xB7AA92E8, false, "hair"},
    {256, 512, D3DFMT_DXT1, 0x1FA83DE9, false, "damage atlas"},
};

bool g_replay_playing = false;       // the replay system is playing back (read at each frame end)
DWORD g_replay_film_logged = 0;      // bit per kReplayFilmTextures entry: its first skip this playback is logged
struct ReplayFilmCacheEntry
{
    UINT width;
    UINT height;
    D3DFORMAT format;
    int index;   // in kReplayFilmTextures, -1 none
};
std::unordered_map<const void*, ReplayFilmCacheEntry> g_replay_film_cache;   // by texture (render thread)

void LoadReplayConfig(const char* ini_path)
{
    g_replay_disable_film = GetPrivateProfileIntA("Replay", "DisableFilmEffect", 0, ini_path) != 0;
    if (g_replay_disable_film) {
        Log("Replay: Film Director's old-film overlay (tint, scratches, dust) is skipped during replay playback");
    }
}

// FNV-1a over the size and format, then each row of 4x4 blocks of mip 0 without the pitch padding.
DWORD ReplayFilmHash(IDirect3DTexture9* texture, const D3DSURFACE_DESC& desc)
{
    D3DLOCKED_RECT rect = {};
    if (FAILED(texture->LockRect(0, &rect, nullptr, D3DLOCK_READONLY))) {
        return 0;
    }
    DWORD hash = 2166136261u;
    const DWORD header[3] = {desc.Width, desc.Height, static_cast<DWORD>(desc.Format)};
    for (size_t i = 0; i < sizeof(header); ++i) {
        hash = (hash ^ reinterpret_cast<const std::uint8_t*>(header)[i]) * 16777619u;
    }
    const UINT block_bytes = desc.Format == D3DFMT_DXT1 ? 8 : 16;
    const size_t row_bytes = static_cast<size_t>(std::max(1u, (desc.Width + 3) / 4)) * block_bytes;
    const UINT rows = std::max(1u, (desc.Height + 3) / 4);
    for (UINT row = 0; row < rows; ++row) {
        const auto* bytes = static_cast<const std::uint8_t*>(rect.pBits) + static_cast<size_t>(row) * rect.Pitch;
        for (size_t i = 0; i < row_bytes; ++i) {
            hash = (hash ^ bytes[i]) * 16777619u;
        }
    }
    texture->UnlockRect(0);
    return hash;
}

// Which overlay texture is bound at stage 0, or -1.
int ReplayFilmTextureIndex(IDirect3DDevice9* device)
{
    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(device->GetTexture(0, &base)) || base == nullptr) {
        return -1;
    }
    int index = -1;
    D3DSURFACE_DESC desc = {};
    const bool texture = base->GetType() == D3DRTYPE_TEXTURE &&
        SUCCEEDED(static_cast<IDirect3DTexture9*>(base)->GetLevelDesc(0, &desc));
    const auto cached = g_replay_film_cache.find(base);
    // The size and format are checked again, in case the address now holds another texture.
    if (cached != g_replay_film_cache.end() && cached->second.width == desc.Width && cached->second.height == desc.Height &&
        cached->second.format == desc.Format) {
        index = cached->second.index;
    } else {
        if (texture && desc.Pool != D3DPOOL_DEFAULT) {
            for (int i = 0; i < static_cast<int>(sizeof(kReplayFilmTextures) / sizeof(kReplayFilmTextures[0])); ++i) {
                const ReplayFilmTexture& t = kReplayFilmTextures[i];
                if (desc.Width == t.width && desc.Height == t.height && desc.Format == t.format) {
                    // Hashed only on a size and format match: few of the GUI's textures get this far.
                    if (ReplayFilmHash(static_cast<IDirect3DTexture9*>(base), desc) == t.hash) {
                        index = i;
                    }
                    break;
                }
            }
        }
        g_replay_film_cache[base] = {desc.Width, desc.Height, desc.Format, index};
    }
    base->Release();
    return index;
}

// The draw hooks, before anything else: true skips the draw.
bool ReplayFilmSkipDraw(IDirect3DDevice9* device)
{
    if (!g_replay_disable_film || !g_replay_playing || g_d5_pass_target_set != 0) {
        return false;
    }
    const int index = ReplayFilmTextureIndex(device);
    if (index < 0 || (kReplayFilmTextures[index].sprite_only && D5ShaderHash(g_d5.ps) != kReplaySpriteShaderHash)) {
        return false;
    }
    if ((g_replay_film_logged & (1u << index)) == 0) {
        g_replay_film_logged |= 1u << index;
        Log("Replay: skipping the film overlay's %s from frame %ld", kReplayFilmTextures[index].what,
            static_cast<long>(g_d5_frame_index));
    }
    return true;
}

bool ReplayPlayingBack()
{
    const ModuleRange exe = GetExeRange();
    if (!IsRvaInRange(exe, kEngineSingletonRva, sizeof(void*))) {
        return false;
    }
    std::uint8_t* engine = *reinterpret_cast<std::uint8_t**>(exe.base + kEngineSingletonRva);
    if (engine == nullptr || !IsReadableMemory(engine + kReplaySystemOffset, sizeof(void*))) {
        return false;
    }
    std::uint8_t* replay = *reinterpret_cast<std::uint8_t**>(engine + kReplaySystemOffset);
    return replay != nullptr && IsReadableMemory(replay + kReplayModeOffset, sizeof(int)) &&
        *reinterpret_cast<const int*>(replay + kReplayModeOffset) == 2;
}

// D5FrameEnd: whether the next frame is a playback frame. The texture cache starts over at each
// change, so a texture freed meanwhile can't be mistaken for another at the same address.
void ReplayFilmFrameEnd()
{
    if (!g_replay_disable_film) {
        return;
    }
    const bool playing = ReplayPlayingBack();
    if (playing != g_replay_playing) {
        g_replay_playing = playing;
        g_replay_film_logged = 0;
        g_replay_film_cache.clear();
        Log("Replay: playback %s (frame %ld)", playing ? "started" : "stopped", static_cast<long>(g_d5_frame_index));
    }
}
