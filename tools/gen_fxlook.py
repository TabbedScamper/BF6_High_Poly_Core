#!/usr/bin/env python
"""Generate the bf6_fx_look struct, its field enum and its resolver.

WHY GENERATED. The look is now 80 authored properties wide, each needing a C
field, an enum entry in the SAME order, and a resolver line with the right arity
for its type. Written by hand, the three drift: an enum entry inserted in the
middle silently reassigns every `has` bit after it, and a Vec2 read with the
Vec3 helper walks off the end of the value. One table, three outputs, no drift.

The pids and types come from data/fx_property_ids_v2.tsv in the research repo -
2,525 named parameters recovered from the game - never from guessing at hashes.
The counts beside each are the SHIPPED OVERRIDE counts, which is what says a
property is worth resolving at all.

    python tools/gen_fxlook.py          # rewrite the generated regions in place
    python tools/gen_fxlook.py --check  # fail if they are out of date
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# name, pid, kind, C field, enum suffix, shipped override count, note
# kind: f=Float f2=Vec2 f3=Vec3 f4=Vec4 i=Int/Bool
FIELDS = [
    # ---- size -------------------------------------------------------------
    ("BaseSize",        0xBD355AD5, "f",  "base_size",        "BASE_SIZE",        22704, "the sprite quad edge"),
    ("BaseSizeBias",    0xBB40784C, "f",  "base_size_bias",   "BASE_SIZE_BIAS",    3034, ""),
    # THREE SEPARATE PARAMETERS, not one Vec3. SizeX/Y/Z are consecutive pids
    # and each is a Vec2 whose .x is the authored magnitude; reading the first
    # as a Vec3 takes SizeX's own .x/.y/.z, which silently drops SizeY and SizeZ
    # (4,949 and 5,340 shipped overrides) and reads the curve's other end as a
    # size.
    ("SizeX",           0x0DCF4958, "f",  "size_x",           "SIZE_X",            5416, ".x of a Vec2"),
    ("SizeY",           0x0DCF4959, "f",  "size_y",           "SIZE_Y",            4949, ".x of a Vec2"),
    ("SizeZ",           0x0DCF495A, "f",  "size_z",           "SIZE_Z",            5340, ".x of a Vec2"),
    ("SpawnSize",       0x8A1C76DB, "f2", "spawn_size",       "SPAWN_SIZE",        6424, "mesh particle scale"),
    ("SizeOverLife",    0x0AFB1F08, "f4", "size_over_life",   "SIZE_OVER_LIFE",     118, "cubic in normalised age"),
    ("SizeCurve",       0x8B256B37, "f4", "size_curve",       "SIZE_CURVE",        5898, "cubic in normalised age"),
    ("SizeYMult",       0x8C70A459, "f",  "size_y_mult",      "SIZE_Y_MULT",       4872, ""),
    ("Scale",           0x0DC8309D, "f3", "scale",            "SCALE",             2453, ""),
    ("ScaleXMult",      0x539877A5, "f",  "scale_x_mult",     "SCALE_X_MULT",       916, ""),
    ("ScaleZMult",      0x537688A7, "f",  "scale_z_mult",     "SCALE_Z_MULT",       915, ""),
    ("PivotY",          0xC9846F88, "f",  "pivot_y",          "PIVOT_Y",           5131, "moves the card's origin off centre"),
    # ---- opacity and colour ----------------------------------------------
    ("Opacity",         0x39F20FDC, "f",  "opacity",          "OPACITY",          24145, ""),
    ("OpacityMult",     0x01C5CADC, "f",  "opacity_mult",     "OPACITY_MULT",         0, ""),
    ("OpacityOverLife", 0x3D1607D4, "f4", "opacity_over_life","OPACITY_OVER_LIFE",22358, "cubic in normalised age"),
    ("Color",           0x0CA8C5F8, "f3", "color",            "COLOR",                0, "linear"),
    ("ColorMult",       0x1EE568B8, "f3", "color_mult",       "COLOR_MULT",           0, ""),
    ("Color0",          0xA1C184C8, "f3", "color0",           "COLOR0",           16276, "gradient start, linear"),
    ("Color1",          0xA1C184C9, "f3", "color1",           "COLOR1",           12742, "gradient end, linear"),
    ("RandomColorMin",  0xE5C35109, "f3", "random_color_min", "RANDOM_COLOR_MIN", 13683, "per-particle tint range, linear"),
    ("RandomColorMax",  0xE5C35217, "f3", "random_color_max", "RANDOM_COLOR_MAX", 13883, ""),
    ("Intensity",       0xE4AABCEA, "f",  "intensity",        "INTENSITY",         1435, ""),
    ("Temperature",     0x84477F09, "f",  "temperature",      "TEMPERATURE",       1485, "blackbody tint for fire"),
    ("Ramp",            0x7C896E0B, "i",  "ramp",             "RAMP",              2596, ""),
    ("AlphaCullThreshold", 0x1487F4B0, "f", "alpha_cull",     "ALPHA_CULL",           0, ""),
    # ---- rotation ---------------------------------------------------------
    ("RotationSpeed",   0x2FD2E956, "f",  "rotation_speed",   "ROTATION_SPEED",   27938, ""),
    ("RotationSpeedBias", 0x47553DCF, "f","rotation_speed_bias","ROTATION_SPEED_BIAS",4167, ""),
    ("RotationOverLife",0x0C8950D9, "f4", "rotation_over_life","ROTATION_OVER_LIFE",8814, "cubic in normalised age"),
    ("SpawnRotationMin",0x80860520, "f3", "spawn_rotation_min","SPAWN_ROTATION_MIN",1331, "radians"),
    ("SpawnRotationMax",0x8086043E, "f3", "spawn_rotation_max","SPAWN_ROTATION_MAX",1652, "radians"),
    ("SpawnRotationSpeed", 0xBB9A5E6D, "f","spawn_rotation_speed","SPAWN_ROTATION_SPEED",6851, ""),
    ("SpawnRotationSpeedMult", 0xA56991AD, "f2","spawn_rotation_speed_mult","SPAWN_ROTATION_SPEED_MULT",1258, ""),
    ("SpawnRotationSpeedMinMult", 0x4C7F1E67, "f","spawn_rotation_speed_min_mult","SPAWN_ROTATION_SPEED_MIN_MULT",1022, ""),
    # ---- motion and forces ------------------------------------------------
    ("SpawnSpeed",      0xCDB76C39, "f",  "spawn_speed",      "SPAWN_SPEED",      28486, ""),
    ("SpawnSpeedMult",  0x77706079, "f2", "spawn_speed_mult", "SPAWN_SPEED_MULT",  6091, "min/max multiplier"),
    ("SpawnSpeedMinMult", 0xF5BFFA33, "f","spawn_speed_min_mult","SPAWN_SPEED_MIN_MULT",15502, ""),
    ("SpawnSpeedBias",  0x77711EE0, "f",  "spawn_speed_bias", "SPAWN_SPEED_BIAS",  1170, ""),
    ("SpawnSpeedCurve", 0x65A739AE, "f4", "spawn_speed_curve","SPAWN_SPEED_CURVE", 2884, "cubic in normalised age"),
    ("SpeedMult",       0x3DAD1982, "f",  "speed_mult",       "SPEED_MULT",        3903, ""),
    ("Drag",            0x7C7FD695, "f",  "drag",             "DRAG",             30351, ""),
    ("DragMinMult",     0x5A7B475F, "f",  "drag_min_mult",    "DRAG_MIN_MULT",    21326, "low end of the drag range"),
    ("DragOverLife",    0x2339F49D, "f4", "drag_over_life",   "DRAG_OVER_LIFE",   11602, "cubic in normalised age"),
    ("Gravity",         0xC46720E3, "f",  "gravity",          "GRAVITY",          12616, ""),
    ("Buoyancy",        0x4E4BFB91, "f",  "buoyancy",         "BUOYANCY",         15173, "what makes smoke RISE, not spawn speed"),
    ("BuoyancyOverLife",0x384AF999, "f4", "buoyancy_over_life","BUOYANCY_OVER_LIFE",7341, "cubic in normalised age"),
    ("WindStrength",    0xE0A01AD4, "f",  "wind_strength",    "WIND",             28123, ""),
    ("RandomForce",     0x441BEE03, "f",  "random_force",     "RANDOM_FORCE",     18590, "the turbulence a plume curls with"),
    ("RandomForceOverLife", 0x3858A30B, "f4","random_force_over_life","RANDOM_FORCE_OVER_LIFE",2346, "cubic"),
    ("LocalForce",      0xEFC8A035, "f3", "local_force",      "LOCAL_FORCE",       4457, "a constant directed push"),
    ("LocalForceOverLife", 0x12079D3D, "f4","local_force_over_life","LOCAL_FORCE_OVER_LIFE",3167, "cubic"),
    ("DirectionFromEmitterOrigin", 0x27319714, "f","direction_from_origin","DIRECTION_FROM_ORIGIN",3468, "1 = radial burst"),
    ("Restitution",     0x8906E021, "f",  "restitution",      "RESTITUTION",       7340, "bounce, collision only"),
    ("RestitutionVariance", 0xB663BD04, "f","restitution_variance","RESTITUTION_VARIANCE",1097, ""),
    ("Friction",        0x12405B67, "f",  "friction",         "FRICTION",          6890, "collision only"),
    ("FrictionVariance",0x773DD7C2, "f",  "friction_variance","FRICTION_VARIANCE",  1012, ""),
    # ---- spawn volume -----------------------------------------------------
    ("SpawnPosition",   0xFECEFDE7, "f3", "spawn_position",   "SPAWN_POSITION",    5893, "offset from the emitter"),
    ("SpawnScale",      0xCDAE4A26, "f3", "spawn_scale",      "SPAWN_SCALE",       2013, "the spawn box's extents"),
    ("InnerRadius",     0xC6BBC2C3, "f",  "inner_radius",     "INNER_RADIUS",      1598, ""),
    ("OuterRadius",     0xCD102164, "f",  "outer_radius",     "OUTER_RADIUS",      5836, ""),
    ("StartZenithAngle",0x3107BE60, "f",  "start_zenith_angle","START_ZENITH",     1021, "cone, radians"),
    ("EndZenithAngle",  0xDC56DBCF, "f",  "end_zenith_angle", "END_ZENITH",        1316, "cone, radians"),
    # ---- lighting ---------------------------------------------------------
    ("SunLightScale",   0xF36E7E8B, "f",  "sun_light_scale",  "SUN_LIGHT",         5873, ""),
    ("LocalLightScale", 0x209877EE, "f",  "local_light_scale","LOCAL_LIGHT",       4759, ""),
    ("AmbientLightScale",0x1558A27B,"f",  "ambient_light_scale","AMBIENT_LIGHT",   1876, ""),
    ("ReceivedShadowScale", 0xEB0563F4, "f","received_shadow_scale","RECEIVED_SHADOW",0, ""),
    ("CloudShadowScale",0xFF6899EA, "f",  "cloud_shadow_scale","CLOUD_SHADOW",        0, ""),
    ("GnomonBacklight", 0xCC64D81A, "f",  "gnomon_backlight", "GNOMON_BACKLIGHT", 17083, ""),
    ("VertexBacklight", 0xC0246618, "f",  "vertex_backlight", "VERTEX_BACKLIGHT", 16856, ""),
    ("BacklightPixelContrast", 0x8454003A, "f","backlight_contrast","BACKLIGHT_CONTRAST",17345, "how sharp the backlit rim is"),
    ("ShadowRadiusMult",0x973336DB, "f",  "shadow_radius_mult","SHADOW_RADIUS_MULT",1021, ""),
    ("LightMultType",   0x1F0B9D63, "i",  "light_mult_type",  "LIGHT_MULT_TYPE",      0, "0 sun 1 local 2 both"),
    ("GnomonLightRigIndex", 0x27F4CED3, "i","gnomon_rig_index","GNOMON_RIG",           0, ""),
    # ---- depth and fade ---------------------------------------------------
    ("CameraBias",      0xBFF3F865, "f",  "camera_bias",      "CAMERA_BIAS",      12256, "pulls the card toward the camera"),
    ("InvZFadeMultiplier", 0x141FF6C3, "f","inv_z_fade",      "INV_Z_FADE",        8714, "the soft-particle fade"),
    # ---- sheet ------------------------------------------------------------
    ("DisableFrameBlend", 0x45056B2D, "i","disable_frame_blend","FRAME_BLEND",        0, ""),
    ("UseRightTile",    0xCB4FC2D2, "i",  "use_right_tile",   "RIGHT_TILE",           0, ""),
    ("Mirror",          0x9CFC66BC, "i",  "mirror",           "MIRROR",            6162, ""),
    ("FlipUProbability",0x694CE72E, "f",  "flip_u_probability","FLIP_U",            5137, "0..1 chance per particle"),
    ("FlipVProbability",0x9FD600CD, "f",  "flip_v_probability","FLIP_V",           14561, "0..1 chance per particle"),
    ("BasedOnLifetime", 0xA05DCC66, "i",  "based_on_lifetime","BASED_ON_LIFETIME",  6130, "flipbook clocked by age, not by rate"),
    ("LocalSpace",      0xF11A114C, "i",  "local_space",      "LOCAL_SPACE",          0, "particles follow the emitter"),
]

CTYPE = {"f": "float {n};", "f2": "float {n}[2];", "f3": "float {n}[3];",
         "f4": "float {n}[4];", "i": "int   {n};"}
READER = {"f": "f1", "f2": "f2", "f3": "f3", "f4": "f4", "i": "i1"}


def gen_struct() -> str:
    out = []
    for name, pid, kind, cname, _e, ovr, note in FIELDS:
        decl = CTYPE[kind].format(n=cname)
        tail = f"{name} 0x{pid:08X}"
        if ovr:
            tail += f", {ovr:,} overrides"
        if note:
            tail += f" - {note}"
        out.append(f"    {decl:<34}/* {tail} */")
    return "\n".join(out)


def gen_enum() -> str:
    names = [f"BF6_FXLOOK_{e}" for _n, _p, _k, _c, e, _o, _t in FIELDS]
    lines, cur = [], "    "
    for i, n in enumerate(names):
        piece = n + (" = 0," if i == 0 else ",")
        if len(cur) + len(piece) > 76:
            lines.append(cur.rstrip())
            cur = "    "
        cur += piece + " "
    lines.append(cur.rstrip())
    lines.append("    BF6_FXLOOK_COUNT")
    return "\n".join(lines)


def gen_pids() -> str:
    out = []
    for name, pid, _k, cname, _e, ovr, _t in FIELDS:
        c = f"static const uint32_t kFx_{cname} = 0x{pid:08X}u;"
        out.append(f"{c:<62}/* {name}{f' {ovr:,}' if ovr else ''} */")
    return "\n".join(out)


def gen_resolve() -> str:
    out = []
    for _n, _p, kind, cname, e, _o, _t in FIELDS:
        fn = READER[kind]
        ref = f"&out->{cname}" if kind in ("f", "i") else f"out->{cname}"
        out.append(f"    {fn}(kFx_{cname}, {ref}, BF6_FXLOOK_{e});")
    return "\n".join(out)


def gen_names() -> str:
    """The field NAMES, in enum order, for bf6_fx_look_field_name.

    Generated for the same reason as the enum: a consumer that maps a name to a
    bit needs the two to agree, and a hand-kept copy of this list in another
    repo is a silent reassignment of every bit after the first insertion. That
    exact bug shipped once - the Godot product carried its own 31-name array,
    the enum grew to 80, and every over-life curve stopped resolving while
    every count still looked plausible."""
    out = []
    for _n, _p, _k, cname, _e, _o, _t in FIELDS:
        out.append(f'    "{cname}",')
    return "\n".join(out)


def gen_value_cases() -> str:
    """The generic value accessor, so a consumer can walk all 80 fields.

    Without it a serialiser has to name each field by hand, and the moment it
    names fewer than the resolver resolves the two disagree in the worst way:
    the `has` mask says a field was authored while its value is simply absent
    from the payload, so the consumer believes it has a value of zero. That
    exact bug shipped - RandomForce and SpawnScale read as authored-zero and
    every emitter came out with no turbulence and the same spawn volume."""
    out = []
    arity = {"f": 1, "f2": 2, "f3": 3, "f4": 4, "i": 1}
    for _n, _p, kind, cname, e, _o, _t in FIELDS:
        n = arity[kind]
        if kind == "i":
            body = f"out4[0] = (float)look->{cname}; return 1;"
        elif n == 1:
            body = f"out4[0] = look->{cname}; return 1;"
        else:
            body = (" ".join(f"out4[{i}] = look->{cname}[{i}];" for i in range(n))
                    + f" return {n};")
        out.append(f"    case BF6_FXLOOK_{e}: {body}")
    return chr(10).join(out)


def gen_int_cases() -> str:
    ints = [e for _n, _p, k, _c, e, _o, _t in FIELDS if k == "i"]
    return chr(10).join("    case BF6_FXLOOK_%s:" % e for e in ints)


def region(text: str, tag: str, body: str) -> str:
    begin, end = f"/* @gen:{tag} */", f"/* @end:{tag} */"
    pat = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.S)
    if not pat.search(text):
        raise SystemExit(f"marker {tag} not found")
    return pat.sub(lambda _m: f"{begin}\n{body}\n{end}", text)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    if len(FIELDS) != len({f[3] for f in FIELDS}):
        raise SystemExit("duplicate C field name")
    if len(FIELDS) != len({f[1] for f in FIELDS}):
        raise SystemExit("duplicate pid")

    jobs = [
        (ROOT / "include" / "bf6_core.h", [("fxlook_struct", gen_struct()),
                                           ("fxlook_enum", gen_enum())]),
        (ROOT / "src" / "fxlook_ext.inc", [("fxlook_pids", gen_pids()),
                                           ("fxlook_resolve", gen_resolve()),
                                           ("fxlook_names", gen_names()),
                                           ("fxlook_values", gen_value_cases()),
                                           ("fxlook_ints", gen_int_cases())]),
    ]
    stale = False
    for path, regions in jobs:
        text = original = path.read_text(encoding="utf-8")
        for tag, body in regions:
            text = region(text, tag, body)
        if text != original:
            stale = True
            if not a.check:
                path.write_text(text, encoding="utf-8")
                print(f"wrote {path.name}")
    if a.check and stale:
        print("generated regions are out of date; run tools/gen_fxlook.py")
        return 1
    print(f"{len(FIELDS)} fields, {(len(FIELDS) + 63) // 64} has-word(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
