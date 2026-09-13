/* Live-only control for package-card -> gameplay-package identity.
 * Generic UI keys such as Test1 carry no unlock reference.  Before pairing
 * one with a skin package, compare its authored texture bytes against every
 * skin-named package texture.  Exact equality is evidence; list order is not.
 */
#include "bf6_core.h"

#include <cstdio>
#include <cstring>

static unsigned long long digest(const unsigned char* p, int n)
{
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::printf("usage: package_art_join_test <game_dir>\n"); return 2; }
    char err[512]{};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c || !bf6_mount_all(c, 0, err, (int)sizeof(err))) return 1;
    const char* ui[] = { "test1", "test2", "test3", "m4a1_range_ads", "m4a1_cqb" };
    const char* skin[] = { "m4a1_wse0053", "m4a1_wse0023", "m4a1_wse0131",
                           "m4a1_wsr0075", "m4a1_wsr0096", "m4a1_wsl0021" };
    for (const char* u : ui)
    {
        char un[512];
        std::snprintf(un, sizeof(un),
            "common/ui/assets/images/cosmetics/generated/weaponpackages/t_ui_%s_icon_small_cropped", u);
        const int uid = bf6_texture_id_by_name(c, un);
        const bf6_texture* ut = uid >= 0 ? bf6_texture_at(c, uid) : nullptr;
        std::printf("ui=%s id=%d", u, uid);
        if (ut) std::printf(" %dx%d len=%d hash=%016llx", ut->width, ut->height,
                            ut->data_len, digest(ut->data, ut->data_len));
        std::printf("\n");
        for (const char* s : skin)
        {
            char sn[512];
            std::snprintf(sn, sizeof(sn),
                "common/ui/assets/images/cosmetics/generated/weaponpackages/t_ui_%s_icon_small_cropped", s);
            const int sid = bf6_texture_id_by_name(c, sn);
            const bf6_texture* st = sid >= 0 ? bf6_texture_at(c, sid) : nullptr;
            std::printf("  candidate=%s id=%d", s, sid);
            if (st) std::printf(" %dx%d len=%d hash=%016llx", st->width, st->height,
                                st->data_len, digest(st->data, st->data_len));
            std::printf("\n");
            if (!ut || !st || ut->data_len != st->data_len ||
                ut->width != st->width || ut->height != st->height) continue;
            const bool equal = std::memcmp(ut->data, st->data, (size_t)ut->data_len) == 0;
            if (equal) std::printf("  EXACT=%s\n", s);
        }
    }
    const int fake = bf6_texture_id_by_name(c,
        "common/ui/assets/images/cosmetics/generated/weaponpackages/t_ui_codex_fake_icon_small_cropped");
    std::printf("fake id=%d (negative control)\n", fake);
    bf6_close(c);
    return fake < 0 ? 0 : 1;
}
