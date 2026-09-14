// bf6_loadout_*: the LootSpawner catalogue, attachments and configured weapon.
//   loadout_test <game_dir> <portal_attachment_enums.txt> [item]
// The Unreal plugin's installed-assets check, on the shared core: the
// catalogue has weapons, the default weapon has mapped attachments, the
// factory weapon and one configured weapon both build bounded geometry, and
// the configured one differs from the factory one.
#include "bf6_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static std::string text(uint8_t* blob, int64_t n)
{
    std::string s = n > 0 && blob ? std::string((const char*)blob, (size_t)n) : std::string();
    bf6_blob_free(blob);
    return s;
}

struct Weapon { std::string json; int sections = 0; long long tris = 0; double size = 0; unsigned long long digest = 0; };

static Weapon weapon(bf6_ctx* c, const char* item, const char* fits, const char* enums)
{
    Weapon w;
    uint8_t* blob = nullptr;
    const int64_t n = bf6_loadout_weapon(c, item, fits, enums, &blob);
    if (n < 12 || !blob) { bf6_blob_free(blob); return w; }
    uint32_t head[3];
    std::memcpy(head, blob, 12);
    w.json.assign((const char*)blob + 12, head[2]);
    const float* body = (const float*)(blob + 12 + head[2]);
    const size_t body_n = (size_t)(n - 12 - head[2]) / 4;
    double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < body_n; ++i) {
        uint32_t bits;
        std::memcpy(&bits, body + i, 4);
        h = (h ^ bits) * 1099511628211ULL;
    }
    w.digest = h;
    // Walk "positions":N / "vertex_count":V pairs for bounds.
    size_t at = 0;
    while ((at = w.json.find("\"vertex_count\":", at)) != std::string::npos) {
        const int vc = std::atoi(w.json.c_str() + at + 15);
        const size_t ic_at = w.json.find("\"index_count\":", at);
        const size_t p_at = w.json.find("\"positions\":", at);
        if (ic_at == std::string::npos || p_at == std::string::npos) break;
        w.tris += std::atoi(w.json.c_str() + ic_at + 14) / 3;
        const size_t off = (size_t)std::atoll(w.json.c_str() + p_at + 12);
        for (int v = 0; v < vc && off + (size_t)v * 3 + 2 < body_n; ++v)
            for (int k = 0; k < 3; ++k) {
                const double x = body[off + (size_t)v * 3 + k];
                if (!std::isfinite(x)) { lo[k] = -1e30; continue; }
                lo[k] = std::min(lo[k], x);
                hi[k] = std::max(hi[k], x);
            }
        w.sections++;
        at = p_at;
    }
    w.size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    bf6_blob_free(blob);
    return w;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: loadout_test <game_dir> <enums.txt> [item]\n"); return 2; }
    const char* item = argc > 3 ? argv[3] : "carbine/m4a1";
    std::ifstream f(argv[2]);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string enums = ss.str();
    char err[512] = {0};
    bf6_ctx* c = bf6_open(argv[1], err, sizeof(err));
    if (!c) { std::printf("open: %s\n", err); return 1; }

    uint8_t* blob = nullptr;
    const std::string cat = text(blob, bf6_loadout_catalogue(c, &blob));
    size_t items = 0;
    for (size_t at = 0; (at = cat.find("\"id\":", at)) != std::string::npos; ++at) items++;
    std::printf("catalogue: %zu items\n", items);
    check(items > 50 && cat.find(std::string("\"id\":\"") + item + "\"") != std::string::npos, "catalogue has the default weapon");

    blob = nullptr;
    const std::string att = text(blob, bf6_loadout_attachments(c, item, enums.c_str(), &blob));
    size_t choices = 0;
    for (size_t at = 0; (at = att.find("\"slot\":", at)) != std::string::npos; ++at) choices++;
    std::printf("%s: %zu mapped attachments\n", item, choices);
    check(choices > 0, "default weapon has mapped public attachments");

    const Weapon factory = weapon(c, item, "", enums.c_str());
    std::printf("factory: %d sections, %lld triangles, %.3f m, body %016llx, json %zu bytes\n", factory.sections, factory.tris,
                factory.size, factory.digest, factory.json.size());
    check(factory.json.find("\"error\":\"\"") != std::string::npos && factory.sections > 0, "factory weapon builds");
    check(factory.size > 0.2 && factory.size < 5.0, "factory weapon is weapon-sized");

    // An optic: the factory weapon carries iron sights, so any scope changes it.
    std::string fit;
    size_t s_at = att.find("\"slot\":\"scp\"");
    if (s_at != std::string::npos) {
        const size_t id_at = att.rfind("{\"id\":\"", s_at);
        const size_t id_end = att.find('"', id_at + 7);
        fit = "scp=" + att.substr(id_at + 7, id_end - id_at - 7);
    }
    if (!fit.empty()) {
        const Weapon configured = weapon(c, item, fit.c_str(), enums.c_str());
        std::printf("configured (%s): %d sections, %lld triangles, %.3f m\n", fit.c_str(), configured.sections,
                    configured.tris, configured.size);
        check(configured.json.find("\"error\":\"\"") != std::string::npos && configured.sections > 0, "configured weapon builds");
        check(configured.digest != factory.digest, "configured weapon differs from the factory weapon");
    }
    const Weapon bogus = weapon(c, item, "mzl=Muzzle_NotAThing", enums.c_str());
    check(bogus.json.find("unavailable") != std::string::npos, "an unknown attachment is refused, not guessed");
    std::printf("anchors: %s\n", factory.json.substr(factory.json.find("\"anchors\"")).c_str());

    // THE SOLDIER: the Unreal plugin's PlayerSpawner check on the shared core.
    size_t characters = 0, outfits = 0;
    if (size_t c_at = cat.find("\"characters\":["); c_at != std::string::npos)
        for (size_t at = c_at; (at = cat.find("{\"id\":", at + 1)) != std::string::npos && at < cat.find("\"outfits\":["); ) characters++;
    for (size_t at = 0; (at = cat.find("{\"character\":", at)) != std::string::npos; ++at) outfits++;
    std::printf("soldier choices: %zu characters, %zu outfits\n", characters, outfits);
    check(characters > 10 && outfits >= characters && cat.find("\"id\":\"cha0001wisp\"") != std::string::npos, "catalogue lists operators and outfits");
    auto soldier = [&](const char* request, Weapon& s) {
        uint8_t* b = nullptr;
        const int64_t n = bf6_loadout_soldier(c, request, enums.c_str(), &b);
        s = Weapon();
        if (n < 12 || !b) { bf6_blob_free(b); return; }
        uint32_t head[3];
        std::memcpy(head, b, 12);
        s.json.assign((const char*)b + 12, head[2]);
        const float* body = (const float*)(b + 12 + head[2]);
        const size_t body_n = (size_t)(n - 12 - head[2]) / 4;
        double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
        unsigned long long h = 1469598103934665603ULL;
        for (size_t i = 0; i < body_n; ++i) { uint32_t bits; std::memcpy(&bits, body + i, 4); h = (h ^ bits) * 1099511628211ULL; }
        s.digest = h;
        size_t at = 0;
        while ((at = s.json.find("\"vertex_count\":", at)) != std::string::npos) {
            const int vc = std::atoi(s.json.c_str() + at + 15);
            const size_t ic_at = s.json.find("\"index_count\":", at), p_at = s.json.find("\"positions\":", at);
            if (ic_at == std::string::npos || p_at == std::string::npos) break;
            s.tris += std::atoi(s.json.c_str() + ic_at + 14) / 3;
            const size_t off = (size_t)std::atoll(s.json.c_str() + p_at + 12);
            for (int v = 0; v < vc && off + (size_t)v * 3 + 2 < body_n; ++v)
                for (int k = 0; k < 3; ++k) {
                    const double x = body[off + (size_t)v * 3 + k];
                    if (!std::isfinite(x)) { lo[k] = -1e30; continue; }
                    lo[k] = std::min(lo[k], x); hi[k] = std::max(hi[k], x);
                }
            s.sections++;
            at = p_at;
        }
        s.size = hi[1] - lo[1];
        std::printf("  soldier %s: %d sections, %lld triangles, height %.3f m, lowest y %.4f, x %.2f..%.2f z %.2f..%.2f\n",
                    request, s.sections, s.tris, s.size, lo[1], lo[0], hi[0], lo[2], hi[2]);
        bf6_blob_free(b);
    };
    Weapon nato, pax, recon, bogus_char;
    soldier("", nato);
    check(nato.json.find("\"error\":\"\"") != std::string::npos && nato.sections > 0, "the default soldier builds");
    check(nato.json.find("Posed soldier") != std::string::npos, "the soldier says what it built");
    check(nato.size > 1.4 && nato.size < 2.4, "the soldier is soldier-sized");
    check(nato.json.find("[57708878,") != std::string::npos, "the soldier keeps its iris binding (0x0370914E)");
    check(nato.json.find("t_patch_faction_alliance_cs") != std::string::npos, "NATO wears the Alliance badge");
    soldier("{\"faction\":\"pax\"}", pax);
    check(pax.json.find("t_patch_faction_pax_01_cs") != std::string::npos && pax.json.find("t_patch_faction_alliance_cs") == std::string::npos && pax.digest == nato.digest, "PAX wears its own badge on the same body");
    soldier("{\"role\":\"recon\",\"item\":\"carbine/m4a1\"}", recon);
    check(recon.json.find("\"error\":\"\"") != std::string::npos && recon.digest != nato.digest, "another role stands differently");
    soldier("{\"character\":\"cha9999nobody\"}", bogus_char);
    check(bogus_char.json.find("unavailable") != std::string::npos && bogus_char.sections == 0, "an unknown operator is refused");
    bf6_close(c);
    std::printf("loadout_test: %s (%d)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
