#!/usr/bin/env python3
"""Pick and download UrbanVerse-100K assets for RammsWorldGen palette slots.

UrbanVerse-100K (https://huggingface.co/datasets/Oatmealliu/UrbanVerse-100K) is a gated dataset of ~100K
metric-scaled, annotated urban objects (GLB). This script runs outside Unreal, in a Python environment with
`pip install --no-deps urbanverse-asset` plus `huggingface_hub pillow tqdm trimesh`, and a Hugging Face login
that has accepted the dataset's terms (`hf auth login`).

    python urbanverse_fetch.py --cache D:/urbanverse --list            # write the category tree (no meshes)
    python urbanverse_fetch.py --cache D:/urbanverse --per-slot 6       # choose and download
    python urbanverse_fetch.py --cache D:/urbanverse --slots Bench,Car --per-slot 10

Each palette slot below lists UrbanVerse leaf categories, size
limits in metres and a minimum quality score (0-10, from the dataset's annotations). Only single objects
(asset_composition_type == "single") are taken. The best-scoring assets per slot are downloaded with their
annotations and thumbnails, and the choice is written to <cache>/ramms_selection.json, which
ramms_worldgen.urbanverse imports into Unreal. <cache>/ATTRIBUTION.md lists every chosen asset with its
license and source, as the CC BY / BY-SA / BY-NC licenses require.

Licenses vary per asset (CC BY, BY-SA, BY-NC, BY-NC-SA, CC0); --commercial keeps only the ones that allow
commercial use. Nothing downloaded here belongs in git.
"""
from __future__ import annotations

import argparse
import ast
import json
import random
import sys
from pathlib import Path

# slot -> UrbanVerse leaf categories (exact names; `--list` writes them all to categories.json),
#         (min, max) height and (min, max) longest side in metres, minimum quality (0-10)
SLOTS = {
    "Bench":            (["bench"], (0.5, 1.3), (0.9, 3.0), 7),
    "Bollard":          (["bollard"], (0.5, 1.4), (0.1, 0.6), 6),
    "BusStop":          (["bus shelter"], (2.0, 4.0), (2.0, 8.0), 6),
    "StreetLight":      (["street light", "lamppost"], (3.0, 12.0), (0.2, 4.0), 6),
    "TrashBin":         (["trash bin"], (0.5, 1.5), (0.3, 1.2), 7),
    "Hydrant":          (["fire hydrant"], (0.4, 1.2), (0.2, 0.8), 6),
    "ParkingMeter":     (["parking meter"], (0.9, 1.8), (0.1, 0.6), 6),
    "NewspaperBox":     (["newspaper stand"], (0.7, 1.8), (0.3, 1.5), 5),
    "Mailbox":          (["mailbox", "postbox"], (0.9, 1.8), (0.3, 1.2), 6),
    "StreetSign":       (["stop sign", "traffic sign", "no parking sign", "do not enter sign", "directional sign"], (1.5, 4.0), (0.2, 1.5), 6),
    "Car":              (["car", "hatchback car", "coupe car", "electric car", "convertible car", "taxi", "minivan"], (1.2, 2.2), (3.5, 5.8), 7),
    "Flowerbed":        (["plant pot", "potted plant"], (0.3, 1.3), (0.3, 2.5), 7),
    "Hazard.Bike":      (["bicycle"], (0.8, 1.4), (1.3, 2.1), 7),
    "Hazard.Scooter":   (["scooter"], (0.8, 1.4), (0.8, 1.4), 6),
    "Hazard.Cone":      (["traffic cone"], (0.3, 1.1), (0.2, 0.6), 6),
    "Hazard.Barrier":   (["jersey barrier", "traffic barricade"], (0.6, 1.4), (0.8, 3.5), 6),
    "Hazard.Cafe":      (["table", "picnic table"], (0.6, 0.85), (0.5, 1.4), 7),
    "Hazard.CafeChair": (["chair"], (0.7, 1.1), (0.4, 0.8), 7),
    "Hazard.Dumpster":  (["dumpster"], (1.0, 2.2), (1.4, 4.0), 6),
    "Hazard.Box":       (["storage box"], (0.15, 0.9), (0.2, 1.0), 6),
}
# slot -> words that disqualify an asset by its description (the "bench" category also holds picnic tables)
EXCLUDE = {"Bench": ["picnic", "table"], "Hazard.Cafe": ["desk", "dining", "coffee table"], "Hazard.Scooter": ["motor", "vespa", "moped"]}
COMMERCIAL_OK = ("cc0", "by", "by-sa")   # license short names that allow commercial use


def _license(ann: dict) -> dict:
    li = ann.get("license_info") or {}
    if isinstance(li, str):
        # annotations store dicts and lists as Python literals ("{'license': 'by', ...}")
        try:
            li = ast.literal_eval(li)
        except Exception:  # noqa: BLE001
            li = {"license": li}
    return li if isinstance(li, dict) else {"license": str(li)}


def _license_name(li: dict) -> str:
    for k in ("license", "license_name", "name", "type"):
        if li.get(k):
            return str(li[k])
    return "unknown"


def _commercial_ok(li: dict) -> bool:
    n = _license_name(li).lower().replace("_", "-")
    if "nc" in n.split("-") or "noncommercial" in n or "non-commercial" in n:
        return False
    return any(t in n for t in ("cc0", "by"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", required=True, help="download folder (outside any git repo)")
    ap.add_argument("--list", action="store_true", help="write categories.json and stop")
    ap.add_argument("--slots", default="", help="comma-separated subset of slots")
    ap.add_argument("--per-slot", type=int, default=6)
    ap.add_argument("--commercial", action="store_true", help="only assets whose license allows commercial use")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    import urbanverse_asset as uva
    from urbanverse_asset import _core

    cache = Path(args.cache)
    uva.set(str(cache))
    cats = uva.object.categories()           # {l3: {class_name_l1, class_name_l2, class_name_l3, asset_uids, asset_count}}
    if args.list:
        out = {l3: {"l1": e.get("class_name_l1"), "l2": e.get("class_name_l2"), "count": e.get("asset_count", len(e.get("asset_uids", [])))}
               for l3, e in sorted(cats.items())}
        (cache / "categories.json").write_text(json.dumps(out, indent=1), encoding="utf-8")
        print("%d leaf categories -> %s" % (len(out), cache / "categories.json"))
        return 0

    want = [s for s in args.slots.split(",") if s] or list(SLOTS)
    _core._ensure_per_asset_annotations()
    ann_dir = cache / "assets_std_annotation_flat"
    ann_path = {p.stem[4:]: p for p in ann_dir.rglob("std_*.json")}   # annotations sit in bucket_NNNN folders
    rng = random.Random(args.seed)
    selection = {"source": "UrbanVerse-100K", "repo": _core.REPO_ID, "slots": {}, "categories": {}}
    to_load: list[str] = []
    for slot in want:
        keywords, hr, lr, qmin = SLOTS[slot]
        leafs = [l3 for l3 in keywords if l3 in cats]
        selection["categories"][slot] = leafs
        cands = []
        for l3 in leafs:
            for uid in cats[l3].get("asset_uids", []):
                p = ann_path.get(uid)
                if p is None:
                    continue
                try:
                    a = json.loads(p.read_text(encoding="utf-8"))
                except Exception:  # noqa: BLE001
                    continue
                try:
                    h, L, W, q = float(a.get("height", 0)), float(a.get("length", 0)), float(a.get("width", 0)), float(a.get("quality", 0))
                except (TypeError, ValueError):
                    continue
                if str(a.get("asset_composition_type", "single")).lower() != "single" or q < qmin:
                    continue
                desc = (str(a.get("description", "")) + " " + str(a.get("description_long", ""))).lower()
                if any(w in desc for w in EXCLUDE.get(slot, ())):
                    continue
                if not (hr[0] <= h <= hr[1] and lr[0] <= max(L, W) <= lr[1]):
                    continue
                li = _license(a)
                if args.commercial and not _commercial_ok(li):
                    continue
                cands.append((q, rng.random(), uid, l3, a, li))
        # Best first, taking the categories in turn (a slot with sedans, taxis and minivans should not end up
        # with six coupes just because coupes score highest).
        cands.sort(key=lambda c: (-c[0], c[1]))
        by_cat: dict[str, list] = {}
        for c in cands:
            by_cat.setdefault(c[3], []).append(c)
        picked = []
        while len(picked) < args.per_slot and any(by_cat.values()):
            for l3 in list(by_cat):
                if by_cat[l3] and len(picked) < args.per_slot:
                    picked.append(by_cat[l3].pop(0))
        print("%-18s %3d categories, %5d candidates, %d picked  (%s)" % (slot, len(leafs), len(cands), len(picked), ", ".join(leafs[:6]) + ("..." if len(leafs) > 6 else "")))
        selection["slots"][slot] = []
        for q, _r, uid, l3, a, li in picked:
            selection["slots"][slot].append({
                "uid": uid, "category": l3, "quality": q, "height": a.get("height"), "length": a.get("length"),
                "width": a.get("width"), "mass": a.get("mass"), "friction": a.get("friction_coefficient"),
                "front_view": a.get("frontView"), "traversability": a.get("traversability"),
                "clearance": a.get("recommended_clearance"), "license": _license_name(li), "license_info": li,
                "description": a.get("description"),
            })
            to_load.append(uid)

    res = uva.object.load(to_load, num_workers=args.workers, what=("std_glb", "std_annotation", "thumbnail"))
    for slot, items in selection["slots"].items():
        for it in items:
            r = res.get(it["uid"], {})
            it["glb"] = str(r.get("std_glb") or "")
            it["thumbnail"] = str(r.get("thumbnail") or "")
        selection["slots"][slot] = [it for it in items if it["glb"]]
    (cache / "ramms_selection.json").write_text(json.dumps(selection, indent=1), encoding="utf-8")

    lines = ["# UrbanVerse-100K assets used by RammsWorldGen", "",
             "From [UrbanVerse-100K](https://huggingface.co/datasets/Oatmealliu/UrbanVerse-100K) (collection: ODC-BY; "
             "each asset keeps its own license, below). Liu et al., *UrbanVerse: Scaling Urban Simulation by Watching "
             "City-Tour Videos*, ICLR 2026.", "",
             "| Slot | Category | UID | License | Source |", "|---|---|---|---|---|"]
    for slot, items in selection["slots"].items():
        for it in items:
            li = it["license_info"] or {}
            who = li.get("creator") or li.get("author") or li.get("creator_display_name") or ""
            url = li.get("uri") or li.get("url") or li.get("source") or ""
            if "api.sketchfab.com/v3/models/" in url:
                url = "https://sketchfab.com/3d-models/" + url.rsplit("/", 1)[-1]
            src = ("%s, %s" % (who, url)).strip(", ")
            lines.append("| %s | %s | `%s` | %s | %s |" % (slot, it["category"], it["uid"], it["license"], src))
    (cache / "ATTRIBUTION.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    n = sum(len(v) for v in selection["slots"].values())
    print("%d assets -> %s" % (n, cache / "ramms_selection.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
