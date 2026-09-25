/* THE VEHICLE MOTION SCOREBOARD, one vehicle per run: open it on a flat plane (a
 * seabed under water for boats), drive a fixed input sweep that exercises every
 * control the step takes, and write bf6_vehicle_motion_report to a JSON file.
 *   vehicle_motion_probe <vehicle dir> <out.json>
 * tools/vehicle_motion_score.py runs it over the fleet and totals the pieces.
 *
 * The sweep, 60 steps a second (aircraft then take off, see below): idle 1 s; throttle 2 s; throttle with full left
 * steer 1 s, then full right 1 s; brake 1 s; handbrake 1 s; throttle with boost 1 s;
 * cyclic pitch, roll, then both reversed, 1 s each. */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: vehicle_motion_probe <vehicle dir> <out.json>\n"); return 2; }
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx || !bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "%s\n", err); return 2; }
    const std::string dir = argv[1];
    const bool boat = dir.find("/boat/") != std::string::npos;
    const float ground_y = boat ? -20.0f : 0.0f;
    const float s = 2000.0f;
    const float tris[18] = {-s, ground_y, -s, s, ground_y, -s, s, ground_y, s,
                            -s, ground_y, -s, s, ground_y, s, -s, ground_y, s};
    bf6_vehicle* v = bf6_vehicle_open_ex(ctx, dir.c_str(), tris, 6, BF6_VEHICLE_OPEN_PRESENTATION,
                                         err, (int32_t)sizeof(err));
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    if (!v) {
        std::string e = err;
        for (char& ch : e) if (ch == '"' || ch == '\\' || (unsigned char)ch < 0x20) ch = ' ';
        std::fprintf(f, "{\"dir\":\"%s\",\"error\":\"%s\"}", dir.c_str(), e.c_str());
        std::fclose(f);
        std::printf("MOTION %s refused: %s\n", dir.c_str(), err);
        return 0;
    }
    if (boat) bf6_vehicle_set_water(v, 0.0f, 1);
    float out[40] = {};
    /* AIRCRAFT GET A TAKE-OFF after the common sweep, so the parts that only move in
     * flight (landing gear, gear doors) are exercised: throttle 14 s, nose up
     * (pitch -0.4 on this rig, measured on the F-16) from 15 s to 20 s, then level.
     * Helicopters get the same throttle, which is their collective, to spin up. */
    const bool air = dir.find("/airplane/") != std::string::npos || dir.find("/helicopter/") != std::string::npos;
    const int seconds = air ? 25 : 11;
    for (int step = 0; step < 60 * seconds; ++step) {
        const int sec = step / 60;
        float in[9] = {0, 0, 0, 0, 1.0f / 60.0f, 0, 0, 0, 0};
        float pitch = 0, roll = 0, boost = 0;
        if (sec >= 11) {
            in[0] = 1;
            if (sec >= 15 && sec < 20 && dir.find("/airplane/") != std::string::npos) pitch = -0.4f;
        }
        switch (sec) {
            case 1: case 2: in[0] = 1; break;
            case 3: in[0] = 1; in[2] = -1; break;
            case 4: in[0] = 1; in[2] = 1; break;
            case 5: in[1] = 1; break;
            case 6: in[3] = 1; break;
            case 7: in[0] = 1; boost = 1; break;
            case 8: pitch = 0.5f; break;
            case 9: roll = 0.5f; break;
            case 10: pitch = -0.5f; roll = -0.5f; break;
            default: break;
        }
        /* the driver's aim sweeps the turret: +-1 rad of yaw, +-0.15 rad of pitch */
        const float t = (float)step / 60.0f;
        bf6_vehicle_set_aim(v, std::sin(t * 0.8f), 0.15f * std::sin(t * 1.3f));
        bf6_vehicle_set_cyclic(v, pitch, roll);
        bf6_vehicle_set_boost(v, boost);
        bf6_vehicle_step(v, in, out);
    }
    const int32_t n = bf6_vehicle_motion_report(v, nullptr, 0);
    std::vector<char> buf((size_t)n + 1);
    bf6_vehicle_motion_report(v, buf.data(), n + 1);
    std::fprintf(f, "{\"dir\":\"%s\",\"report\":%s}", dir.c_str(), buf.data());
    std::fclose(f);
    std::printf("MOTION %s ok (%d bytes)\n", dir.c_str(), n);
    bf6_vehicle_close(v);
    return 0;
}
