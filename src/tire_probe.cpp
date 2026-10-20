// Steps R2/R2b: logging-only D3D11 hooks for tire textures. See tire_probe.h.
//
// Hot paths (PSSetShaderResources, DrawIndexed*, Map/Unmap, UpdateSubresource)
// run thousands of times per frame, so they only touch lock-free tables:
// insert-only open-addressing pointer maps (texture candidates, tire views)
// and a small per-context table of which pixel shader slots hold a tire view.
// Hashing, logging and the capture only run on the rare paths.

#include "tire_probe.h"

#include "player_tracker.h"
#include "wear_transform.h"

#include <windows.h>
#include <d3d11_1.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace catalog_data {
// catalog/<game>/tire_textures.csv and tire_regions.csv, zero-terminated.
extern const unsigned char kEts2TireTextures[];
extern const unsigned char kEts2TireRegions[];
extern const unsigned char kAtsTireTextures[];
extern const unsigned char kAtsTireRegions[];
}  // namespace catalog_data

namespace tire_probe {

const char* CtxFnName(int fn) {
    static const char* names[kCtxFnCount] = {"PSSetShaderResources", "DrawIndexed", "DrawIndexedInstanced",
                                             "Map", "Unmap", "CopySubresourceRegion", "CopyResource",
                                             "UpdateSubresource", "UpdateSubresource1"};
    return names[fn];
}

int CtxFnSlot(int fn) {
    static const int slots[kCtxFnCount] = {8, 12, 20, 14, 15, 46, 47, 48, 116};
    return slots[fn];
}

namespace {

// ---- catalog ----

struct CatalogEntry {
    std::string name;  // source:path
    uint32_t width = 0, format = 0;
};
std::vector<CatalogEntry> g_catalog;                         // id = index + 1
std::unordered_map<uint64_t, std::pair<int, int>> g_hashes;  // hash -> (id, mip)

void (*g_log)(const std::string&) = nullptr;
std::wstring g_capture_path;
std::atomic<bool> g_active{false};

void Log(const std::string& s) {
    if (g_log) g_log("tires: " + s);
}

constexpr uint64_t kFnvSeed = 1469598103934665603ull;
uint64_t Fnv1a(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t BlockBytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default:
            return 0;
    }
}

std::string Ptr(const void* p) {
    char b[24];
    snprintf(b, sizeof(b), "%p", p);
    return b;
}

// ---- lock-free insert-only pointer -> int map ----

template <size_t kSlots>  // power of two
class PtrMap {
public:
    void Set(void* key, int value) {
        size_t i = Hash(key);
        for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            void* cur = keys_[i].load(std::memory_order_acquire);
            if (cur == key) {
                values_[i].store(value, std::memory_order_release);
                return;
            }
            if (!cur) {
                values_[i].store(value, std::memory_order_relaxed);
                void* expected = nullptr;
                if (keys_[i].compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                    count_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (expected == key) {
                    values_[i].store(value, std::memory_order_release);
                    return;
                }
            }
        }
        full_.store(true, std::memory_order_relaxed);
    }
    int Get(const void* key) const {
        if (!key) return 0;
        size_t i = Hash(key);
        for (size_t n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            void* cur = keys_[i].load(std::memory_order_acquire);
            if (cur == key) return values_[i].load(std::memory_order_relaxed);
            if (!cur) return 0;
        }
        return 0;
    }
    int Count() const { return count_.load(); }
    bool Full() const { return full_.load(); }

private:
    static size_t Hash(const void* p) {
        return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull) & (kSlots - 1);
    }
    std::atomic<void*> keys_[kSlots] = {};
    std::atomic<int> values_[kSlots] = {};
    std::atomic<int> count_{0};
    std::atomic<bool> full_{false};
};

// Texture candidates: value = 1 + index into g_cands (0 = not a candidate).
PtrMap<16384> g_cand_map;
// Tire views: value = catalog id (0 = not a tire view).
PtrMap<8192> g_srv_map;

struct Candidate {
    ID3D11Texture2D* tex;
    uint32_t width, height, mips;
    DXGI_FORMAT format;
    D3D11_USAGE usage;
    int tire_id = 0;
    std::vector<void*> srvs;
};

// Rare-path state, all under g_mutex.
std::mutex g_mutex;
std::vector<Candidate> g_cands;
std::map<std::pair<void*, UINT>, std::pair<void*, UINT>> g_open_maps;  // (res, sub) -> (pData, RowPitch)
std::map<std::pair<void*, UINT>, int> g_staging_ids;                   // (res, sub) -> catalog id (0 = unknown)
int g_detail_logged[kCtxFnCount] = {};
int g_unmatched_logged = 0;

std::atomic<int> g_cand_default{0}, g_cand_staging{0}, g_cand_other{0}, g_cand_with_data{0};
std::atomic<int> g_recognised{0}, g_unmatched{0}, g_partial_updates{0};
std::atomic<int> g_path_count[kCtxFnCount] = {};  // uploads touching a candidate, per function

int Recognise(Candidate& c, int id, const char* how) {
    if (c.tire_id == id) return id;
    c.tire_id = id;
    for (void* srv : c.srvs) g_srv_map.Set(srv, id);
    const int n = g_recognised.fetch_add(1) + 1;
    if (n <= 60)
        Log("texture recognised via " + std::string(how) + ": " + g_catalog[id - 1].name + " (" +
            std::to_string(c.width) + "px, " + std::to_string(c.srvs.size()) + " view(s) so far)");
    return id;
}

// Hash one mip level given as rows of BC blocks; returns the catalog id or 0.
int MatchRows(const Candidate& c, UINT mip, const void* data, UINT row_pitch) {
    const uint32_t bb = BlockBytes(c.format);
    const uint32_t w = (c.width >> mip) ? (c.width >> mip) : 1, h = (c.height >> mip) ? (c.height >> mip) : 1;
    if (w < 16) return 0;  // tiny mips are too generic to identify a texture
    const uint32_t row_bytes = (w + 3) / 4 * bb, rows = (h + 3) / 4;
    if (row_pitch < row_bytes) return 0;
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t hash = kFnvSeed;
    for (uint32_t r = 0; r < rows; ++r) hash = Fnv1a(hash, p + size_t(r) * row_pitch, row_bytes);
    auto it = g_hashes.find(hash);
    if (it != g_hashes.end()) return it->second.first;
    g_unmatched.fetch_add(1);
    if (g_unmatched_logged < 10 && c.usage != D3D11_USAGE_STAGING) {
        ++g_unmatched_logged;
        char b[160];
        snprintf(b, sizeof(b), "data not in catalog: %ux%u mip %u format %d hash %016llx", c.width, c.height, mip,
                 c.format, static_cast<unsigned long long>(hash));
        Log(b);
    }
    return 0;
}

Candidate* FindCand(const void* res) {
    const int v = g_cand_map.Get(res);
    return v ? &g_cands[v - 1] : nullptr;
}

// The address may have been reused by another resource (a buffer, another
// texture) since the candidate was recorded: confirm with D3D before any data
// is read with the candidate's dimensions.
bool StillCandidate(ID3D11Resource* res, const Candidate& c) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return false;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    tex->Release();
    return d.Width == c.width && d.Height == c.height && d.Format == c.format && (d.MipLevels ? d.MipLevels : 1) == c.mips;
}

void LogDetail(int fn, const std::string& text) {
    if (g_detail_logged[fn] < 5) {
        ++g_detail_logged[fn];
        Log(std::string(CtxFnName(fn)) + ": " + text);
    }
}

// Original device functions (trampolines filled by MinHook).
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
CreateTexture2DFn g_orig_create_tex = nullptr;
CreateSrvFn g_orig_create_srv = nullptr;

// ---- R5: worn copies of the tire textures, one per wear level ----
//
// For every recognised tire texture that has a tread rectangle (catalog\
// tire_regions.csv) we keep one clone per wear level 1..n-1 (level 0 is the
// game's own texture) and fill each, mip by mip, from the very upload the game
// makes, through wear::Apply (the same code as tools\wearpreview). At draw
// time the clones of the current level replace the game's views for that one
// draw. tint_test keeps the R3 magenta test on the colour clones.

bool g_swap = false;
int g_levels = wear::kDefaultLevels;
bool g_tint = false;                // paint the colour of tires magenta (test only)
std::atomic<float> g_unit_wear[tracker::kMaxUnits];  // 0 = truck, 1 + i = trailer i
float g_wear_override = -1.f;       // >= 0 forces this wear on every tire (test)
const wear::Params g_params;

int LevelForWear(float w) {
    if (g_wear_override >= 0.f) w = g_wear_override;
    w = w < 0.f ? 0.f : w > 1.f ? 1.f : w;
    return static_cast<int>(w * (g_levels - 1) + 0.5f);
}

constexpr int kMaxLevels = 16;
struct Clone {
    ID3D11Texture2D* tex = nullptr;
    uint32_t width = 0, mips = 0;
    bool failed = false;
    std::atomic<ID3D11ShaderResourceView*> srv{nullptr};
    // R5d: filled on the render thread from the worker's results; the clone
    // is drawn only when something is in and nothing queued is missing.
    std::atomic<int> pending{0};
    std::atomic<bool> filled{false};
    bool Ready() const { return filled.load(std::memory_order_acquire) && pending.load(std::memory_order_acquire) == 0; }
};
struct TireInfo {
    bool has_rect = false;
    wear::Rect rect;
    bool trailer = false;  // path under /trailer_owned/ (until R4 tells the units apart)
    Clone level[kMaxLevels];
};
std::unique_ptr<TireInfo[]> g_tires;  // index = catalog id
std::atomic<int> g_clones_made{0};
std::atomic<uint64_t> g_swapped_draws{0};

bool IsNormalFormat(uint32_t f) { return wear::IsNormal(f); }
bool IsColorFormat(uint32_t f) { return wear::IsColor(f); }

// Interim rule (R5b, used when the R4b separation is off or has failed): the
// texture's folder picks the unit, /trailer_owned/ = trailer 0, else truck.
int InterimLevel(int id) {
    return LevelForWear(g_unit_wear[g_tires[id].trailer ? 1 : 0].load(std::memory_order_relaxed));
}

// ---- R4b: which unit a tire draw belongs to ----
//
// The wheel's camera-relative position is in VS cb0 rows 0-2, column 3
// [verified: R4a]. The game writes cb0 through Map/Unmap of a 2 MB dynamic
// ring buffer and binds 16 vec4 of it per draw (VSSetConstantBuffers1). We
// keep the pointer of the latest Map of that buffer and read the 48 bytes of
// the draw at draw time. Reading after Unmap is outside the D3D11 contract
// (works for dynamic buffers in practice), so the read is SEH-guarded and the
// matrix is sanity-checked; any exception, or no valid matrix at all, turns
// the separation off and the interim rule takes over.

bool g_separate = false;                 // INI: separate = 1
std::atomic<bool> g_sep_failed{false};
// Latest Map pointer of every constant buffer the game maps (the game rotates
// 6 of them [verified: R4b2]); written by the Map detour, render thread only.
constexpr int kWatchedBuffers = 64;
std::atomic<void*> g_cb_buffer[kWatchedBuffers];
std::atomic<const uint8_t*> g_cb_pointer[kWatchedBuffers];
int g_cb_next = 0;  // round-robin slot when the table is full
std::atomic<long long> g_sep_reads{0}, g_sep_invalid{0}, g_sep_nopointer{0};
std::atomic<uint64_t> g_player_draws{0}, g_traffic_draws{0};
constexpr int kUnknownUnit = -2;         // separation not available: use the interim rule

void SeparationFailed(const std::string& why) {
    if (!g_sep_failed.exchange(true)) Log("player separation off (" + why + "); using the texture-folder rule");
}

// Plain body for __try: copies 48 bytes, false on an access fault.
bool ReadRows(const uint8_t* src, float* rows12) {
    __try {
        memcpy(rows12, src, 48);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// R4b-diag: what ObserveDraw saw, for the capture.
struct ObserveInfo {
    const char* what = "not observed";  // "read", or why there is no position
    float pos[3] = {};
    const void* rtv = nullptr;
    const void* dsv = nullptr;
    float vp[4] = {};  // viewport x, y, w, h
    int run = 0;       // R4c2: which run of this viewport in the frame
    int obs_index = -1;
};

int g_run_ordinal = 0;  // R4c2: run of the current viewport (see NoteViewport)

// Unit of this tire draw (tracker index); -1 = not the player's, or not
// readable this time (the game's tire stays); kUnknownUnit only when the
// separation is off or has failed (the texture-folder rule applies).
int ObserveDraw(ID3D11DeviceContext* c, ObserveInfo* info) {
    if (!g_separate || g_sep_failed.load(std::memory_order_relaxed)) {
        info->what = "separation off";
        return kUnknownUnit;
    }
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) return kUnknownUnit;
    ID3D11Buffer* buf = nullptr;
    UINT first = 0, num = 0;
    c1->VSGetConstantBuffers1(0, 1, &buf, &first, &num);
    c1->Release();
    if (!buf) return kUnknownUnit;
    const uint8_t* base = nullptr;
    int slot = -1;
    for (int i = 0; i < kWatchedBuffers && slot < 0; ++i)
        if (g_cb_buffer[i].load(std::memory_order_acquire) == buf) slot = i;
    if (slot >= 0) base = g_cb_pointer[slot].load(std::memory_order_acquire);
    D3D11_BUFFER_DESC bd = {};
    buf->GetDesc(&bd);
    buf->Release();
    if (!base || first * 16 + 48 > bd.ByteWidth) {
        g_sep_nopointer.fetch_add(1, std::memory_order_relaxed);
        info->what = "no pointer yet";
        return -1;  // not readable yet: leave the game's tire this time
    }
    float r[12];
    if (!ReadRows(base + size_t(first) * 16, r)) {
        SeparationFailed("access fault reading the constant buffer");
        info->what = "access fault";
        return kUnknownUnit;
    }
    g_sep_reads.fetch_add(1, std::memory_order_relaxed);
    const float pos[3] = {r[3], r[7], r[11]};
    for (int i = 0; i < 3; ++i) info->pos[i] = pos[i];
    for (int row = 0; row < 3; ++row) {
        const float n = r[row * 4] * r[row * 4] + r[row * 4 + 1] * r[row * 4 + 1] + r[row * 4 + 2] * r[row * 4 + 2];
        if (!(n > 0.8f && n < 1.2f)) {  // not a rotation row: stale or foreign data
            const long long bad = g_sep_invalid.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bad > 500 && bad * 2 > g_sep_reads.load()) SeparationFailed("constant buffer data does not look like a matrix");
            info->what = "not a matrix";
            return -1;  // not a usable position: leave the game's tire
        }
    }
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    info->rtv = rtv;
    info->dsv = dsv;
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    // The camera is keyed by the viewport: the game rotates its render
    // targets every frame [verified: R4b-diag], the viewport stays.
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    if (nvp) info->vp[0] = vp.TopLeftX, info->vp[1] = vp.TopLeftY, info->vp[2] = vp.Width, info->vp[3] = vp.Height;
    info->run = g_run_ordinal;
    const void* camera = tracker::ViewportKey(info->vp, g_run_ordinal);
    info->what = "read";
    const float axle[3] = {r[0], r[4], r[8]};  // the wheel's local x (its axle) in the camera
    return tracker::Classify(camera, pos, &info->obs_index, info->vp[2] * info->vp[3], axle);
}

// R4c2: cameras that share a viewport (the two small mirrors: same size and
// render target [verified: R4c]) are told apart by the order of their runs
// in the frame. Every draw on the immediate context checks the viewport; a
// change starts a run, numbered per viewport (render thread only).
struct ViewportRuns {
    float vp[4];
    int count;
};
ViewportRuns g_runs[32];
int g_run_n = 0;
float g_run_vp[4] = {-1, -1, -1, -1};

void NoteViewport(ID3D11DeviceContext* c) {
    D3D11_VIEWPORT vp = {};
    UINT n = 1;
    c->RSGetViewports(&n, &vp);
    const float v[4] = {vp.TopLeftX, vp.TopLeftY, n ? vp.Width : 0.f, n ? vp.Height : 0.f};
    if (!memcmp(v, g_run_vp, sizeof(v))) return;
    memcpy(g_run_vp, v, sizeof(v));
    for (int i = 0; i < g_run_n; ++i)
        if (!memcmp(g_runs[i].vp, v, sizeof(v))) {
            g_run_ordinal = g_runs[i].count++;
            return;
        }
    g_run_ordinal = 0;
    if (g_run_n < 32) {
        memcpy(g_runs[g_run_n].vp, v, sizeof(v));
        g_runs[g_run_n++].count = 1;
    }
}

// Once per frame: the largest viewport of the frame is the main camera.
void ResetViewportRuns() {
    float largest = 0;
    for (int i = 0; i < g_run_n; ++i) largest = (std::max)(largest, g_runs[i].vp[2] * g_runs[i].vp[3]);
    if (largest > 0) tracker::SetMainViewportArea(largest);
    g_run_n = 0;
    g_run_ordinal = 0;
    for (float& f : g_run_vp) f = -1;
}

// Called from the Map detour for every Map: remembers the pointer of each
// constant buffer's latest Map.
inline void NoteMap(ID3D11Resource* res, const D3D11_MAPPED_SUBRESOURCE* m) {
    const auto* p = static_cast<const uint8_t*>(m->pData);
    for (int i = 0; i < kWatchedBuffers; ++i)
        if (g_cb_buffer[i].load(std::memory_order_relaxed) == res) {
            g_cb_pointer[i].store(p, std::memory_order_release);
            return;
        }
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC bd = {};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
    // Only the big ring buffers (2 MB [verified: R4a]): small per-draw
    // buffers would push them out of the table.
    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || bd.ByteWidth < 65536) return;
    int slot = -1;
    for (int i = 0; i < kWatchedBuffers && slot < 0; ++i)
        if (!g_cb_buffer[i].load(std::memory_order_relaxed)) slot = i;
    if (slot < 0) slot = g_cb_next++ % kWatchedBuffers;
    g_cb_pointer[slot].store(nullptr, std::memory_order_release);
    g_cb_buffer[slot].store(res, std::memory_order_release);
    g_cb_pointer[slot].store(p, std::memory_order_release);
}

// Creates the clone for (id, level) with the source texture's layout.
bool MakeClone(ID3D11Resource* dst, int id, int level) {
    Clone& cl = g_tires[id].level[level];
    ID3D11Texture2D* src = nullptr;
    if (FAILED(dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src)))) return false;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    src->GetDevice(&dev);
    src->Release();
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = g_orig_create_tex(dev, &d, nullptr, &tex);
    if (SUCCEEDED(hr)) hr = g_orig_create_srv(dev, tex, nullptr, &srv);
    dev->Release();
    if (FAILED(hr)) {
        if (tex) tex->Release();
        cl.failed = true;
        Log("clone failed for " + g_catalog[id - 1].name + " level " + std::to_string(level));
        return false;
    }
    cl.tex = tex;  // kept for the life of the process (DLL is pinned)
    cl.width = d.Width;
    cl.mips = d.MipLevels;
    cl.srv.store(srv, std::memory_order_release);
    g_clones_made.fetch_add(1);
    return true;
}

// R5d: the per-pixel transform takes up to ~0.7 s per 1024 px texture pair
// for the 7 levels [verified: wearpreview bench, 2026-10-20], too long to run
// inside the game's upload call. The copies go to a worker thread; its
// results are uploaded into the clones on the render thread at Present
// (UploadWorn), and a clone is drawn only once all that was queued for it is
// in (Clone::Ready). Until then the tire keeps the game's texture.
struct WearJob {
    Clone* cl = nullptr;
    UINT sub = 0;
    bool has_box = false;
    D3D11_BOX box{};
    uint32_t format = 0, w = 0, h = 0;
    wear::Rect rect;
    float wear = 0.f;
    bool tint = false;
    std::vector<uint8_t> data;
    UINT pitch = 0;
};
std::mutex g_job_mutex;
std::condition_variable g_job_cv;
std::deque<WearJob> g_jobs, g_done;
std::atomic<int> g_done_count{0};
bool g_worker_started = false;

void WearWorker() {
    for (;;) {
        WearJob j;
        {
            std::unique_lock<std::mutex> lock(g_job_mutex);
            g_job_cv.wait(lock, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        wear::Apply(j.format, j.data.data(), j.w, j.h, j.rect, j.wear, g_params);
        if (j.tint) {
            static const uint8_t magenta[8] = {0x1F, 0xF8, 0x1F, 0xF8, 0, 0, 0, 0};
            for (size_t o = 0; o + 16 <= j.data.size(); o += 16) memcpy(j.data.data() + o + 8, magenta, 8);
        }
        std::lock_guard<std::mutex> lock(g_job_mutex);
        g_done.push_back(std::move(j));
        g_done_count.fetch_add(1, std::memory_order_release);
    }
}

void QueueJob(WearJob&& j) {
    j.cl->pending.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(g_job_mutex);
    if (!g_worker_started) {
        std::thread(WearWorker).detach();  // lives as long as the process (DLL is pinned)
        g_worker_started = true;
    }
    g_jobs.push_back(std::move(j));
    g_job_cv.notify_one();
}

// Called under g_mutex for an upload of a whole mip to a recognised texture:
// queues the transform of that mip for every level's clone.
void PrepareMirrors(ID3D11Resource* dst, const Candidate& c, UINT sub, const D3D11_BOX* box, const void* data,
                    UINT row_pitch) {
    const int id = c.tire_id;
    TireInfo& ti = g_tires[id];
    const uint32_t f = g_catalog[id - 1].format;
    if (!g_swap || !ti.has_rect || (!IsNormalFormat(f) && !IsColorFormat(f))) return;
    const UINT mip = sub % c.mips;
    const uint32_t bb = BlockBytes(c.format);
    const uint32_t w = (c.width >> mip) ? (c.width >> mip) : 1, h = (c.height >> mip) ? (c.height >> mip) : 1;
    const uint32_t row_bytes = (w + 3) / 4 * bb, rows = (h + 3) / 4;
    if (row_pitch < row_bytes) return;
    bool made_any = false;
    for (int level = 1; level < g_levels; ++level) {
        Clone& cl = ti.level[level];
        if (cl.failed) continue;
        if (!cl.tex) {
            if (!MakeClone(dst, id, level)) continue;
            made_any = true;
        }
        if (cl.width != c.width || cl.mips != c.mips) continue;  // another size of the same texture: leave it
        WearJob j;
        j.cl = &cl, j.sub = sub, j.has_box = box != nullptr;
        if (box) j.box = *box;
        j.format = c.format, j.w = w, j.h = h, j.rect = ti.rect, j.wear = wear::LevelWear(level, g_levels);
        j.tint = g_tint && IsColorFormat(f);
        j.data.resize(size_t(row_bytes) * rows), j.pitch = row_bytes;
        const auto* p = static_cast<const uint8_t*>(data);
        for (uint32_t r = 0; r < rows; ++r)
            memcpy(j.data.data() + size_t(r) * row_bytes, p + size_t(r) * row_pitch, row_bytes);
        QueueJob(std::move(j));
    }
    if (made_any)
        Log("wear clones made for " + g_catalog[id - 1].name + " (levels 1-" + std::to_string(g_levels - 1) +
            (ti.trailer ? ", trailer wear" : ", truck wear") + ")");
}

// ---- per-context pixel shader slots (hot path) ----

constexpr int kTrackedSlots = 16;
constexpr int kMaxContexts = 64;
struct CtxState {
    std::atomic<void*> ctx{nullptr};
    uint16_t slot_id[kTrackedSlots] = {};
    uint32_t tire_mask = 0;
    bool deferred = false;
};
CtxState g_ctx[kMaxContexts];
std::atomic<int> g_ctx_imm{0}, g_ctx_def{0}, g_ctx_overflow{0};

CtxState* FindCtx(ID3D11DeviceContext* c, bool create) {
    for (auto& s : g_ctx) {
        void* cur = s.ctx.load(std::memory_order_acquire);
        if (cur == c) return &s;
        if (!cur) break;  // slots fill in order
    }
    if (!create) return nullptr;
    for (auto& s : g_ctx) {
        void* expected = nullptr;
        if (s.ctx.compare_exchange_strong(expected, c, std::memory_order_acq_rel)) {
            s.deferred = c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
            (s.deferred ? g_ctx_def : g_ctx_imm).fetch_add(1);
            return &s;
        }
        if (expected == c) return &s;
    }
    g_ctx_overflow.fetch_add(1);
    return nullptr;
}

// ---- stats / capture ----

std::atomic<uint64_t> g_draws[2] = {}, g_tire_draws{0}, g_frames{0};
std::atomic<int> g_capture_state{0};  // 0 idle, 1 requested, 2 capturing
std::mutex g_capture_mutex;
int g_capture_count = 0;
int g_capture_frames = 1;              // R4b-diag: consecutive frames per capture
std::atomic<int> g_capture_frame{0};   // frame being captured (1-based)

// R4a: the part of a VS constant buffer one tire draw uses, copied by the GPU
// into a staging buffer at draw time and read after the frame.
struct CbCopy {
    UINT slot, first, num, bytes;
    ID3D11Buffer* staging;
};
struct CapturedDraw {
    std::string line;
    std::vector<CbCopy> cbs;
};
std::vector<CapturedDraw> g_capture;
std::string g_capture_telemetry;
uint64_t g_capture_draws = 0;
constexpr UINT kMaxCbBytes = 4096;

constexpr UINT kColorSlot = 6, kNormalSlot = 14;  // [verified: R2c capture] tire draws use t6 colour, t14 normal

void CaptureDraw(ID3D11DeviceContext* c, const CtxState& s, UINT index_count, UINT instances, UINT start_index,
                 INT base_vertex, int unit, const ObserveInfo& info) {
    CapturedDraw cd;
    std::ostringstream line;
    const bool trace = g_capture_frames > 1;
    if (trace) line << "f" << g_capture_frame.load(std::memory_order_relaxed) << " ";
    line << (s.deferred ? "def " : "imm ") << c << " idx " << index_count << " start " << start_index << " base "
         << base_vertex << " inst " << instances << " |";
    for (int i = 0; i < kTrackedSlots; ++i)
        if (s.tire_mask & (1u << i)) line << " t" << i << "=" << g_catalog[s.slot_id[i] - 1].name;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    line << " | vs " << vs << " ps " << ps;
    if (vs) vs->Release();
    if (ps) ps->Release();

    // Constant buffers with their offsets (ID3D11DeviceContext1); the data is
    // only copied for real tire draws (colour in slot 6).
    const bool real_tire = (s.tire_mask & (1u << kColorSlot)) != 0;
    ID3D11DeviceContext1* c1 = nullptr;
    ID3D11Buffer* cbs[4] = {};
    UINT first[4] = {}, num[4] = {};
    if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1)))) {
        c1->VSGetConstantBuffers1(0, 4, cbs, first, num);
        c1->Release();
    } else {
        c->VSGetConstantBuffers(0, 4, cbs);
    }
    line << " | vs_cb";
    ID3D11Device* dev = nullptr;
    if (real_tire) c->GetDevice(&dev);
    for (UINT i = 0; i < 4; ++i) {
        if (!cbs[i]) continue;
        D3D11_BUFFER_DESC d = {};
        cbs[i]->GetDesc(&d);
        line << " " << i << ":" << cbs[i] << "/" << d.ByteWidth << (d.Usage == D3D11_USAGE_DYNAMIC ? "dyn" : "")
             << " first " << first[i] << " num " << num[i];
        if (dev && (!trace || i == 0)) {  // trace: only cb0 rows 0-2 (the wheel matrix)
            const UINT begin = first[i] * 16;
            UINT bytes = num[i] ? num[i] * 16 : d.ByteWidth;
            if (begin < d.ByteWidth) {
                bytes = (std::min)((std::min)(bytes, d.ByteWidth - begin), trace ? 48u : kMaxCbBytes);
                D3D11_BUFFER_DESC sd = {};
                sd.ByteWidth = (bytes + 15) & ~15u;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Buffer* staging = nullptr;
                if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging))) {
                    const D3D11_BOX box{begin, 0, 0, begin + bytes, 1, 1};
                    c->CopySubresourceRegion(staging, 0, 0, 0, 0, cbs[i], 0, &box);
                    cd.cbs.push_back({i, first[i], num[i], bytes, staging});
                }
            }
        }
        cbs[i]->Release();
    }
    if (dev) dev->Release();
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    line << " | vb0 " << vb << " stride " << stride << " off " << offset;
    if (vb) vb->Release();
    if (real_tire) {
        // R4b-diag: what the separation saw (CPU read of cb0) and decided.
        char b[256];
        snprintf(b, sizeof(b), " | sep %s cpu (%.3f %.3f %.3f) rtv %p dsv %p vp %.0f,%.0f %.0fx%.0f run %d obs #%d unit %d",
                 info.what, info.pos[0], info.pos[1], info.pos[2], info.rtv, info.dsv, info.vp[0], info.vp[1],
                 info.vp[2], info.vp[3], info.run, info.obs_index, unit);
        line << b;
    }
    cd.line = line.str();
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    if (g_capture.size() < 4000) g_capture.push_back(std::move(cd));
    else
        for (auto& cb : cd.cbs) cb.staging->Release();
}

// Reads the staging copies back (after the frame) and writes the capture file.
void WriteCapture(ID3D11DeviceContext* ctx) {
    std::vector<CapturedDraw> draws;
    uint64_t total;
    std::string telemetry;
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        draws.swap(g_capture);
        total = g_capture_draws;
        telemetry.swap(g_capture_telemetry);
    }
    ++g_capture_count;
    std::wstring path = g_capture_path;
    const size_t dot = path.rfind(L".txt");
    if (dot != std::wstring::npos) path.insert(dot, L"_" + std::to_wstring(g_capture_count));
    FILE* f = _wfopen(path.c_str(), L"wb");
    int real = 0;
    if (f) {
        fprintf(f, "# %d frame(s): %llu indexed draws, %zu with a tire texture bound in PS slots 0-15\n",
                g_capture_frames, static_cast<unsigned long long>(total), draws.size());
        fputs(telemetry.c_str(), f);
        fputs("# draws (constant buffer data for draws with the tire colour in t6; vec4 rows as floats)\n", f);
    }
    for (auto& d : draws) {
        if (f) fprintf(f, "%s\n", d.line.c_str());
        if (!d.cbs.empty()) ++real;
        for (auto& cb : d.cbs) {
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (f && ctx && SUCCEEDED(ctx->Map(cb.staging, 0, D3D11_MAP_READ, 0, &m))) {
                fprintf(f, "  cb%u first %u num %u bytes %u\n", cb.slot, cb.first, cb.num, cb.bytes);
                const float* v = static_cast<const float*>(m.pData);
                for (UINT k = 0; k + 4 <= cb.bytes / 4; k += 4)
                    fprintf(f, "    [%3u] %14.6g %14.6g %14.6g %14.6g\n", k / 4, v[k], v[k + 1], v[k + 2], v[k + 3]);
                ctx->Unmap(cb.staging, 0);
            }
            cb.staging->Release();
        }
    }
    if (f) fclose(f);
    Log("capture " + std::to_string(g_capture_count) + " written: " + std::to_string(draws.size()) +
        " tire draws (" + std::to_string(real) + " with constant buffer data) of " + std::to_string(total) +
        " indexed draws");
}

// ---- device detours ----

bool IsCandidateShape(const D3D11_TEXTURE2D_DESC* d) {
    return BlockBytes(d->Format) && d->ArraySize == 1 && d->Width == d->Height &&
           (d->Width == 256 || d->Width == 512 || d->Width == 1024);
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d,
                                              const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    const HRESULT hr = g_orig_create_tex(dev, d, init, out);
    if (FAILED(hr) || !out || !*out || !d || !g_active.load(std::memory_order_relaxed)) return hr;
    if (!IsCandidateShape(d)) {
        if (g_cand_map.Get(*out)) g_cand_map.Set(*out, 0);  // address reused by another texture
        return hr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cands.size() >= g_cands.capacity()) {
        // Growing would move the entries other threads point at: stop adding.
        if (g_cand_map.Get(*out)) g_cand_map.Set(*out, 0);
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("candidate table full; newer textures are not tracked");
        }
        return hr;
    }
    Candidate c{*out, d->Width, d->Height, d->MipLevels ? d->MipLevels : 1, d->Format, d->Usage};
    (d->Usage == D3D11_USAGE_DEFAULT ? g_cand_default : d->Usage == D3D11_USAGE_STAGING ? g_cand_staging
                                                                                         : g_cand_other)
        .fetch_add(1);
    g_cands.push_back(std::move(c));
    g_cand_map.Set(*out, static_cast<int>(g_cands.size()));
    Candidate& cand = g_cands.back();
    if (init && init->pSysMem) {
        g_cand_with_data.fetch_add(1);
        if (int id = MatchRows(cand, 0, init->pSysMem, init->SysMemPitch)) Recognise(cand, id, "CreateTexture2D");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* dev, ID3D11Resource* res,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** out) {
    const HRESULT hr = g_orig_create_srv(dev, res, d, out);
    if (FAILED(hr) || !out || !*out || !g_active.load(std::memory_order_relaxed)) return hr;
    if (Candidate* c = FindCand(res)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        c->srvs.push_back(*out);
        g_srv_map.Set(*out, c->tire_id);
    } else if (g_srv_map.Get(*out)) {
        g_srv_map.Set(*out, 0);  // view address reused for something else
    }
    return hr;
}

// ---- context detours, one instantiation per [immediate, deferred] ----

using PSSetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                         D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using CopyRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                              ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                          const void*, UINT, UINT);
using Update1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                           const void*, UINT, UINT, UINT);

void OnPSSetSrv(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* views) {
    if (start >= kTrackedSlots) return;
    CtxState* s = FindCtx(c, true);
    if (!s) return;
    for (UINT i = 0; i < n && start + i < kTrackedSlots; ++i) {
        const int id = views ? g_srv_map.Get(views[i]) : 0;
        s->slot_id[start + i] = static_cast<uint16_t>(id);
        if (id) s->tire_mask |= 1u << (start + i);
        else s->tire_mask &= ~(1u << (start + i));
    }
}

// Views to put in place for one draw (and the game's, to put back).
struct Swap {
    int n = 0;
    UINT slot[2] = {};
    ID3D11ShaderResourceView* repl[2] = {};
    ID3D11ShaderResourceView* orig[2] = {};  // AddRef'd while swapped
};

// A real tire draw has the tire colour in slot 6 (slot 14 alone is a binding
// left over from an earlier draw). Bindings are read back from D3D because
// the runtime can unbind views behind our tracking (e.g. on OMSetRenderTargets).
void PlanSwap(ID3D11DeviceContext* c, int unit, Swap* sw) {
    ID3D11ShaderResourceView* bound[2] = {};
    c->PSGetShaderResources(kColorSlot, 1, &bound[0]);
    c->PSGetShaderResources(kNormalSlot, 1, &bound[1]);
    const int color_id = g_srv_map.Get(bound[0]);
    // Level from the unit the draw belongs to (R4b); traffic (-1) keeps the
    // game's tire; without separation the texture folder picks the unit.
    // The normal map of the same draw follows the colour.
    int level = 0;
    if (color_id && IsColorFormat(g_catalog[color_id - 1].format)) {
        if (unit == kUnknownUnit) level = InterimLevel(color_id);
        else if (unit >= 0) level = LevelForWear(g_unit_wear[unit].load(std::memory_order_relaxed));
    }
    if (level > 0) {
        const UINT slots[2] = {kColorSlot, kNormalSlot};
        for (int i = 0; i < 2; ++i) {
            const int id = i == 0 ? color_id : g_srv_map.Get(bound[1]);
            const Clone* cl = id ? &g_tires[id].level[level] : nullptr;
            ID3D11ShaderResourceView* r = cl && cl->Ready() ? cl->srv.load(std::memory_order_acquire) : nullptr;
            if (!r || !bound[i]) continue;
            sw->slot[sw->n] = slots[i];
            sw->repl[sw->n] = r;
            sw->orig[sw->n] = bound[i];  // keeps the reference from PSGetShaderResources
            bound[i] = nullptr;
            ++sw->n;
        }
    }
    for (auto* b : bound)
        if (b) b->Release();
}

void OnDraw(int v, ID3D11DeviceContext* c, UINT index_count, UINT instances, UINT start_index, INT base_vertex,
            Swap* sw) {
    g_draws[v].fetch_add(1, std::memory_order_relaxed);
    if (v == 0 && g_separate && !g_sep_failed.load(std::memory_order_relaxed)) NoteViewport(c);
    const int cap = g_capture_state.load(std::memory_order_relaxed);
    if (cap == 2) {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        ++g_capture_draws;
    }
    CtxState* s = FindCtx(c, false);
    if (!s || !s->tire_mask) return;
    g_tire_draws.fetch_add(1, std::memory_order_relaxed);
    ObserveInfo info;
    if (!(s->tire_mask & (1u << kColorSlot))) {  // not a real tire draw (left-over normal binding)
        if (cap == 2) CaptureDraw(c, *s, index_count, instances, start_index, base_vertex, -1, info);
        return;
    }
    // Observe every real tire draw, swapped or not, so the tracks stay current.
    const int unit = ObserveDraw(c, &info);
    if (cap == 2) CaptureDraw(c, *s, index_count, instances, start_index, base_vertex, unit, info);
    if (unit >= 0) g_player_draws.fetch_add(1, std::memory_order_relaxed);
    else if (unit == -1) g_traffic_draws.fetch_add(1, std::memory_order_relaxed);
    if (g_swap) PlanSwap(c, unit, sw);
}

// Draws with the swapped views, then puts the game's views back.
template <typename DrawCall>
void DrawSwapped(ID3D11DeviceContext* c, const Swap& sw, PSSetSrvFn set, DrawCall draw) {
    for (int i = 0; i < sw.n; ++i) set(c, sw.slot[i], 1, &sw.repl[i]);
    draw();
    for (int i = 0; i < sw.n; ++i) {
        set(c, sw.slot[i], 1, &sw.orig[i]);
        sw.orig[i]->Release();
    }
    g_swapped_draws.fetch_add(1, std::memory_order_relaxed);
}

void OnUpdate(int fn, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT row_pitch) {
    Candidate* c = FindCand(dst);
    if (!c || !data || !StillCandidate(dst, *c)) return;
    g_path_count[fn].fetch_add(1);
    std::lock_guard<std::mutex> lock(g_mutex);
    const UINT mip = sub % c->mips;
    const UINT w = (c->width >> mip) ? (c->width >> mip) : 1, h = (c->height >> mip) ? (c->height >> mip) : 1;
    // The game passes an explicit box even for whole mips; BC boxes may be
    // rounded up to the 4x4 block, so "covers the mip" means >= its size.
    const bool whole = !box || (box->left == 0 && box->top == 0 && box->right >= w && box->bottom >= h);
    std::string detail = "dst " + Ptr(dst) + " " + std::to_string(c->width) + "px mip " + std::to_string(mip) +
                         " pitch " + std::to_string(row_pitch);
    if (box)
        detail += " box " + std::to_string(box->left) + "," + std::to_string(box->top) + "-" +
                  std::to_string(box->right) + "," + std::to_string(box->bottom) + (whole ? " (whole mip)" : " (partial)");
    LogDetail(fn, detail);
    if (!whole) {
        g_partial_updates.fetch_add(1);
        return;
    }
    // Once recognised, later mips (including the tiny ones that cannot be
    // hashed) are not hashed again, only mirrored.
    if (!c->tire_id)
        if (int id = MatchRows(*c, mip, data, row_pitch)) Recognise(*c, id, CtxFnName(fn));
    if (c->tire_id) PrepareMirrors(dst, *c, sub, box, data, row_pitch);
}

void OnMapped(ID3D11Resource* res, UINT sub, const D3D11_MAPPED_SUBRESOURCE* m) {
    Candidate* c = FindCand(res);
    if (!c || !m || !m->pData || !StillCandidate(res, *c)) return;
    g_path_count[kMap].fetch_add(1);
    std::lock_guard<std::mutex> lock(g_mutex);
    LogDetail(kMap, "res " + Ptr(res) + " " + std::to_string(c->width) + "px usage " + std::to_string(c->usage) +
                        " sub " + std::to_string(sub));
    g_open_maps[{res, sub}] = {m->pData, m->RowPitch};
}

void OnUnmap(ID3D11Resource* res, UINT sub) {
    Candidate* c = FindCand(res);
    if (!c) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_open_maps.find({res, sub});
    if (it == g_open_maps.end()) return;
    const int id = MatchRows(*c, sub % c->mips, it->second.first, it->second.second);
    g_open_maps.erase(it);
    if (c->usage == D3D11_USAGE_STAGING) g_staging_ids[{res, sub}] = id;
    else if (id) Recognise(*c, id, "Map");
}

void OnCopy(int fn, ID3D11Resource* dst, UINT dst_sub, ID3D11Resource* src, UINT src_sub, bool whole) {
    Candidate* c = FindCand(dst);
    if (!c || !StillCandidate(dst, *c)) return;
    g_path_count[fn].fetch_add(1);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string src_text = "src " + Ptr(src);
    if (Candidate* s = FindCand(src)) src_text += " (candidate, usage " + std::to_string(s->usage) + ")";
    else {
        D3D11_RESOURCE_DIMENSION dim;
        src->GetType(&dim);
        src_text += " (not a candidate, dimension " + std::to_string(dim) + ")";
    }
    LogDetail(fn, "dst " + Ptr(dst) + " " + std::to_string(c->width) + "px sub " + std::to_string(dst_sub) + ", " +
                      src_text + " sub " + std::to_string(src_sub));
    if (whole) {
        for (auto& [key, id] : g_staging_ids)
            if (key.first == src && id) {
                Recognise(*c, id, CtxFnName(fn));
                return;
            }
    } else {
        auto it = g_staging_ids.find({src, src_sub});
        if (it != g_staging_ids.end() && it->second) Recognise(*c, it->second, CtxFnName(fn));
    }
}

template <int V>
struct Detours {
    static inline PSSetSrvFn ps_set_srv = nullptr;
    static inline DrawIndexedFn draw_indexed = nullptr;
    static inline DrawIndexedInstancedFn draw_indexed_inst = nullptr;
    static inline MapFn map = nullptr;
    static inline UnmapFn unmap = nullptr;
    static inline CopyRegionFn copy_region = nullptr;
    static inline CopyResourceFn copy_resource = nullptr;
    static inline UpdateFn update = nullptr;
    static inline Update1Fn update1 = nullptr;

    static void STDMETHODCALLTYPE PSSetSrv(ID3D11DeviceContext* c, UINT start, UINT n,
                                           ID3D11ShaderResourceView* const* views) {
        if (g_active.load(std::memory_order_relaxed)) OnPSSetSrv(c, start, n, views);
        ps_set_srv(c, start, n, views);
    }
    static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
        Swap sw;
        if (g_active.load(std::memory_order_relaxed)) OnDraw(V, c, count, 1, start, base, &sw);
        if (sw.n) DrawSwapped(c, sw, ps_set_srv, [&] { draw_indexed(c, count, start, base); });
        else draw_indexed(c, count, start, base);
    }
    static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances,
                                                       UINT start, INT base, UINT start_instance) {
        Swap sw;
        if (g_active.load(std::memory_order_relaxed)) OnDraw(V, c, count, instances, start, base, &sw);
        if (sw.n)
            DrawSwapped(c, sw, ps_set_srv, [&] { draw_indexed_inst(c, count, instances, start, base, start_instance); });
        else draw_indexed_inst(c, count, instances, start, base, start_instance);
    }
    static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type,
                                         UINT flags, D3D11_MAPPED_SUBRESOURCE* m) {
        const HRESULT hr = map(c, res, sub, type, flags, m);
        if (SUCCEEDED(hr) && type != D3D11_MAP_READ && m && g_active.load(std::memory_order_relaxed)) {
            if (g_separate) NoteMap(res, m);
            OnMapped(res, sub, m);
        }
        return hr;
    }
    static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
        // Hash before Unmap: the mapped pointer is only valid until then.
        if (g_active.load(std::memory_order_relaxed)) OnUnmap(res, sub);
        unmap(c, res, sub);
    }
    static void STDMETHODCALLTYPE CopyRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dst_sub, UINT x,
                                             UINT y, UINT z, ID3D11Resource* src, UINT src_sub,
                                             const D3D11_BOX* box) {
        if (g_active.load(std::memory_order_relaxed)) OnCopy(kCopySubresourceRegion, dst, dst_sub, src, src_sub, false);
        copy_region(c, dst, dst_sub, x, y, z, src, src_sub, box);
    }
    static void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
        if (g_active.load(std::memory_order_relaxed)) OnCopy(kCopyResource, dst, 0, src, 0, true);
        copy_resource(c, dst, src);
    }
    static void STDMETHODCALLTYPE Update(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub,
                                         const D3D11_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch) {
        if (g_active.load(std::memory_order_relaxed)) OnUpdate(kUpdateSubresource, dst, sub, box, data, row_pitch);
        update(c, dst, sub, box, data, row_pitch, depth_pitch);
    }
    static void STDMETHODCALLTYPE Update1(ID3D11DeviceContext1* c, ID3D11Resource* dst, UINT sub,
                                          const D3D11_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch,
                                          UINT flags) {
        if (g_active.load(std::memory_order_relaxed)) OnUpdate(kUpdateSubresource1, dst, sub, box, data, row_pitch);
        update1(c, dst, sub, box, data, row_pitch, depth_pitch, flags);
    }

    static void Fill(int fn, void** detour, void*** original) {
        switch (fn) {
            case kPSSetShaderResources: *detour = reinterpret_cast<void*>(&PSSetSrv); *original = reinterpret_cast<void**>(&ps_set_srv); break;
            case kDrawIndexed: *detour = reinterpret_cast<void*>(&DrawIndexed); *original = reinterpret_cast<void**>(&draw_indexed); break;
            case kDrawIndexedInstanced: *detour = reinterpret_cast<void*>(&DrawIndexedInstanced); *original = reinterpret_cast<void**>(&draw_indexed_inst); break;
            case kMap: *detour = reinterpret_cast<void*>(&Map); *original = reinterpret_cast<void**>(&map); break;
            case kUnmap: *detour = reinterpret_cast<void*>(&Unmap); *original = reinterpret_cast<void**>(&unmap); break;
            case kCopySubresourceRegion: *detour = reinterpret_cast<void*>(&CopyRegion); *original = reinterpret_cast<void**>(&copy_region); break;
            case kCopyResource: *detour = reinterpret_cast<void*>(&CopyResource); *original = reinterpret_cast<void**>(&copy_resource); break;
            case kUpdateSubresource: *detour = reinterpret_cast<void*>(&Update); *original = reinterpret_cast<void**>(&update); break;
            case kUpdateSubresource1: *detour = reinterpret_cast<void*>(&Update1); *original = reinterpret_cast<void**>(&update1); break;
        }
    }
};

// Stats window (render thread only).
uint64_t g_stats_frames0 = 0, g_stats_draws0[2] = {}, g_stats_tire0 = 0, g_stats_swapped0 = 0;
uint64_t g_stats_player0 = 0, g_stats_traffic0 = 0;

Game g_game = Game::kEts2;

// The CSV at csv_path when it exists (lets a user add tires without a new
// build), otherwise the copy built into the DLL (catalog_data.cpp, made by
// CMake from catalog/<game>/*.csv).
std::unique_ptr<std::istream> OpenCsv(const std::wstring& csv_path, const unsigned char* built_in, const char* what) {
    auto file = std::make_unique<std::ifstream>(csv_path);
    if (*file) {
        Log(std::string("tires: ") + what + " from the TireWear folder");
        return file;
    }
    return std::make_unique<std::istringstream>(std::string(reinterpret_cast<const char*>(built_in)));
}

}  // namespace

void SetGame(Game game) { g_game = game; }

int LoadCatalog(const std::wstring& csv_path, std::string* error) {
    std::unique_ptr<std::istream> src = OpenCsv(
        csv_path, g_game == Game::kAts ? catalog_data::kAtsTireTextures : catalog_data::kEts2TireTextures,
        g_game == Game::kAts ? "catalog (ATS)" : "catalog (ETS2)");
    std::istream& in = *src;
    if (!in) {
        *error = "cannot open catalog";
        return -1;
    }
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        // source,path,width,height,dxgi_format,mips,file_bytes,data_bytes,expected_bytes,hash_mip0,hash_all_mips,error
        if (f.size() < 11 || f[10].empty()) continue;
        CatalogEntry e;
        e.name = f[0] + ":" + f[1];
        e.width = static_cast<uint32_t>(std::stoul(f[2]));
        e.format = static_cast<uint32_t>(std::stoul(f[4]));
        g_catalog.push_back(e);
        const int id = static_cast<int>(g_catalog.size());
        std::stringstream hs(f[10]);
        std::string hex;
        for (int mip = 0; std::getline(hs, hex, ';'); ++mip) {
            if ((e.width >> mip) < 16) break;  // tiny mips are too generic
            g_hashes.emplace(std::stoull(hex, nullptr, 16), std::make_pair(id, mip));
        }
    }
    if (g_catalog.empty()) {
        *error = "catalog is empty";
        return -1;
    }
    g_cands.reserve(16384);  // keeps Candidate* stable (the map holds indices, FindCand returns pointers)
    g_tires.reset(new TireInfo[g_catalog.size() + 1]);
    for (size_t i = 0; i < g_catalog.size(); ++i)
        g_tires[i + 1].trailer = g_catalog[i].name.find("/trailer_owned/") != std::string::npos;
    return static_cast<int>(g_catalog.size());
}

int LoadRegions(const std::wstring& csv_path, std::string* error) {
    std::unique_ptr<std::istream> src = OpenCsv(
        csv_path, g_game == Game::kAts ? catalog_data::kAtsTireRegions : catalog_data::kEts2TireRegions,
        "tread regions");
    std::istream& in = *src;
    if (!in) {
        *error = "cannot open regions";
        return -1;
    }
    std::unordered_map<std::string, int> by_name;
    for (size_t i = 0; i < g_catalog.size(); ++i) by_name[g_catalog[i].name] = static_cast<int>(i + 1);
    std::string line;
    std::getline(in, line);  // header: color,normal,u0,v0,u1,v1,how
    int n = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() < 6) continue;
        const wear::Rect r{std::stof(f[2]), std::stof(f[3]), std::stof(f[4]), std::stof(f[5])};
        for (int k = 0; k < 2; ++k) {
            auto it = by_name.find(f[k]);
            if (it == by_name.end()) continue;
            g_tires[it->second].has_rect = true;
            g_tires[it->second].rect = r;
            ++n;
        }
    }
    return n;
}

void SetLog(void (*log)(const std::string&)) { g_log = log; }
void SetCaptureFile(const std::wstring& path) { g_capture_path = path; }

int MakeHooks(const Targets& t, Hook* out) {
    static char names[2][kCtxFnCount][40];
    int n = 0;
    if (t.create_texture2d)
        out[n++] = {"CreateTexture2D", t.create_texture2d, reinterpret_cast<void*>(&HookCreateTexture2D),
                    reinterpret_cast<void**>(&g_orig_create_tex)};
    if (t.create_srv)
        out[n++] = {"CreateShaderResourceView", t.create_srv, reinterpret_cast<void*>(&HookCreateSrv),
                    reinterpret_cast<void**>(&g_orig_create_srv)};
    for (int v = 0; v < 2; ++v) {
        for (int fn = 0; fn < kCtxFnCount; ++fn) {
            void* target = t.ctx[v][fn];
            if (!target) continue;
            if (v == 1 && target == t.ctx[0][fn]) continue;  // same code as the immediate context
            void* detour = nullptr;
            void** original = nullptr;
            if (v == 0) Detours<0>::Fill(fn, &detour, &original);
            else Detours<1>::Fill(fn, &detour, &original);
            snprintf(names[v][fn], sizeof(names[v][fn]), "%s%s", v ? "deferred " : "", CtxFnName(fn));
            out[n++] = {names[v][fn], target, detour, original};
        }
    }
    return n;
}

void SetActive(bool active) { g_active.store(active); }

void SetWearLevels(bool enabled, int levels, bool tint, float wear_override) {
    g_swap = enabled;
    g_levels = levels < 2 ? 2 : levels > kMaxLevels ? kMaxLevels : levels;
    g_tint = tint;
    g_wear_override = wear_override > 1.f ? 1.f : wear_override;
}

void SetUnitWear(const float* wear, int count) {
    for (int u = 0; u < tracker::kMaxUnits; ++u)
        g_unit_wear[u].store(u < count && wear[u] > 0.f ? wear[u] : 0.f, std::memory_order_relaxed);
}

void SetSeparation(bool enabled) { g_separate = enabled; }

void SetCaptureFrames(int frames) { g_capture_frames = frames < 1 ? 1 : frames > 60 ? 60 : frames; }

bool RequestCapture(const std::string& telemetry) {
    if (g_capture_state.load() != 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        g_capture_telemetry = telemetry;
    }
    int expected = 0;
    return g_capture_state.compare_exchange_strong(expected, 1);
}

// R5d: uploads the worker's finished results into the clones. Runs at
// Present, on the thread that drives the immediate context (the game uses
// that one only [verified: R2c log, "contexts imm 1 def 0"]).
void UploadWorn(IDXGISwapChain* sc) {
    if (g_done_count.load(std::memory_order_acquire) == 0 || !sc) return;
    std::deque<WearJob> done;
    {
        std::lock_guard<std::mutex> lock(g_job_mutex);
        done.swap(g_done);
        g_done_count.store(0, std::memory_order_release);
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
        dev->GetImmediateContext(&ctx);
        dev->Release();
    }
    for (auto& j : done) {
        if (ctx) {
            ctx->UpdateSubresource(j.cl->tex, j.sub, j.has_box ? &j.box : nullptr, j.data.data(), j.pitch, 0);
            j.cl->filled.store(true, std::memory_order_release);
        }
        j.cl->pending.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (ctx) ctx->Release();
}

void OnPresent(IDXGISwapChain* sc) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    UploadWorn(sc);
    ResetViewportRuns();  // R4c2: runs are numbered per frame
    const int cap = g_capture_state.load();
    std::string debug;
    if (g_separate && !g_sep_failed.load(std::memory_order_relaxed)) tracker::EndFrame(cap == 2 ? &debug : nullptr);
    if (cap == 2) {
        // End of a captured frame: what the tracker did with its draws.
        CapturedDraw marker;
        marker.line = "# end of frame " + std::to_string(g_capture_frame.load()) + "\n" + debug;
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        g_capture.push_back(std::move(marker));
    }
    if (cap == 1) {
        {
            std::lock_guard<std::mutex> lock(g_capture_mutex);
            g_capture.clear();
            g_capture_draws = 0;
        }
        g_capture_frame.store(1);
        g_capture_state.store(2);
    } else if (cap == 2 && g_capture_frame.load() < g_capture_frames) {
        g_capture_frame.fetch_add(1);  // keep capturing the next frame
    } else if (cap == 2) {
        // The frame is complete: read the copies back on the immediate context
        // (this stalls once, only for the capture frame).
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (sc && SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) {
            dev->GetImmediateContext(&ctx);
            dev->Release();
        }
        g_capture_state.store(3);  // stop collecting while writing
        WriteCapture(ctx);
        if (ctx) ctx->Release();
        g_capture_state.store(0);  // ready for the next request
    }
}

std::string Stats() {
    const uint64_t frames = g_frames.load(), tire = g_tire_draws.load(), swapped = g_swapped_draws.load();
    const uint64_t d0 = g_draws[0].load(), d1 = g_draws[1].load();
    const uint64_t df = frames - g_stats_frames0;
    const auto per = [df](uint64_t now, uint64_t before) { return df ? double(now - before) / df : 0.0; };
    char b[600];
    snprintf(b, sizeof(b),
             "tires: candidates default %d staging %d other %d (with initial data %d), recognised %d, unmatched "
             "hashes %d, partial boxes %d, tire views %d; uploads to candidates: UpdateSubresource %d, UpdateSubresource1 %d, Map %d, "
             "CopyResource %d, CopySubresourceRegion %d; contexts imm %d def %d; per frame: %.0f imm draws, %.0f "
             "def draws, %.1f tire draws, %.1f swapped; clones %d%s",
             g_cand_default.load(), g_cand_staging.load(), g_cand_other.load(), g_cand_with_data.load(),
             g_recognised.load(), g_unmatched.load(), g_partial_updates.load(), g_srv_map.Count(), g_path_count[kUpdateSubresource].load(),
             g_path_count[kUpdateSubresource1].load(), g_path_count[kMap].load(), g_path_count[kCopyResource].load(),
             g_path_count[kCopySubresourceRegion].load(), g_ctx_imm.load(), g_ctx_def.load(),
             per(d0, g_stats_draws0[0]), per(d1, g_stats_draws0[1]), per(tire, g_stats_tire0),
             per(swapped, g_stats_swapped0), g_clones_made.load(),
             g_cand_map.Full() || g_srv_map.Full() ? " [TABLE FULL]" : "");
    std::string out = b;
    if (g_separate) {
        const uint64_t player = g_player_draws.load(), traffic = g_traffic_draws.load();
        const auto ts = tracker::GetStats();
        snprintf(b, sizeof(b),
                 "\ntires: separation %s; per frame: %.1f player tire draws, %.1f traffic; reads %lld, invalid %lld, "
                 "no pointer yet %lld; tracker searches %lld, found %lld, completed %lld, predicted %lld, paired %lld",
                 g_sep_failed.load() ? "OFF (failed)" : "on", per(player, g_stats_player0), per(traffic, g_stats_traffic0),
                 g_sep_reads.load(), g_sep_invalid.load(), g_sep_nopointer.load(), ts.searches, ts.found, ts.completed, ts.predicted, ts.paired);
        out += b;
        g_stats_player0 = player;
        g_stats_traffic0 = traffic;
    }
    g_stats_frames0 = frames;
    g_stats_draws0[0] = d0;
    g_stats_draws0[1] = d1;
    g_stats_tire0 = tire;
    g_stats_swapped0 = swapped;
    return out;
}

}  // namespace tire_probe
