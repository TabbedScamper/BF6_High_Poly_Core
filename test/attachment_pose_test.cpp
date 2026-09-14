/* Runtime controls for configured weapon attachment placement.
 *
 * Proves full Binding preservation, exact gameplay-bone identity, selected
 * dependency variants, one-source placement (no duplicated binding + anchor
 * write), and a fabricated-fit no-op. No exported table is consumed. */
#include "bf6_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string find_m4_md(bf6_ctx* c)
{
    const int n = bf6_list_ebx(c, "md_m4a1", nullptr, 0);
    if (n <= 0) return {};
    std::vector<bf6_asset> rows((size_t)n);
    const int got = bf6_list_ebx(c, "md_m4a1", rows.data(), n);
    std::string best;
    for (int i = 0; i < got; ++i)
    {
        const std::string s = rows[(size_t)i].name ? rows[(size_t)i].name : "";
        if (s.find("/md_m4a1") == std::string::npos ||
            s.find("_bundle") != std::string::npos) continue;
        if (best.empty() || s.size() < best.size()) best = s;
    }
    return best;
}

struct Row {
    std::string key;
    float m[12]{};
    int has = 0;
    std::string bone_partition, bone_instance;
    std::string bone_path;
};

struct Snapshot {
    std::vector<Row> rows;
    std::vector<bf6_bone_xform> skin;
};

static Snapshot read(bf6_ctx* c, const std::string& md,
                     const bf6_weapon_fit* fit, int fit_count = -1)
{
    std::vector<bf6_weapon_part_pose> raw(256);
    Snapshot out;
    out.skin.resize(128);
    int bone_count = 0;
    const int nfit = fit_count >= 0 ? fit_count : (fit ? 1 : 0);
    const int n = bf6_weapon_configured_assembly(
        c, md.c_str(), fit, nfit, raw.data(), (int)raw.size(),
        out.skin.data(), (int)out.skin.size(), &bone_count);
    if (n < 0 || n > (int)raw.size()) return out;
    if (bone_count <= 0 || bone_count > (int)out.skin.size()) out.skin.clear();
    else out.skin.resize((size_t)bone_count);
    for (int i = 0; i < n; ++i)
    {
        Row r;
        r.key = std::string(raw[(size_t)i].mesh ? raw[(size_t)i].mesh : "") +
                "\n" + (raw[(size_t)i].bundle ? raw[(size_t)i].bundle : "");
        std::memcpy(r.m, raw[(size_t)i].attach_transform, sizeof(r.m));
        r.has = raw[(size_t)i].has_attach_transform;
        r.bone_partition = raw[(size_t)i].gameplay_bone_partition;
        r.bone_instance = raw[(size_t)i].gameplay_bone_instance;
        r.bone_path = raw[(size_t)i].gameplay_bone_path;
        out.rows.push_back(r);
    }
    std::sort(out.rows.begin(), out.rows.end(), [](const Row& a, const Row& b) {
        return a.key < b.key;
    });
    return out;
}

static bool exact_equal(const std::vector<Row>& a, const std::vector<Row>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].key != b[i].key || a[i].has != b[i].has ||
            std::memcmp(a[i].m, b[i].m, sizeof(a[i].m)) != 0 ||
            a[i].bone_partition != b[i].bone_partition ||
            a[i].bone_instance != b[i].bone_instance ||
            a[i].bone_path != b[i].bone_path) return false;
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::printf("usage: attachment_pose_test <game_dir>\n"); return 2; }
    char err[512]{};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c || !bf6_mount_all(c, 1, err, (int)sizeof(err)))
    { std::printf("mount failed: %s\n", err); return 1; }
    const std::string md = find_m4_md(c);
    if (md.empty()) { std::printf("md_m4a1 not found\n"); bf6_close(c); return 1; }

    bf6_weapon_fit canted{ "sca", "cantedreflex" };
    bf6_weapon_fit fake{ "sca", "absent_fake_attachment_7f93" };
    const Snapshot base = read(c, md, nullptr);
    const Snapshot control = read(c, md, &fake);
    const Snapshot fitted = read(c, md, &canted);
    bf6_weapon_fit short_fits[2] = {
        { "brl", "shortbarrel" }, { "mzl", "m4qdflashhider" }
    };
    bf6_weapon_fit extended_fits[2] = {
        { "brl", "extendedbarrel" }, { "mzl", "m4qdflashhider" }
    };
    const Snapshot short_barrel = read(c, md, short_fits, 2);
    const Snapshot extended_barrel = read(c, md, extended_fits, 2);
    bf6_weapon_fit comp_piggyback[2] = {
        { "scp", "compm5b" }, { "sca", "qmk171areflex" }
    };
    bf6_weapon_fit pg_piggyback[2] = {
        { "scp", "pg350" }, { "sca", "qmk171areflex" }
    };
    const Snapshot comp_secondary = read(c, md, comp_piggyback, 2);
    const Snapshot pg_secondary = read(c, md, pg_piggyback, 2);
    for (const Row& r : short_barrel.rows)
        if (r.key.find("barrel") != std::string::npos ||
            r.key.find("m4qd") != std::string::npos)
            std::printf("short selected %s\n", r.key.c_str());
    for (const Row& r : extended_barrel.rows)
        if (r.key.find("barrel") != std::string::npos ||
            r.key.find("m4qd") != std::string::npos)
            std::printf("extended selected %s\n", r.key.c_str());

    bool found = false, finite = true, has_guid = false;
    bool canted_single_source = true;
    float basis_delta = 0.f;
    for (const Row& r : fitted.rows)
    {
        if (r.key.find("canted") == std::string::npos &&
            r.key.find("offset") == std::string::npos) continue;
        found = true;
        for (float v : r.m) finite = finite && std::isfinite(v);
        const float identity[9] = { 1,0,0, 0,1,0, 0,0,1 };
        for (int i = 0; i < 9; ++i)
            basis_delta = std::max(basis_delta, std::fabs(r.m[i] - identity[i]));
        has_guid = !r.bone_partition.empty() && !r.bone_instance.empty();
        canted_single_source = canted_single_source && !r.has;
        std::printf("canted row %s\n", r.key.c_str());
        std::printf("basis [% .6f % .6f % .6f] [% .6f % .6f % .6f] "
                    "[% .6f % .6f % .6f] trans [% .6f % .6f % .6f]\n",
                    r.m[0],r.m[1],r.m[2],r.m[3],r.m[4],r.m[5],
                    r.m[6],r.m[7],r.m[8],r.m[9],r.m[10],r.m[11]);
        std::printf("bone partition=%s instance=%s path=%s\n",
                    r.bone_partition.c_str(), r.bone_instance.c_str(),
                    r.bone_path.c_str());
    }
    const bool fake_noop = exact_equal(base.rows, control.rows);
    bool fake_skin_noop = base.skin.size() == control.skin.size();
    if (fake_skin_noop && !base.skin.empty())
        fake_skin_noop = std::memcmp(base.skin.data(), control.skin.data(),
                                     base.skin.size() * sizeof(base.skin[0])) == 0;
    float configured_skin_delta = 0.f;
    int changed_bone = -1;
    if (base.skin.size() == fitted.skin.size())
        for (size_t b = 0; b < base.skin.size(); ++b)
            for (int k = 0; k < 12; ++k)
            {
                const float d = std::fabs(base.skin[b].m[k] - fitted.skin[b].m[k]);
                if (d > configured_skin_delta)
                { configured_skin_delta = d; changed_bone = (int)b; }
            }
    std::printf("real canted record found=%d; basis delta from identity=%.6f\n",
                found ? 1 : 0, basis_delta);
    std::printf("binding finite=%d; exact gameplay-bone import halves=%d\n",
                finite ? 1 : 0, has_guid ? 1 : 0);
    std::printf("canted placement uses selected anchor write once=%d\n",
                canted_single_source ? 1 : 0);
    std::printf("selected bone-write changes palette=%d; max delta=%.6f at bone %d\n",
                configured_skin_delta > 1e-5f ? 1 : 0,
                configured_skin_delta, changed_bone);
    std::printf("fake fit equals unfitted graph=%d; palette=%d (negative control)\n",
                fake_noop ? 1 : 0, fake_skin_noop ? 1 : 0);
    float barrel_delta = 0.f;
    if (short_barrel.skin.size() > 7 && extended_barrel.skin.size() > 7)
        barrel_delta = extended_barrel.skin[6].m[11] - short_barrel.skin[6].m[11];
    const bool barrel_tracks_length =
        std::fabs(std::fabs(barrel_delta) - 0.076406f) < 0.002f;
    std::printf("short->extended MuzzleAdaptor delta z=%.6f; expected 0.076406; pass=%d\n",
                barrel_delta, barrel_tracks_length ? 1 : 0);
    float optic_dependency_delta = 0.f;
    if (comp_secondary.skin.size() > 54 && pg_secondary.skin.size() > 54)
        for (int k = 0; k < 12; ++k)
            optic_dependency_delta = std::max(optic_dependency_delta,
                std::fabs(comp_secondary.skin[54].m[k] -
                          pg_secondary.skin[54].m[k]));
    bool qmk_found = false, qmk_single_source = true;
    for (const Row& r : comp_secondary.rows)
        if (r.key.find("qmk171a_reflex") != std::string::npos)
        { qmk_found = true; qmk_single_source = qmk_single_source && !r.has; }
    const bool optic_dependency_pass = qmk_found && qmk_single_source &&
                                       optic_dependency_delta > 0.005f;
    std::printf("compm5b->pg350 moves qmk171a anchor max=%.6f; "
                "selected once=%d; pass=%d\n",
                optic_dependency_delta, qmk_single_source ? 1 : 0,
                optic_dependency_pass ? 1 : 0);
    bf6_close(c);
    return found && finite && has_guid && configured_skin_delta > 1e-5f &&
           canted_single_source && fake_noop && fake_skin_noop &&
           barrel_tracks_length && optic_dependency_pass ? 0 : 1;
}
