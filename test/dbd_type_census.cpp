/* What the DBD TypeRef signature means, measured instead of guessed.
 *
 * bf6_rime_dbd_fields deliberately hands back the type signature as an opaque
 * payload - "evidence, not a guessed semantic type", which is the right call
 * for a reader. But a consumer still has to know whether a field is a bool it
 * can gate visibility on or a float it must format, and today it has to guess
 * from the field's NAME.
 *
 * The signature is decodable by census: if one code lands overwhelmingly on
 * "Is" and "Has" names across many unrelated data bindings, it is a bool, and
 * that is a measurement rather than an inference from a single table.
 *
 * The control is the naming itself. Field names were NOT used to assign codes;
 * they are only used afterwards to see whether each code's population is
 * semantically coherent. A code whose names are a random mix is reported as
 * such rather than forced into a type.
 *
 *   dbd_type_census <game_dir>
 */
#include "bf6_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static bool starts_with(const std::string& s, const char* p)
{
    const size_t n = std::strlen(p);
    return s.size() >= n && _strnicmp(s.c_str(), p, n) == 0;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    { std::fprintf(stderr, "usage: dbd_type_census <game_dir>\n"); return 2; }
    char err[512]{};
    bf6_ctx* ctx = bf6_open(argv[1], err, (int)sizeof(err));
    if (!ctx) { std::fprintf(stderr, "open: %s\n", err); return 1; }
    if (!bf6_mount_all(ctx, 0, err, (int)sizeof(err)))
    { std::fprintf(stderr, "mount: %s\n", err); bf6_close(ctx); return 1; }

    const int total = bf6_list_ebx(ctx, "databindings/", nullptr, 0);
    if (total <= 0)
    { std::printf("no databindings partitions found\n"); bf6_close(ctx); return 1; }
    std::vector<bf6_asset> assets((size_t)total);
    const int got = bf6_list_ebx(ctx, "databindings/", assets.data(), total);
    std::printf("databindings partitions: %d\n", got);

    struct CodeStat {
        int fields = 0;
        std::map<std::string, int> names;
        std::set<std::string> partitions;
    };
    std::map<uint64_t, CodeStat> byCode;
    int readable = 0, unreadable = 0, fields_total = 0;

    for (int i = 0; i < got; ++i)
    {
        const char* name = assets[(size_t)i].name;
        if (!name || !*name) continue;
        char data_name[256]{};
        const int count = bf6_rime_dbd_fields(ctx, name, data_name,
                                              sizeof(data_name), nullptr, 0);
        if (count <= 0) { unreadable++; continue; }
        std::vector<bf6_rime_dbd_field> fields((size_t)count);
        const int filled = bf6_rime_dbd_fields(ctx, name, data_name,
                                               sizeof(data_name),
                                               fields.data(), count);
        if (filled != count) { unreadable++; continue; }
        readable++;
        for (const bf6_rime_dbd_field& f : fields)
        {
            fields_total++;
            CodeStat& st = byCode[f.type_signature];
            st.fields++;
            st.names[f.name]++;
            st.partitions.insert(name);
        }
    }

    /* The in-scope control: a fabricated partition must not read. */
    const int fake = bf6_rime_dbd_fields(
        ctx, "common/ui/__control__/fabricated_dbd", nullptr, 0, nullptr, 0);

    std::printf("readable=%d unreadable=%d fields=%d distinct codes=%zu "
                "fake-control=%d\n\n",
                readable, unreadable, fields_total, byCode.size(), fake);

    /* Name-shape buckets, applied AFTER the grouping, never to form it. */
    struct Shape { const char* label; bool (*test)(const std::string&); };
    static const Shape shapes[] = {
        { "bool-ish (Is/Has/Can/Should/Use/Show/Enable)",
          [](const std::string& n) {
              return starts_with(n, "Is") || starts_with(n, "Has") ||
                     starts_with(n, "Can") || starts_with(n, "Should") ||
                     starts_with(n, "Use") || starts_with(n, "Show") ||
                     starts_with(n, "Enable"); } },
        { "count-ish (Nr/Num/Count/Index/Amount/Duration/Time)",
          [](const std::string& n) {
              return starts_with(n, "Nr") || starts_with(n, "Num") ||
                     n.find("Count") != std::string::npos ||
                     n.find("Index") != std::string::npos ||
                     n.find("Amount") != std::string::npos ||
                     n.find("Duration") != std::string::npos ||
                     n.find("Time") != std::string::npos; } },
        { "colour-ish (Color/Colour/Tint)",
          [](const std::string& n) {
              return n.find("Color") != std::string::npos ||
                     n.find("Colour") != std::string::npos ||
                     n.find("Tint") != std::string::npos; } },
        { "image-ish (Icon/Image/Texture/Svg/Sprite/Art)",
          [](const std::string& n) {
              return n.find("Icon") != std::string::npos ||
                     n.find("Image") != std::string::npos ||
                     n.find("Texture") != std::string::npos ||
                     n.find("Svg") != std::string::npos ||
                     n.find("Sprite") != std::string::npos ||
                     n.find("Art") != std::string::npos; } },
        { "text-ish (Name/Label/Text/Title/Desc/Sid)",
          [](const std::string& n) {
              return n.find("Name") != std::string::npos ||
                     n.find("Label") != std::string::npos ||
                     n.find("Text") != std::string::npos ||
                     n.find("Title") != std::string::npos ||
                     n.find("Desc") != std::string::npos ||
                     n.find("Sid") != std::string::npos; } },
    };

    std::vector<std::pair<uint64_t, CodeStat*>> ordered;
    for (auto& kv : byCode) ordered.push_back({ kv.first, &kv.second });
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b)
              { return a.second->fields > b.second->fields; });

    for (const auto& kv : ordered)
    {
        if (kv.second->fields < 4) continue;
        std::printf("code %016llX  fields=%d  partitions=%zu  distinct names=%zu\n",
                    (unsigned long long)kv.first, kv.second->fields,
                    kv.second->partitions.size(), kv.second->names.size());
        int best = -1; const char* bestLabel = "unclassified";
        for (const Shape& sh : shapes)
        {
            int hit = 0;
            for (const auto& n : kv.second->names) if (sh.test(n.first)) hit += n.second;
            const int pct = kv.second->fields ? hit * 100 / kv.second->fields : 0;
            if (pct > best) { best = pct; bestLabel = sh.label; }
            if (pct >= 10)
                std::printf("      %3d%%  %s\n", pct, sh.label);
        }
        std::printf("      -> dominant shape: %s (%d%%)\n", bestLabel, best);
        int shown = 0;
        std::printf("      names:");
        for (const auto& n : kv.second->names)
        {
            if (shown++ >= 10) { std::printf(" ..."); break; }
            std::printf(" %s", n.first.c_str());
        }
        std::printf("\n\n");
    }
    bf6_close(ctx);
    return 0;
}
