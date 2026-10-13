"""Read-only authoritative registry closure for the single retained preview."""
import json
from pathlib import Path
import unreal

PROJECT = Path(unreal.Paths.project_dir()).resolve()
ROOTS = [
    '/Game/Leon/Preview/LeonVideoPreview',
    '/Game/Leon/Groom/DF_Leon_REHair',
    '/Game/Leon/Animations/FullRig/ch0100_emote_233d0854/A_ch0100_Emote_0710_Idle_Relax_hair_id710',
    '/Game/Leon/Animations/FullRig/ch0100_generald_385e2faa/A_ch0100_GeneralD_0000_Idle_caution_id0',
    '/Game/Leon/Animations/FullRig/ch0100_generald_385e2faa/A_ch0100_GeneralD_2270_walk_Loop_id2270',
]
registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.search_all_assets(True)
options = unreal.AssetRegistryDependencyOptions(
    include_soft_package_references=True,
    include_hard_package_references=True,
    include_searchable_names=True,
    include_soft_management_references=True,
    include_hard_management_references=True,
)
pending = list(ROOTS)
visited = set()
edges = {}
missing = []
while pending:
    package = pending.pop()
    if package in visited:
        continue
    visited.add(package)
    registered = registry.get_assets_by_package_name(package, include_only_on_disk_assets=True)
    if not registered:
        missing.append(package)
    deps = sorted({str(x) for x in registry.get_dependencies(package, options) if str(x).startswith('/Game/')})
    edges[package] = deps
    pending.extend(x for x in deps if x not in visited)

loaded = {}
for package in ROOTS:
    asset = unreal.load_asset(package)
    loaded[package] = {'loaded': asset is not None, 'class': asset.get_class().get_name() if asset else None}
result = {
    'generated_by': 'UE 5.8 AssetRegistry, all package/searchable/management dependency types',
    'root': ROOTS[0], 'roots': ROOTS,
    'closure_package_count': len(visited),
    'closure_packages': sorted(visited),
    'direct_edges': edges, 'missing_registry_packages': sorted(missing),
    'root_load_results': loaded,
}
output = PROJECT / 'Scripts' / 'leon_video_registry_dependencies.json'
output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
unreal.log('LEON_CLEANUP_AUDIT: packages=%d missing=%d roots_loaded=%d/%d output=%s' % (
    len(visited), len(missing), sum(x['loaded'] for x in loaded.values()), len(ROOTS), output))
if missing or not all(x['loaded'] for x in loaded.values()):
    raise RuntimeError('Retained root or dependency failed registry/load audit; see report')
