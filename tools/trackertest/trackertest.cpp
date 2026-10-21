// trackertest (step R4b): runs player_tracker on an R4a capture file.
//
// Usage: trackertest <tire_capture_..._n.txt>
// Reads the unit configs (wheel positions) and the tire draws (cb0 rows 0-2,
// column 3 = wheel position relative to the camera), splits the draws into
// cameras where the constant buffer offset jumps, then replays the frame:
//   frame 1: nothing tracked yet (all -1), search at frame end;
//   frame 2: every player wheel must be claimed;
//   frame 3: same, plus a copy of the player's truck 6 m to the side
//            ("traffic" with the same layout), which must stay unclaimed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>

#include "../../src/player_tracker.h"

namespace {

struct Draw {
    std::string tex;
    int first = 0;
    float p[3] = {};
    const void* camera = nullptr;
};

}  // namespace

// Step R4b2: replays an R4b-diag capture (several frames; per real tire draw
// the CPU-read position, render target and viewport) frame by frame, keying
// the camera by render target ("rtv", as R4b did) or by viewport (default).
bool g_debug = false;     // flag "debug": print the tracker's per-frame text
bool g_no_run = false;    // flag "norun": one camera per viewport (R4c behaviour)
bool g_no_axle = false;   // flag "noaxle": no wheel orientation (R4c behaviour)
int g_inject = 0;         // flag "passing" (1) or "stopped" (2): add a traffic pair to the big mirror
std::map<std::string, std::pair<int, int>> g_per;  // viewport + run + texture -> claimed, total (frame >= 2)

struct DiagDraw {
    float pos[3];
    float vp[4];
    int run = -1;  // from the capture (R4c2 builds), else derived below
    unsigned long long rtv = 0;
    std::string tex;
    float axle[3] = {};
    int rows = 0;
};

int ReplayDiag(const char* path, bool by_viewport, tracker::Unit* units) {
    FILE* f = fopen(path, "rb");
    if (!f) return 1;
    tracker::SetUnits(units, tracker::kMaxUnits);
    // The main camera of the captures is 3440x2880 [verified: R4b-diag]; in the
    // cab it draws no tires, so it is not seen in the tire draws.
    tracker::SetMainViewportArea(3440.f * 2880.f);
    tracker::EndFrame();  // applies the units
    char line[4096];
    int frame = 0;
    long long total_claimed = 0, total_draws = 0, claimed_after_1 = 0, draws_after_1 = 0;
    printf("-- camera keyed by %s%s%s\n", by_viewport ? "viewport" : "render target",
           g_no_run ? "" : " + run", g_no_axle ? ", no wheel orientation" : "");
    std::vector<DiagDraw> fr;  // this frame's readable tire draws
    bool in_draw = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'f' && strstr(line, " imm ")) {
            in_draw = false;
            if (!strstr(line, "| sep read cpu (")) continue;
            const char* p = strstr(line, "cpu (");
            DiagDraw d;
            if (sscanf(p, "cpu (%f %f %f)", &d.pos[0], &d.pos[1], &d.pos[2]) != 3) continue;
            const char* v = strstr(p, " vp ");
            if (!v || sscanf(v, " vp %f,%f %fx%f", &d.vp[0], &d.vp[1], &d.vp[2], &d.vp[3]) != 4) continue;
            const char* rn = strstr(v, " run ");
            if (rn) d.run = atoi(rn + 5);
            const char* r = strstr(p, " rtv ");
            if (r) d.rtv = strtoull(r + 5, nullptr, 16);
            const char* t6 = strstr(line, "t6=");
            d.tex = t6 ? std::string(t6 + 3, strcspn(t6 + 3, " ")) : "";
            d.tex = d.tex.substr(d.tex.rfind('/') + 1);
            frame = atoi(line + 1);
            fr.push_back(d);
            in_draw = true;
        } else if (in_draw && !strncmp(line, "    [", 5)) {
            int row;
            float a, b, c, w;
            if (sscanf(line, "    [%d] %f %f %f %f", &row, &a, &b, &c, &w) == 5 && row <= 2) {
                fr.back().axle[row] = a;  // column 0 of the wheel matrix
                ++fr.back().rows;
            }
        } else if (!strncmp(line, "# end of frame", 14)) {
            in_draw = false;
            // Runs of a viewport in draw order, when the capture has none
            // (only tire draws are seen here; the plugin counts all draws).
            std::map<std::string, int> runs;
            std::string prev;
            int cur_run = 0;
            int draws = 0, claimed = 0;
            if (g_inject) {
                // Traffic in the big mirror: two wheels on one side 1.295 m
                // apart (the truck's rear spacing), axles square to them,
                // 4-5 m from the camera; "passing" moves 0.05 m per frame
                // (~20 km/h at 110 fps), "stopped" stays put.
                for (int w = 0; w < 2; ++w) {
                    DiagDraw t;
                    const float shift = g_inject == 1 ? 0.05f * frame : 0.f;
                    t.pos[0] = 2.5f, t.pos[1] = -1.f, t.pos[2] = -4.f - 1.295f * w - shift;
                    t.vp[0] = t.vp[1] = 0, t.vp[2] = 512, t.vp[3] = 1024;
                    t.tex = g_inject == 1 ? "TRAFFIC-passing" : "TRAFFIC-stopped";
                    t.axle[0] = 1, t.rows = 3;
                    fr.push_back(t);
                }
            }
            for (auto& d : fr) {
                char vk[64];
                snprintf(vk, sizeof(vk), "%.0f,%.0f %.0fx%.0f", d.vp[0], d.vp[1], d.vp[2], d.vp[3]);
                if (vk != prev) cur_run = runs[vk]++, prev = vk;
                const int run = g_no_run ? 0 : d.run >= 0 ? d.run : cur_run;
                const void* key = by_viewport ? tracker::ViewportKey(d.vp, run)
                                              : reinterpret_cast<const void*>(static_cast<uintptr_t>(d.rtv));
                const int u = tracker::Classify(key, d.pos, nullptr, d.vp[2] * d.vp[3],
                                                g_no_axle || d.rows < 3 ? nullptr : d.axle);
                char k[96];
                snprintf(k, sizeof(k), "%4.0fx%-4.0f run %d %s", d.vp[2], d.vp[3], run, d.tex.c_str());
                if (frame > 1) {
                    ++g_per[k].second;
                    if (u >= 0) ++g_per[k].first;
                }
                ++draws;
                claimed += u >= 0;
            }
            fr.clear();
            std::string debug;
            tracker::EndFrame(g_debug ? &debug : nullptr);
            printf("  frame %2d: %2d tire draws, %2d claimed\n%s", frame, draws, claimed, debug.c_str());
            total_draws += draws, total_claimed += claimed;
            if (frame > 1) draws_after_1 += draws, claimed_after_1 += claimed;
        }
    }
    fclose(f);
    printf("  total: %lld of %lld claimed; from frame 2 on: %lld of %lld\n", total_claimed, total_draws,
           claimed_after_1, draws_after_1);
    for (const auto& e : g_per) printf("    %-34s %3d of %3d\n", e.first.c_str(), e.second.first, e.second.second);
    const auto s = tracker::GetStats();
    printf("  tracker: searches %lld found %lld completed %lld predicted %lld paired %lld\n", s.searches, s.found,
           s.completed, s.predicted, s.paired);
    return 0;
}

// "pos (x y z) h H p P r R" of a world.placement line (angles in turns).
void ParsePlacement(const char* line, tracker::Unit* u) {
    const char* s = strstr(line, "pos (");
    double x, y, z, h, p, r;
    if (!s || sscanf(s, "pos (%lf %lf %lf) h %lf p %lf r %lf", &x, &y, &z, &h, &p, &r) != 6) return;
    u->placed = true;
    u->pos[0] = x, u->pos[1] = y, u->pos[2] = z;
    u->heading = h, u->pitch = p, u->roll = r;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    tracker::Unit units[tracker::kMaxUnits];
    std::vector<Draw> draws;
    char line[4096];
    int cfg_unit = -1, row = -1;
    bool in_cb0 = false;
    bool diag = false;
    // Flags after the key mode: debug, noplace (no telemetry placements), norun, noaxle.
    bool no_place = false;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "debug")) g_debug = true;
        if (!strcmp(argv[i], "noplace")) no_place = true;
        if (!strcmp(argv[i], "norun")) g_no_run = true;
        if (!strcmp(argv[i], "noaxle")) g_no_axle = true;
        if (!strcmp(argv[i], "passing")) g_inject = 1;
        if (!strcmp(argv[i], "stopped")) g_inject = 2;
    }
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "# ", 2) && strstr(line, " frame(s):") && atoi(line + 2) > 1) diag = true;
        if (!strncmp(line, "truck.world.placement", 21) && !no_place) ParsePlacement(line, &units[0]);
        if (!strncmp(line, "trailer.", 8) && strstr(line, ".world.placement") && !no_place) {
            const int i = atoi(line + 8);
            if (1 + i < tracker::kMaxUnits) ParsePlacement(line, &units[1 + i]);
        }
        if (!strncmp(line, "config truck:", 13)) cfg_unit = 0;
        else if (!strncmp(line, "config trailer.", 15)) cfg_unit = 1 + atoi(line + 15);
        else if (!strncmp(line, "config ", 7) || line[0] == '#') cfg_unit = -1;
        if (cfg_unit >= 0) {
            int k;
            float x, y, z;
            if (sscanf(line, "  wheel.position[%d] = (%f %f %f)", &k, &x, &y, &z) == 4 && k < tracker::kMaxWheels) {
                units[cfg_unit].wheel[k][0] = x, units[cfg_unit].wheel[k][1] = y, units[cfg_unit].wheel[k][2] = z;
                if (k + 1 > units[cfg_unit].wheel_count) units[cfg_unit].wheel_count = k + 1;
            }
        }
        if (!strncmp(line, "imm ", 4)) {
            Draw d;
            const char* t6 = strstr(line, "t6=");
            d.tex = t6 ? std::string(t6 + 3, strcspn(t6 + 3, " ")) : "";
            const char* fi = strstr(line, "0:");
            fi = fi ? strstr(fi, "first ") : nullptr;
            d.first = fi ? atoi(fi + 6) : 0;
            draws.push_back(d);
            in_cb0 = false;
            row = -1;
        } else if (!strncmp(line, "  cb0", 5)) {
            in_cb0 = true;
        } else if (!strncmp(line, "  cb1", 5)) {
            in_cb0 = false;
        } else if (in_cb0 && !draws.empty() && sscanf(line, "    [%d]", &row) == 1 && row <= 2) {
            float a, b, c, w;
            const char* br = strchr(line, ']');
            if (br && sscanf(br + 1, "%f %f %f %f", &a, &b, &c, &w) == 4) draws.back().p[row] = w;
        }
    }
    fclose(f);
    for (int u = 0; u < tracker::kMaxUnits; ++u)
        if (units[u].wheel_count) printf("unit %d: %d wheels\n", u, units[u].wheel_count);
    if (diag) {
        // argv[2] = "rtv" replays with the R4b key; default is the viewport.
        const bool by_viewport = !(argc > 2 && !strcmp(argv[2], "rtv"));
        return ReplayDiag(argv[1], by_viewport, units);
    }

    // Cameras: the game draws trailer wheels, then truck wheels, per camera;
    // a trailer draw right after a truck draw starts the next camera (the
    // plugin itself keys cameras by render target).
    static int camera_ids[64];
    int cam = 0;
    for (size_t i = 0; i < draws.size(); ++i) {
        const bool trl = draws[i].tex.find("trail55") != std::string::npos;
        if (i && trl && draws[i - 1].tex.find("trail55") == std::string::npos) ++cam;
        draws[i].camera = &camera_ids[cam];
    }

    tracker::SetUnits(units, tracker::kMaxUnits);
    tracker::EndFrame();  // applies the units
    for (int frame = 1; frame <= 3; ++frame) {
        printf("-- frame %d\n", frame);
        for (const auto& d : draws) {
            int wheel = -1;
            const int u = tracker::Classify(d.camera, d.p, nullptr, 0, nullptr, &wheel);
            printf("  cam %d first %5d %-6s (%7.3f %7.3f %7.3f) -> unit %d wheel %d\n",
                   int(static_cast<const int*>(d.camera) - camera_ids), d.first,
                   d.tex.find("trail55") != std::string::npos ? "trl55" : "trk70", d.p[0], d.p[1], d.p[2], u, wheel);
        }
        if (frame == 3) {
            // Same truck layout, 6 m to the side, in camera 0: must stay -1.
            int claimed = 0;
            for (const auto& d : draws)
                if (d.camera == &camera_ids[0] && d.tex.find("trail70") != std::string::npos) {
                    const float q[3] = {d.p[0] + 6.f, d.p[1], d.p[2]};
                    claimed += tracker::Classify(d.camera, q) >= 0;
                }
            printf("  traffic copy of the truck: %d draw(s) claimed (expected 0)\n", claimed);
        }
        tracker::EndFrame();
        const auto s = tracker::GetStats();
        printf("  stats: claimed %lld unclaimed %lld searches %lld found %lld mirror-fixed %lld turned %lld\n", s.claimed,
               s.unclaimed,
               s.searches, s.found, s.flipped, s.turned);
    }
    return 0;
}
