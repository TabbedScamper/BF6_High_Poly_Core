/* ui_label_font_probe - which labels resolve a font style, live.
 *
 *   ui_label_font_probe <game-dir> [root ...]
 *
 * For each root, walks the live Rime tree and prints every label with its
 * resolved font style, point size, capitalization rule and authored StringId,
 * then a census of labels whose style did NOT resolve (point_size == 0).  A
 * label with no style falls back to the viewer's default face and size, which
 * is one way a screen ends up with uniformly tiny text.  Finally it prints the
 * style table: the shipped font file, weight and capitalization each style
 * resolves to, so "are the fonts right" is answered by data, not by eye.
 */
#include "bf6_core.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: ui_label_font_probe <game-dir> [root ...]\n"); return 2; }
    char err[512]{};
    bf6_ctx* c = bf6_open(argv[1], err, (int)sizeof(err));
    if (!c) { std::fprintf(stderr, "open: %s\n", err); return 1; }
    if (!bf6_mount_frontend(c, err, (int)sizeof(err)))
    { std::fprintf(stderr, "mount: %s\n", err); bf6_close(c); return 1; }

    std::vector<std::string> roots;
    for (int i = 2; i < argc; ++i) roots.emplace_back(argv[i]);
    if (roots.empty())
    {
        roots.emplace_back("common/ui/universalmenu/screens/um_overlayscreen");
        roots.emplace_back("common/ui/home/screens/home_screen");
        roots.emplace_back("common/ui/weapons/screens/menuweaponscreen");
    }
    std::map<std::string, int> used;
    for (const std::string& root : roots)
    {
        bf6_rime_tree_stats stats{};
        const int n = bf6_rime_tree(c, root.c_str(), 0, nullptr, 0, &stats);
        if (n < 0) { std::printf("%s: unreadable\n", root.c_str()); continue; }
        std::vector<bf6_rime_node> rows((size_t)n);
        bf6_rime_tree(c, root.c_str(), 0, rows.data(), n, &stats);
        int labels = 0, styled = 0, unstyled = 0;
        std::map<std::string, int> styles;
        std::printf("== %s rows=%d\n", root.c_str(), n);
        for (const bf6_rime_node& r : rows)
        {
            if (r.kind != BF6_RIME_LABEL) continue;
            ++labels;
            if (r.point_size > 0.f) { ++styled; ++styles[r.font_style]; ++used[r.font_style]; }
            else ++unstyled;
            /* Colour is printed for LABELS too, not only the image family.
             * Whether a label names its own ColorId decides the fix when it
             * paints the wrong colour: named means the paint path is ignoring
             * an authored value; unnamed means the colour has to be inherited
             * and the bug is in the inheritance, not the label. */
            std::printf("  %-40s style=%-52s pt=%6.2f cap=%d "
                        "align=%d/%d overflow=%d wrap=%d markup=%d id=%s "
                        "colour=%s/#%06X src=%d\n", r.name,
                        r.font_style[0] ? r.font_style : "-", r.point_size,
                        r.text_capitalization, r.text_halign, r.text_valign,
                        r.text_overflow_mode, r.text_word_wrap,
                        r.text_parse_markup,
                        r.text_string_id[0] ? r.text_string_id : "-",
                        r.color_name[0] ? r.color_name
                                        : (r.color_id[0] ? "(unnamed-id)" : "-"),
                        r.color_rgb & 0xFFFFFFu, r.color_source);
        }
        /* Image-family presentation, to prove the per-kind offsets read the
         * authored values rather than zeros. */
        for (const bf6_rime_node& r : rows)
        {
            const bool img = r.kind == BF6_RIME_TEXTURE || r.kind == BF6_RIME_SVG ||
                             r.kind == BF6_RIME_FLIPBOOK || r.kind == BF6_RIME_VECTOR_SHAPE ||
                             r.kind == BF6_RIME_TEXTURE_BLEND;
            if (!img) continue;
            std::printf("  IMG kind=%d %-30s resize=%d h=%d v=%d raster=%.0fx%.0f/%d "
                        "flip(p=%.2f a=%d l=%d r=%d) glow=%.1f scale=%d "
                        "blend(g0=%.2f g1=%.2f th=%.2f inv=%d) "
                        "colour=%s/#%06X source=%d\n",
                        r.kind, r.name, r.image_resize_mode, r.image_h_align, r.image_v_align,
                        r.svg_raster_size[0], r.svg_raster_size[1], r.svg_raster_size_mode,
                        r.flipbook_progress, r.flipbook_auto_play, r.flipbook_loop,
                        r.flipbook_random_frames, r.vector_glow_size, r.vector_scale_line_widths,
                        r.blend_gradient_start, r.blend_gradient_end, r.blend_mask_threshold,
                        r.blend_invert_mask,
                        r.color_name[0] ? r.color_name : "-",
                        r.color_rgb & 0xFFFFFFu, r.color_source);
        }
        std::printf("-- labels=%d styled=%d unstyled=%d\n", labels, styled, unstyled);
        for (const auto& kv : styles) std::printf("   %3d  %s\n", kv.second, kv.first.c_str());

        /* COLOUR AMBIGUITY.  rime-colorid-is-not-globally-unique: a ColorId is
         * unique within the list it selects from, not across the palette, so a
         * single id->colour map silently picks a winner.  Count how many
         * elements on THIS root name an id that more than one palette entry
         * carries - that is the population that can be painted wrong. */
        const int np = bf6_rime_palette(c, nullptr, 0);
        std::vector<bf6_rime_color> pal((size_t)(np > 0 ? np : 0));
        if (np > 0) bf6_rime_palette(c, pal.data(), np);
        std::map<std::string, std::vector<std::string> > byId;
        for (const bf6_rime_color& p : pal)
        {
            std::string entry = std::string(p.palette) + "/" + p.name;
            byId[p.id].push_back(entry);
        }
        int ambiguous = 0, resolved = 0, unresolved = 0, painted = 0;
        std::map<std::string, int> ambiguousIds, unresolvedIds;
        for (const bf6_rime_node& r : rows)
        {
            if (!r.color_id[0]) continue;
            ++resolved;
            if (r.color_source == BF6_RIME_COLOR_PALETTE) ++painted;
            const auto hit = byId.find(r.color_id);
            if (hit == byId.end()) { ++unresolved; ++unresolvedIds[r.color_id]; }
            else if (hit->second.size() > 1)
            { ++ambiguous; ++ambiguousIds[r.color_id]; }
        }
        std::printf("-- palette=%d elements-naming-an-id=%d painted=%d "
                    "UNRESOLVED=%d AMBIGUOUS=%d\n",
                    np, resolved, painted, unresolved, ambiguous);
        for (const auto& kv : unresolvedIds)
            std::printf("   MISSING id %-40s x%d\n", kv.first.c_str(), kv.second);
        /* Every distinct colour this root actually paints, so a wrong hue on
         * screen can be traced to a palette entry or excluded from one. */
        std::map<std::string, int> usedColours;
        for (const bf6_rime_node& r : rows)
        {
            if (r.color_source != BF6_RIME_COLOR_PALETTE || !r.color_name[0]) continue;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%-28s #%06X", r.color_name, r.color_rgb & 0xFFFFFFu);
            ++usedColours[buf];
        }
        for (const auto& kv : usedColours)
            std::printf("   COLOUR %s  x%d\n", kv.first.c_str(), kv.second);
        for (const auto& kv : ambiguousIds)
        {
            std::printf("   id %-34s x%-4d carried by:", kv.first.c_str(), kv.second);
            for (const std::string& s : byId[kv.first]) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
    }

    /* The style table itself, restricted to the styles the roots used. */
    const int ns = bf6_rime_font_styles(c, nullptr, 0);
    std::vector<bf6_rime_font_style> st((size_t)(ns > 0 ? ns : 0));
    if (ns > 0) bf6_rime_font_styles(c, st.data(), ns);
    std::printf("== font styles in mount: %d (showing the %zu used above)\n", ns, used.size());
    for (const bf6_rime_font_style& f : st)
    {
        std::string key = f.name;
        { const size_t slash = key.find_last_of("/\\"); if (slash != std::string::npos) key = key.substr(slash + 1); }
        for (char& ch : key) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (!used.count(key)) continue;
        std::printf("  %-56s pt=%6.2f lh=%5.1f w=%d cap=%d font=%s\n", f.name, f.point_size,
                    f.line_height, f.weight, f.capitalization, f.font_asset);
    }
    bf6_close(c);
    return 0;
}
