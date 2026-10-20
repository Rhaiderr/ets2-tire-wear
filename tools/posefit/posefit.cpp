// posefit (step R4c-a): offline check of pose-based wheel prediction.
//
// Usage: posefit <tire_capture_....txt> [...]
// For each capture (R4a single frame or R4b-diag multi-frame), per frame and
// viewport:
//  - fits the trailer's wheel layout (telemetry config) to its tire draws
//    (rigid, and rigid with a reflection, best wheel assignment) and prints
//    the residual;
//  - predicts the truck's wheels in that camera from the trailer pose and the
//    two world placements of the telemetry, and prints the distance to the
//    nearest truck tire draw;
//  - fits the truck to its own draws (>= 3) and prints where the wheels it
//    did not see (fronts) are predicted, with the nearest draw of any kind.
// Texture names tell trailer (trail55) and truck (trail70) apart, as in the
// R4 test saves.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(double s, V3 a) { return {s * a.x, s * a.y, s * a.z}; }
double Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
V3 Mul(const M3& r, V3 v) {
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z, r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
            r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}
M3 Mul(const M3& a, const M3& b) {
    M3 c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            c.m[i][j] = 0;
            for (int k = 0; k < 3; ++k) c.m[i][j] += a.m[i][k] * b.m[k][j];
        }
    return c;
}
M3 Transpose(const M3& a) {
    M3 t;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t.m[i][j] = a.m[j][i];
    return t;
}

// SCS placement: angles in turns; heading about y, pitch about x, roll about z
// [estimativa: order heading * pitch * roll; pitch/roll are ~0 in the saves].
M3 Euler(double h, double p, double r) {
    const double a = h * 2 * M_PI, b = p * 2 * M_PI, c = r * 2 * M_PI;
    M3 ry, rx, rz;
    ry.m[0][0] = std::cos(a), ry.m[0][2] = std::sin(a), ry.m[2][0] = -std::sin(a), ry.m[2][2] = std::cos(a);
    rx.m[1][1] = std::cos(b), rx.m[1][2] = -std::sin(b), rx.m[2][1] = std::sin(b), rx.m[2][2] = std::cos(b);
    rz.m[0][0] = std::cos(c), rz.m[0][1] = -std::sin(c), rz.m[1][0] = std::sin(c), rz.m[1][1] = std::cos(c);
    return Mul(Mul(ry, rx), rz);
}

struct Placement {
    bool ok = false;
    V3 pos;
    M3 rot;
};

// Jacobi eigen decomposition of a symmetric 4x4 matrix; returns the
// eigenvector of the largest eigenvalue.
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
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (a[i][i] > a[best][best]) best = i;
    for (int i = 0; i < 4; ++i) out[i] = v[i][best];
}

struct Pose {
    M3 r;  // includes the reflection when `reflected`
    V3 t;
    bool reflected = false;
    double rms = 1e9;
};

// Horn's quaternion method: rotation r and translation t with b ~ r a + t.
Pose FitRigid(const std::vector<V3>& a, const std::vector<V3>& b, bool reflect) {
    const size_t n = a.size();
    std::vector<V3> src = a;
    if (reflect)
        for (auto& p : src) p.x = -p.x;
    V3 ca, cb;
    for (size_t i = 0; i < n; ++i) ca = ca + src[i], cb = cb + b[i];
    ca = (1.0 / n) * ca, cb = (1.0 / n) * cb;
    double s[3][3] = {};
    for (size_t i = 0; i < n; ++i) {
        const V3 p = src[i] - ca, q = b[i] - cb;
        const double pv[3] = {p.x, p.y, p.z}, qv[3] = {q.x, q.y, q.z};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s[r][c] += pv[r] * qv[c];
    }
    double N[4][4] = {
        {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
        {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
        {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
        {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]}};
    double q[4];
    LargestEigen(N, q);
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    Pose pose;
    M3& r = pose.r;
    r.m[0][0] = w * w + x * x - y * y - z * z, r.m[0][1] = 2 * (x * y - w * z), r.m[0][2] = 2 * (x * z + w * y);
    r.m[1][0] = 2 * (x * y + w * z), r.m[1][1] = w * w - x * x + y * y - z * z, r.m[1][2] = 2 * (y * z - w * x);
    r.m[2][0] = 2 * (x * z - w * y), r.m[2][1] = 2 * (y * z + w * x), r.m[2][2] = w * w - x * x - y * y + z * z;
    pose.t = cb - Mul(r, ca);
    if (reflect) {
        M3 f;
        f.m[0][0] = -1;
        pose.r = Mul(r, f);
        pose.reflected = true;
    }
    double e = 0;
    for (size_t i = 0; i < n; ++i) {
        const V3 d = Mul(pose.r, a[i]) + pose.t - b[i];
        e += d.x * d.x + d.y * d.y + d.z * d.z;
    }
    pose.rms = std::sqrt(e / n);
    return pose;
}

// Best fit over all assignments of the draws to distinct config wheels.
Pose BestFit(const std::vector<V3>& wheels, const std::vector<V3>& draws, std::vector<int>* assign) {
    Pose best;
    const int n = static_cast<int>(wheels.size()), m = static_cast<int>(draws.size());
    if (m < 3 || m > n) return best;
    std::vector<int> cur(m, -1);
    std::vector<bool> used(n, false);
    std::vector<V3> a(m);
    // Depth-first over injective maps (6P6 = 720 at most here).
    struct Rec {
        static void Go(int i, int n, int m, const std::vector<V3>& wheels, const std::vector<V3>& draws,
                       std::vector<int>& cur, std::vector<bool>& used, std::vector<V3>& a, Pose& best,
                       std::vector<int>* assign) {
            if (i == m) {
                for (int k = 0; k < m; ++k) a[k] = wheels[cur[k]];
                for (int refl = 0; refl < 2; ++refl) {
                    Pose p = FitRigid(a, draws, refl == 1);
                    if (p.rms < best.rms) {
                        best = p;
                        if (assign) *assign = cur;
                    }
                }
                return;
            }
            for (int k = 0; k < n; ++k) {
                if (used[k]) continue;
                used[k] = true, cur[i] = k;
                Go(i + 1, n, m, wheels, draws, cur, used, a, best, assign);
                used[k] = false;
            }
        }
    };
    Rec::Go(0, n, m, wheels, draws, cur, used, a, best, assign);
    return best;
}

struct Draw {
    int frame = 1;
    std::string vp = "all";
    std::string tex;
    int idx = 0;
    V3 p;
    int rows = 0;
};

bool ParsePlacement(const char* line, Placement* out) {
    const char* s = strstr(line, "pos (");
    double x, y, z, h, p, r;
    if (!s || sscanf(s, "pos (%lf %lf %lf) h %lf p %lf r %lf", &x, &y, &z, &h, &p, &r) != 6) return false;
    out->ok = true;
    out->pos = {x, y, z};
    out->rot = Euler(h, p, r);
    return true;
}

void Analyse(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        return;
    }
    std::vector<V3> wheels[2];  // 0 truck, 1 trailer.0
    Placement truck, trailer;
    std::vector<Draw> draws;
    char line[4096];
    int cfg = -1;
    bool in_cb0 = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "truck.world.placement", 21)) ParsePlacement(line, &truck);
        if (!strncmp(line, "trailer.0.world.placement", 25)) ParsePlacement(line, &trailer);
        if (!strncmp(line, "config truck:", 13)) cfg = 0;
        else if (!strncmp(line, "config trailer.0:", 17)) cfg = 1;
        else if (!strncmp(line, "config ", 7) || line[0] == '#') cfg = -1;
        if (cfg >= 0) {
            int k;
            double x, y, z;
            if (sscanf(line, "  wheel.position[%d] = (%lf %lf %lf)", &k, &x, &y, &z) == 4) {
                if (static_cast<int>(wheels[cfg].size()) <= k) wheels[cfg].resize(k + 1);
                wheels[cfg][k] = {x, y, z};
            }
        }
        const char* imm = nullptr;
        if (!strncmp(line, "imm ", 4)) imm = line;
        else if (line[0] == 'f' && strstr(line, " imm ") && strstr(line, " imm ") - line < 6) imm = strstr(line, " imm ") + 1;
        if (imm) {
            Draw d;
            if (line[0] == 'f') d.frame = atoi(line + 1);
            const char* t6 = strstr(line, "t6=");
            d.tex = t6 ? std::string(t6 + 3, strcspn(t6 + 3, " ")) : "";
            const char* ix = strstr(imm, " idx ");
            d.idx = ix ? atoi(ix + 5) : 0;
            const char* v = strstr(line, " vp ");
            if (v) d.vp = std::string(v + 4, strcspn(v + 4, " ")) + " " +
                          std::string(v + 5 + strcspn(v + 4, " "), strcspn(v + 5 + strcspn(v + 4, " "), " "));
            draws.push_back(d);
            in_cb0 = false;
            continue;
        }
        if (!strncmp(line, "  cb0", 5)) in_cb0 = !draws.empty();
        else if (!strncmp(line, "  cb", 4)) in_cb0 = false;
        else if (in_cb0) {
            int row;
            double a, b, c, w;
            if (sscanf(line, "    [%d] %lf %lf %lf %lf", &row, &a, &b, &c, &w) == 5 && row <= 2) {
                Draw& d = draws.back();
                (row == 0 ? d.p.x : row == 1 ? d.p.y : d.p.z) = w;
                ++d.rows;
            }
        }
    }
    fclose(f);
    printf("=== %s\n", path);
    printf("truck wheels %zu, trailer wheels %zu, placements %s\n", wheels[0].size(), wheels[1].size(),
           truck.ok && trailer.ok ? "ok" : "missing");
    // Truck-local -> trailer-local from the two world placements.
    M3 rel_r;
    V3 rel_t;
    const bool rel_ok = truck.ok && trailer.ok;
    if (rel_ok) {
        const M3 inv_tl = Transpose(trailer.rot);
        rel_r = Mul(inv_tl, truck.rot);
        rel_t = Mul(inv_tl, truck.pos - trailer.pos);
        printf("truck origin in trailer space: (%.3f %.3f %.3f)\n", rel_t.x, rel_t.y, rel_t.z);
    }
    // Group draws by frame and viewport.
    std::map<std::pair<int, std::string>, std::vector<const Draw*>> groups;
    for (const auto& d : draws)
        if (d.rows == 3) groups[{d.frame, d.vp}].push_back(&d);
    for (const auto& g : groups) {
        std::vector<V3> trl, trk;
        std::vector<const Draw*> trk_draws;
        for (const Draw* d : g.second) {
            if (d->tex.find("trail55") != std::string::npos) trl.push_back(d->p);
            else if (d->tex.find("trail70") != std::string::npos) trk.push_back(d->p), trk_draws.push_back(d);
        }
        printf("-- frame %d vp %s: %zu trailer draws, %zu truck draws\n", g.first.first, g.first.second.c_str(),
               trl.size(), trk.size());
        // Trailer pose -> truck wheels.
        if (trl.size() >= 3 && trl.size() <= wheels[1].size()) {
            const Pose tp = BestFit(wheels[1], trl, nullptr);
            printf("   trailer fit: rms %.3f m%s\n", tp.rms, tp.reflected ? " (REFLECTED)" : "");
            if (rel_ok && !wheels[0].empty()) {
                printf("   truck predicted from trailer + telemetry:");
                for (size_t k = 0; k < wheels[0].size(); ++k) {
                    const V3 in_trl = Mul(rel_r, wheels[0][k]) + rel_t;
                    const V3 cam = Mul(tp.r, in_trl) + tp.t;
                    double best = 1e9;
                    for (const V3& q : trk) best = std::fmin(best, Len(q - cam));
                    if (best < 1e8) printf(" w%zu %.2f", k, best);
                    else printf(" w%zu -", k);
                }
                printf("  (m to nearest truck draw)\n");
            }
        }
        // Truck pose from its own draws -> where the unseen wheels are.
        if (trk.size() >= 3 && trk.size() <= wheels[0].size()) {
            std::vector<int> assign;
            const Pose kp = BestFit(wheels[0], trk, &assign);
            printf("   truck fit: rms %.3f m%s, draws ->", kp.rms, kp.reflected ? " (REFLECTED)" : "");
            std::vector<bool> seen(wheels[0].size(), false);
            for (size_t i = 0; i < assign.size(); ++i) {
                printf(" w%d(idx %d)", assign[i], trk_draws[i]->idx);
                seen[assign[i]] = true;
            }
            printf("\n");
            for (size_t k = 0; k < wheels[0].size(); ++k) {
                if (seen[k]) continue;
                const V3 cam = Mul(kp.r, wheels[0][k]) + kp.t;
                double best = 1e9;
                const Draw* bd = nullptr;
                for (const Draw* d : g.second)
                    if (Len(d->p - cam) < best) best = Len(d->p - cam), bd = d;
                printf("   unseen truck w%zu predicted (%.2f %.2f %.2f): nearest draw %.2f m%s\n", k, cam.x, cam.y,
                       cam.z, best, bd ? (" " + bd->tex.substr(bd->tex.rfind('/') + 1)).c_str() : "");
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) Analyse(argv[i]);
    return argc > 1 ? 0 : 2;
}
