#!/usr/bin/env python3
"""Pick and download NVIDIA SimReady Central assets for RammsWorldGen palette slots.

SimReady Central (https://github.com/NVIDIA-Omniverse/simready-central) is an index of OpenUSD "SimReady"
assets that NVIDIA and its partners host in their own public Hugging Face datasets: each file under its
submissions/ folder lists one dataset's reviewed packages, one URL per package pinned to a dataset commit. A
package is a USD asset with metric geometry, UsdPreviewSurface (and MDL) materials, physics (mass, colliders,
friction) and a SimReady_Metadata block (extents, mass, license, author). This script runs outside Unreal, in a
Python environment with `pip install huggingface_hub usd-core`. The datasets are public and ungated: no
Hugging Face login is needed.

    python simready_fetch.py --cache D:/simready --list             # write the candidates per slot (no downloads)
    python simready_fetch.py --cache D:/simready --per-slot 4        # choose, download and flatten
    python simready_fetch.py --cache D:/simready --slots Furniture.Sofa,TrashBin --per-slot 6
    python simready_fetch.py --cache D:/simready --packages ForgeCAD/simready-assets:refrigerator,microwave_oven
                                                                     # named packages as they are, for simready_mjcf.py

Each palette slot below lists asset-name patterns, size limits in metres (from the package's SimReady_Metadata
extents, checked again on the merged mesh) and the datasets to look in. Candidates are spread over asset families
(box_cardboard_a01/a02/b01 are one family), so a slot is not filled with near-copies. For each candidate only the
package definition and the small root USD are read first; the chosen packages are then downloaded at the commit the
index pins (without thumbnails and NVIDIA's duplicate source_assets textures) and flattened into one mesh each
(simready_flatten.py), which ramms_worldgen.simready imports into Unreal. The choice goes to
<cache>/ramms_selection.json; <cache>/ATTRIBUTION.md lists every asset with its source, author and license.

Every package declares its own license (all those listed below are CC BY 4.0 at the time of writing);
--commercial keeps only the ones that allow commercial use. Nothing downloaded here belongs in git.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

INDEX = "https://raw.githubusercontent.com/NVIDIA-Omniverse/simready-central/main/submissions/"
# Hugging Face datasets to look in (namespace/dataset = submissions/<namespace>/<dataset>). Left out: robot arms,
# grippers and humanoids (isaacsim-assets, FANUC, Comau, ...), data-centre equipment (Vertiv, simready-dsx) and
# PalatialSim's scanned tabletop objects, whose packages are named only by number.
DATASETS = ["nvidia/simready-assets", "ForgeCAD/simready-assets", "LGElectronics/simready-assets"]
URL_RE = re.compile(r"https://huggingface\.co/datasets/([^/]+/[^/]+)/resolve/([0-9a-f]{40})/(.+)/com\.nvidia\.simready\.packaging\.json$")

# slot -> asset-name patterns (full match), (min, max) height and (min, max) longest side in metres
SLOTS = {
    # street furniture and hazards
    "TrashBin":            ([r"trashCan_wheeled_.*", r"wasteManagement_trashBin_wheeled_.*", r"container_recycle_.*_wheeled_.*",
                             r"wheelie_bin", r"wasteManagement_recyclingBin_multiStream_.*"], (0.6, 1.5), (0.4, 1.4)),
    "Hazard.BinOut":       ([r"trashCan_wheeled_.*", r"wasteManagement_trashBin_wheeled_.*", r"container_recycle_.*_wheeled_.*", r"wheelie_bin"],
                            (0.6, 1.5), (0.4, 1.4)),
    "Hazard.Barrier":      ([r"environment_barricade_.*", r"barrier_wall_plastic_.*", r"barrier_fence_metal_.*"], (0.5, 1.6), (0.8, 3.5)),
    "Hazard.AFrame":       ([r"sign_wetFloor_.*", r"sign_floor_movable_.*"], (0.4, 1.3), (0.2, 0.9)),
    "Hazard.Box":          ([r"box_cardboard_[a-d]\d+", r"box_cardboard_large_.*", r"box_corrugated_.*", r"crate_plywood_.*", r"shipping_box"],
                            (0.15, 0.9), (0.2, 1.0)),
    # indoor furniture
    "Furniture.OfficeChair": ([r"chair_office_.*", r"warehouse_chair_office_.*"], (0.8, 1.35), (0.5, 0.9)),
    "Furniture.Chair":     ([r"dining_chair", r"gas_lift_bar_stool"], (0.65, 1.15), (0.35, 0.7)),
    "Furniture.Sofa":      ([r"sofa_.*"], (0.6, 1.1), (1.3, 3.1)),
    "Furniture.Armchair":  ([r"sofa_.*"], (0.6, 1.1), (0.6, 1.25)),
    "Furniture.CoffeeTable": ([r"furniture_table_coffee_.*"], (0.3, 0.6), (0.5, 1.6)),
    "Furniture.BedTable":  ([r"table_bedside_.*"], (0.4, 0.95), (0.3, 0.8)),
    "Furniture.Desk":      ([r"desk_[a-z]\d+"], (0.65, 0.85), (0.9, 2.2)),
    "Furniture.OfficeStorage": ([r"cabinet_file_.*", r"storage_shelf_basic_.*", r"facilityFurniture_lockerUnit_.*"], (0.6, 2.2), (0.3, 1.6)),
    "Furniture.Cupboard":  ([r"chest_of_drawers"], (0.6, 2.2), (0.4, 2.0)),
    "Furniture.Monitor":   ([r"monitor_[a-c]\d+", r"computer_monitor_office_.*", r"monitor_\d+gx\w+"], (0.25, 0.75), (0.3, 1.2)),
    "Furniture.Keyboard":  ([r"computer_keyboard_.*"], (0.01, 0.08), (0.25, 0.55)),
    "Furniture.TV":        ([r"TV_[a-z]\d+", r"tv_\w+"], (0.4, 1.7), (0.6, 2.6)),
    "Furniture.Lamp":      ([r"lamp_table_.*", r"desk_lamp"], (0.2, 0.9), (0.1, 0.7)),
    "Furniture.Toilet":    ([r"toilet_[a-z]\d+"], (0.6, 1.1), (0.5, 0.9)),
    "Furniture.Sink":      ([r"washBasin_[a-z]\d+", r"fixture_sink_basin_.*", r"vanity_sink_faucet"], (0.1, 1.1), (0.35, 1.2)),
    "Furniture.RestroomSink": ([r"washBasin_long_.*"], (0.1, 1.1), (0.9, 3.0)),
    "Furniture.Mirror":    ([r"mirror_bathroom_.*", r"fixture_mirror_.*"], (0.3, 1.5), (0.3, 1.5)),
    "Furniture.HandDryer": ([r"appliance_handDryer_.*"], (0.15, 0.8), (0.15, 0.6)),
    "Furniture.Appliance": ([r"laundry_\w+", r"front_load_washer"], (0.8, 1.4), (0.5, 1.0)),
    "Furniture.KitchenTall": ([r"refrigerator_\w+", r"appliance_refrigerator_.*"], (1.5, 2.1), (0.5, 1.2)),
    "Furniture.KitchenDishwasher": ([r"appliance_dishwasher_.*", r"dishwasher_\w+"], (0.75, 0.95), (0.5, 0.75)),
    "Furniture.KitchenLow": ([r"kitchen_base_cabinet"], (0.75, 0.95), (0.3, 1.5)),
    "Furniture.KitchenUpper": ([r"kitchen_wall_cabinet"], (0.3, 1.0), (0.3, 1.5)),
    "Furniture.KitchenStove": ([r"range_oven", r"cooking_ldgl\w+"], (0.85, 1.25), (0.5, 1.0)),
    "Furniture.OfficeProp": ([r"computer_printer_office_.*", r"ladder_step_.*", r"fireExtinguisher_stand_.*"], (0.3, 1.9), (0.2, 1.2)),
    "Furniture.Clutter":   ([r"trashCan_office_.*", r"box_cardboard_[a-d]\d+", r"bucket_mop_.*", r"bucket_utility_.*", r"pedal_trash_can", r"storage_box_.*"],
                            (0.15, 1.3), (0.2, 1.2)),
    # retail
    "Store.Shelf":         ([r"gondola_shelf"], (1.2, 2.4), (0.8, 3.0)),
    "Store.Clutter":       ([r"sign_wetFloor_.*", r"pallet_[a-b]\d+", r"bucket_mop_.*", r"crate_(black|blue|gray|green|orange|red|white|yellow|violet)_.*",
                             r"box_cardboard_[a-d]\d+"], (0.1, 1.0), (0.2, 1.3)),
    "Store.Basket":        ([r"storage_basket_plastic_[a-z]\d+"], (0.1, 0.5), (0.3, 0.7)),
}
# Wall-mounted things: their packages' extents can include the height they hang at, so only the merged mesh
# (re-based on its own bottom) is checked against the height limits.
WALL_SLOTS = {"Furniture.KitchenUpper", "Furniture.Mirror", "Furniture.HandDryer"}
# words that disqualify a candidate (whole words of its name)
EXCLUDE = {"Furniture.Clutter": ["folded", "open"], "Hazard.Box": ["folded", "open", "flat"]}
COMMERCIAL_OK = {"cc0", "cc-by-4.0", "cc-by-3.0", "cc-by-sa-4.0", "apache-2.0", "mit"}
# skipped when downloading a package: thumbnails, and NVIDIA's copy of every texture under source_assets/
IGNORE = ["*/.thumbs/*", "*/source_assets/*"]
MAX_PACKAGE_MB = 600.0


def family(name: str) -> str:
    """box_cardboard_a01 / _b02 -> box_cardboard; sofa_soft_b01 -> sofa_soft."""
    return re.sub(r"(_[a-z]\d{2})+$", "", name)


def read_index(cache: Path, datasets: list[str], refresh: bool) -> dict:
    """{(repo, asset): (revision, url)} from the SimReady Central submission files (cached under <cache>/index)."""
    out = {}
    idx = cache / "index"
    idx.mkdir(parents=True, exist_ok=True)
    for ds in datasets:
        local = idx / (ds.replace("/", "__") + ".txt")
        if refresh or not local.exists():
            try:
                with urllib.request.urlopen(INDEX + ds, timeout=60) as r:
                    local.write_bytes(r.read())
            except Exception as exc:  # noqa: BLE001
                if not local.exists():
                    raise RuntimeError("cannot read the SimReady Central index for %s: %s" % (ds, exc))
                print("warning: index for %s not refreshed (%s); using %s" % (ds, exc, local))
        for line in local.read_text(encoding="utf-8").splitlines():
            m = URL_RE.match(line.strip())
            if m:
                out[(m.group(1), m.group(3))] = (m.group(2), line.strip())
    return out


def candidates(index: dict, slot: str) -> list[tuple[str, str]]:
    pats, _h, _l = SLOTS[slot]
    rx = [re.compile(p) for p in pats]
    bad = EXCLUDE.get(slot, [])
    out = []
    for (repo, asset) in sorted(index):
        if any(r.fullmatch(asset) for r in rx) and not any(w in asset.lower().split("_") for w in bad):
            out.append((repo, asset))
    return out


def _metadata(layer_path: str) -> dict:
    """SimReady_Metadata (and a few other customLayerData keys) of a root USD, without loading its payloads."""
    from pxr import Sdf
    lay = Sdf.Layer.FindOrOpen(layer_path)
    if lay is None:
        return {}
    cd = dict(lay.customLayerData)
    md = dict(cd.get("SimReady_Metadata") or {})
    out = {}
    for k in ("asset_name", "asset_license", "author", "category", "asset_type", "qcode", "mass", "asset_extents", "profile"):
        v = md.get(k, cd.get(k))
        if v is None:
            continue
        if k == "asset_extents":
            try:
                v = [float(x) for x in (v if not isinstance(v, str) else re.findall(r"[-0-9.eE]+", v))][:3]
            except (TypeError, ValueError):
                continue
        elif k == "mass":
            try:
                v = float(v)
            except (TypeError, ValueError):
                continue
        else:
            v = str(v)
        out[k] = v
    return out


class Hub:
    """The few Hugging Face calls needed, with the repo type fixed to datasets."""

    def __init__(self, cache: Path):
        from huggingface_hub import HfApi, hf_hub_download, snapshot_download
        self.api, self.dl, self.snap = HfApi(), hf_hub_download, snapshot_download
        self.cache = cache

    def local_dir(self, repo: str) -> Path:
        return self.cache / repo.replace("/", "__")

    def tree(self, repo: str, rev: str, asset: str) -> list[tuple[str, int]]:
        files = []
        for e in self.api.list_repo_tree(repo, path_in_repo=asset, revision=rev, repo_type="dataset", recursive=True):
            size = getattr(e, "size", None)
            if size is not None:
                files.append((e.path, int(size)))
        return files

    def get(self, repo: str, rev: str, path: str) -> str:
        return self.dl(repo, path, revision=rev, repo_type="dataset", local_dir=str(self.local_dir(repo)))

    def package(self, repo: str, rev: str, asset: str) -> str:
        self.snap(repo, revision=rev, repo_type="dataset", local_dir=str(self.local_dir(repo)),
                  allow_patterns=[asset + "/*", asset + "/**"], ignore_patterns=[asset + "/" + p for p in IGNORE])
        return str(self.local_dir(repo) / asset)


def _ignored(path: str) -> bool:
    import fnmatch
    return any(fnmatch.fnmatch(path, "*" + p.lstrip("*")) for p in IGNORE) or "/.thumbs/" in path


def root_usd(files: list[tuple[str, int]], asset: str, hub: Hub, repo: str, rev: str) -> str | None:
    """The package's root USD: from .metadata/com.nvidia.simready.root_usds.json, else the one USD at the top of
    its asset folder."""
    names = {p for p, _s in files}
    rj = asset + "/.metadata/com.nvidia.simready.root_usds.json"
    if rj in names:
        try:
            entries = json.loads(Path(hub.get(repo, rev, rj)).read_text(encoding="utf-8")).get("entries") or []
            if entries:
                return asset + "/" + entries[0]
        except Exception:  # noqa: BLE001
            pass
    tops = sorted(p for p in names if re.fullmatch(re.escape(asset) + r"/(simready_usd/)?[^/]+\.usd[ac]?", p))
    return tops[0] if tops else None


def probe(hub: Hub, repo: str, rev: str, asset: str) -> dict | None:
    """Package definition, root USD metadata and download size of one candidate (small downloads only)."""
    try:
        files = hub.tree(repo, rev, asset)
    except Exception as exc:  # noqa: BLE001
        print("  %s/%s: listing failed (%s)" % (repo, asset, exc))
        return None
    root = root_usd(files, asset, hub, repo, rev)
    if root is None:
        print("  %s/%s: no root USD" % (repo, asset))
        return None
    try:
        pkg = json.loads(Path(hub.get(repo, rev, asset + "/com.nvidia.simready.packaging.json")).read_text(encoding="utf-8"))
        md = _metadata(hub.get(repo, rev, root))
    except Exception as exc:  # noqa: BLE001
        print("  %s/%s: metadata failed (%s)" % (repo, asset, exc))
        return None
    mb = sum(s for p, s in files if not _ignored(p)) / 1e6
    # the package's preview picture (kept although .thumbs is not downloaded with the package)
    stem = os.path.basename(root)
    thumbs = sorted(p for p, _s in files if "/.thumbs/" in p and p.endswith(".png") and "/Textures/" not in p and "/source_assets/" not in p)
    thumb = next((p for p in thumbs if p.endswith("_thumbnail.png")), None) or next((p for p in thumbs if p.endswith(stem + ".png")), None)
    return {"package": pkg, "metadata": md, "root": root, "mb": round(mb, 1), "thumb": thumb}


def _fits(slot: str, ext, merged: bool = True) -> bool:
    """Whether a size (x, y, z in metres) fits a slot's limits. A package's metadata extents (merged=False) skip
    the height check for wall-mounted slots."""
    _p, hr, lr = SLOTS[slot]
    if not ext or len(ext) < 3:
        return True             # no metadata extents: decided on the merged mesh
    h, L = float(ext[2]), max(float(ext[0]), float(ext[1]))
    if not merged and slot in WALL_SLOTS:
        return lr[0] <= L <= lr[1]
    return hr[0] <= h <= hr[1] and lr[0] <= L <= lr[1]


def _license_ok(pkg: dict, commercial: bool) -> bool:
    return not commercial or str(pkg.get("license", "")).lower() in COMMERCIAL_OK


def attribution(selection: dict) -> str:
    lines = ["# NVIDIA SimReady Central assets used by RammsWorldGen", "",
             "From [SimReady Central](https://github.com/NVIDIA-Omniverse/simready-central): packages hosted in the "
             "Hugging Face datasets named below, each at the dataset commit the SimReady Central index pins. Each "
             "package keeps its own license (below).", "",
             "Changes: the asset's renderable parts merged into one mesh (world transforms baked, colliders and "
             "physics dropped), converted to Unreal static meshes; collision and physical material set up for "
             "simulation.", "",
             "| Slot | Asset | Dataset | Author | License |", "|---|---|---|---|---|"]
    for slot, items in selection["slots"].items():
        for it in items:
            lic = it.get("license") or "unknown"
            if lic.lower() == "cc-by-4.0":
                lic = "[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)"
            url = "https://huggingface.co/datasets/%s/tree/%s/%s" % (it["repo"], it["revision"], it["asset"])
            lines.append("| %s | [%s](%s) | [%s](https://huggingface.co/datasets/%s) | %s | %s |" % (
                slot, it["asset"], url, it["repo"], it["repo"], it.get("author") or it["repo"].split("/")[0], lic))
    return "\n".join(lines) + "\n"


def fetch_packages(cache: Path, index: dict, spec: str) -> int:
    """Download named packages whole (at the index's commit) and print each root USD with its license."""
    hub = Hub(cache)
    repo_of = ""
    status = 0
    for item in [x.strip() for x in spec.split(",") if x.strip()]:
        if ":" in item:
            repo_of, asset = item.rsplit(":", 1)
        else:
            asset = item
        keys = [k for k in sorted(index) if k[1] == asset and (not repo_of or k[0] == repo_of)]
        if not keys:
            print("%s: not in the index%s" % (asset, " of " + repo_of if repo_of else ""))
            status = 1
            continue
        repo, asset = keys[0]
        rev, _url = index[(repo, asset)]
        pr = probe(hub, repo, rev, asset)
        if pr is None:
            print("%s/%s: no root USD found" % (repo, asset))
            status = 1
            continue
        hub.package(repo, rev, asset)
        md = pr["metadata"]
        print("%s/%s@%s  %s  (%s, %s)" % (repo, asset, rev[:12], os.path.normpath(os.path.join(hub.local_dir(repo), pr["root"])),
                                         pr["package"].get("license") or md.get("asset_license") or "license ?",
                                         md.get("author") or "author ?"))
    return status


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache", required=True, help="download folder (outside any git repo)")
    ap.add_argument("--list", action="store_true", help="write candidates.json (slot -> matching assets) and stop")
    ap.add_argument("--slots", default="", help="comma-separated subset of slots")
    ap.add_argument("--datasets", default=",".join(DATASETS), help="Hugging Face datasets to look in")
    ap.add_argument("--per-slot", type=int, default=4)
    ap.add_argument("--commercial", action="store_true", help="only packages whose license allows commercial use")
    ap.add_argument("--max-mb", type=float, default=MAX_PACKAGE_MB, help="skip packages bigger than this (download size)")
    ap.add_argument("--refresh-index", action="store_true", help="read the SimReady Central index again")
    ap.add_argument("--reflatten", action="store_true", help="flatten already downloaded packages again")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--packages", default="",
                    help="comma-separated [namespace/dataset:]asset: download these packages at their pinned commit "
                         "and print their root USD (no slot choice, no flattening; for simready_mjcf.py)")
    args = ap.parse_args()

    cache = Path(args.cache)
    cache.mkdir(parents=True, exist_ok=True)
    datasets = [d for d in args.datasets.split(",") if d]
    index = read_index(cache, datasets, args.refresh_index)
    want = [s for s in args.slots.split(",") if s] or list(SLOTS)
    for s in want:
        if s not in SLOTS:
            print("unknown slot %r (known: %s)" % (s, ", ".join(SLOTS)))
            return 2
    if args.packages:
        return fetch_packages(cache, index, args.packages)
    if args.list:
        out = {s: ["%s/%s" % c for c in candidates(index, s)] for s in want}
        (cache / "candidates.json").write_text(json.dumps(out, indent=1), encoding="utf-8")
        print("\n".join("%-28s %3d" % (s, len(v)) for s, v in out.items()))
        print("-> %s" % (cache / "candidates.json"))
        return 0

    from simready_flatten import flatten
    hub = Hub(cache)
    sel_path = cache / "ramms_selection.json"
    # Fetching a subset of slots must not drop the others from an earlier run.
    selection = {"source": "SimReady Central", "index": INDEX, "slots": {}}
    if sel_path.exists():
        try:
            selection["slots"].update(json.loads(sel_path.read_text(encoding="utf-8")).get("slots", {}))
        except Exception as exc:  # noqa: BLE001
            print("warning: %s unreadable (%s); starting a new selection" % (sel_path, exc))
    probed: dict = {}
    flat: dict = {}
    for slot in want:
        # One RNG per slot: a slot's choice does not depend on which other slots ran.
        rng = random.Random("%s:%s" % (args.seed, slot))
        cands = candidates(index, slot)
        by_fam: dict[str, list] = {}
        for c in cands:
            by_fam.setdefault(family(c[1]), []).append(c)
        fams = list(by_fam)
        rng.shuffle(fams)
        for f in fams:
            rng.shuffle(by_fam[f])
        picked = []
        # families in turn, so a slot with five box families is not five cardboard boxes
        while len(picked) < args.per_slot and any(by_fam.values()):
            for f in fams:
                if len(picked) >= args.per_slot or not by_fam[f]:
                    continue
                repo, asset = by_fam[f].pop(0)
                rev, url = index[(repo, asset)]
                key = (repo, asset)
                if key not in probed:
                    probed[key] = probe(hub, repo, rev, asset)
                pr = probed[key]
                if pr is None or not _license_ok(pr["package"], args.commercial):
                    continue
                if pr["mb"] > args.max_mb:
                    print("  %s: %.0f MB, skipped" % (asset, pr["mb"]))
                    continue
                if not _fits(slot, pr["metadata"].get("asset_extents"), merged=False):
                    print("  %s: %s m does not fit %s" % (asset, [round(x, 2) for x in pr["metadata"]["asset_extents"]], slot))
                    continue
                if key not in flat:
                    pkg_dir = hub.package(repo, rev, asset)
                    merged = os.path.join(pkg_dir, "ramms_merged.usdc")
                    try:
                        if args.reflatten or not os.path.exists(merged):
                            rep = flatten(os.path.normpath(os.path.join(hub.local_dir(repo), pr["root"])), merged)
                            Path(merged + ".json").write_text(json.dumps(rep, indent=1), encoding="utf-8")
                        else:
                            rep = json.loads(Path(merged + ".json").read_text(encoding="utf-8"))
                    except Exception as exc:  # noqa: BLE001
                        print("  %s: flatten failed (%s)" % (asset, exc))
                        flat[key] = None
                        continue
                    flat[key] = (pkg_dir, merged, rep)
                if flat[key] is None:
                    continue
                pkg_dir, merged, rep = flat[key]
                size = rep.get("size_m") or [0, 0, 0]
                if not _fits(slot, size):
                    print("  %s: merged mesh %s m does not fit %s" % (asset, size, slot))
                    continue
                md = pr["metadata"]
                thumb = ""
                if pr.get("thumb"):
                    try:
                        thumb = hub.get(repo, rev, pr["thumb"])
                    except Exception:  # noqa: BLE001
                        pass
                picked.append({
                    "uid": "%s/%s" % (repo, asset), "repo": repo, "asset": asset, "revision": rev, "source_url": url,
                    "package_id": pr["package"].get("package_id"), "license": pr["package"].get("license"),
                    "usd_license": md.get("asset_license"), "author": md.get("author"), "category": md.get("category") or family(asset),
                    "family": family(asset), "qcode": md.get("qcode"), "mass": md.get("mass"), "friction": rep.get("friction"),
                    "length": size[0], "width": size[1], "height": size[2], "faces": rep.get("faces"),
                    "materials": sorted(rep.get("materials", {})), "root_usd": os.path.normpath(os.path.join(hub.local_dir(repo), pr["root"])),
                    "usd": merged, "thumbnail": thumb, "download_mb": pr["mb"],
                })
        print("%-28s %3d candidates in %2d families, %d picked  (%s)" % (slot, len(cands), len(by_fam), len(picked), ", ".join(p["asset"] for p in picked)))
        selection["slots"][slot] = picked
    sel_path.write_text(json.dumps(selection, indent=1), encoding="utf-8")
    (cache / "ATTRIBUTION.md").write_text(attribution(selection), encoding="utf-8")
    n = sum(len(v) for v in selection["slots"].values())
    print("%d assets -> %s" % (n, sel_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
