// Step R4b: tells the player's tire draws apart from traffic. See player_tracker.h.

#include "player_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace tracker {
namespace {

constexpr float kTrackRadius = 0.35f;    // m, wheel movement relative to the camera between frames
constexpr float kMaxVelocity = 3.f;      // m per frame, cap of the extrapolation (R4c)
constexpr float kDistTol = 0.12f;        // m, layout match [verified: R4a errors <= 0.04]
constexpr float kPredictRadius = 0.2f;   // m, pose prediction match [verified: R4c-a errors <= 0.13]
constexpr float kFitTol = 0.06f;         // m, rms of a usable pose fit [verified: R4c-a rms 0.000-0.010]
constexpr float kSymTol = 0.05f;         // m, distance tolerance of a layout symmetry
constexpr float kPairNear = 3.5f;        // m, pair rule: both draws this close to a mirror camera [verified: R4c-a 2.6-3.1]
constexpr float kMirrorAreaRatio = 0.5f; // pair rule: viewport smaller than this share of the main one
constexpr float kSideNear = 6.f;         // m, side pair: both draws this close [verified: R4c 2.3-5.5]
constexpr float kSideTol = 0.06f;        // m, side pair distance match
constexpr float kAxleTol = 0.06f;        // side pair: |axle . pair direction| vs layout [verified: R4c 0.039 vs 0.036]
constexpr float kSteadyTol = 0.1f;       // m, a pair counts as the same one between frames
constexpr int kSteadyFrames = 8;         // frames a pair must stay put before it is claimed
constexpr int kMinWheels = 3;
constexpr int kMaxObs = 128;             // tire draws per camera per frame
constexpr int kMaxPasses = 8;
constexpr int kForgetFrames = 30;        // a wheel not seen for this long is dropped
constexpr int kPassIdleFrames = 300;
constexpr int kMaxVariants = 8;          // layout symmetries tried per pose fit

struct Obs {
    float p[3];
    int unit = -1, wheel = -1;
    float axle[3] = {};     // R4c2: the wheel's axle direction in the camera
    bool has_axle = false;
};

// R4c2: a pair seen by the pair rule, waiting to be steady.
struct PairCand {
    float a[3], b[3];    // where its two draws are this frame
    float a0[3], b0[3];  // where they were when the pair was first seen
    int u = -1, i = -1, j = -1, oa = -1, ob = -1;
    int count = 1;
};

struct Pass {
    const void* key = nullptr;
    float area = 0;                             // viewport area (pair rule)
    float pos[kMaxUnits][kMaxWheels][3] = {};
    float vel[kMaxUnits][kMaxWheels][3] = {};   // movement over the last frame (R4c)
    int last_seen[kMaxUnits][kMaxWheels] = {};  // frame number, 0 = no track
    int found[kMaxUnits] = {};                  // most wheels claimed at once (R4b3: re-search below this)
    std::vector<Obs> obs;
    std::vector<PairCand> cands;                // R4c2: pair rule candidates of the last frame
    int idle = 0;
};

std::mutex g_mutex;
Unit g_pending[kMaxUnits];
bool g_pending_dirty = false;

Unit g_units[kMaxUnits];
float g_dist[kMaxUnits][kMaxWheels][kMaxWheels];
std::vector<Pass> g_passes;
float g_main_area = 0;  // largest viewport seen: the main camera
int g_frame = 1;
Stats g_stats;

float Dist(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Norm(const float* a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

bool SameLayout(const Unit* a, const Unit* b) {
    for (int u = 0; u < kMaxUnits; ++u) {
        if (a[u].wheel_count != b[u].wheel_count) return false;
        for (int k = 0; k < a[u].wheel_count; ++k)
            for (int c = 0; c < 3; ++c)
                if (a[u].wheel[k][c] != b[u].wheel[k][c]) return false;
    }
    return true;
}

void ApplyPendingUnits() {
    if (!g_pending_dirty) return;
    g_pending_dirty = false;
    const bool same = SameLayout(g_units, g_pending);
    for (int u = 0; u < kMaxUnits; ++u) g_units[u] = g_pending[u];  // placements change every frame
    if (same) return;
    for (int u = 0; u < kMaxUnits; ++u)
        for (int i = 0; i < g_units[u].wheel_count; ++i)
            for (int j = 0; j < g_units[u].wheel_count; ++j)
                g_dist[u][i][j] = Dist(g_units[u].wheel[i], g_units[u].wheel[j]);
    for (auto& p : g_passes) {
        for (auto& row : p.last_seen)
            for (int& v : row) v = 0;
        for (int& v : p.found) v = 0;
    }
}

Pass& PassFor(const void* key) {
    for (auto& p : g_passes)
        if (p.key == key) return p;
    if (static_cast<int>(g_passes.size()) >= kMaxPasses) {
        // Reuse the most idle camera.
        Pass* worst = &g_passes[0];
        for (auto& p : g_passes)
            if (p.idle > worst->idle) worst = &p;
        *worst = Pass();
        worst->key = key;
        return *worst;
    }
    g_passes.emplace_back();
    g_passes.back().key = key;
    return g_passes.back();
}

// Records draw `o` as wheel k of unit u; the velocity only when the wheel was
// also seen in the previous frame.
void SetTrack(Pass& pass, int u, int k, const float* p) {
    const bool cont = pass.last_seen[u][k] == g_frame - 1;
    for (int c = 0; c < 3; ++c) {
        pass.vel[u][k][c] = cont ? p[c] - pass.pos[u][k][c] : 0.f;
        pass.pos[u][k][c] = p[c];
    }
    pass.last_seen[u][k] = g_frame;
}

// Finds the largest subset of the unclaimed observations matching unit u's
// wheel layout. Fills assign[wheel] = obs index (or -1); returns the count.
int Search(const Pass& pass, int u, int* assign) {
    const int n = g_units[u].wheel_count;
    const int m = static_cast<int>(pass.obs.size());
    int best = 0;
    int cur[kMaxWheels];
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int a = 0; a < m; ++a) {
                if (pass.obs[a].unit >= 0) continue;
                for (int b = 0; b < m; ++b) {
                    if (b == a || pass.obs[b].unit >= 0) continue;
                    if (std::fabs(Dist(pass.obs[a].p, pass.obs[b].p) - g_dist[u][i][j]) > kDistTol) continue;
                    for (int& v : cur) v = -1;
                    cur[i] = a;
                    cur[j] = b;
                    int count = 2;
                    for (int k = 0; k < n; ++k) {
                        if (cur[k] >= 0) continue;
                        for (int c = 0; c < m; ++c) {
                            if (pass.obs[c].unit >= 0) continue;
                            bool ok = true;
                            for (int l = 0; l < n && ok; ++l) {
                                if (cur[l] < 0) continue;
                                if (cur[l] == c ||
                                    std::fabs(Dist(pass.obs[c].p, pass.obs[cur[l]].p) - g_dist[u][k][l]) > kDistTol)
                                    ok = false;
                            }
                            if (ok) {
                                cur[k] = c;
                                ++count;
                                break;
                            }
                        }
                    }
                    if (count <= best) continue;
                    // Collinear wheels (one side only) are too weak a match:
                    // require wheels on both sides of the unit.
                    float xmin = 1e9f, xmax = -1e9f;
                    for (int k = 0; k < n; ++k)
                        if (cur[k] >= 0) {
                            xmin = std::fmin(xmin, g_units[u].wheel[k][0]);
                            xmax = std::fmax(xmax, g_units[u].wheel[k][0]);
                        }
                    if (xmax - xmin < 0.5f) continue;
                    best = count;
                    for (int k = 0; k < n; ++k) assign[k] = cur[k];
                    if (best == n) return best;
                }
            }
    return best;
}

// ---- R4c: rigid poses ----

struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

void MulV(const M3& r, const double* v, double* out) {
    for (int i = 0; i < 3; ++i) out[i] = r.m[i][0] * v[0] + r.m[i][1] * v[1] + r.m[i][2] * v[2];
}

M3 MulM(const M3& a, const M3& b) {
    M3 c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return c;
}

M3 Transpose(const M3& a) {
    M3 t;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t.m[i][j] = a.m[j][i];
    return t;
}

// SCS placement angles are in turns: heading about y, pitch about x, roll
// about z [estimativa: order heading * pitch * roll; checked in R4c-a with
// pitch/roll ~0].
M3 Euler(double h, double p, double r) {
    constexpr double kTurn = 6.283185307179586;
    const double a = h * kTurn, b = p * kTurn, c = r * kTurn;
    M3 ry, rx, rz;
    ry.m[0][0] = std::cos(a), ry.m[0][2] = std::sin(a), ry.m[2][0] = -std::sin(a), ry.m[2][2] = std::cos(a);
    rx.m[1][1] = std::cos(b), rx.m[1][2] = -std::sin(b), rx.m[2][1] = std::sin(b), rx.m[2][2] = std::cos(b);
    rz.m[0][0] = std::cos(c), rz.m[0][1] = -std::sin(c), rz.m[1][0] = std::sin(c), rz.m[1][1] = std::cos(c);
    return MulM(MulM(ry, rx), rz);
}

// Eigenvector of the largest eigenvalue of a symmetric 4x4 matrix (Jacobi).
void LargestEigen(double a[4][4], double out[4]) {
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = 0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) off += a[p][q] * a[p][q];
        if (off < 1e-18) break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) {
                if (std::fabs(a[p][q]) < 1e-15) continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq, a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk, a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq, v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (a[i][i] > a[best][best]) best = i;
    for (int i = 0; i < 4; ++i) out[i] = v[i][best];
}

struct Pose {
    M3 r;
    double t[3] = {};
    double rms = 1e9;
};

// Horn's method: proper rotation r and translation t with cam ~ r * local + t.
Pose FitPose(const double (*local)[3], const double (*cam)[3], int n) {
    double ca[3] = {}, cb[3] = {};
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) ca[c] += local[i][c] / n, cb[c] += cam[i][c] / n;
    double s[3][3] = {};
    for (int i = 0; i < n; ++i)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s[r][c] += (local[i][r] - ca[r]) * (cam[i][c] - cb[c]);
    double N[4][4] = {
        {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
        {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
        {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
        {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]}};
    double q[4];
    LargestEigen(N, q);
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    Pose p;
    p.r.m[0][0] = w * w + x * x - y * y - z * z, p.r.m[0][1] = 2 * (x * y - w * z), p.r.m[0][2] = 2 * (x * z + w * y);
    p.r.m[1][0] = 2 * (x * y + w * z), p.r.m[1][1] = w * w - x * x + y * y - z * z, p.r.m[1][2] = 2 * (y * z - w * x);
    p.r.m[2][0] = 2 * (x * z - w * y), p.r.m[2][1] = 2 * (y * z + w * x), p.r.m[2][2] = w * w - x * x - y * y + z * z;
    double rc[3];
    MulV(p.r, ca, rc);
    for (int c = 0; c < 3; ++c) p.t[c] = cb[c] - rc[c];
    double e = 0;
    for (int i = 0; i < n; ++i) {
        double v[3];
        MulV(p.r, local[i], v);
        for (int c = 0; c < 3; ++c) {
            const double d = v[c] + p.t[c] - cam[i][c];
            e += d * d;
        }
    }
    p.rms = std::sqrt(e / n);
    return p;
}

// Distance-preserving re-labellings of a unit's seen wheels (the layout's
// symmetries: a rectangle of wheels fits front/back and left/right swapped
// equally well [verified: R4c-a]). out[v][i] = wheel used for seen[i].
int Symmetries(int u, const int* seen, int count, int (*out)[kMaxWheels]) {
    int found = 0, cur[kMaxWheels];
    bool used[kMaxWheels] = {};
    struct Rec {
        static void Go(int u, const int* seen, int count, int i, int* cur, bool* used, int (*out)[kMaxWheels],
                       int& found) {
            if (found >= kMaxVariants) return;
            if (i == count) {
                for (int j = 0; j < count; ++j) out[found][j] = cur[j];
                ++found;
                return;
            }
            for (int k = 0; k < g_units[u].wheel_count; ++k) {
                if (used[k]) continue;
                bool ok = true;
                for (int j = 0; j < i && ok; ++j)
                    if (std::fabs(g_dist[u][k][cur[j]] - g_dist[u][seen[i]][seen[j]]) > kSymTol) ok = false;
                if (!ok) continue;
                used[k] = true, cur[i] = k;
                Go(u, seen, count, i + 1, cur, used, out, found);
                used[k] = false;
            }
        }
    };
    Rec::Go(u, seen, count, 0, cur, used, out, found);
    return found;
}

// Unit b's wheel k in unit a's frame, from the two world placements.
void InUnit(int a, int b, int k, double* out) {
    const Unit& ua = g_units[a];
    const Unit& ub = g_units[b];
    const M3 ra = Euler(ua.heading, ua.pitch, ua.roll), rb = Euler(ub.heading, ub.pitch, ub.roll);
    const double w[3] = {ub.wheel[k][0], ub.wheel[k][1], ub.wheel[k][2]};
    double world[3];
    MulV(rb, w, world);
    double d[3];
    for (int c = 0; c < 3; ++c) d[c] = world[c] + ub.pos[c] - ua.pos[c];
    MulV(Transpose(ra), d, out);
}

// R4c: claims draws of units that have wheels without a draw, predicted from
// the pose of another unit seen with >= kMinWheels wheels and the telemetry
// placements (e.g. the truck's wheels in a mirror that shows the trailer).
void PredictFromPoses(Pass& pass, int* claimed, std::string* debug) {
    const int m = static_cast<int>(pass.obs.size());
    for (int a = 0; a < kMaxUnits; ++a) {
        if (claimed[a] < kMinWheels || !g_units[a].placed) continue;
        int seen[kMaxWheels], seen_obs[kMaxWheels], count = 0;
        for (int i = 0; i < m && count < kMaxWheels; ++i)
            if (pass.obs[i].unit == a && pass.obs[i].wheel >= 0) seen[count] = pass.obs[i].wheel, seen_obs[count++] = i;
        if (count < kMinWheels) continue;
        int variants[kMaxVariants][kMaxWheels];
        const int nv = Symmetries(a, seen, count, variants);
        Pose poses[kMaxVariants];
        for (int v = 0; v < nv; ++v) {
            double local[kMaxWheels][3], cam[kMaxWheels][3];
            for (int i = 0; i < count; ++i)
                for (int c = 0; c < 3; ++c)
                    local[i][c] = g_units[a].wheel[variants[v][i]][c], cam[i][c] = pass.obs[seen_obs[i]].p[c];
            poses[v] = FitPose(local, cam, count);
        }
        for (int b = 0; b < kMaxUnits; ++b) {
            if (b == a || !g_units[b].wheel_count || !g_units[b].placed) continue;
            bool missing = false;
            int have[kMaxWheels];
            for (int& h : have) h = -1;
            for (int i = 0; i < m; ++i)
                if (pass.obs[i].unit == b && pass.obs[i].wheel >= 0) have[pass.obs[i].wheel] = i;
            for (int k = 0; k < g_units[b].wheel_count; ++k) missing |= have[k] < 0;
            if (!missing) continue;
            // Each symmetry predicts somewhere else; keep the one that matches
            // the most free draws, and nothing when two disagree.
            int best_v = -1, best_n = 0, best_match[kMaxWheels];
            bool tie = false;
            for (int v = 0; v < nv; ++v) {
                if (poses[v].rms > kFitTol) continue;
                int match[kMaxWheels], n = 0;
                bool taken[kMaxObs] = {};
                for (int k = 0; k < g_units[b].wheel_count; ++k) {
                    match[k] = -1;
                    if (have[k] >= 0) continue;
                    double in_a[3], cam[3];
                    InUnit(a, b, k, in_a);
                    MulV(poses[v].r, in_a, cam);
                    float pc[3];
                    for (int c = 0; c < 3; ++c) pc[c] = static_cast<float>(cam[c] + poses[v].t[c]);
                    float best = kPredictRadius;
                    for (int i = 0; i < m; ++i) {
                        if (pass.obs[i].unit >= 0 || taken[i]) continue;
                        const float d = Dist(pc, pass.obs[i].p);
                        if (d < best) best = d, match[k] = i;
                    }
                    if (match[k] >= 0) taken[match[k]] = true, ++n;
                }
                if (n == 0) continue;
                if (n > best_n) {
                    best_v = v, best_n = n, tie = false;
                    for (int k = 0; k < kMaxWheels; ++k) best_match[k] = k < g_units[b].wheel_count ? match[k] : -1;
                } else if (n == best_n) {
                    // Same draws (e.g. a left/right swap) is not a disagreement.
                    bool same = true;
                    for (int k = 0; k < g_units[b].wheel_count && same; ++k) {
                        bool in_best = false;
                        for (int l = 0; l < g_units[b].wheel_count; ++l) in_best |= best_match[l] == match[k];
                        same = match[k] < 0 || in_best;
                    }
                    tie |= !same;
                }
            }
            if (best_v < 0 || tie) continue;
            for (int k = 0; k < g_units[b].wheel_count; ++k) {
                if (best_match[k] < 0) continue;
                Obs& o = pass.obs[best_match[k]];
                o.unit = b, o.wheel = k;
                SetTrack(pass, b, k, o.p);
                ++claimed[b];
                ++g_stats.predicted;
                if (debug)
                    *debug += "    predicted u" + std::to_string(b) + " w" + std::to_string(k) + "=#" +
                              std::to_string(best_match[k]) + " from u" + std::to_string(a) + "\n";
            }
        }
    }
}

// R4c/R4c2: in a mirror camera (viewport much smaller than the main one) a
// unit not seen there by any other rule can be recognised from two free draws:
//  - an axle of the truck (same z, opposite sides), both draws within
//    kPairNear of the camera (front wheels in the small mirror [verified: R4c-a]);
//  - two wheels on one side of a unit, both within kSideNear, whose axles
//    (where the wheel's matrix gives one) are square to the line between them
//    as the layout says (the side mirrors [verified: R4c capture 100209]).
// Mirrors are fixed to the cab, so the player's wheels stay put in them: a
// pair is only claimed after kSteadyFrames frames in the same place, which
// keeps out traffic passing by.
bool AxleFits(const Obs& o, const float* d_unit, float expect) {
    if (!o.has_axle) return false;
    const float c = std::fabs(o.axle[0] * d_unit[0] + o.axle[1] * d_unit[1] + o.axle[2] * d_unit[2]);
    return std::fabs(c - expect) < kAxleTol;
}

void PairRule(Pass& pass, int* claimed, std::string* debug) {
    std::vector<PairCand> next;
    if (pass.area > 0 && pass.area < kMirrorAreaRatio * g_main_area) {
        const int m = static_cast<int>(pass.obs.size());
        for (int u = 0; u < kMaxUnits; ++u) {
            if (claimed[u] || g_units[u].wheel_count < 2) continue;
            const Unit& un = g_units[u];
            for (int i = 0; i < un.wheel_count; ++i)
                for (int j = i + 1; j < un.wheel_count; ++j) {
                    const float dx = std::fabs(un.wheel[i][0] - un.wheel[j][0]);
                    const float dz = std::fabs(un.wheel[i][2] - un.wheel[j][2]);
                    const bool axle = u == 0 && dz < 0.05f && un.wheel[i][0] * un.wheel[j][0] < 0;
                    const bool side = dx < 0.3f && dz > 0.5f;
                    if (!axle && !side) continue;
                    const float near = axle ? kPairNear : kSideNear;
                    const float tol = axle ? kDistTol : kSideTol;
                    for (int a = 0; a < m; ++a) {
                        if (pass.obs[a].unit >= 0 || Norm(pass.obs[a].p) > near) continue;
                        for (int b = a + 1; b < m; ++b) {
                            if (pass.obs[b].unit >= 0 || Norm(pass.obs[b].p) > near) continue;
                            const float d = Dist(pass.obs[a].p, pass.obs[b].p);
                            if (std::fabs(d - g_dist[u][i][j]) > tol) continue;
                            if (side) {
                                // At least one non-steered wheel shows its axle
                                // square to the pair, as far as the layout's
                                // sideways offset allows.
                                float du[3];
                                for (int c = 0; c < 3; ++c) du[c] = (pass.obs[b].p[c] - pass.obs[a].p[c]) / d;
                                const float expect = dx / g_dist[u][i][j];
                                if (!AxleFits(pass.obs[a], du, expect) && !AxleFits(pass.obs[b], du, expect)) continue;
                            }
                            PairCand pc;
                            for (int c = 0; c < 3; ++c) pc.a[c] = pass.obs[a].p[c], pc.b[c] = pass.obs[b].p[c];
                            pc.u = u, pc.i = i, pc.j = j, pc.oa = a, pc.ob = b;
                            next.push_back(pc);
                        }
                    }
                }
        }
        // Steady: the same pair (either order) still where it was first seen
        // (measured from there, so slow drift adds up and breaks it).
        for (auto& pc : next) {
            for (int c = 0; c < 3; ++c) pc.a0[c] = pc.a[c], pc.b0[c] = pc.b[c];
            for (const auto& old : pass.cands) {
                const bool same = Dist(old.a0, pc.a) < kSteadyTol && Dist(old.b0, pc.b) < kSteadyTol;
                const bool swapped = Dist(old.a0, pc.b) < kSteadyTol && Dist(old.b0, pc.a) < kSteadyTol;
                if (old.u != pc.u || !(same || swapped) || old.count + 1 <= pc.count) continue;
                pc.count = old.count + 1;
                for (int c = 0; c < 3; ++c)
                    pc.a0[c] = same ? old.a0[c] : old.b0[c], pc.b0[c] = same ? old.b0[c] : old.a0[c];
            }
        }
        for (const auto& pc : next) {
            Obs& oa = pass.obs[pc.oa];
            Obs& ob = pass.obs[pc.ob];
            if (pc.count < kSteadyFrames || oa.unit >= 0 || ob.unit >= 0 || claimed[pc.u]) continue;
            oa.unit = pc.u, oa.wheel = pc.i;
            ob.unit = pc.u, ob.wheel = pc.j;
            SetTrack(pass, pc.u, pc.i, oa.p);
            SetTrack(pass, pc.u, pc.j, ob.p);
            claimed[pc.u] += 2;
            g_stats.paired += 2;
            if (debug)
                *debug += "    pair u" + std::to_string(pc.u) + " w" + std::to_string(pc.i) + "=#" +
                          std::to_string(pc.oa) + " w" + std::to_string(pc.j) + "=#" + std::to_string(pc.ob) +
                          " (steady " + std::to_string(pc.count) + " frames)\n";
        }
    }
    pass.cands.swap(next);
}

}  // namespace

void SetUnits(const Unit* units, int count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int u = 0; u < kMaxUnits; ++u) g_pending[u] = u < count ? units[u] : Unit();
    g_pending_dirty = true;
}

const void* ViewportKey(const float vp[4], int run) {
    uint64_t h = 14695981039346656037ull;  // FNV-1a over the rounded values and the run
    for (int i = 0; i < 5; ++i) {
        h ^= static_cast<uint64_t>(i < 4 ? static_cast<int64_t>(std::lround(vp[i])) : run);
        h *= 1099511628211ull;
    }
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(h | 1));  // never null
}

int Classify(const void* camera_key, const float pos[3], int* obs_index, float viewport_area, const float* axle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Pass& pass = PassFor(camera_key);
    if (viewport_area > pass.area) pass.area = viewport_area;
    if (viewport_area > g_main_area) g_main_area = viewport_area;
    Obs o;
    o.p[0] = pos[0], o.p[1] = pos[1], o.p[2] = pos[2];
    if (axle && Norm(axle) > 0.5f) {
        const float n = Norm(axle);
        for (int c = 0; c < 3; ++c) o.axle[c] = axle[c] / n;
        o.has_axle = true;
    }
    // Nearest wheel where it is expected now: where it was, moved by its last
    // frame's movement when it was seen in the latest frame (R4c).
    float best_score = 1.f;  // distance / radius
    for (int u = 0; u < kMaxUnits; ++u)
        for (int k = 0; k < g_units[u].wheel_count; ++k) {
            if (!pass.last_seen[u][k]) continue;
            float expect[3], radius = kTrackRadius;
            const float* v = pass.vel[u][k];
            const float speed = std::fmin(Norm(v), kMaxVelocity);
            const bool latest = pass.last_seen[u][k] == g_frame;
            for (int c = 0; c < 3; ++c) expect[c] = pass.pos[u][k][c] + (latest && speed > 0 ? v[c] * speed / Norm(v) : 0);
            if (latest) radius += speed;
            const float score = Dist(pos, expect) / radius;
            if (score < best_score) best_score = score, o.unit = u, o.wheel = k;
        }
    if (obs_index) *obs_index = static_cast<int>(pass.obs.size()) < kMaxObs ? static_cast<int>(pass.obs.size()) : -1;
    if (static_cast<int>(pass.obs.size()) < kMaxObs) pass.obs.push_back(o);
    (o.unit >= 0 ? g_stats.claimed : g_stats.unclaimed)++;
    return o.unit;
}

void SetMainViewportArea(float area) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (area > g_main_area) g_main_area = area;
}

void EndFrame(std::string* debug) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_frame;
    char b[160];
    for (auto& pass : g_passes) {
        if (pass.obs.empty()) {
            ++pass.idle;
            continue;
        }
        pass.idle = 0;
        bool seen[kMaxUnits] = {};
        int claimed[kMaxUnits] = {};
        for (const auto& o : pass.obs) {
            if (o.unit < 0) continue;
            seen[o.unit] = true;
            ++claimed[o.unit];
            SetTrack(pass, o.unit, o.wheel, o.p);
        }
        if (debug) {
            snprintf(b, sizeof(b), "  tracker camera %p (area %.0f): %zu draws, claimed by tracking:", pass.key,
                     pass.area, pass.obs.size());
            *debug += b;
            for (int u = 0; u < kMaxUnits; ++u)
                if (g_units[u].wheel_count) *debug += " u" + std::to_string(u) + "=" + std::to_string(claimed[u]);
            *debug += "\n";
        }
        const int m = static_cast<int>(pass.obs.size());
        for (int u = 0; u < kMaxUnits; ++u) {
            for (int k = 0; k < kMaxWheels; ++k)
                if (pass.last_seen[u][k] && g_frame - pass.last_seen[u][k] > kForgetFrames) pass.last_seen[u][k] = 0;
            const int n = g_units[u].wheel_count;
            if (n < kMinWheels) continue;
            // Search when the unit is not tracked, or tracked with fewer wheels
            // than it had (R4b3: partial tracks used to stay partial). The
            // unit's own draws are released for the search; the result is
            // kept only if it has more wheels and, when the unit was partly
            // tracked, shares a draw with it (no jumping to a look-alike).
            if (!seen[u] || claimed[u] < pass.found[u]) {
                ++g_stats.searches;
                bool was_mine[kMaxObs] = {};
                for (int i = 0; i < m; ++i)
                    if (pass.obs[i].unit == u) was_mine[i] = true, pass.obs[i].unit = -1;
                int assign[kMaxWheels];
                for (int& v : assign) v = -1;
                const int got = Search(pass, u, assign);
                bool overlap = !seen[u];
                for (int k = 0; k < n; ++k)
                    if (assign[k] >= 0 && was_mine[assign[k]]) overlap = true;
                const bool adopt = got >= kMinWheels && got > claimed[u] && overlap;
                if (debug) {
                    snprintf(b, sizeof(b), "    search u%d: %d of %d wheels%s", u, got, n,
                             adopt ? "" : got >= kMinWheels ? " (kept the tracks)" : "");
                    *debug += b;
                    if (adopt)
                        for (int k = 0; k < n; ++k)
                            if (assign[k] >= 0) *debug += " w" + std::to_string(k) + "=#" + std::to_string(assign[k]);
                    *debug += "\n";
                }
                if (adopt) {
                    ++g_stats.found;
                    for (int k = 0; k < kMaxWheels; ++k) pass.last_seen[u][k] = 0;
                    for (int i = 0; i < m; ++i)
                        if (was_mine[i]) pass.obs[i].wheel = -1;
                    for (int k = 0; k < n; ++k) {
                        if (assign[k] < 0) continue;
                        Obs& o = pass.obs[assign[k]];
                        o.unit = u, o.wheel = k;  // not available to the next unit's search
                        SetTrack(pass, u, k, o.p);
                    }
                    claimed[u] = got;
                    seen[u] = true;
                } else {
                    for (int i = 0; i < m; ++i)
                        if (was_mine[i]) pass.obs[i].unit = u;
                }
            }
            // Completion (R4b3): an unclaimed draw whose distances to >= 3
            // claimed wheels of the unit fit a wheel that has no draw yet.
            if (claimed[u] >= kMinWheels) {
                int have[kMaxWheels];
                for (int& v : have) v = -1;
                for (int i = 0; i < m; ++i)
                    if (pass.obs[i].unit == u && pass.obs[i].wheel >= 0) have[pass.obs[i].wheel] = i;
                for (int k = 0; k < n; ++k) {
                    if (have[k] >= 0) continue;
                    for (int c = 0; c < m; ++c) {
                        if (pass.obs[c].unit >= 0) continue;
                        int refs = 0;
                        bool ok = true;
                        for (int l = 0; l < n && ok; ++l) {
                            if (have[l] < 0) continue;
                            if (std::fabs(Dist(pass.obs[c].p, pass.obs[have[l]].p) - g_dist[u][k][l]) > kDistTol)
                                ok = false;
                            ++refs;
                        }
                        if (!ok || refs < kMinWheels) continue;
                        Obs& o = pass.obs[c];
                        o.unit = u, o.wheel = k;
                        have[k] = c;
                        SetTrack(pass, u, k, o.p);
                        ++claimed[u];
                        ++g_stats.completed;
                        if (debug) *debug += "    completed u" + std::to_string(u) + " w" + std::to_string(k) + "=#" +
                                             std::to_string(c) + "\n";
                        break;
                    }
                }
            }
        }
        PredictFromPoses(pass, claimed, debug);
        PairRule(pass, claimed, debug);
        for (int u = 0; u < kMaxUnits; ++u)
            if (claimed[u] > pass.found[u]) pass.found[u] = claimed[u];
        pass.obs.clear();
    }
    for (size_t i = 0; i < g_passes.size();)
        if (g_passes[i].idle > kPassIdleFrames) g_passes.erase(g_passes.begin() + i);
        else ++i;
    ApplyPendingUnits();
}

Stats GetStats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stats;
}

}  // namespace tracker
