// The armory FLOW: pick a category, pick a weapon, edit its attachments.
//
// The goal here is functional parity, not a pixel-perfect reimplementation of
// the Rime element tree. The game's flow is:
//
//     [category tabs]  ->  [weapon grid]  ->  [customize]  ->  [slot picker]
//      ASSAULT RIFLE        tile per         slot tiles       options for one
//      CARBINE, SMG...      weapon, with     around the       slot, each with
//                           cost/lock/pips   3D weapon        its points cost
//
// with a points budget spent across the fitted set ("ATTACHMENT POINTS
// 70/100") and the camera reframing onto whichever slot is being edited.
//
// Everything it shows is read from the install at runtime: the categories from
// the screen's own config asset, the roster and slots from the mount's name
// table, the costs from the attachment records. Nothing is loaded from an
// exported table.
//
// Drawn through ImDrawList as a quad batcher, in the game's own palette and
// typefaces, with square corners - the front end has no rounded-rectangle
// idiom (5,709 of 7,402 authored corners have curvature exactly 0).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace bf6 { struct Armory; struct ArmoryWeapon; }
struct ImFont;

namespace armory_ui {

enum class Page { Weapons, Packages, Customize, SlotPicker };

struct Fitted {
    // slot code -> attachment token. Empty means the slot is unfitted, which
    // is a real state and not the same as "no such slot".
    std::map<std::string, std::string> by_slot;
};

struct State {
    Page        page = Page::Weapons;
    std::string category;        // weapon class tab: assaultrifle, carbine...
    std::string weapon;          // "carbine/m4a1"
    std::string slot;            // slot code being edited, when on SlotPicker
    Fitted      fitted;
    bool        factory_package = true;

    // The points budget, read from the weapon's own equipment record: 100 on
    // primaries, 60 on sidearms. -1 until read.
    int budget = -1;

    // Set when the user picks something, so the caller can reframe the camera
    // and rebuild the mesh without polling every field for changes.
    bool weapon_changed = false;
    bool slot_changed   = false;
    bool fitted_changed = false;
};

// Draw one weapon's outline sprite into a rect, if the caller has it.
// A hook rather than a dependency: the UI should not know how an atlas is
// loaded, and a weapon with no atlas (melee, vehicle) simply draws nothing.
typedef bool (*DrawIconFn)(const char* weapon_bare, const char* weapon_class,
                           const Fitted* fitted,
                           float x, float y, float w, float h, void* user);

// The colours this screen paints with, every one read out of the game's own
// 111-entry palette at runtime rather than typed in here.
//
// Filled by the caller, because armory_ui does not read the install - the same
// reason DrawIconFn is a hook. Two entries are DERIVED rather than named, and
// say so where they are computed: the game authors no "hovered tile" and no
// "hairline" colour, it composites ones it does author at an authored alpha
// step.
// The front end's six-step alpha ramp, by the names the theme tokens use.
// Not a linear percentage scale - roughly a 1.8 gamma of the numbers in the
// names - which is exactly why it has to be read rather than computed.
enum Alpha { A05, A10, A25, A50, A75, A100, kAlphaSteps };

struct Palette {
    bool  valid = false;
    // Filled from the alphas the game's own elements carry - not defaulted
    // here, because a default is the hardcode wearing a hat: it would survive
    // a failed read and paint a plausible screen. Every transparency this
    // screen draws is one of these six, so there is no loose 0.37 below.
    float alpha[kAlphaSteps] = { 0, 0, 0, 0, 0, 0 };
    unsigned surface = 0;     // colorlist/DarkNavy  - the screen ground
    unsigned tile = 0;        // colorlist/Charcoal  - a card
    unsigned tile_hover = 0;  // derived: tile over FE-Focus at the 0.08 step
    unsigned text = 0;        // FE-Text
    unsigned text_dim = 0;    // FE-Text at the authored 0.60 alpha step
    unsigned line = 0;        // FE-Text at the authored 0.27 alpha step
    unsigned accent = 0;      // FE-Class-Proficiency
    unsigned positive = 0;    // FE-Online-Positive  - equipped / owned
    unsigned negative = 0;    // FE-Error-Negative   - locked
};

// A weapon-class folder name -> the label the GAME shows for that class.
//
// Supplied by the caller because building it means reading one weapon's
// UIWeaponAbilityMetadata per class. Returning null is a valid answer and the
// screen then shows the folder name: that says "the game gave us no label
// here", which is a true statement, where an invented English string is not.
typedef const char* (*ClassLabelFn)(const char* cls, void* user);

// slot + token -> the name the GAME shows for that attachment, or null when
// the roster and the catalogue do not meet on it. Null is a real answer and
// the screen then shows the filename token, which is true where an invented
// prettified string would not be.
typedef const char* (*AttachmentNameFn)(const char* slot, const char* token,
                                        void* user);

// slot + token -> the shorter authored title used on a fitted Customize tile.
// This is distinct from the catalogue display name: XPS3 is `SU-123 1.50x` in
// the picker but `Sight 1.50x` on the fitted tile.
typedef const char* (*AttachmentTitleFn)(const char* slot, const char* token,
                                         void* user);

// Draw one attachment's icon into a rect. The catalogue resolves it to an
// atlas plus an index; the caller owns loading that page and blitting the uv
// rect. Returns false when the attachment has no art, which is a real state -
// six of m4a1's ninety-nine rows - and draws nothing rather than a stand-in.
typedef bool (*DrawAttachmentIconFn)(const char* slot, const char* token,
                                     float x, float y, float w, float h,
                                     void* user);

// Draw an already-resolved package texture. `texture_id` is the live resource
// id returned with the package payload; it is deliberately opaque here.
typedef bool (*DrawPackageIconFn)(int texture_id, float x, float y,
                                  float w, float h, void* user);

// A weapon's bare token -> the name the GAME shows for it. The grid was
// labelling cards `m4a1` and `rpk74m` where the game says M4A1 and RPK-74M;
// worse, `m16a3` is shown as M16A4 and `minifix` as Mini Scout, so the token
// is not even a shortened form of the name. Null falls back to the token.
typedef const char* (*WeaponNameFn)(const char* bare, void* user);

// slot + token -> the attachment's localised DESCRIPTION, or null. 95 of
// m4a1's 99 catalogue rows carry one and the game shows it under the name on
// the picker screen.
typedef const char* (*AttachmentDescFn)(const char* slot, const char* token,
                                        void* user);

// One floating attachment tile on the Customize screen, read from the game's
// own menuweaponattachmenticonscreen rather than transcribed.
//
// `x`,`y` is the point the tile hangs from on the 1920x1080 authored canvas -
// every one of the twelve is point-anchored at the screen's bottom-right, so
// solving it is anchor*canvas + offset. `attach` is the point ON THE TILE the
// leader line leaves from, which is why the upper tiles' lines run down and
// the lower ones' run up.
//
// The gun end is deliberately absent. All twelve authored StartAnchors are
// byte-identical 20x20 boxes at the origin because the game writes that
// position at runtime; a consumer that used them would draw a starburst.
struct TileAnchor {
    std::string slot;     // the game's own slot name: Sight, OpticAccessory, ...
    std::string code;     // the three-letter code the same slot uses in filenames
    std::string caption;  // what the GAME captions this tile, e.g. "Scope"
    float x = 0.f, y = 0.f;
    // THE ANCHOR IS NOT THE TOP-LEFT CORNER. All twelve tiles are authored with
    // SizingPivot 1.0 on both axes, so (x,y) is the box's BOTTOM-RIGHT. Drawing
    // from it as though it were the top-left puts every tile one whole tile down
    // and to the right of where the game puts it.
    float pivot_x = 0.f, pivot_y = 0.f;
    float attach_x = 0.f, attach_y = 0.f;
    // The matching <Slot>StartAnchor owns a separate attachment point.  Its
    // authored position is intentionally empty because the runtime writes the
    // projected weapon point, but its pivot/attach/size are still shipped UI
    // data and must be retained when drawing the connector.
    float start_pivot_x = 0.f, start_pivot_y = 0.f;
    float start_attach_x = 0.f, start_attach_y = 0.f;
    float start_width = 20.f, start_height = 20.f;
    // The attachment widget's own content box. This is 168x136 on the
    // current install, but remains per-row and live so a patched layout moves
    // and resizes without a viewer update.
    float width = 0.f, height = 0.f;
    // False means this weapon has no attachment rows for the authored
    // category.  The category remains on screen as the game's locked control;
    // it must not disappear merely because no three-letter filename code can
    // be joined for it.
    bool available = false;
};

// WHERE THE GAME PUTS THINGS, on its 1920x1080 canvas.
//
// Solved from menuweaponscreen's own element tree, not measured off a picture
// and not invented here. The three that matter:
//
//   Grid Container              48,709 - 1290,1050   the card grid, bottom left
//   NavigationTabs              64,634 - 1146,674    the class tabs above it
//   Weapon Stats Main Container 1324,570 - 1812,1008 the stat block, bottom right
//
// The stat block solves to the same box on the attachment picker screen too,
// which is what says these are screen regions rather than one screen's quirk.
// Zero width means the region was not read and the caller falls back.
struct Regions {
    bool  valid = false;
    float grid[4]  = { 0, 0, 0, 0 };
    float tabs[4]  = { 0, 0, 0, 0 };
    float stats[4] = { 0, 0, 0, 0 };

    // weaponattachmentcategoryscreen is a separate authored screen. These
    // cannot be inferred from menuweaponscreen: its header, cosmetic row and
    // input footers are different widget references with their own anchors.
    bool  customize_valid = false;
    float customize_header[4] = { 0, 0, 0, 0 };
    float customize_stack[4]  = { 0, 0, 0, 0 };
    float footer_left[4]      = { 0, 0, 0, 0 };
    float footer_right[4]     = { 0, 0, 0, 0 };
    // Solved live after fit-to-content measurement. Data order is Stickers,
    // WeaponCharm, WeaponSkin; screen order is reversed by the Rime stack.
    float cosmetics[3][4] = {};
};

// One authored type family's ladder.  The style-name pixel value is the
// selector used by manual/provider-backed UI; the authored PointSize is the
// font em that must reach the rasteriser.  Those values are deliberately not
// related by one universal ratio--each shipped face has different metrics.
//
// BF6's text sizes are a DISCRETE SCALE, not a free number: body is 12/14/16/20
// px, label adds 24, subheader runs 14 to 32, headerhero 64/72/96, numbers 8 to
// 144. This screen used to draw at 11, 13, 15, 25, 36 and 38 px - none of which
// the game authors anywhere.
//
// So a draw site names the step it wants and `at()` returns the authored size
// nearest it. The number at the call site is a selector into the game's scale,
// the way a colour name is a selector into its palette; the value that reaches
// the rasteriser is always one the game authored.
struct Scale {
    float selector_px[12] = { 0 };
    float em[12] = { 0 };
    int   n = 0;
    void add(float selector, float authored_em)
    {
        for (int i = 0; i < n; i++)
            if (selector_px[i] == selector)
            {
                em[i] = authored_em;
                return;
            }
        if (n < 12)
        {
            selector_px[n] = selector;
            em[n] = authored_em;
            ++n;
        }
    }
    float at(float want) const
    {
        if (n <= 0) return want;          // unread: the caller's number, and it shows
        int best = 0;
        float bd = want > selector_px[0]
            ? want - selector_px[0] : selector_px[0] - want;
        for (int i = 1; i < n; i++)
        {
            const float d = want > selector_px[i]
                ? want - selector_px[i] : selector_px[i] - want;
            if (d < bd) { bd = d; best = i; }
        }
        return em[best];
    }
};

struct Fonts {
    Scale body_s, label_s, sub_s, hero_s, num_s;
    ImFont* header = nullptr;   // BF_HEADLINE_SEMI_BOLD
    ImFont* hero   = nullptr;   // same face, separately baked for weapon names
    ImFont* label  = nullptr;   // BF_SUB_HEADLINE_BOLD
    ImFont* body   = nullptr;   // BFText-Regular
    ImFont* mono   = nullptr;   // BF_SUB_HEADLINE_MONO, for costs and counts
};

struct BaseStats {
    bool valid = false;
    int damage = 0;
    int rate_of_fire = 0;
    int magazine = 0;
};

struct WeaponInfo {
    bool valid = false;
    std::string name, description, class_label, factory_label;
    std::vector<std::string> traits;
};

struct PackageRow {
    std::string key, name, description;
    std::string unlock_asset, skin;
    std::vector<std::pair<std::string, std::string>> fits;
    int ordinal = -1;
    int texture_id = -1;
    bool has_gameplay_config = false;
};

// Points spent by the current fitted set, and the per-attachment cost lookup
// that produced it. Separate so a caller can show both halves of "70/100".
int spent(const bf6::Armory& a, const State& s);

// Draw the whole flow for one frame. Returns true if anything changed that the
// caller needs to act on (new weapon, new slot, new fitted part).
// `tiles` are the customization tile anchors read from the game. When it is
// empty the Customize page says so and draws no tiles, rather than falling
// back to positions typed in here.
bool draw(const bf6::Armory& a, State& s, const Fonts& f, const Palette& pal,
          float x, float y, float w, float h,
          DrawIconFn icon = nullptr, void* icon_user = nullptr,
          const BaseStats* stats = nullptr, const WeaponInfo* info = nullptr,
          ClassLabelFn class_label = nullptr, void* class_label_user = nullptr,
          const std::vector<TileAnchor>* tiles = nullptr,
          AttachmentNameFn attachment_name = nullptr, void* attachment_user = nullptr,
          AttachmentTitleFn attachment_title = nullptr,
          DrawAttachmentIconFn attachment_icon = nullptr,
          WeaponNameFn weapon_name = nullptr,
          const Regions* regions = nullptr,
          AttachmentDescFn attachment_desc = nullptr,
          // Ordered class tokens emitted by the installed
          // um_weaponnavigationlistgenerator input. Null falls back to the
          // roster only when that runtime provider could not be read.
          const std::vector<std::string>* navigation_classes = nullptr,
          // The attachment picker's authored rect on the 1920x1080 canvas,
          // solved from the game's own selection screen. Null lays the grid out
          // from the top, which is where it used to sit and is over the gun.
          const float* picker_rect = nullptr,
          const std::vector<PackageRow>* packages = nullptr,
          DrawPackageIconFn package_icon = nullptr);

}  // namespace armory_ui
