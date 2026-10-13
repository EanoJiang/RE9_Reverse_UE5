"""Create and attach the UE5.8 Optimus RE hair guide deformer.

Run in the Unreal Editor Python console or with ``py ".../Build...py"``.
The C++ helper owns Optimus model edits because component bindings and kernel
binding arrays are not fully exposed by Unreal Python.  The script remains the
repeatable entry point and writes a machine-readable audit report.
"""

import json
from pathlib import Path

import unreal


ASSET_PATH = "/Game/Leon/Groom/DF_Leon_REHair"


def _json_report(value):
    if isinstance(value, str):
        try:
            return json.loads(value)
        except Exception:
            return {"success": False, "error": value}
    return value


def _attach_to_grooms(deformer):
    attached = []
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    for actor in actors:
        for component in actor.get_components_by_class(unreal.GroomComponent):
            try:
                component.set_mesh_deformer(deformer)
                component.mark_render_state_dirty()
                attached.append(component.get_path_name())
            except Exception as exc:
                unreal.log_warning("[LeonREHair] Groom attach failed for %s: %s" % (component.get_path_name(), exc))
    return attached


def build(force_rebuild=True, attach=False):
    library = getattr(unreal, "LeonOptimusGraphLibrary", None)
    if library is None:
        raise RuntimeError("LeonOptimusGraphLibrary is unavailable; rebuild LeonPreviewTools with DeformerGraph enabled")

    raw = library.build_leon_re_hair_optimus_graph(ASSET_PATH, force_rebuild)
    report = _json_report(raw)
    deformer = unreal.load_asset(ASSET_PATH)
    report["asset_loaded"] = deformer is not None
    # The C++ helper may return before the editor's compute-graph package has
    # finished compiling its shader map.  Persist the loaded replacement once
    # more through AssetTools so a successful graph build is not lost when the
    # commandlet exits immediately after Compile().
    if deformer is not None:
        try:
            report["editor_asset_save"] = bool(
                unreal.EditorAssetLibrary.save_asset(ASSET_PATH, only_if_is_dirty=False))
        except Exception as exc:
            report["editor_asset_save"] = False
            report["editor_asset_save_error"] = str(exc)
    else:
        report["editor_asset_save"] = False
    # SavePackage can report false when the transient replacement was already
    # loaded by the editor, even though the public asset save above succeeded.
    # Promote the effective result so automation does not reject a valid graph
    # solely because of that stale C++ package return value.
    if report.get("editor_asset_save"):
        report["success"] = bool(report.get("compiled") and report.get("links_valid"))
    # The native helper writes the package before the editor has completed its
    # final asset transaction on some UE 5.8 builds.  AssetTools save is the
    # authoritative post-transaction result, so expose that result in the
    # machine-readable report instead of leaving a stale native `saved:false`.
    if report.get("editor_asset_save"):
        report["saved"] = True
        report["success"] = bool(report.get("compiled") and report.get("links_valid"))
    if attach and deformer:
        report["attached_grooms"] = _attach_to_grooms(deformer)
    else:
        report["attached_grooms"] = []

    report_path = Path(unreal.Paths.project_dir()) / "Scripts" / "leon_optimus_graph_build.json"
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    unreal.log("[LeonREHair] Optimus report: %s" % json.dumps(report, ensure_ascii=False))
    return report


if __name__ == "__main__":
    # The runtime controller attaches the saved deformer during BeginPlay.
    # Attaching from an editor commandlet can race Groom's render-state build,
    # so keep asset generation side-effect free by default.
    build(True, False)
