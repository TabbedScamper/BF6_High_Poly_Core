#include "armory_ui.h"
#include "armory.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace armory_ui {
namespace {

// THE PALETTE IS READ, NOT TYPED.
//
// These nine were literal hex until the palette became a live call. Every one
// of them had been eyeballed out of the same assets by hand, so they were
// right - and would have stayed right-looking after a patch moved them, which
// is the failure mode that matters. They are now filled from the game's own
// 111 entries at the top of draw() and are only mutable so that the ~40 use
// sites below did not have to be threaded with a parameter.
//
// kBg keeps its name for the same reason. The GAME calls it DarkNavy.
ImU32 kBg      = 0;   // colorlist/DarkNavy
ImU32 kTile    = 0;   // colorlist/Charcoal
ImU32 kTileHov = 0;   // derived, see build_palette()
ImU32 kText    = 0;   // FE-Text
ImU32 kTextDim = 0;   // FE-Text at the authored 0.60 alpha step
ImU32 kLine    = 0;   // FE-Text at the authored 0.27 alpha step
ImU32 kAccent  = 0;   // FE-Class-Proficiency
ImU32 kEquip   = 0;   // FE-Online-Positive
ImU32 kLocked  = 0;   // FE-Error-Negative

// The two colours the tabs and pills invert to: text on an active tab is the
// ground colour, not black. Derived from the two above rather than restated.
ImU32 kOnAccent = 0;  // what sits ON kText when a tab is active

// The live ramp, taken from the palette at the top of draw(). Named steps, so
// no call site below carries a raw 0..255 number.
float g_alpha[kAlphaSteps] = { 0, 0, 0, 0, 0, 0 };

// One of the nine read colours, at one of the six read alpha steps. This pair
// is the whole vocabulary: a colour the game names, at a transparency the game
// authored.
ImU32 at(ImU32 rgba, Alpha step)
{
    const int a = (int)(g_alpha[step] * 255.0f + 0.5f);
    return (rgba & 0x00FFFFFFu) | ((ImU32)(a < 0 ? 0 : (a > 255 ? 255 : a)) << 24);
}

struct Ctx {
    ImDrawList* dl = nullptr;
    ImVec2      mouse{};
    bool        clicked = false;
};

bool hit(const Ctx& c, float x, float y, float w, float h)
{
    return c.mouse.x >= x && c.mouse.x <= x + w &&
           c.mouse.y >= y && c.mouse.y <= y + h;
}

void text(const Ctx& c, ImFont* f, float px, float x, float y,
          ImU32 col, const char* s)
{
    if (f) c.dl->AddText(f, px, ImVec2(x, y), col, s);
    else   c.dl->AddText(ImVec2(x, y), col, s);
}

float text_w(ImFont* f, float px, const char* s)
{
    if (!f) return (float)strlen(s) * px * 0.5f;
    return f->CalcTextSizeA(px, FLT_MAX, 0.f, s).x;
}

// Uppercase, because the front end's button and tab labels are uppercase
// styles (fe_buttonlabel_*_uppercase) rather than uppercased strings.
std::string upper(std::string s)
{
    for (char& ch : s) ch = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    return s;
}

// THE CLASS LABELS ARE THE GAME'S, OR THEY ARE THE FOLDER NAME.
//
// This was a 24-line table of English strings - "assaultrifle" -> "ASSAULT
// RIFLE", "boltaction" -> "SNIPER RIFLE", and an "OTHER" for anything it had
// not seen. Every one of those was invented here. The game localises a class
// label for each weapon in its UIWeaponAbilityMetadata, so the caller resolves
// one weapon per class and hands the mapping in.
//
// When the game gives us nothing the folder name is shown. That is the whole
// point: "boltaction" is a true thing to display and "SNIPER RIFLE" is a guess
// that happens to be right in English and wrong in every other language the
// game ships.
const char* class_label(const std::string& cls, ClassLabelFn fn, void* user)
{
    if (fn)
        if (const char* s = fn(cls.c_str(), user))
            if (*s) return s;
    return cls.c_str();
}

// The name the GAME shows for a fitted attachment, or the token when the
// roster and the catalogue do not meet on it. The token is a true thing to
// show; a prettified guess is not.
const char* attachment_label(const std::string& slot, const std::string& token,
                             AttachmentNameFn fn, void* user)
{
    if (fn)
        if (const char* s = fn(slot.c_str(), token.c_str(), user))
            if (*s) return s;
    return token.c_str();
}

// The name the GAME shows for a weapon, or its token when the metadata does
// not name it. 69 of the roster's entries are named; the rest keep the token.
const char* weapon_label(const std::string& bare, WeaponNameFn fn, void* user)
{
    if (fn)
        if (const char* s = fn(bare.c_str(), user))
            if (*s) return s;
    return bare.c_str();
}

const bf6::ArmoryWeapon* find(const bf6::Armory& a, const std::string& key)
{
    for (const bf6::ArmoryWeapon& w : a.weapons)
        if (w.cls + "/" + w.name == key) return &w;
    return nullptr;
}

int cost_of(const bf6::ArmoryWeapon& w, const std::string& slot,
            const std::string& att)
{
    for (const bf6::ArmoryAttachment& at : w.attachments)
        if (at.slot == slot && at.name == att) return at.cost;
    return -1;
}

// The transcribed anchor table that used to sit here is gone. It held eleven
// rows of hand-copied 1920x1080 pixels and eleven invented English labels, and
// it was wrong in two ways that were invisible: it was missing the twelfth
// slot entirely, and two of its labels named the wrong slot - the tile it
// called ERGONOMICS is the game's Launcher anchor and the one it called
// UNDERBARREL is Rangefinder. The caller now reads all twelve live from
// menuweaponattachmenticonscreen, labelled with the game's own slot names.

}  // namespace

int spent(const bf6::Armory& a, const State& s)
{
    const bf6::ArmoryWeapon* w = find(a, s.weapon);
    if (!w) return 0;
    int total = 0;
    for (const auto& kv : s.fitted.by_slot)
    {
        if (kv.second.empty()) continue;
        const int c = cost_of(*w, kv.first, kv.second);
        if (c > 0) total += c;
    }
    return total;
}

bool draw(const bf6::Armory& a, State& s, const Fonts& f, const Palette& pal,
          float x, float y, float w, float h,
          DrawIconFn icon, void* icon_user, const BaseStats* stats,
          const WeaponInfo* info, ClassLabelFn class_label_fn,
          void* class_label_user, const std::vector<TileAnchor>* tiles,
          AttachmentNameFn attachment_name_fn, void* attachment_user,
          AttachmentTitleFn attachment_title_fn,
          DrawAttachmentIconFn attachment_icon, WeaponNameFn weapon_name_fn,
          const Regions* regions, AttachmentDescFn attachment_desc_fn,
          const std::vector<std::string>* navigation_classes,
          const float* picker_rect,
          const std::vector<PackageRow>* packages,
          DrawPackageIconFn package_icon)
{
    // Take the live palette. Nothing below reads a literal colour, so if the
    // palette has not been read this refuses to draw rather than falling back
    // to the constants that used to be here - a screen that quietly paints
    // approximately-right colours is the failure this change exists to stop.
    if (!pal.valid) return false;
    kBg = pal.surface; kTile = pal.tile; kTileHov = pal.tile_hover;
    kText = pal.text;  kTextDim = pal.text_dim; kLine = pal.line;
    kAccent = pal.accent; kEquip = pal.positive; kLocked = pal.negative;
    kOnAccent = pal.surface;
    for (int i = 0; i < kAlphaSteps; i++) g_alpha[i] = pal.alpha[i];

    // The authored canvas is 1920x1080 and the window is not, so a region
    // crosses over by the same scale the Rime layer uses. Nothing rounds to a
    // "nice" number here: an authored 709 stays 709, scaled.
    const float RX = w / 1920.f, RY = h / 1080.f;
    auto region = [&](const float* r, int i) {
        return (i & 1) ? y + r[i] * RY : x + r[i] * RX;
    };

    Ctx c;
    // THE FOREGROUND LIST, so the game's own authored chrome can be drawn
    // beneath us. Both lists are just vertex buffers; background is submitted
    // first, so this is the whole of the layering.
    c.dl = ImGui::GetForegroundDrawList();
    c.mouse = ImGui::GetIO().MousePos;
    c.clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::GetIO().WantCaptureMouse;
    bool changed = false;
    s.weapon_changed = s.slot_changed = s.fitted_changed = false;

    // ---- title bar ---------------------------------------------------------
    const float pad = 24.f;
    char title[160];
    if (s.page == Page::Weapons)
        snprintf(title, sizeof(title), "SELECT PRIMARY WEAPON");
    else if (s.page == Page::Packages)
        snprintf(title, sizeof(title), "SELECT %s PACKAGE",
                 upper(weapon_label(s.weapon.substr(s.weapon.find('/') + 1),
                                    weapon_name_fn, class_label_user)).c_str());
    else if (s.page == Page::Customize)
        snprintf(title, sizeof(title), "CUSTOMIZE %s",
                 upper(weapon_label(s.weapon.substr(s.weapon.find('/') + 1),
                                    weapon_name_fn, class_label_user)).c_str());
    else
    {
        const char* slotTitle = s.slot.c_str();
        if (tiles)
            for (const TileAnchor& tile : *tiles)
                if (tile.code == s.slot && !tile.caption.empty())
                { slotTitle = tile.caption.c_str(); break; }
        snprintf(title, sizeof(title), "SELECT %s", upper(slotTitle).c_str());
    }

    // The global shell is transparent over the studio. The old implementation
    // painted an opaque 56-pixel bar and a second BACK button at the right;
    // neither exists in the capture. Runtime text still comes from the live
    // selected weapon because the authored label is a binding target.
    text(c, f.header, f.sub_s.at(25.f), x + pad, y + 13.f, kText, "<");
    c.dl->AddLine(ImVec2(x + pad + 30.f, y + 10.f),
                  ImVec2(x + pad + 30.f, y + 46.f), kLine);
    text(c, f.header, f.sub_s.at(24.f), x + pad + 76.f, y + 16.f, kText, title);

    // The left chevron is the sole back affordance in this shell.
    if (s.page != Page::Weapons)
    {
        const bool over = hit(c, x, y, pad + 54.f, 56.f);
        if (over && c.clicked)
        {
            s.page = (s.page == Page::SlotPicker) ? Page::Customize : Page::Weapons;
            changed = true;
        }
    }

    float cy = y + 56.f;

    // ---- weapon selection --------------------------------------------------
    if (s.page == Page::Weapons || s.page == Page::Packages)
    {
        const bool packagePage = s.page == Page::Packages;
        // THE TABS ARE THE WEAPON CLASSES, NOT EVERY ROSTER FOLDER.
        //
        // This listed all twenty - callins, intel, mines, misc, supply,
        // throwables, tools - and wrapped onto two rows, where the game shows
        // seven. It also showed MACHINE GUN twice, because two folders resolve
        // to the same localised label and the label is what a person sees.
        // A class is kept when the game named it and its name is not already up.
        std::vector<std::string> classes;
        std::vector<std::string> shown;
        if (!packagePage && navigation_classes && !navigation_classes->empty())
            classes = *navigation_classes;
        for (const bf6::ArmoryWeapon& wep : a.weapons)
        {
            if (packagePage) break;
            if (!classes.empty() && navigation_classes &&
                !navigation_classes->empty())
                break;
            if (std::find(classes.begin(), classes.end(), wep.cls) != classes.end()) continue;
            const char* lab = class_label(wep.cls, class_label_fn, class_label_user);
            // An unnamed folder falls back to its own token, which is how a
            // gadget category gets in; if the game did not name it, it is not
            // a weapon class the armory shows.
            if (!lab || wep.cls == lab) continue;
            if (std::find(shown.begin(), shown.end(), std::string(lab)) != shown.end()) continue;
            shown.push_back(lab);
            classes.push_back(wep.cls);
        }
        // If nothing was named the screen still needs tabs; fall back to the
        // raw folders rather than showing an empty strip.
        if (!packagePage && classes.empty())
            for (const bf6::ArmoryWeapon& wep : a.weapons)
                if (std::find(classes.begin(), classes.end(), wep.cls) == classes.end())
                    classes.push_back(wep.cls);
        if (s.category.empty() && !classes.empty()) s.category = classes[0];

        // LEFT: hierarchy and controls.  Keep the center transparent so the
        // D3D weapon remains the dominant visual rather than another panel.
        const float leftX = x + 90.f;
        const float leftY = packagePage ? y + 452.f : y + 320.f;
        const float leftW = 520.f;
        const std::string bare = info && info->valid && !info->name.empty() ? upper(info->name) :
            (s.weapon.empty() ? std::string("SELECT WEAPON") :
             upper(weapon_label(s.weapon.substr(s.weapon.find('/') + 1),
                                weapon_name_fn, class_label_user)));
        text(c, f.hero ? f.hero : f.header, f.hero_s.at(64.f),
             leftX, leftY, kText, bare.c_str());
        const char* factory = info && info->valid && !info->factory_label.empty() ?
            info->factory_label.c_str() : "FACTORY";
        text(c, f.label, f.label_s.at(20.f), leftX, leftY + 58.f,
             kTextDim, upper(factory).c_str());
        if (!packagePage && info && info->valid)
        {
            float infoY = leftY + 94.f, chipX = leftX;
            for (const std::string& trait : info->traits)
            {
                const std::string t = upper(trait);
                const float cw = text_w(f.label, f.label_s.at(16.f), t.c_str()) + 16.f;
                c.dl->AddRectFilled(ImVec2(chipX, infoY), ImVec2(chipX + cw, infoY + 25.f), at(kTile, A50));
                c.dl->AddLine(ImVec2(chipX, infoY), ImVec2(chipX, infoY + 25.f), kTextDim);
                text(c, f.label, f.label_s.at(16.f), chipX + 8.f, infoY + 4.f, kText, t.c_str());
                chipX += cw + 5.f;
            }
            infoY += info->traits.empty() ? 0.f : 40.f;
            std::string line;
            size_t p = 0; int lines = 0;
            while (p <= info->description.size() && lines < 4)
            {
                const size_t q = info->description.find(' ', p);
                const std::string word = info->description.substr(
                    p, (q == std::string::npos ? info->description.size() : q) - p);
                const std::string trial = line.empty() ? word : line + " " + word;
                if (!line.empty() && text_w(f.body, f.body_s.at(20.f), trial.c_str()) > leftW)
                {
                    text(c, f.body, f.body_s.at(20.f), leftX, infoY + lines * 24.f, kTextDim, line.c_str());
                    line = word; lines++;
                }
                else line = trial;
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (!line.empty() && lines < 4)
                text(c, f.body, f.body_s.at(20.f), leftX, infoY + lines * 24.f, kTextDim, line.c_str());
        }

        const float actionY = leftY + (packagePage ? 120.f : 206.f);
        const float equipW = 132.f, modW = 176.f, favoriteW = 154.f, rangeW = 180.f;
        c.dl->AddRectFilled(ImVec2(leftX, actionY), ImVec2(leftX + equipW, actionY + 48.f), kEquip);
        text(c, f.label, f.label_s.at(16.f), leftX + 12.f, actionY + 12.f, at(kOnAccent, A100), "EQUIPPED");
        const bool modOver = hit(c, leftX + equipW + 7.f, actionY, modW, 48.f);
        c.dl->AddRectFilled(ImVec2(leftX + equipW + 7.f, actionY),
                            ImVec2(leftX + equipW + 7.f + modW, actionY + 48.f),
                            modOver ? kTileHov : kTile);
        c.dl->AddRect(ImVec2(leftX + equipW + 7.f, actionY),
                      ImVec2(leftX + equipW + 7.f + modW, actionY + 48.f), kLine);
        text(c, f.label, f.label_s.at(16.f), leftX + equipW + 18.f, actionY + 12.f, kText, "X  CUSTOMIZE");
        float rangeX = leftX + equipW + modW + 14.f;
        if (packagePage)
        {
            const float favoriteX = rangeX;
            c.dl->AddRectFilled(ImVec2(favoriteX, actionY),
                                ImVec2(favoriteX + favoriteW, actionY + 48.f), kTile);
            c.dl->AddRect(ImVec2(favoriteX, actionY),
                          ImVec2(favoriteX + favoriteW, actionY + 48.f), kLine);
            text(c, f.label, f.label_s.at(16.f), favoriteX + 12.f,
                 actionY + 12.f, kText, "F  FAVORITE");
            rangeX += favoriteW + 7.f;
        }
        c.dl->AddRectFilled(ImVec2(rangeX, actionY),
                            ImVec2(rangeX + rangeW, actionY + 48.f), kTile);
        c.dl->AddRect(ImVec2(rangeX, actionY),
                      ImVec2(rangeX + rangeW, actionY + 48.f), kLine);
        text(c, f.label, f.label_s.at(16.f), rangeX + 12.f,
             actionY + 12.f, kText, "Z  FIRING RANGE");
        if (modOver && c.clicked && !s.weapon.empty()) { s.page = Page::Customize; changed = true; }
        // RIGHT: only the three values whose runtime read path is verified.
        // The four delegate-computed bars are omitted, never fabricated.
        // The stat block sits bottom-RIGHT beside the grid, not top-right:
        // Weapon Stats Main Container solves to 1324,570 - 1812,1008, and it
        // solves to the SAME box on the attachment picker screen, which is what
        // says it is a screen region rather than one screen's quirk.
        const bool haveStats = regions && regions->valid && regions->stats[2] > regions->stats[0];
        const float statX = haveStats ? region(regions->stats, 0) + 20.f : x + w - 270.f;
        const float statY = haveStats ? region(regions->stats, 1) + 210.f : y + 700.f;
        const float statR = haveStats ? region(regions->stats, 2) : x + w - 42.f;
        if (stats && stats->valid)
        {
            const char* labs[3] = { "DMG", "ROF", "MAG" };
            const int vals[3] = { stats->damage, stats->rate_of_fire, stats->magazine };
            for (int i = 0; i < 3; i++)
            {
                char value[24]; snprintf(value, sizeof(value), "%d", vals[i]);
                text(c, f.label, f.label_s.at(13.f), statX, statY + i * 70.f + 10.f, kTextDim, labs[i]);
                // The direct value and its label share the authored left edge.
                // Offsetting the value by 70 px was a host-compositor artifact
                // and visibly disagreed with Weapon Stats Main Container.
                text(c, f.header, f.sub_s.at(36.f), statX, statY + i * 70.f + 22.f, kText, value);
                c.dl->AddLine(ImVec2(statX, statY + i * 70.f + 52.f),
                              ImVec2(statR, statY + i * 70.f + 52.f), kLine);
            }
        }

        // Category row immediately above the card grid. It wraps at the
        // right edge instead of clipping a raw folder name mid-word.
        // The game's own tab strip, not a guess: NavigationTabs solves to
        // 64,634 - 1146,674 on the authored canvas.
        const bool haveTabs = regions && regions->valid && regions->tabs[2] > regions->tabs[0];
        const float tabLeft  = haveTabs ? region(regions->tabs, 0) : x + pad;
        const float tabRight = haveTabs ? region(regions->tabs, 2) : x + w - pad;
        float tx   = tabLeft;
        float tabY = haveTabs ? region(regions->tabs, 1) : y + h - 370.f;
        const float th = haveTabs ? (region(regions->tabs, 3) - tabY) : 30.f;
        for (const std::string& cls : classes)
        {
            const char* lab = class_label(cls, class_label_fn, class_label_user);
            std::string compactLabel = lab ? lab : cls;
            if (_stricmp(compactLabel.c_str(), "MACHINE GUN") == 0)
                compactLabel = "LMG";
            else if (_stricmp(compactLabel.c_str(),
                              "MARKSMAN RIFLE") == 0)
                compactLabel = "DMR";
            const float tw = text_w(f.label, f.label_s.at(12.f),
                                    compactLabel.c_str()) + 20.f;
            if (tx + tw > tabRight) { tx = tabLeft; tabY += th + 4.f; }
            const bool on = (cls == s.category);
            const bool over = hit(c, tx, tabY, tw, th);
            c.dl->AddRectFilled(ImVec2(tx, tabY), ImVec2(tx + tw, tabY + th),
                                on ? at(kText, A100) : (over ? kTileHov : kTile));
            text(c, f.label, f.label_s.at(12.f), tx + 10.f, tabY + 9.f,
                 on ? at(kOnAccent, A100) : kText,
                 compactLabel.c_str());
            if (over && c.clicked)
            {
                s.category = cls;
                for (const bf6::ArmoryWeapon& nw : a.weapons) if (nw.cls == cls)
                {
                    s.weapon = nw.cls + "/" + nw.name;
                    s.fitted.by_slot.clear();
                    s.weapon_changed = true;
                    break;
                }
                changed = true;
            }
            tx += tw + 4.f;
        }

        // The game's own card grid: Grid Container solves to 48,709 - 1290,1050,
        // which is the bottom LEFT of the screen with the stat block beside it -
        // not a full-width strip across the bottom, which is what this drew.
        const bool haveGrid = regions && regions->valid && regions->grid[2] > regions->grid[0];
        const float gridX0 = (haveGrid ? region(regions->grid, 0) : x + pad) + 16.f;
        const float gridX1 = (haveGrid ? region(regions->grid, 2) : x + w - pad) - 32.f;
        const float gridY  = haveGrid ? region(regions->grid, 1) : y + h - 292.f;
        const float gridY1 = haveGrid ? region(regions->grid, 3) : y + h - 20.f;
        const float gap = 8.f;
        // Four across and two rows, filling the authored region rather than
        // the whole window width. The screenshot shows exactly that shape.
        const int cols = 4, gridRows = 2;
        const float tileW = ((gridX1 - gridX0) - gap * (cols - 1)) / (float)cols;
        const float tileH = std::min(152.f * RY,
            ((gridY1 - gridY) - gap * (gridRows - 1)) / (float)gridRows);
        std::vector<const bf6::ArmoryWeapon*> visibleWeapons;
        if (!packagePage)
        {
            for (const bf6::ArmoryWeapon& wep : a.weapons)
                if (wep.cls == s.category) visibleWeapons.push_back(&wep);
        }
        else if ((!packages || packages->empty()))
            if (const bf6::ArmoryWeapon* selected = find(a, s.weapon))
                visibleWeapons.push_back(selected);
        std::stable_sort(visibleWeapons.begin(), visibleWeapons.end(),
            [&](const bf6::ArmoryWeapon* lhs, const bf6::ArmoryWeapon* rhs) {
                const bool leftSelected = lhs->cls + "/" + lhs->name == s.weapon;
                const bool rightSelected = rhs->cls + "/" + rhs->name == s.weapon;
                return leftSelected != rightSelected && leftSelected;
            });
        int i = 0;
        if (packagePage && packages && !packages->empty())
        {
            for (const PackageRow& package : *packages)
            {
                const float tx2 = gridX0 + (i % cols) * (tileW + gap);
                const float ty2 = gridY + (i / cols) * (tileH + gap);
                ++i;
                if (i > cols * gridRows || ty2 + tileH > gridY1 + 1.f) break;
                const bool on = package.ordinal == 0;
                const bool over = hit(c, tx2, ty2, tileW, tileH);
                c.dl->AddRectFilled(ImVec2(tx2, ty2), ImVec2(tx2 + tileW, ty2 + tileH),
                                    over ? kTileHov : at(kTile, A50));
                c.dl->AddRect(ImVec2(tx2, ty2), ImVec2(tx2 + tileW, ty2 + tileH),
                              on ? kEquip : kLine, 0.f, 0, on ? 2.f : 1.f);
                bool drewArt = package_icon && package.texture_id >= 0 &&
                    package_icon(package.texture_id, tx2 + 8.f, ty2 + 10.f,
                                 tileW - 16.f, tileH - 48.f, icon_user);
                if (!drewArt && icon)
                {
                    const bf6::ArmoryWeapon* selected = find(a, s.weapon);
                    if (selected)
                        icon(selected->name.c_str(), selected->cls.c_str(),
                             nullptr, tx2 + 8.f, ty2 + 16.f,
                             tileW - 16.f, tileH - 50.f, icon_user);
                }
                const std::string label = !package.name.empty()
                    ? upper(package.name) : upper(package.key);
                const bf6::ArmoryWeapon* selected = find(a, s.weapon);
                const std::string weaponName = selected
                    ? upper(weapon_label(selected->name, weapon_name_fn,
                                         class_label_user)) : std::string();
                text(c, f.label, f.label_s.at(15.f), tx2 + 8.f,
                     ty2 + tileH - 40.f, kText, weaponName.c_str());
                text(c, f.body, f.body_s.at(12.f), tx2 + 8.f,
                     ty2 + tileH - 22.f, kTextDim, label.c_str());
            }
        }
        for (const bf6::ArmoryWeapon* weaponRow : visibleWeapons)
        {
            const bf6::ArmoryWeapon& wep = *weaponRow;
            const float tx2 = gridX0 + (i % cols) * (tileW + gap);
            const float ty2 = gridY + (i / cols) * (tileH + gap);
            i++;
            if (i > cols * gridRows || ty2 + tileH > gridY1 + 1.f) break;

            const std::string key = wep.cls + "/" + wep.name;
            const bool on = packagePage || (key == s.weapon);
            const bool over = hit(c, tx2, ty2, tileW, tileH);
            c.dl->AddRectFilled(ImVec2(tx2, ty2), ImVec2(tx2 + tileW, ty2 + tileH),
                                over ? kTileHov : at(kTile, A50));
            c.dl->AddRect(ImVec2(tx2, ty2), ImVec2(tx2 + tileW, ty2 + tileH),
                          on ? kEquip : kLine, 0.f, 0, on ? 2.f : 1.f);

            // Progress pips along the top, the way the game shows mastery.
            for (int p = 0; p < 10; p++)
                c.dl->AddRectFilled(ImVec2(tx2 + 8.f + p * 9.f, ty2 + 8.f),
                                    ImVec2(tx2 + 14.f + p * 9.f, ty2 + 12.f),
                                    p < 3 ? kTextDim : kLine);

            // The line-art silhouette, drawn from the game's own atlas above
            // the name - the same sprite the armory shows on its tiles.
            // The selected card reflects its live fitted set. Every other
            // card is deliberately the zero-attachment presentation.
            if (icon) icon(wep.name.c_str(), wep.cls.c_str(), on ? &s.fitted : nullptr,
                           tx2 + 8.f, ty2 + 16.f, tileW - 16.f, tileH - 50.f,
                           icon_user);

            text(c, f.label, f.label_s.at(15.f), tx2 + 8.f, ty2 + tileH - 40.f, kText,
                 upper(weapon_label(wep.name, weapon_name_fn, class_label_user)).c_str());
            bool hasFit = false;
            if (on) for (const auto& fit : s.fitted.by_slot)
                if (!fit.second.empty()) { hasFit = true; break; }
            // The game puts the PACKAGE name here - FACTORY on a stock weapon -
            // not a count of what is fitted. Every card in the reference shows
            // it, including the locked ones.
            const std::string packageLabel = packagePage ? upper(factory) :
                (s.factory_package ? std::string("FACTORY") :
                 (hasFit ? std::string("CUSTOM") : std::string("FACTORY")));
            text(c, f.body, f.body_s.at(12.f), tx2 + 8.f, ty2 + tileH - 22.f,
                 kTextDim, packageLabel.c_str());

            // The game shows no attachment-count suffix on a weapon card.

            if (!packagePage && over && c.clicked)
            {
                s.weapon = key;
                s.fitted.by_slot.clear();
                s.factory_package = true;
                s.weapon_changed = changed = true;
            }
        }
        return changed;
    }

    // ---- customize ---------------------------------------------------------
    const bf6::ArmoryWeapon* wep = find(a, s.weapon);
    if (!wep) return changed;

    const bool haveCustomize = s.page == Page::Customize && regions &&
        regions->customize_valid &&
        regions->customize_header[2] > regions->customize_header[0];
    const float customHeaderX = haveCustomize ? region(regions->customize_header, 0) : x + pad;
    const float customHeaderY = haveCustomize ? region(regions->customize_header, 1) : y + 54.f;

    // ATTACHMENT POINTS spent/total, with the pip strip the game draws beside
    // it. The budget is read from the weapon's own record; -1 means not read.
    {
        const int sp = spent(a, s);
        const int bud = s.budget > 0 ? s.budget : 100;
        const float pointsY = s.page == Page::Customize ?
            (haveCustomize ? customHeaderY + 90.f * RY : y + 142.f) : cy + 14.f;
        char pts[64];
        snprintf(pts, sizeof(pts), "%d/%d", sp, bud);
        if (s.page != Page::Customize)
            text(c, f.label, f.label_s.at(13.f), x + pad, pointsY, kTextDim,
                 "ATTACHMENT POINTS");
        text(c, f.mono, f.num_s.at(15.f),
             s.page == Page::Customize ? customHeaderX : x + pad,
             pointsY + (s.page == Page::Customize ? 0.f : 18.f), kText, pts);

        // ONE PIP PER TEN POINTS. A 100-point primary draws ten and a 60-point
        // sidearm six, which is what the screen shows; the twelve that used to
        // be here matched no budget the game issues. The count is taken from
        // the budget rather than fixed, so a weapon with a different allowance
        // gets a strip that still reads as tenths.
        const float px0 = (s.page == Page::Customize ? customHeaderX : x + pad) + 70.f * RX;
        const int pips = bud >= 10 ? bud / 10 : 1;
        const float pw = 13.f;
        for (int p = 0; p < pips; p++)
        {
            const bool full = (float)p / pips < (float)sp / (float)std::max(bud, 1);
            const float pipY = pointsY + (s.page == Page::Customize ? 1.f : 18.f);
            c.dl->AddRectFilled(ImVec2(px0 + p * pw, pipY),
                                ImVec2(px0 + p * pw + pw - 3.f, pipY + 14.f),
                                full ? kText : kLine);
        }
    }
    cy += 62.f;

    if (s.page == Page::Customize)
    {
        // This header names the active PACKAGE (FACTORY on a stock weapon),
        // not the weapon. The screen title already says CUSTOMIZE M4A1. The
        // old duplicate M4A1 heading was a semantic binding error.
        const std::string weaponName = info && info->valid && !info->factory_label.empty() ?
            upper(info->factory_label) : std::string("FACTORY");
        ImFont* hero = f.hero ? f.hero : f.header;
        const float packageY = haveCustomize ? customHeaderY + 16.f * RY : y + 70.f;
        text(c, hero, f.hero_s.at(76.f), customHeaderX, packageY, kText, weaponName.c_str());
        text(c, f.header, f.sub_s.at(38.f),
             customHeaderX + text_w(hero, f.hero_s.at(76.f), weaponName.c_str()) + 12.f,
             packageY + 14.f, kTextDim, "*");
        const float equipY = haveCustomize ? customHeaderY + 136.f * RY : y + 200.f;
        c.dl->AddRectFilled(ImVec2(customHeaderX, equipY),
                            ImVec2(customHeaderX + 92.f, equipY + 30.f), kEquip);
        text(c, f.label, f.label_s.at(12.f), customHeaderX + 12.f, equipY + 9.f,
             at(kOnAccent, A100), "EQUIPPED");

        const float sx = w / 1920.f, sy = h / 1080.f;
        const float gunX = x + w * .51f, gunY = y + h * .48f;
        const float gunRx = std::max(150.f, 235.f * sx);
        const float gunRy = std::max(55.f, 88.f * sy);

        if (!tiles || tiles->empty())
            text(c, f.label, f.label_s.at(13.f), x + pad, y + h * .5f, kLocked,
                 "customization tile anchors were not read from the install");

        for (const TileAnchor& anchor : (tiles ? *tiles : std::vector<TileAnchor>()))
        {
            const auto slotDef = wep->slots.find(anchor.code);
            if (slotDef == wep->slots.end()) continue;
            const std::string code = anchor.code;
            const float tileW = (anchor.width > 0.f ? anchor.width : 168.f) * sx;
            const float tileH = (anchor.height > 0.f ? anchor.height : 136.f) * sy;
            // The anchor is the point the tile HANGS FROM, and the authored
            // SizingPivot says which of the tile's own corners that is - 1.0 on
            // both axes for all twelve, i.e. the bottom-right. Treating it as
            // the top-left drew every tile a whole tile down and right.
            const float rx = x + anchor.x * sx - tileW * anchor.pivot_x;
            const float ry = y + anchor.y * sy - tileH * anchor.pivot_y;
            auto it = s.fitted.by_slot.find(code);
            const bool fitted = it != s.fitted.by_slot.end() && !it->second.empty();
            const bool over = hit(c, rx, ry, tileW, tileH);

            const float tileEndX = rx + tileW * anchor.attach_x;
            const float tileEndY = ry + tileH * anchor.attach_y;
            const float dx = tileEndX - gunX, dy = tileEndY - gunY;
            const float txGun = gunRx / std::max(std::fabs(dx), 1.f);
            const float tyGun = gunRy / std::max(std::fabs(dy), 1.f);
            const float tGun = std::min(txGun, tyGun);
            const float gunEndX = gunX + dx * tGun;
            const float gunEndY = gunY + dy * tGun;
            c.dl->AddLine(ImVec2(tileEndX, tileEndY), ImVec2(gunEndX, gunEndY),
                          at(kText, A50), 1.f);
            c.dl->AddCircleFilled(ImVec2(gunEndX, gunEndY), 3.f, kTextDim);

            if (fitted)
            {
                // The shipped attachment card is transparent line work over
                // the studio, not an opaque blue panel.
                if (over)
                    c.dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + tileW, ry + tileH),
                                        at(kTileHov, A25));
                c.dl->AddRect(ImVec2(rx, ry), ImVec2(rx + tileW, ry + tileH), kLine);
                text(c, f.label, f.label_s.at(12.f), rx + 10.f, ry + 8.f, kTextDim,
                     upper(anchor.caption.empty() ? anchor.slot : anchor.caption).c_str());
                float nameX = rx + 10.f;
                if (attachment_icon &&
                    attachment_icon(code.c_str(), it->second.c_str(),
                                    rx + 18.f, ry + 30.f, tileW - 36.f,
                                    tileH - 62.f, attachment_user))
                    nameX = rx + 10.f;
                text(c, f.body, f.body_s.at(14.f), nameX, ry + tileH - 24.f, kText,
                     attachment_label(code, it->second,
                                      attachment_title_fn ? attachment_title_fn
                                                          : attachment_name_fn,
                                      attachment_user));
                const int cst = cost_of(*wep, code, it->second);
                if (cst >= 0)
                {
                    char cb[24];
                    snprintf(cb, sizeof(cb), "%d", cst);
                    c.dl->AddRectFilled(ImVec2(rx + tileW - 34.f, ry),
                                        ImVec2(rx + tileW, ry + 24.f), kAccent);
                    text(c, f.mono, f.num_s.at(13.f), rx + tileW - 8.f - text_w(f.mono, f.num_s.at(13.f), cb),
                         ry + 5.f, at(kOnAccent, A100), cb);
                }
            }
            else
            {
                // Empty slots collapse to the compact input box authored by
                // the StartAnchor widget (32x32); retaining the full card is
                // what made the old screen look like a blue flowchart.
                const float iw = 32.f * sx, ih = 32.f * sy;
                const float ix = rx + (tileW - iw) * 0.5f;
                const float iy = ry + (tileH - ih) * 0.5f;
                c.dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + iw, iy + ih),
                                    at(kBg, A50));
                c.dl->AddRect(ImVec2(ix, iy), ImVec2(ix + iw, iy + ih), kLine);
                text(c, f.header, f.sub_s.at(24.f), ix + 8.f * sx, iy + 2.f * sy,
                     kTextDim, "+");
                const std::string cap = upper(anchor.caption.empty() ? anchor.slot : anchor.caption);
                const float capW = text_w(f.label, f.label_s.at(12.f), cap.c_str());
                const float capX = rx + (tileW - capW) * 0.5f;
                const float capY = anchor.attach_y >= 0.5f ? iy - 24.f * sy : iy + ih + 9.f * sy;
                text(c, f.label, f.label_s.at(12.f), capX, capY, kTextDim, cap.c_str());
            }
            if (over && c.clicked)
            {
                s.slot = code;
                s.page = Page::SlotPicker;
                s.slot_changed = changed = true;
            }
        }

        // The cosmetic footer is a separate live widget: three 192x144 cells
        // in a 12px Rime stack, in screen order WeaponSkin, WeaponCharm,
        // Stickers. It used to be three anonymous boxes placed under the gun.
        if (haveCustomize)
        {
            struct Cosmetic { int region_index; const char* title; const char* empty; };
            const Cosmetic cos[] = {
                { 2, "CAMOS /",    "NO CAMO" },
                { 1, "CHARMS /",   "NO CHARM" },
                { 0, "STICKERS /", "NO STICKER" }
            };
            for (const Cosmetic& q : cos)
            {
                const float* rr = regions->cosmetics[q.region_index];
                if (rr[2] <= rr[0] || rr[3] <= rr[1])
                    continue;
                const float x0 = region(rr, 0), y0 = region(rr, 1);
                const float x1 = region(rr, 2), y1 = region(rr, 3);
                c.dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), kLine);
                c.dl->AddLine(ImVec2(x0 + 12.f, y0 + 12.f),
                              ImVec2(x1 - 12.f, y1 - 24.f), kLine);
                c.dl->AddLine(ImVec2(x1 - 12.f, y0 + 12.f),
                              ImVec2(x0 + 12.f, y1 - 24.f), kLine);
                text(c, f.label, f.label_s.at(14.f), x0 + 10.f, y1 - 48.f,
                     kText, q.title);
                text(c, f.body, f.body_s.at(12.f), x0 + 10.f, y1 - 27.f,
                     kTextDim, q.empty);
            }
        }

        // The input footer is also authored on this screen. Its reference is
        // only the anchor; button widths are content-sized by their live font.
        const bool haveFooter = haveCustomize &&
            regions->footer_left[2] > regions->footer_left[0] &&
            regions->footer_left[3] > regions->footer_left[1];
        const float by = haveFooter ? region(regions->footer_left, 1) : y + h - 42.f;
        c.dl->AddRectFilled(ImVec2(x, by), ImVec2(x + w, y + h), kBg);
        struct Action { const char* key; const char* label; bool enabled; };
        const Action actions[] = {
            { "Z", "FIRING RANGE", true },
            { "X", "FAVORITE", true },
            { "X", "RESET (HOLD)", false },
            { "I", "INSPECT", true }
        };
        float ax = haveFooter ? region(regions->footer_left, 0) : x + pad;
        for (const Action& action : actions)
        {
            const ImU32 col = action.enabled ? kText : kTextDim;
            const float labelW = text_w(f.label, f.label_s.at(12.f), action.label);
            const float bw = labelW + 48.f;
            c.dl->AddRect(ImVec2(ax, by), ImVec2(ax + bw, by + 40.f * RY), kLine);
            c.dl->AddRectFilled(ImVec2(ax + 10.f, by + 11.f),
                                ImVec2(ax + 29.f, by + 31.f), at(kText, A25));
            text(c, f.mono, f.num_s.at(12.f), ax + 15.f, by + 14.f, col, action.key);
            text(c, f.label, f.label_s.at(12.f), ax + 38.f, by + 13.f, col,
                 action.label);
            ax += bw + 8.f;
        }

        // Same verified-only stat block as selection, moved to bottom-right.
        if (stats && stats->valid)
        {
            const char* labs[3] = { "DMG", "ROF", "MAG" };
            const int vals[3] = { stats->damage, stats->rate_of_fire, stats->magazine };
            const bool haveStats = regions && regions->valid &&
                regions->stats[2] > regions->stats[0];
            const float statX = haveStats ? region(regions->stats, 0) + 20.f * RX
                                          : x + w - 255.f;
            const float statY = haveStats ? region(regions->stats, 1) + 150.f * RY
                                          : y + h - 230.f;
            for (int i = 0; i < 3; i++)
            {
                char value[24]; snprintf(value, sizeof(value), "%d", vals[i]);
                const float rowY = statY + i * 84.f * RY;
                text(c, f.label, f.label_s.at(13.f), statX, rowY, kTextDim, labs[i]);
                text(c, f.header, f.sub_s.at(36.f), statX, rowY + 22.f, kText, value);
            }
        }
        return changed;
    }

    // ---- slot picker -------------------------------------------------------
    auto sit = wep->slots.find(s.slot);
    if (sit == wep->slots.end()) return changed;

    // The fitted attachment's identity, the way the game presents it: its name
    // large on the left with the description beneath. Both come from the
    // catalogue; an attachment the catalogue does not cover shows neither
    // rather than a placeholder.
    {
        const auto fitIt = s.fitted.by_slot.find(s.slot);
        if (fitIt != s.fitted.by_slot.end() && !fitIt->second.empty())
        {
            const char* nm = attachment_label(s.slot, fitIt->second,
                                              attachment_name_fn, attachment_user);
            text(c, f.hero ? f.hero : f.header, f.hero_s.at(64.f),
                 x + 90.f, y + 400.f, kText, upper(nm).c_str());
            const float chipY = y + 478.f;
            const float chipW = text_w(f.label, f.label_s.at(14.f), "DEFAULT") + 28.f;
            c.dl->AddRectFilled(ImVec2(x + 90.f, chipY),
                                ImVec2(x + 90.f + chipW, chipY + 26.f), at(kTile, A50));
            c.dl->AddLine(ImVec2(x + 90.f, chipY),
                          ImVec2(x + 90.f, chipY + 26.f), kTextDim);
            text(c, f.label, f.label_s.at(14.f), x + 100.f, chipY + 5.f,
                 kText, "DEFAULT");
            if (attachment_desc_fn)
                if (const char* d = attachment_desc_fn(s.slot.c_str(),
                                                       fitIt->second.c_str(),
                                                       attachment_user))
                    if (*d)
                        text(c, f.body, f.body_s.at(16.f), x + 90.f, y + 520.f,
                             kTextDim, d);

            const float actionY = y + 560.f;
            c.dl->AddRectFilled(ImVec2(x + 90.f, actionY),
                                ImVec2(x + 222.f, actionY + 48.f), kEquip);
            text(c, f.label, f.label_s.at(16.f), x + 102.f, actionY + 12.f,
                 at(kOnAccent, A100), "EQUIPPED");
            c.dl->AddRectFilled(ImVec2(x + 229.f, actionY),
                                ImVec2(x + 409.f, actionY + 48.f), kTile);
            c.dl->AddRect(ImVec2(x + 229.f, actionY),
                          ImVec2(x + 409.f, actionY + 48.f), kLine);
            text(c, f.label, f.label_s.at(16.f), x + 241.f, actionY + 12.f,
                 kText, "Z  FIRING RANGE");
        }
    }

    // THE PICKER SITS WHERE THE GAME PUTS IT.
    //
    // The authored panel is pinned to the bottom of the screen so the 3D weapon
    // stays readable behind it. Laying the grid out from the top instead put it
    // straight over the gun - which is what it looked like.
    const float sxp = w / 1920.f, syp = h / 1080.f;
    const float gridX = picker_rect ? x + picker_rect[0] * sxp : x + pad;
    const bool haveSharedGrid = regions && regions->valid &&
        regions->grid[3] > regions->grid[1];
    // The picker root's weighted parent height is a host-owned runtime value;
    // solving that old-schema partition alone leaves its child 292 px too
    // high.  The shared MetaCustomization shell already gives us the live
    // bottom-grid origin used by the weapon page and picker alike.
    const float gridY = haveSharedGrid ? region(regions->grid, 1) :
        (picker_rect ? y + picker_rect[1] * syp : cy);
    const float gridW = picker_rect ? (picker_rect[2] - picker_rect[0]) * sxp : w - pad * 2.f;
    const float gridH = picker_rect ? (picker_rect[3] - picker_rect[1]) * syp : h - cy - 20.f;

    const float gap = 8.f;
    // The selection cell uses the same live 168x136 attachment-card content
    // box as the category screen.  Derive it from those decoded anchors rather
    // than estimating columns from a screenshot or stretching rows to fill.
    float authoredCellW = 168.f, authoredCellH = 136.f;
    if (tiles)
        for (const TileAnchor& tile : *tiles)
            if (tile.width > 0.f && tile.height > 0.f)
            { authoredCellW = tile.width; authoredCellH = tile.height; break; }
    const float tileW = authoredCellW * sxp;
    const float tileH = authoredCellH * syp;
    const int cols = (int)std::max(1.f,
        std::floor((gridW + gap) / (tileW + gap)));
    int i = 0;

    // The authored container scroll-clips; a following row may be partially
    // visible at the bottom, but it must never be compressed to fit.
    c.dl->PushClipRect(ImVec2(gridX, gridY),
                       ImVec2(gridX + gridW, gridY + gridH), true);

    // THE STOCK PART IS NOT A CHOICE.
    //
    // Every slot that has one ships exactly one attachment costing 0 points -
    // nomuzzle, ergonomic, secondarysight, rightrail, toprail, bottomrail - and
    // it is what the weapon already has when nothing is fitted. Listing it
    // beside "None" offered the same state twice and made a bare rail look like
    // something you buy. It now names the None tile instead.
    //
    // 268 of the 632 populated slots carry exactly one; 350 carry none, and
    // those keep a plain "None". Fourteen carry more than one, and there the
    // first is taken as stock and the rest stay listed as free options.
    std::string stock;
    for (const std::string& t : sit->second)
        if (cost_of(*wep, s.slot, t) == 0) { stock = t; break; }

    // "None" first: unfitting is a real choice and the game offers it.
    for (int pass = 0; pass < 2; pass++)
    for (size_t k = 0; k < (pass == 0 ? size_t(1) : sit->second.size()); k++)
    {
        const bool none = (pass == 0);
        const std::string tok = none ? stock : sit->second[k];
        if (!none && !stock.empty() && tok == stock) continue;   // it IS the None tile
        const float tx2 = gridX + (i % cols) * (tileW + gap);
        const float ty2 = gridY + (i / cols) * (tileH + gap);
        i++;
        if (ty2 >= gridY + gridH) break;

        auto it = s.fitted.by_slot.find(s.slot);
        const bool on = (it == s.fitted.by_slot.end() || it->second.empty()) ? none
                                                                            : it->second == tok;
        const bool over = hit(c, tx2, ty2, tileW, tileH);
        c.dl->AddRectFilled(ImVec2(tx2, ty2), ImVec2(tx2 + tileW, ty2 + tileH),
                            over ? kTileHov : kTile);
        c.dl->AddRect(ImVec2(tx2, ty2), ImVec2(tx2 + tileW, ty2 + tileH),
                      on ? kEquip : kLine, 0.f, 0, on ? 2.f : 1.f);

        const int cst = none ? 0 : cost_of(*wep, s.slot, tok);
        if (cst >= 0)
        {
            char cb[24];
            snprintf(cb, sizeof(cb), "%d", cst);
            c.dl->AddRectFilled(ImVec2(tx2 + 6.f, ty2 + 6.f),
                                ImVec2(tx2 + 6.f + text_w(f.mono, f.num_s.at(12.f), cb) + 12.f,
                                       ty2 + 24.f), at(kOnAccent, A100));
            text(c, f.mono, f.num_s.at(12.f), tx2 + 12.f, ty2 + 8.f, kText, cb);
        }
        // The attachment's own art, from the catalogue's atlas and index. The
        // cell is mostly picture in the game; here it takes the upper band and
        // leaves the label its line.
        if (attachment_icon && !tok.empty())
            attachment_icon(s.slot.c_str(), tok.c_str(),
                            tx2 + 8.f, ty2 + 4.f,
                            tileW - 16.f, tileH - 30.f, attachment_user);

        const char* cell_label =
            none ? (stock.empty() ? "None"
                                  : attachment_label(s.slot, stock, attachment_name_fn, attachment_user))
                 : attachment_label(s.slot, tok, attachment_name_fn, attachment_user);
        text(c, f.body, f.body_s.at(13.f), tx2 + 8.f, ty2 + tileH - 24.f, on ? kEquip : kText,
             cell_label);

        if (over && c.clicked)
        {
            // Picking the stock tile UNFITS the slot rather than fitting the
            // stock token: "stock" and "nothing fitted" are the same state and
            // the assembly path expects the empty one.
            s.fitted.by_slot[s.slot] = none ? std::string() : tok;
            s.factory_package = false;
            s.fitted_changed = changed = true;
        }
    }
    c.dl->PopClipRect();

    // The picker reuses the same authored Weapon Stats Main Container as the
    // weapon screen. Keep the three values whose read paths are complete; the
    // four DiceExpression bars remain absent until their tainted providers are
    // decoded, rather than copying the values from an old screenshot.
    if (stats && stats->valid)
    {
        const bool haveStats = regions && regions->valid &&
            regions->stats[2] > regions->stats[0];
        const float statX = haveStats ? region(regions->stats, 0) + 20.f * RX
                                      : x + w - 255.f;
        const float statY = haveStats ? region(regions->stats, 1) + 150.f * RY
                                      : y + h - 230.f;
        const char* labs[3] = { "DMG", "ROF", "MAG" };
        const int vals[3] = { stats->damage, stats->rate_of_fire, stats->magazine };
        for (int row = 0; row < 3; ++row)
        {
            char value[24]; snprintf(value, sizeof(value), "%d", vals[row]);
            const float rowY = statY + row * 84.f * RY;
            text(c, f.label, f.label_s.at(13.f), statX, rowY, kTextDim, labs[row]);
            text(c, f.header, f.sub_s.at(36.f), statX, rowY + 22.f, kText, value);
        }
    }
    return changed;
}

}  // namespace armory_ui
