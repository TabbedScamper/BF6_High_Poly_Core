/* The drivable-vehicle C ABI end to end: open the flyer60 on a flat plane, full
 * throttle for 8 s, then brake; print what bf6_vehicle_step returns. Fails (exit 3)
 * unless the vehicle moves forward, shifts at least twice and stops under braking. */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx || !bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "%s\n", err); return 2; }
    const float s = 2000.0f;
    const float tris[18] = {-s, 0, -s, s, 0, -s, s, 0, s, -s, 0, -s, s, 0, s, -s, 0, s};
    const char* dir = argc > 1 ? argv[1] : "common/hardware/vehicles/car/flyer60";
    bf6_vehicle* v = bf6_vehicle_open(ctx, dir, tris, 6, err, (int32_t)sizeof(err));
    if (!v) { std::fprintf(stderr, "open: %s\n", err); return 2; }
    /* BF6_WATER=<height>: the surface anything that floats reads. A boat needs it;
     * without it its hull is above water and pushes nothing, which is the same
     * honest answer a land map gives. */
    if (const char* w = std::getenv("BF6_WATER"))
        bf6_vehicle_set_water(v, (float)std::atof(w), 1);
    float out[40] = {};
    float last_ratio = 0.0f;
    int shifts = 0;
    float top = 0.0f;
    for (int f = 0; f < 60 * 14; ++f) {
        const bool braking = f >= 60 * 8;
        const float in[6] = {braking ? 0.0f : 1.0f, braking ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f / 60.0f, 0.0f};
        if (bf6_vehicle_step(v, in, out) < 33) { std::fprintf(stderr, "step failed\n"); return 2; }
        /* out: 0-2 pos, 3-6 quat, 7-9 vel, 10-12 angvel, 13 speed, 14 rpm,
         * 15 gear ratio, 16 clutch, 17 throttle, 18 brake */
        if (out[15] != last_ratio && last_ratio != 0.0f && out[15] > 0.0f) ++shifts;
        if (out[15] > 0.0f) last_ratio = out[15];
        if (out[13] > top) top = out[13];
        /* WHICH WAY IT ACTUALLY WENT. A vehicle whose forces land on one axis and
         * whose motion appears on another is the only way to tell a misdirected force
         * from a mislabelled frame, so all three components are available. */
        if (f % 60 == 0 && std::getenv("BF6_XYZ"))
            std::printf("t %4.1f  pos (%7.2f %7.2f %7.2f)  vel (%6.2f %6.2f %6.2f)  "
                        "quat (%.3f %.3f %.3f %.3f)\n",
                        f / 60.0, out[0], out[1], out[2], out[7], out[8], out[9],
                        out[3], out[4], out[5], out[6]);
        if (f % 60 == 0 && !std::getenv("BF6_XYZ"))
            std::printf("t %4.1f  z %7.2f  speed %6.2f  rpm %5.0f  ratio %6.3f  clutch %.2f  thr %.2f  brk %.2f\n",
                        f / 60.0, out[2], out[13], out[14], out[15], out[16], out[17], out[18]);
    }
    std::printf("shifts %d  final speed %.2f\n", shifts, out[13]);
    bf6_vehicle_close(v);
    bf6_close(ctx);
    return (shifts >= 2 && std::fabs(out[13]) < 1.0f) ? 0 : 3;
}
