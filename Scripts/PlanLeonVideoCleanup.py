"""Create a reviewable, hash-verified cleanup plan; does not move assets."""
import datetime
import hashlib
import json
from pathlib import Path

PROJECT = Path(r'E:\RE9_Leon_UE5').resolve()
def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(4 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()

registry = json.loads((PROJECT / 'Scripts/leon_video_registry_dependencies.json').read_text(encoding='utf-8'))
assert not registry['missing_registry_packages']
assert all(x['loaded'] for x in registry['root_load_results'].values())
keep = []
for package in registry['closure_packages']:
    assert package.startswith('/Game/')
    stem = PROJECT / 'Content' / package[len('/Game/'):]
    candidates = [stem.with_suffix(suffix) for suffix in ('.uasset', '.umap')]
    candidates = [p for p in candidates if p.is_file()]
    assert len(candidates) == 1, (package, candidates)
    for file in candidates + [stem.with_suffix(s) for s in ('.uexp', '.ubulk', '.uptnl') if stem.with_suffix(s).is_file()]:
        keep.append({'path': file.relative_to(PROJECT).as_posix(), 'bytes': file.stat().st_size,
                     'sha256': sha256_file(file)})
keep_paths = {x['path'] for x in keep}
all_content = [{'path': p.relative_to(PROJECT).as_posix(), 'bytes': p.stat().st_size}
               for p in (PROJECT / 'Content').rglob('*') if p.is_file()]
excluded = [x for x in all_content if x['path'] not in keep_paths]
maps = [x['path'] for x in keep if x['path'].endswith('.umap')]
assert maps == ['Content/Leon/Preview/LeonVideoPreview.umap'], maps
stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
archive = PROJECT.parent / ('RE9_Leon_UE5_CleanupArchive_' + stamp)
assert not archive.exists()
preserve_scripts = [
    'AuditLeonVideoDependencies.py', 'leon_video_registry_dependencies.json',
    'BuildLeonREHairOptimusGraph.py', 'VerifySavedLeonVideoGroomMap.py',
    'VerifyNativeLeonGroom.py', 'OpenLeonPreviewUI.py',
    'VerifyLeonVideoCleanup.py', 'PlanLeonVideoCleanup.py',
    'ArchiveLeonVideoUnusedAssets.ps1', 'RestoreLeonVideoCleanup.ps1',
    'leon_video_cleanup_plan.json',
]
other_directories = ['SourceAssets', 'Tools', 'Intermediate', 'Saved', 'DerivedDataCache',
                     'Preview', 'video_frames', 'Diagnostics', 'Plugins/LeonPreviewTools/Intermediate']
other_files = ['LaunchLeonVariantStage.cmd', 'README.md', 'DELIVERY_STATUS.md', 'handoff_manifest.json',
               'Plugins/LeonPreviewTools/README.md', 'Plugins/LeonPreviewTools/build_result.json']
other_files += [p.relative_to(PROJECT).as_posix() for p in (PROJECT/'Plugins/LeonPreviewTools').glob('*.log')]
plan = {
    'schema_version': 1, 'project': str(PROJECT), 'archive': str(archive),
    'state': 'planned', 'root_map': registry['root'], 'created_local': stamp,
    'registry_package_count': registry['closure_package_count'],
    'content_package_count_before': sum(x['path'].endswith(('.uasset', '.umap')) for x in all_content),
    'content_bytes_before': sum(x['bytes'] for x in all_content),
    'content_bytes_retained': sum(x['bytes'] for x in keep),
    'retained_files': keep, 'archived_content_files': excluded,
    'preserve_scripts': preserve_scripts, 'archive_directories': other_directories,
    'archive_files': other_files,
    'preserve_diagnostics': ['Cleanup_DependencyRegistry.log'],
    'critical_nonasset_dependencies': ['Shaders/LeonREHairGuideWrite.usf',
        'Plugins/LeonPreviewTools/Shaders/Private/LeonREHairGuide.ush'],
    'notes': ['Same-volume archive reduces the active project, not disk usage.',
              'Registry closure includes hard/soft package, searchable-name and management dependencies.',
              'Dynamic roots include DF_Leon_REHair and Idle/Walk/CombHair animations.',
              'External original game files and user reference video are outside the cleanup scope.'],
}
output = PROJECT / 'Scripts/leon_video_cleanup_plan.json'
output.write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({k: plan[k] for k in ('project','archive','registry_package_count',
    'content_package_count_before','content_bytes_before','content_bytes_retained')}, ensure_ascii=False))
print('Excluded Content files:', len(excluded), 'Retained maps:', maps)
