"""The vehicle motion scoreboard: every vehicle in the install, one number.

Runs build/Release/vehicle_motion_probe.exe on each vehicle in parallel, then counts
motion PIECES:

  input   a public channel a graph reads. Solved when the step supplies it (host_set),
          unsolved when nobody does (unsupplied: an engine-native input still missing).
  bone    a bone channel a graph binds. Solved when the sweep moved it (over 0.05 deg
          or 0.1 mm); unsolved when it never moved (not exercised, not reachable, or
          wrong), or when the rig has no bone for it.
  orphan  a rig bone whose name says it is a moving part (rotor, fan, nozzle, track,
          turret, gear...) that no graph binds: its driver is not hosted or is native.

  python tools/vehicle_motion_score.py [--jobs 8] [--only substr] [--out dir]

Writes <out>/<vehicle>.json per vehicle and <out>/SCOREBOARD.md, and prints the total.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.dirname(HERE)
PROBE = os.path.join(CORE, "build", "Release", "vehicle_motion_probe.exe")

# Every vehicle folder that ships a ske_veh_<class>_<name>_base rig (docs/VEHICLES.md).
FLEET = [
    "airplane/f14", "airplane/f16", "airplane/f22", "airplane/fa18f", "airplane/jas39",
    "airplane/mq9", "airplane/su57",
    "boat/cb90", "boat/jetski", "boat/rhib",
    "car/flyer60", "car/jltv", "car/marauder", "car/ptv", "car/quadbike", "car/vector",
    "helicopter/ah64e", "helicopter/ah6m", "helicopter/eurocopter", "helicopter/mh47",
    "helicopter/trv150", "helicopter/uh60",
    "motorcycle/dirtbike01", "motorcycle/dirtbike02",
    "stationary/bgm71tow", "stationary/cws", "stationary/gdf009", "stationary/m2mg",
    "stationary/phalanx",
    "stationary/ssmbattery",
    "tank/aav7a1", "tank/abrams", "tank/bradley", "tank/cheetah", "tank/cv90",
    "tank/gepard", "tank/leopard",
]

MOVING = re.compile(r"rotor|blade|fan|nozzle|prop|track|turret|barrel|gear|flap|rudder|"
                    r"elevator|aileron|slat|airbrake|spoiler|throttle|handle|pedal|steer|"
                    r"wheel|suspension|controlarm|piston|door|hatch|ramp|canopy|radar|"
                    r"antenna|sprocket|idler|kickstand|gauge|needle|arrow|wiper|exhaust",
                    re.I)
# names that match but are not parts that move on their own
STATIC = re.compile(r"^loc_|_state[1-9]|_lod|dressing|decal|light|socket|attach|_col$|"
                    r"trajectory|connect|^ai", re.I)


def run(veh, out_dir):
    path = os.path.join(out_dir, veh.replace("/", "_") + ".json")
    r = subprocess.run([PROBE, "common/hardware/vehicles/" + veh, path],
                       capture_output=True, text=True, timeout=900)
    try:
        with open(path, encoding="utf-8") as f:
            return veh, json.load(f)
    except (OSError, ValueError) as e:
        return veh, {"error": "probe wrote nothing (%s): %s" % (e, (r.stderr or r.stdout)[-300:])}


SOCKET = re.compile(r"^Attach |Locator|(^|[ _])(Muzzle[ _])?FX$|^AITrajectory$|External Force$")


def score(veh, data):
    rep = data.get("report")
    if not rep:
        return {"veh": veh, "error": data.get("error", "no report")}
    inputs_ok = sorted(rep["host_set"])
    inputs_missing = sorted(rep["unsupplied"])
    bones_ok, bones_still, bones_unmapped, sockets = [], [], [], []
    bound_rig = set()
    for b in rep["bones"]:
        # SOCKETS: attachment points, locators and effect points no graph ever writes
        # (Attach Vehicle Root, Attach HardPoint, Muzzle_FX...). They carry other things;
        # they have no motion of their own, so they are listed, not scored.
        if b.get("writes", 0) == 0 and SOCKET.search(b["channel"]):
            sockets.append(b["channel"])
            continue
        if b["rig_index"] < 0:
            bones_unmapped.append(b["channel"])
            continue
        bound_rig.add(b["bone"])
        (bones_ok if (b["rot_deg"] > 0.05 or b["move_m"] > 1e-4 or b.get("scale", 0) > 1e-3) else bones_still).append(b["bone"])
    orphans = sorted(n for n in rep["rig"]
                     if MOVING.search(n) and not STATIC.search(n) and n not in bound_rig)
    # CARRIED: an orphan rigidly attached under a bone that moved (a rotor blade under
    # its hub, a gear link under its leg) moves exactly as its parent does, so it is
    # solved - but counted in its own column, never folded into the bone count.
    parents = rep.get("rig_parent", [])
    index = {n: i for i, n in enumerate(rep["rig"])}
    moved = {index[n] for n in bones_ok if n in index}
    def carried_by_ancestor(n):
        i = index.get(n, -1)
        seen = 0
        while 0 <= i < len(parents) and seen < 512:
            i = parents[i]
            seen += 1
            if i in moved:
                return True
        return False
    carried = [n for n in orphans if carried_by_ancestor(n)]
    orphans = [n for n in orphans if n not in set(carried)]
    solved = len(inputs_ok) + len(bones_ok) + len(carried)
    total = solved + len(inputs_missing) + len(bones_still) + len(bones_unmapped) + len(orphans)
    return {"veh": veh, "graphs": [g.rsplit("/", 1)[-1] for g in rep["graphs"]],
            "inputs_ok": inputs_ok, "inputs_missing": inputs_missing,
            "bones_ok": sorted(bones_ok), "bones_still": sorted(bones_still),
            "bones_unmapped": sorted(bones_unmapped), "orphans": orphans, "carried": sorted(carried),
            "sockets": sorted(sockets),
            "parts_unsupplied": rep["parts_unsupplied"], "solved": solved, "total": total}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default=os.path.join(CORE, "build", "vehicle_motion"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    fleet = [v for v in FLEET if a.only in v]
    with ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda v: run(v, a.out), fleet))
    rows = [score(v, d) for v, d in results]
    solved = sum(r.get("solved", 0) for r in rows)
    total = sum(r.get("total", 0) for r in rows)
    lines = ["# Vehicle motion scoreboard", "",
             "**%d / %d motion pieces solved** across %d vehicles "
             "(inputs supplied + bones that move, over inputs + bound bones + orphan parts; "
             "sockets, attachment points no graph moves, are listed but not scored)." %
             (solved, total, len(rows)), "",
             "| Vehicle | Solved | Inputs ok/missing | Bones moved/still/unmapped | Carried | Orphan parts | Sockets |",
             "|---|---|---|---|---|---|---|"]
    for r in rows:
        if "error" in r:
            lines.append("| %s | refused | %s | | |" % (r["veh"], r["error"][:80].replace("|", "/")))
            continue
        lines.append("| %s | %d/%d | %d/%d | %d/%d/%d | %d | %d | %d |" % (
            r["veh"], r["solved"], r["total"], len(r["inputs_ok"]), len(r["inputs_missing"]),
            len(r["bones_ok"]), len(r["bones_still"]), len(r["bones_unmapped"]), len(r["carried"]),
            len(r["orphans"]), len(r["sockets"])))
    lines += ["", "## Unsolved, per vehicle", ""]
    for r in rows:
        if "error" in r:
            continue
        lines.append("**%s** (%s)" % (r["veh"], ", ".join(r["graphs"])))
        for k, label in (("inputs_missing", "missing inputs"), ("bones_still", "bones that never moved"),
                         ("bones_unmapped", "bound channels with no rig bone"), ("orphans", "orphan parts")):
            if r[k]:
                lines.append("- %s: %s" % (label, ", ".join(r[k])))
        lines.append("")
    with open(os.path.join(a.out, "SCOREBOARD.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    with open(os.path.join(a.out, "scoreboard.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1)
    print("MOTION SCORE %d / %d  (of which carried %d)" % (solved, total,
          sum(len(r.get("carried", [])) for r in rows)))
    for r in rows:
        print("  %-24s %s" % (r["veh"], "refused: " + r["error"][:90] if "error" in r
                                        else "%d/%d" % (r["solved"], r["total"])))


if __name__ == "__main__":
    sys.exit(main())
