/* THE WALKER'S NUMBERS COME FROM THE GAME.
 *
 *   walk_tuning_test <game_dir>
 *
 * Eight authored values were sitting decoded in a findings file while the
 * walker ran on the host engine's character defaults. This checks they are READ
 * from the install, and - the half that actually matters - that reading them
 * CHANGES WHAT THE WALKER DOES. A tuning struct that is filled correctly and
 * then ignored looks identical from the outside to one that works.
 *
 * The speeds are deliberately NOT checked against the game: BF6 does not author
 * an absolute walk or sprint speed anywhere, so those stay the engine's and
 * saying otherwise would be the lie this whole exercise is meant to avoid.
 */
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int fails = 0;
void bad(const char* what) { std::printf("  FAIL %s\n", what); ++fails; }

/* A floor at y=0 with a ledge of `ledge` height beyond z=2, as triangles. */
std::vector<float> world(float ledge)
{
    std::vector<float> t = {
        -20, 0, -20,   20, 0, -20,   20, 0, 20,
        -20, 0, -20,   20, 0, 20,   -20, 0, 20,
    };
    const float q[18] = {
        -20, ledge, 2,   20, ledge, 2,   20, ledge, 20,
        -20, ledge, 2,   20, ledge, 20,  -20, ledge, 20,
    };
    t.insert(t.end(), q, q + 18);
    return t;
}

/* Walk forward for `seconds` and report the EYE HEIGHT the walker ends at.
 *
 * Height, not distance - which the first version of this test got wrong. The
 * ledge is a flat shelf with no vertical face, so a walker that cannot climb it
 * simply passes underneath and covers exactly the same ground. Both tunings
 * reached z = 10.250 and the test called that a pass. What separates them is
 * whether the walker ends up ON the shelf. */
double walk_height(bf6_ray_scene* scene, const bf6_walk_tuning* tuning, double seconds)
{
    bf6_walk_state st{};
    st.pos[0] = 0; st.pos[1] = 1.72; st.pos[2] = 0;
    st.eye = 1.72;
    bf6_walk_input in{};
    in.wish[2] = 1.0;
    in.dt = 1.0 / 60.0;
    for (int i = 0; i < (int)(seconds * 60); ++i)
        bf6_walk_step_scene_tuned(&st, &in, tuning, scene);
    return st.pos[1];
}

bf6_ray_scene* scene_of(const std::vector<float>& tris)
{
    bf6_ray_scene* s = bf6_ray_scene_create();
    if (!s) return nullptr;
    std::vector<int32_t> idx(tris.size() / 3);
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int32_t)i;
    const int32_t mesh = bf6_ray_scene_add_mesh(s, tris.data(), (int32_t)(tris.size() / 3),
                                                idx.data(), (int32_t)idx.size());
    const double I[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    if (mesh < 0 || bf6_ray_scene_add_instance(s, mesh, I) < 0) { bf6_ray_scene_free(s); return nullptr; }
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: walk_tuning_test <game>\n"); return 2; }
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    bf6_walk_tuning t{};
    const int found = bf6_walk_tuning_read(c, &t);
    std::printf("read %d value(s) from the install\n", found);
    std::printf("   step height            %.4f\n", t.step_height);
    std::printf("   jump horizontal cap    %.4f\n", t.jump_horizontal_cap);
    std::printf("   jump min for impulse   %.4f\n", t.jump_min_speed_for_impulse);
    std::printf("   landing penalty        %.4f (floor %.3f, recover %.3f/s)\n",
                t.landing_penalty_strength, t.landing_penalty_floor,
                t.landing_recovery_per_second);
    std::printf("   slide needs            %.4f m/s\n", t.slide_needed_velocity);
    std::printf("   vault max height       %.4f\n", t.vault_max_height);
    std::printf("   field of view          %.1f vertical (%.0f..%.0f)\n",
                t.fov_vertical, t.fov_vertical_min, t.fov_vertical_max);
    std::printf("   speeds                 crouch %.3f, forward %.3f, sprint %.3f m/s\n",
                t.crouch_speed, t.walk_speed, t.run_speed);

    /* ---- THE SPEEDS ARE MEASURED, NOT INHERITED ---------------------------
     * These used to be the host engine's character defaults (1.4 / 2.6 / 6.0)
     * because no partition states them. They are now read off the game's own
     * locomotion cycles - root displacement over cycle duration, which is what
     * a motion-matched gait's speed is. The check is that they are the GAME's
     * numbers and ordered like gaits, not that they equal any constant. */
    bf6_walk_tuning engine;
    bf6_walk_tuning_defaults(&engine);
    if (std::fabs(t.walk_speed - engine.walk_speed) < 1e-4
        && std::fabs(t.run_speed - engine.run_speed) < 1e-4)
        bad("the speeds are still the engine's defaults - nothing was measured");
    if (!(t.crouch_speed < t.walk_speed && t.walk_speed < t.run_speed))
        bad("the gaits are not ordered crouch < forward < sprint");
    if (t.walk_speed < 2.0f || t.walk_speed > 6.0f)
        bad("the forward speed is not a plausible human pace");
    if (t.run_speed < 4.0f || t.run_speed > 12.0f)
        bad("the sprint speed is not a plausible human pace");

    if (found < 8) bad("fewer than eight authored values were read from the install");
    /* The numbers as decoded. A mismatch means the install retuned them, which
     * is worth seeing rather than silently walking differently. */
    if (std::fabs(t.step_height - 0.32) > 1e-4) bad("StepHeight is not the 0.32 this was built against");
    if (std::fabs(t.jump_horizontal_cap - 8.0) > 1e-4) bad("Jump_HorizontalVelocityCap moved");
    if (std::fabs(t.fov_vertical - 59.0) > 1e-4) bad("the default field of view moved");
    if (t.fov_vertical_min >= t.fov_vertical_max) bad("the field of view range is inverted");

    /* ---- the step height actually bites ----------------------------------
     * A 0.40 m ledge is under the old 0.45 default and over the authored 0.32.
     * With the game's value the walker must be STOPPED by it; with the engine's
     * it walked straight over. Same world, same input, two tunings. */
    std::vector<float> tris = world(0.40f);
    bf6_ray_scene* scene = scene_of(tris);
    if (!scene) { std::printf("FAIL could not build the test world\n"); return 1; }

    bf6_walk_tuning loose;
    bf6_walk_tuning_defaults(&loose);
    loose.step_height = 0.45f;              /* what it used to be */

    const double with_game = walk_height(scene, &t, 4.0);
    const double with_engine = walk_height(scene, &loose, 4.0);
    std::printf("\na 0.40 m shelf: authored step %.2f ends at eye y = %.3f, "
                "engine step %.2f ends at eye y = %.3f\n",
                t.step_height, with_game, loose.step_height, with_engine);
    /* On the shelf is 1.72 + 0.40 = 2.12; under it is 1.72. */
    if (with_game > 1.9)
        bad("the authored 0.32 step climbed a 0.40 m shelf - the tuning is not being used");
    if (with_engine < 1.9)
        bad("the 0.45 step did NOT climb it either, so this proves nothing about the change");

    /* ---- a standing jump goes straight up --------------------------------
     * Below Jump_MinSpeedForHorizontalImpulse there is no horizontal carry. */
    {
        bf6_walk_state st{};
        st.pos[1] = 1.72; st.eye = 1.72; st.grounded = 1;
        bf6_walk_input in{};
        in.dt = 1.0 / 60.0;
        in.jump = 1;
        in.wish[2] = 1.0;                 /* leaning forward, but barely moving */
        bf6_walk_step_scene_tuned(&st, &in, &t, scene);
        in.jump = 0;
        for (int i = 0; i < 30; ++i) bf6_walk_step_scene_tuned(&st, &in, &t, scene);
        const double drift = std::sqrt(st.pos[0] * st.pos[0] + st.pos[2] * st.pos[2]);
        std::printf("standing jump drifted %.3f m in half a second\n", drift);
        if (drift > 1.0)
            bad("a standing jump carried a full run's worth of momentum");
    }

    bf6_ray_scene_free(scene);
    bf6_close(c);
    std::printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
