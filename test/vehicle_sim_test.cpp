/* Tick a vehicle's own drivetrain + suspension graphs frame by frame.
 *
 *   vehicle_sim_test [frames=120]
 *
 * flyer60 on a flat ground plane at y = 0, full throttle with a driver present.
 * Prints, per frame, what the graphs publish. The vehicle does not move: the force
 * solver is native and not modelled - this shows what the graphs themselves do. */
#include "bf6_core.h"
#include "vehicle_sim.h"
#include "vehicle_dynamics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int trace_scene(void* user, const double* from, const double* to, double* hit, double* normal) {
    bf6_ray_scene* s = (bf6_ray_scene*)user;
    double out[7] = {};
    if (bf6_ray_scene_trace(s, from, to, out) < 0) return 0;
    for (int i = 0; i < 3; ++i) { hit[i] = out[i]; normal[i] = out[3 + i]; }
    return 1;
}

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 120;
    const char* game = std::getenv("BF6_GAME") ? std::getenv("BF6_GAME")
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Battlefield 6";
    char err[1024] = {};
    bf6_ctx* ctx = bf6_open(game, err, (int)sizeof(err));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 2; }
    if (!bf6_mount_all(ctx, 1, err, (int)sizeof(err))) { std::fprintf(stderr, "mount: %s\n", err); return 2; }
    bf6_ray_scene* scene = bf6_ray_scene_create();
    const float verts[12] = {-200.f, 0.f, -200.f, 200.f, 0.f, -200.f, 200.f, 0.f, 200.f, -200.f, 0.f, 200.f};
    const int32_t tris[6] = {0, 1, 2, 0, 2, 3};
    const double ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    bf6_ray_scene_add_instance(scene, bf6_ray_scene_add_mesh(scene, verts, 4, tris, 6), ident);

    bf6::VehicleSim sim;
    std::string e;
    const std::string exe = std::string(game) + "\\bf6.exe";
    const std::vector<std::string> graphs = {
        "common/hardware/vehicles/car/flyer60/simex_car_flyer60",
        "common/hardware/vehicles/car/flyer60/presex_flyer60_suspensionmovement_1"};
    if (!sim.open(ctx, exe, graphs, "common/hardware/vehicles/car/flyer60/art/ske_veh_car_flyer60_base",
                  trace_scene, scene, e)) {
        std::fprintf(stderr, "sim: %s\n", e.c_str());
        return 3;
    }
    const float root[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    sim.set_root(root);

    auto debug_dump = [&]() {
    /* BF6_CHDUMP=1: every channel written or read, with mode, value and counts. */
    if (std::getenv("BF6_CHDUMP")) {
        const auto names = sim.channel_name_map();
        auto nm = [&](uint64_t k) {
            const auto it = names.find((uint32_t)k);
            return it == names.end() ? std::string("?") : it->second;
        };
        for (const auto& kv : sim.state().channels()) {
            std::printf("CH %-32s mode %u  writes %u  =", nm(kv.first).c_str(), (unsigned)(kv.first >> 32),
                        sim.state().channel_writes().count(kv.first) ? sim.state().channel_writes().at(kv.first) : 0u);
            for (size_t i = 0; i + 4 <= kv.second.size() && i < 16; i += 4) {
                float f; std::memcpy(&f, kv.second.data() + i, 4);
                std::printf(" %g", f);
            }
            std::printf("\n");
        }
        for (const auto& kv : sim.state().unsupplied_channels())
            std::printf("UNSUPPLIED %-32s mode %u  reads %u\n", nm(kv.first).c_str(), (unsigned)(kv.first >> 32), kv.second);
    }
    if (const char* ls = std::getenv("BF6_LIST")) {
        char g[128] = {}; unsigned lo = 0, hi = 0;
        if (std::sscanf(ls, "%127[^:]:%x-%x", g, &lo, &hi) == 3) std::printf("%s", sim.list(g, lo, hi).c_str());
    }
    if (const char* sr = std::getenv("BF6_SRC"))
        std::printf("%s", sim.sources((uint32_t)std::strtoul(sr, nullptr, 16)).c_str());
    if (std::getenv("BF6_INSTLAYOUT")) std::printf("%s", sim.instance_layout().c_str());
    if (const char* rr = std::getenv("BF6_REC"))
        std::printf("%s", sim.record_info((uint32_t)std::strtoul(rr, nullptr, 16)).c_str());
    /* BF6_TOUCH=region:lo-hi (hex): records naming those operands. */
    if (const char* t = std::getenv("BF6_TOUCH")) {
        unsigned r = 2, lo = 0, hi = 0;
        if (std::sscanf(t, "%u:%x-%x", &r, &lo, &hi) == 3) std::printf("%s", sim.touch(r, lo, hi).c_str());
    }
    /* BF6_WHY_CH=Name,Name: why each channel is unknown after the last frame. */
    if (const char* w = std::getenv("BF6_WHY_CH")) {
        std::string list = w;
        size_t at = 0;
        while (at <= list.size()) {
            const size_t c = list.find(',', at);
            const std::string nm = list.substr(at, c == std::string::npos ? std::string::npos : c - at);
            if (!nm.empty()) std::printf("%s", sim.why(nm).c_str());
            if (c == std::string::npos) break;
            at = c + 1;
        }
    }
    };
    /* BF6_DRIVE=1: close the loop. The graphs' outputs drive VehicleDynamics, whose
     * wheel spin, velocity and pose go back through the channels the native code
     * would write. BF6_STEER=<-1..1>, BF6_THROTTLE=<0..1>, BF6_BRAKE_AT=<frame>. */
    if (std::getenv("BF6_DRIVE")) {
        bf6::VehicleDynamics dyn;
        bf6_skeleton* sk = bf6_skeleton_read(ctx, "common/hardware/vehicles/car/flyer60/art/ske_veh_car_flyer60_base");
        const char* loc[4] = {"loc_Wheel_FrontLeft", "loc_Wheel_FrontRight", "loc_Wheel_RearLeft", "loc_Wheel_RearRight"};
        int found = 0;
        for (int b = 0; sk && b < sk->bone_count; ++b)
            for (int w = 0; w < 4; ++w)
                if (sk->bones[b].name && std::strcmp(sk->bones[b].name, loc[w]) == 0) {
                    for (int i = 0; i < 3; ++i) dyn.p.wheel[w].mount[i] = sk->bones[b].model[9 + i];
                    ++found;
                }
        if (sk) bf6_free(ctx, sk);
        if (found != 4) { std::fprintf(stderr, "wheel locators: found %d of 4\n", found); return 4; }
        dyn.init();
        std::printf("%s", dyn.describe_params().c_str());
        const float thr = std::getenv("BF6_THROTTLE") ? (float)std::atof(std::getenv("BF6_THROTTLE")) : 1.0f;
        const float steer_in = std::getenv("BF6_STEER") ? (float)std::atof(std::getenv("BF6_STEER")) : 0.0f;
        const int brake_at = std::getenv("BF6_BRAKE_AT") ? std::atoi(std::getenv("BF6_BRAKE_AT")) : 1 << 30;
        /* compression channel per wheel, in the order FL FR RL RR (A = front axle,
         * 1 = left: an assumption checked by the roll test in the notes) */
        const char* compch[4] = {"SpringCompression_A1", "SpringCompression_A2",
                                 "SpringCompression_B1", "SpringCompression_B2"};
        const char* wch[4] = {"AngularVelocity_Wheel_FL", "AngularVelocity_Wheel_FR",
                              "AngularVelocity_Wheel_RL", "AngularVelocity_Wheel_RR"};
        const float dt = 1.0f / 60.0f;
        for (int f = 0; f < frames; ++f) {
            const bool braking = f >= brake_at;
            sim.set_bool("EntryActive_Driver", true);
            sim.set_float("InputThrottle", braking ? 0.0f : thr);
            sim.set_float("InputBrake", braking ? 1.0f : 0.0f);
            sim.set_float("InputYaw", steer_in);
            sim.set_bool("Handbrake", false);
            sim.set_time(f * (double)dt);
            float rows[16];
            dyn.root_rows(rows);
            sim.set_root(rows);
            for (int w = 0; w < 4; ++w) sim.set_float(wch[w], dyn.omega[w]);
            /* The per-wheel spin the drivetrain reads: frame-relative state field 5,
             * index = wheel (0xFFFF0005 + 0x100 * i), in the root frame and in the
             * feature frame the graph pushes (0x1671 on the flyer60). Native code
             * writes these in the game; nothing in the graphs does. */
            for (int w = 0; w < 4; ++w) {
                uint32_t bits;
                std::memcpy(&bits, &dyn.omega[w], 4);
                /* descriptors {0xFFFFii05, 0xFFFFFFFF, 0, 3} at pool 0x860..0x890 */
                sim.state_mut().set_frame_cell(0u, 0xFFFF0005u + 0x100u * (uint32_t)w, 0u, 3u, bits);
                sim.state_mut().set_frame_cell(0x1671u, 0xFFFF0005u + 0x100u * (uint32_t)w, 0u, 3u, bits);
                /* field 4 of the same element: the wheel's ground-contact bit (the
                 * gearbox refuses to shift above 1st while no wheel touches) */
                const uint32_t contact = dyn.fz[w] > 0.0f ? 1u : 0u;
                sim.state_mut().set_frame_cell(0u, 0xFFFF0005u + 0x100u * (uint32_t)w, 0u, 4u, contact);
                sim.state_mut().set_frame_cell(0x1671u, 0xFFFF0005u + 0x100u * (uint32_t)w, 0u, 4u, contact);
            }
            sim.set_vec3("LinearVelocity", dyn.vel, 0);
            sim.set_vec3("AngularVelocity", dyn.angvel, 0);
            sim.set_vec3("LinearAcceleration", dyn.last_accel, 0);
            sim.tick();
            bf6::VehicleDriveInputs in;
            bool k = false;
            in.throttle = sim.get_float("VehicleThrottle", &k);
            in.brake = sim.get_float("VehicleBrake", &k);
            in.gear_ratio = sim.get_float("GearRatio", &k);
            in.clutch = sim.get_float("Clutch", &k);
            in.steer = sim.get_float("SteeringAngle", &k);
            for (int w = 0; w < 4; ++w) in.compression[w] = sim.get_float(compch[w], &in.compression_known[w]);
            dyn.step(dt, in);
            if (f % 30 == 0 || f == frames - 1)
                std::printf("t %5.2f  pos (%7.2f %5.2f %7.2f)  speed %6.2f m/s  thr %.2f brk %.2f gear %.3f clutch %.2f "
                            "rpm %6.0f steer %.2f  omega FL %.1f RR %.1f  Fz %.0f %.0f %.0f %.0f\n",
                            f * dt, dyn.pos[0], dyn.pos[1], dyn.pos[2], dyn.forward_speed(), in.throttle, in.brake,
                            in.gear_ratio, in.clutch, sim.get_float("RPM", &k), in.steer, dyn.omega[0], dyn.omega[3],
                            dyn.fz[0], dyn.fz[1], dyn.fz[2], dyn.fz[3]);
        }
        debug_dump();
        bf6_close(ctx);
        return 0;
    }
    const char* outs[] = {"IgnitionSequenceActive", "VehicleThrottle", "VehicleBrake", "RPM", "GearRatio", "Clutch",
                          "EngineLoadNormalizedSigned", "SpringCompression_A1", "SpringCompression_B1"};
    for (int f = 0; f < frames; ++f) {
        /* BF6_ENTER_AT=<frame>: the driver enters at that frame (an entry edge). */
        const int enter_at = std::getenv("BF6_ENTER_AT") ? std::atoi(std::getenv("BF6_ENTER_AT")) : 0;
        sim.set_bool("EntryActive_Driver", f >= enter_at);
        sim.set_float("InputThrottle", 1.0f);
        sim.set_float("InputBrake", 0.0f);
        sim.set_bool("Handbrake", false);
        sim.tick();
        if (f == 0) std::printf("%s", sim.report().c_str());
        if (f < 5 || f % 20 == 0 || f == frames - 1) {
            std::printf("frame %3d", f);
            for (const char* o : outs) {
                bool k = false;
                const float v = sim.get_float(o, &k);
                if (k) std::printf("  %s=%g", o, v); else std::printf("  %s=?", o);
            }
            std::printf("\n");
        }
    }
    debug_dump();
    bf6_close(ctx);
    return 0;
}
