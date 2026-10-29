// Per-object motion for vehicles (research/dlss5-renderer.md, "Per-object motion").
//
// The game's vehicle shaders ("Uber2_0\Vehicles", recognised by g_PanelTransforms in their
// constant table) carry each car's object-to-world matrix in g_ObjectToWorld (c96, c100 or
// c164, read from the constant table). Every panel of a car, its wheels and interior included,
// shares that matrix, so it identifies the car within a frame.
//
// During the scene pass each vehicle draw writes its car's index into stencil bits 1-7 (1 to 127
// cars); every other depth-writing draw writes 0 there, so a car hidden behind something later
// loses its tag. The game's own stencil use in the scene (imposters: EQUAL 0, then REPLACE 0,
// on a buffer that is otherwise always 0) is tested on bit 0 only, which keeps its result the
// same; when such a draw writes depth it clears bits 1-7 like any other. At the scene end each
// car is matched with the previous frame's cars (an unchanged matrix first, then nearest pairs to
// where each was heading, turning less than 30 degrees), and the motion pass draws one
// stencil-tested quad per moving car, scissored to the car, that reprojects its pixels through
// VP_prev * W_prev * inverse(W_cur) * inverse(VP_cur), built in double precision on the CPU.
// Everything else keeps camera motion.

bool g_d5_object_motion = true;   // ObjectMotion (with Helper=1)
bool g_d5_object_debug = false;   // ObjectMotionDebug

// FrameTimeLog=1: the object motion's own cost, summed over the log window.
extern bool g_d5_frame_time_log;
double D5NowMs();
double g_d5_window_object_cpu_ms = 0.0;   // tagging, matching (render thread)
long long g_d5_window_object_cars = 0;
long long g_d5_window_object_moving = 0;
int g_d5_window_object_frames = 0;

struct D5ShaderInfo
{
    bool vehicle;
    int object_to_world;   // first register of g_ObjectToWorld, -1 if none
};

SRWLOCK g_d5_shader_info_lock = SRWLOCK_INIT;
std::unordered_map<const void*, D5ShaderInfo> g_d5_shader_info;

// Reads the constant table (CTAB comment) of shader model 1-3 bytecode.
D5ShaderInfo D5ParseShaderInfo(const DWORD* function)
{
    D5ShaderInfo info = {false, -1};
    if (function == nullptr) {
        return info;
    }
    for (size_t i = 1; i < 4096 && function[i] != 0x0000FFFF;) {
        const DWORD token = function[i];
        if ((token & 0xFFFF) != 0xFFFE) {
            break;   // comments come right after the version token
        }
        const DWORD length = (token >> 16) & 0x7FFF;
        if (length >= 8 && function[i + 1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
            const auto* table = reinterpret_cast<const std::uint8_t*>(&function[i + 2]);
            const size_t size = (length - 1) * sizeof(DWORD);
            const DWORD count = *reinterpret_cast<const DWORD*>(table + 12);
            const DWORD info_offset = *reinterpret_cast<const DWORD*>(table + 16);
            for (DWORD c = 0; c < count && info_offset + (c + 1) * 20 <= size; ++c) {
                const std::uint8_t* constant = table + info_offset + c * 20;
                const DWORD name_offset = *reinterpret_cast<const DWORD*>(constant);
                const WORD register_set = *reinterpret_cast<const WORD*>(constant + 4);
                const WORD register_index = *reinterpret_cast<const WORD*>(constant + 6);
                const WORD register_count = *reinterpret_cast<const WORD*>(constant + 8);
                const DWORD type_offset = *reinterpret_cast<const DWORD*>(constant + 12);
                if (name_offset >= size) {
                    continue;
                }
                const char* name = reinterpret_cast<const char*>(table + name_offset);
                if (std::strncmp(name, "g_PanelTransforms", size - name_offset) == 0) {
                    info.vehicle = true;
                } else if (std::strncmp(name, "g_ObjectToWorld", size - name_offset) == 0 && register_set == 2 /* D3DXRS_FLOAT4 */ &&
                           register_count >= 3 && register_index + 3 <= 256 && type_offset + 8 <= size) {
                    // float4x4, column-major (D3DXPC_MATRIX_COLUMNS): each register is one row of
                    // the 3x4 transform as the shader dots it with the position.
                    const WORD type_class = *reinterpret_cast<const WORD*>(table + type_offset);
                    const WORD rows = *reinterpret_cast<const WORD*>(table + type_offset + 4);
                    const WORD columns = *reinterpret_cast<const WORD*>(table + type_offset + 6);
                    if (type_class == 3 && rows == 4 && columns == 4) {
                        info.object_to_world = register_index;
                    }
                }
            }
            break;
        }
        i += 1 + length;
    }
    return info;
}

volatile LONG g_d5_shader_info_generation = 0;   // bumped on every change: drops the lookup cache

void D5NoteShader(const void* shader, const DWORD* function)
{
    const D5ShaderInfo info = D5ParseShaderInfo(function);
    AcquireSRWLockExclusive(&g_d5_shader_info_lock);
    if (info.vehicle) {
        g_d5_shader_info[shader] = info;
    } else {
        g_d5_shader_info.erase(shader);   // a released vehicle shader's address, reused
    }
    ReleaseSRWLockExclusive(&g_d5_shader_info_lock);
    InterlockedIncrement(&g_d5_shader_info_generation);
}

int D5VehicleObjectRegister(const void* shader)
{
    // Render thread only. Consecutive draws mostly share a shader.
    static const void* cached_shader = nullptr;
    static int cached_reg = -1;
    static LONG cached_generation = -1;
    const LONG generation = g_d5_shader_info_generation;
    if (shader == cached_shader && generation == cached_generation) {
        return cached_reg;
    }
    AcquireSRWLockShared(&g_d5_shader_info_lock);
    auto it = g_d5_shader_info.find(shader);
    const int reg = it != g_d5_shader_info.end() && it->second.vehicle ? it->second.object_to_world : -1;
    ReleaseSRWLockShared(&g_d5_shader_info_lock);
    cached_shader = shader;
    cached_reg = reg;
    cached_generation = generation;
    return reg;
}

// ---------------------------------------------------------------------------------------
// The frame's cars.

constexpr int kD5MaxObjects = 127;

struct D5Object
{
    float world[12];    // rows of g_ObjectToWorld
    double delta[12];   // rows of W_prev * inverse(W_cur): this frame's world points -> last frame's
    double move[3];     // translation since last frame (when matched)
    bool move_valid;
    bool moving;
    DWORD shader;       // first vertex shader that drew it (trace)
    int draws;
};

D5Object g_d5_objects[kD5MaxObjects + 1] = {};      // index 1..count
int g_d5_object_count = 0;
D5Object g_d5_objects_prev[kD5MaxObjects + 1] = {};
int g_d5_object_count_prev = 0;
LONG g_d5_objects_frame = -1;
int g_d5_moving_objects = 0;

int D5ObjectIndex(const float* world)
{
    // A car's draws mostly come one after another: try the last one first.
    static int last = 0;
    if (last >= 1 && last <= g_d5_object_count && std::memcmp(g_d5_objects[last].world, world, sizeof(g_d5_objects[last].world)) == 0) {
        ++g_d5_objects[last].draws;
        return last;
    }
    for (int k = 1; k <= g_d5_object_count; ++k) {
        if (std::memcmp(g_d5_objects[k].world, world, sizeof(g_d5_objects[k].world)) == 0) {
            ++g_d5_objects[k].draws;
            last = k;
            return k;
        }
    }
    if (g_d5_object_count >= kD5MaxObjects) {
        return 0;
    }
    const int k = ++g_d5_object_count;
    last = k;
    std::memcpy(g_d5_objects[k].world, world, sizeof(g_d5_objects[k].world));
    g_d5_objects[k].moving = false;
    g_d5_objects[k].move_valid = false;
    g_d5_objects[k].shader = g_d5_trace_frame != nullptr ? D5ShaderHash(g_d5.vs) : 0;
    g_d5_objects[k].draws = 1;
    return k;
}

bool D5InvertAffine(const float* m, double* out)
{
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) {
        return false;
    }
    const double s = 1.0 / det;
    const double r[9] = {(e * k - f * h) * s, (c * h - b * k) * s, (b * f - c * e) * s, (f * g - d * k) * s, (a * k - c * g) * s,
        (c * d - a * f) * s, (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
    const double t[3] = {m[3], m[7], m[11]};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0];
        out[row * 4 + 1] = r[row * 3 + 1];
        out[row * 4 + 2] = r[row * 3 + 2];
        out[row * 4 + 3] = -(r[row * 3 + 0] * t[0] + r[row * 3 + 1] * t[1] + r[row * 3 + 2] * t[2]);
    }
    return true;
}

// Cosine of the rotation between two transforms (rows of their 3x3 parts): trace(Ra * Rb^T).
double D5RotationCosine(const float* a, const float* b)
{
    double trace = 0.0;
    for (int row = 0; row < 3; ++row) {
        trace += static_cast<double>(a[row * 4 + 0]) * b[row * 4 + 0] + static_cast<double>(a[row * 4 + 1]) * b[row * 4 + 1] +
            static_cast<double>(a[row * 4 + 2]) * b[row * 4 + 2];
    }
    return (trace - 1.0) / 2.0;
}

extern float g_d5_scene_dt_ms;

// Scene end: match the cars with the previous frame's. An unchanged matrix is the same car
// (parked). The rest pair up nearest first: the distance from where the old car was heading
// (its last move, scaled to the frame time), within max(6 m, 90 m/s * dt) (max(3 m, 45 m/s * dt)
// with a known last move), and turning less than 30 degrees. So a car that despawns next to one
// that spawns doesn't hand it a 180-degree turn.
void D5FinishObjects()
{
    const double started = g_d5_frame_time_log ? D5NowMs() : 0.0;
    g_d5_moving_objects = 0;
    const bool consecutive = g_d5_objects_frame == g_d5_frame_index - 1;
    int match[kD5MaxObjects + 1] = {};
    float match_distance[kD5MaxObjects + 1] = {};
    static double previous_dt_ms = 0.0;
    const double dt_ms = g_d5_scene_dt_ms > 0.0f ? g_d5_scene_dt_ms : 16.7;
    const double dt = dt_ms / 1000.0;
    const double ratio = previous_dt_ms > 0.0 ? std::min(dt_ms / previous_dt_ms, 4.0) : 1.0;
    previous_dt_ms = dt_ms;
    for (int k = 1; k <= g_d5_object_count; ++k) {
        g_d5_objects[k].moving = false;
    }
    if (consecutive) {
        bool taken[kD5MaxObjects + 1] = {};
        for (int k = 1; k <= g_d5_object_count; ++k) {
            for (int j = 1; j <= g_d5_object_count_prev; ++j) {
                if (!taken[j] && std::memcmp(g_d5_objects_prev[j].world, g_d5_objects[k].world, sizeof(g_d5_objects[k].world)) == 0) {
                    match[k] = j;
                    taken[j] = true;
                    break;
                }
            }
        }
        struct Pair
        {
            float distance;
            int k, j;
        };
        static Pair pairs[kD5MaxObjects * kD5MaxObjects];
        int pair_count = 0;
        for (int k = 1; k <= g_d5_object_count; ++k) {
            if (match[k] != 0) {
                continue;
            }
            const float* world = g_d5_objects[k].world;
            for (int j = 1; j <= g_d5_object_count_prev; ++j) {
                if (taken[j]) {
                    continue;
                }
                const D5Object& old = g_d5_objects_prev[j];
                double expected[3] = {old.world[3], old.world[7], old.world[11]};
                if (old.move_valid) {
                    for (int i = 0; i < 3; ++i) {
                        expected[i] += old.move[i] * ratio;
                    }
                }
                const double dx = world[3] - expected[0], dy = world[7] - expected[1], dz = world[11] - expected[2];
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                const double limit = old.move_valid ? std::max(3.0, 45.0 * dt) : std::max(6.0, 90.0 * dt);
                if (distance < limit && D5RotationCosine(world, old.world) > 0.866) {   // 30 degrees
                    pairs[pair_count++] = {static_cast<float>(distance), k, j};
                }
            }
        }
        std::sort(pairs, pairs + pair_count, [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
        for (int i = 0; i < pair_count; ++i) {
            if (match[pairs[i].k] == 0 && !taken[pairs[i].j]) {
                match[pairs[i].k] = pairs[i].j;
                match_distance[pairs[i].k] = pairs[i].distance;
                taken[pairs[i].j] = true;
            }
        }
        for (int k = 1; k <= g_d5_object_count; ++k) {
            D5Object& object = g_d5_objects[k];
            if (match[k] != 0) {
                const float* p = g_d5_objects_prev[match[k]].world;
                object.move[0] = static_cast<double>(object.world[3]) - p[3];
                object.move[1] = static_cast<double>(object.world[7]) - p[7];
                object.move[2] = static_cast<double>(object.world[11]) - p[11];
                object.move_valid = true;
            }
            double inverse[12] = {};
            if (match[k] == 0 || std::memcmp(g_d5_objects_prev[match[k]].world, object.world, sizeof(object.world)) == 0 ||
                !D5InvertAffine(object.world, inverse)) {
                continue;   // new this frame (camera motion), or parked (camera motion is right)
            }
            const float* p = g_d5_objects_prev[match[k]].world;
            double largest = 0.0;   // largest difference from the identity
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const double value = p[row * 4 + 0] * inverse[0 * 4 + col] + p[row * 4 + 1] * inverse[1 * 4 + col] +
                        p[row * 4 + 2] * inverse[2 * 4 + col] + (col == 3 ? p[row * 4 + 3] : 0.0);
                    object.delta[row * 4 + col] = value;
                    largest = std::max(largest, std::fabs(value - (row == col ? 1.0 : 0.0)) * (col == 3 ? 1.0 : 4.0));
                }
            }
            if (largest < 1e-3) {
                continue;   // under a millimetre at 4 m from the car's origin: not worth a pass
            }
            object.moving = true;
            ++g_d5_moving_objects;
        }
    }
    if (g_d5_trace_frame != nullptr) {
        Log("DLSS5  cars frame %ld: %d tagged, %d moving%s", static_cast<long>(g_d5_frame_index), g_d5_object_count, g_d5_moving_objects,
            consecutive ? "" : " (no previous frame)");
        const float* vp = g_d5_vp_current;
        for (int k = 1; k <= g_d5_object_count; ++k) {
            const D5Object& o = g_d5_objects[k];
            const float* w = o.world;
            const float clip_x = vp[0] * w[3] + vp[1] * w[7] + vp[2] * w[11] + vp[3];
            const float clip_y = vp[4] * w[3] + vp[5] * w[7] + vp[6] * w[11] + vp[7];
            const float clip_w = vp[12] * w[3] + vp[13] * w[7] + vp[14] * w[11] + vp[15];
            const float sx = clip_w > 0.0f ? (clip_x / clip_w * 0.5f + 0.5f) : -1.0f;
            const float sy = clip_w > 0.0f ? (0.5f - clip_y / clip_w * 0.5f) : -1.0f;
            Log("DLSS5  car %d vs=%08lX draws=%d at %.1f %.1f %.1f screen %.3f %.3f matched %d (%.3f m from the expected place)%s, delta t %.3f %.3f %.3f", k,
                static_cast<unsigned long>(o.shader), o.draws, w[3], w[7], w[11], sx, sy, match[k], match_distance[k],
                o.moving ? " moving" : "", o.moving ? o.delta[3] : 0.0, o.moving ? o.delta[7] : 0.0, o.moving ? o.delta[11] : 0.0);
            // The camera in the car's own frame, this frame and last: equal when the camera rides with the car.
            double camera[3] = {}, direction[3] = {}, before[3] = {}, inverse[12] = {}, inverse_prev[12] = {}, scale = 0.0;
            if (match[k] != 0 && std::fabs(sx - 0.5f) < 2.0f && D5CameraOf(g_d5_vp_current, camera, direction, &scale) &&
                D5CameraOf(g_d5_vp_previous, before, direction, &scale) && D5InvertAffine(w, inverse) &&
                D5InvertAffine(g_d5_objects_prev[match[k]].world, inverse_prev)) {
                double local[3] = {}, local_prev[3] = {};
                for (int row = 0; row < 3; ++row) {
                    local[row] = inverse[row * 4] * camera[0] + inverse[row * 4 + 1] * camera[1] + inverse[row * 4 + 2] * camera[2] + inverse[row * 4 + 3];
                    local_prev[row] = inverse_prev[row * 4] * before[0] + inverse_prev[row * 4 + 1] * before[1] + inverse_prev[row * 4 + 2] * before[2] +
                        inverse_prev[row * 4 + 3];
                }
                if (std::fabs(local[0]) + std::fabs(local[1]) + std::fabs(local[2]) < 12.0) {
                    Log("DLSS5    camera in car %d: %.4f %.4f %.4f, last frame %.4f %.4f %.4f", k, local[0], local[1], local[2], local_prev[0],
                        local_prev[1], local_prev[2]);
                }
            }
        }
    }
    std::memcpy(g_d5_objects_prev, g_d5_objects, sizeof(g_d5_objects));
    g_d5_object_count_prev = g_d5_object_count;
    g_d5_objects_frame = g_d5_frame_index;
    if (g_d5_frame_time_log) {
        g_d5_window_object_cpu_ms += D5NowMs() - started;
        g_d5_window_object_cars += g_d5_object_count;
        g_d5_window_object_moving += g_d5_moving_objects;
        ++g_d5_window_object_frames;
    }
}

void D5ResetObjectsFrame()
{
    g_d5_object_count = 0;
}

// ---------------------------------------------------------------------------------------
// Stencil tagging in the scene pass. The device's stencil states as last set (by the game or by
// us), so only differences are sent.

constexpr D3DRENDERSTATETYPE kD5StencilStates[8] = {D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK, D3DRS_STENCILPASS, D3DRS_STENCILZFAIL, D3DRS_STENCILFAIL};
DWORD g_d5_stencil_actual[8] = {FALSE, D3DCMP_ALWAYS, 0, 0xFFFFFFFF, 0xFFFFFFFF, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP};
bool g_d5_stencil_tagging = false;   // our states are on the device

// The game's own value of a stencil state (from the SetRenderState hook's shadow).
DWORD D5GameStencilState(int i)
{
    return i == 0 ? g_d5.stencil : g_d5.stencil_state[i - 1];
}

// IDirect3DDevice9::Reset puts every render state back to its default.
void D5ResetStencilShadows()
{
    const DWORD defaults[8] = {D3DCMP_ALWAYS, 0, 0xFFFFFFFF, 0xFFFFFFFF, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, FALSE};
    std::memcpy(g_d5.stencil_state, defaults, sizeof(defaults));
    g_d5.stencil = FALSE;
    const DWORD actual[8] = {FALSE, D3DCMP_ALWAYS, 0, 0xFFFFFFFF, 0xFFFFFFFF, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP};
    std::memcpy(g_d5_stencil_actual, actual, sizeof(actual));
    g_d5_stencil_tagging = false;
    g_d5_object_count = 0;
    g_d5_object_count_prev = 0;
    g_d5_objects_frame = -1;
}

void D5ApplyStencil(IDirect3DDevice9* device, const DWORD* want)
{
    for (int i = 0; i < 8; ++i) {
        if (g_d5_stencil_actual[i] != want[i]) {
            g_d5_set_render_state(device, kD5StencilStates[i], want[i]);
            g_d5_stencil_actual[i] = want[i];
        }
    }
}

// The game set a stencil state itself: the device now has its value.
void D5NoteGameStencilState(D3DRENDERSTATETYPE state, DWORD value)
{
    for (int i = 0; i < 8; ++i) {
        if (kD5StencilStates[i] == state) {
            g_d5_stencil_actual[i] = value;
        }
    }
}

// Before a scene draw.
void D5TagSceneDraw(IDirect3DDevice9* device)
{
    const double started = g_d5_frame_time_log ? D5NowMs() : 0.0;
    DWORD want[8] = {};
    if (g_d5.stencil != 0) {
        // The game's own stencil draw: its states, tested and written on bit 0 only. When it
        // writes depth it also clears the car tag (bits 1-7) under it, like any depth-writing
        // draw: a pass op of KEEP becomes REPLACE with the reference's bit 0 and bit 0 masked
        // off the writes, a pass op of REPLACE writes all bits. Only with KEEP on fail and
        // z-fail, so nothing outside the drawn pixels changes.
        for (int i = 0; i < 8; ++i) {
            want[i] = D5GameStencilState(i);
        }
        want[2] &= 0x01;
        want[3] &= 0x01;
        const bool keeps_failures = want[6] == D3DSTENCILOP_KEEP && want[7] == D3DSTENCILOP_KEEP;
        if (g_d5.zwrite != 0 && keeps_failures && want[5] == D3DSTENCILOP_KEEP) {
            want[5] = D3DSTENCILOP_REPLACE;
            want[4] = 0xFE;
        } else if (g_d5.zwrite != 0 && keeps_failures && want[5] == D3DSTENCILOP_REPLACE) {
            want[4] = 0xFF;
        } else {
            want[4] &= 0x01;
        }
    } else {
        int k = 0;
        const int reg = D5VehicleObjectRegister(g_d5.vs);
        if (reg >= 0 && reg + 2 < 256) {
            float world[12] = {};
            for (int row = 0; row < 3; ++row) {
                std::memcpy(&world[row * 4], g_d5.vs_c[reg + row].v, sizeof(float) * 4);
            }
            k = D5ObjectIndex(world);
        }
        want[0] = TRUE;
        want[1] = D3DCMP_ALWAYS;
        want[2] = static_cast<DWORD>(k) << 1;
        want[3] = 0xFF;
        want[4] = 0xFE;
        want[5] = g_d5.zwrite != 0 ? D3DSTENCILOP_REPLACE : D3DSTENCILOP_KEEP;
        want[6] = D3DSTENCILOP_KEEP;
        want[7] = D3DSTENCILOP_KEEP;
    }
    D5ApplyStencil(device, want);
    g_d5_stencil_tagging = true;
    if (g_d5_frame_time_log) {
        g_d5_window_object_cpu_ms += D5NowMs() - started;
    }
}

// After the scene pass: the game's stencil states back.
void D5EndSceneTagging(IDirect3DDevice9* device)
{
    if (!g_d5_stencil_tagging) {
        return;
    }
    DWORD want[8] = {};
    for (int i = 0; i < 8; ++i) {
        want[i] = D5GameStencilState(i);
    }
    D5ApplyStencil(device, want);
    g_d5_stencil_tagging = false;
}
