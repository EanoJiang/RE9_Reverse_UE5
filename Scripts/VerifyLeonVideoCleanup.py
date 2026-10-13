"""Read-only fresh-process verification after the LeonVideoPreview cleanup.

Loads the AssetRegistry dependency closure and the saved map without saving
packages or maps. Historical import SourceFile metadata is intentionally not
validated: source exports are not required for loading the saved UE assets.
Only the JSON verification report is written.
"""
import hashlib
import json
import os
import traceback
from pathlib import Path

import unreal


ROOT = Path('E:/RE9_Leon_UE5')
CONTENT = ROOT / 'Content'
MANIFEST = ROOT / 'Scripts/leon_video_registry_dependencies.json'
OUTPUT = ROOT / 'Scripts/leon_video_cleanup_verification.json'
MAP = '/Game/Leon/Preview/LeonVideoPreview'
DF = '/Game/Leon/Groom/DF_Leon_REHair'
ANIMATIONS = (
    '/Game/Leon/Animations/FullRig/ch0100_emote_233d0854/A_ch0100_Emote_0710_Idle_Relax_hair_id710',
    '/Game/Leon/Animations/FullRig/ch0100_generald_385e2faa/A_ch0100_GeneralD_0000_Idle_caution_id0',
    '/Game/Leon/Animations/FullRig/ch0100_generald_385e2faa/A_ch0100_GeneralD_2270_walk_Loop_id2270',
)
EXPECTED_STAGE_MATERIALS = {
    'Face_Mat': '/Game/Leon/Preview/Materials/MI_LeonFace_Stage',
    'Set00_Inner_Cloth1_Mat': '/Game/Leon/Preview/Materials/MI_LeonStageCloth_Set00_Inner_Cloth1_Mat',
    'Set00_Inner_Cloth3_Mat': '/Game/Leon/Preview/Materials/MI_LeonStageCloth_Set00_Inner_Cloth3_Mat',
    'Set00_Jacket_Cloth_Mat': '/Game/Leon/Preview/Materials/MI_LeonStageCloth_Set00_Jacket_Cloth_Mat',
    'Set00_Pants_Cloth_Mat': '/Game/Leon/Preview/Materials/MI_LeonStageCloth_Set00_Pants_Cloth_Mat',
}

report = {
    'schema_version': 1,
    'success': False,
    'asset_writes': False,
    'map_saved': False,
    'editor_process_id': os.getpid(),
    'root': MAP,
    'checks': {},
    'errors': [],
    'historical_source_metadata': 'Excluded from failure conditions; saved packages do not require import SourceFile paths.',
}


def package_name(obj):
    return obj.get_path_name().split('.')[0] if obj else None


def check(name, passed, message):
    report['checks'][name] = bool(passed)
    if not passed:
        report['errors'].append(message)
    return bool(passed)


def package_file(package):
    if not package.startswith('/Game/'):
        raise RuntimeError('Unexpected non-project package in closure: ' + package)
    stem = CONTENT / package[6:]
    candidates = [Path(str(stem) + ext) for ext in ('.uasset', '.umap')]
    found = [path for path in candidates if path.is_file()]
    return found[0] if len(found) == 1 else None


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


try:
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8-sig'))
    closure = sorted(set(manifest.get('closure_packages', [])))
    check('manifest_root', manifest.get('root') == MAP, 'Dependency manifest root does not match LeonVideoPreview.')
    check('manifest_closure_nonempty', bool(closure), 'Dependency manifest has no closure packages.')
    check('manifest_count', len(closure) == manifest.get('closure_package_count'), 'Dependency manifest closure count mismatch.')
    required_roots = {MAP, DF} | set(ANIMATIONS)
    check('runtime_roots_in_closure', required_roots.issubset(closure), 'Dependency closure omits a required runtime root.')
    report['closure_package_count'] = len(closure)

    disk_maps = sorted(str(path.relative_to(CONTENT)).replace('\\', '/') for path in CONTENT.rglob('*.umap'))
    report['disk_maps'] = disk_maps
    check('only_video_preview_map', disk_maps == ['Leon/Preview/LeonVideoPreview.umap'], 'Content must contain only LeonVideoPreview.umap.')

    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous(['/Game'], force_rescan=True)
    assets = registry.get_assets_by_path('/Game', recursive=True)
    animation_packages = sorted({str(asset.package_name) for asset in assets
                                 if str(asset.asset_class_path.asset_name) == 'AnimSequence'})
    report['animation_packages'] = animation_packages
    check('only_three_animation_sequences', animation_packages == sorted(ANIMATIONS), 'Saved AnimSequence assets do not match the three retained preview animations.')

    loaded = {}
    package_rows = []
    for package in closure:
        path = package_file(package)
        row = {'package': package, 'file': str(path) if path else None, 'physical_file_exists': path is not None,
               'loaded': False, 'object_path': None}
        if path is None:
            report['errors'].append('Missing or ambiguous package file: ' + package)
        else:
            row['sha256_before'] = digest(path)
            try:
                obj = unreal.load_asset(package)
                if obj:
                    loaded[package] = obj
                    row['loaded'] = True
                    row['object_path'] = obj.get_path_name()
                else:
                    report['errors'].append('unreal.load_asset returned None: ' + package)
            except Exception as exc:
                row['load_error'] = str(exc)
                report['errors'].append('Cannot load package %s: %s' % (package, exc))
        package_rows.append(row)
    report['packages'] = package_rows
    check('all_closure_files_exist', all(row['physical_file_exists'] for row in package_rows), 'One or more closure package files are absent.')
    check('all_closure_assets_load', all(row['loaded'] for row in package_rows), 'One or more closure packages did not load.')
    check('deformer_root_loads', DF in loaded, 'RE hair deformer root could not be loaded.')
    check('three_animation_roots_load', all(isinstance(loaded.get(package), unreal.AnimSequence) for package in ANIMATIONS), 'One or more retained runtime animation roots are not loaded AnimSequence assets.')

    world = unreal.EditorLoadingAndSavingUtils.load_map(MAP)
    check('target_map_loads', bool(world) and package_name(world) == MAP, 'Target LeonVideoPreview map could not be loaded.')
    if world:
        actors = list(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
        heroes = [actor for actor in actors if actor.actor_has_tag('LeonHero')]
        native_hairs = [actor for actor in actors if isinstance(actor, unreal.GroomActor)
                        and (actor.actor_has_tag('LeonNativeHair') or 'Native' in actor.get_actor_label())]
        report['hero_actors'] = [actor.get_path_name() for actor in heroes]
        report['native_groom_actors'] = [actor.get_path_name() for actor in native_hairs]
        check('exactly_one_leon_hero', len(heroes) == 1, 'Expected exactly one LeonHero tagged actor.')
        check('exactly_one_native_groom_actor', len(native_hairs) == 1, 'Expected exactly one native GroomActor.')

        if len(heroes) == 1:
            body = heroes[0].get_component_by_class(unreal.SkeletalMeshComponent)
            mesh = body.get_skeletal_mesh_asset() if body else None
            report['hero_mesh'] = mesh.get_path_name() if mesh else None
            check('hero_mesh_exists', mesh is not None, 'Hero SkeletalMeshComponent has no mesh.')
            check('hero_preview_mesh', package_name(mesh) == '/Game/Leon/Mesh/SK_Leon_Preview', 'Hero mesh is not the retained SK_Leon_Preview.')
            if mesh:
                mesh_slots = list(mesh.get_editor_property('materials'))
                report['hero_materials'] = [{'slot': str(slot.material_slot_name), 'material': body.get_material(index).get_path_name() if body.get_material(index) else None}
                                            for index, slot in enumerate(mesh_slots)]
                check('hero_materials_exist', bool(mesh_slots) and all(body.get_material(index) is not None for index in range(len(mesh_slots))), 'Hero has a missing material assignment.')
                slots = {str(slot.material_slot_name): index for index, slot in enumerate(mesh_slots)}
                stage_matches = all(name in slots and package_name(body.get_material(slots[name])) == path
                                    for name, path in EXPECTED_STAGE_MATERIALS.items())
                check('stage_material_overrides_exist', stage_matches, 'Saved Hero stage material overrides are missing or incorrect.')

        if len(native_hairs) == 1:
            component = native_hairs[0].groom_component
            groom = component.get_editor_property('groom_asset') if component else None
            binding = component.get_editor_property('binding_asset') if component else None
            report['groom_asset'] = groom.get_path_name() if groom else None
            report['groom_binding'] = binding.get_path_name() if binding else None
            check('native_groom_exists', package_name(groom) == '/Game/Leon/Groom/Leon_Hair_Native', 'Native Groom asset is missing or incorrect.')
            check('native_binding_exists', package_name(binding) == '/Game/Leon/Groom/GB_Leon_Native_Hair', 'Native Groom binding is missing or incorrect.')
            groom_materials = [component.get_material(index) for index in range(component.get_num_materials())] if component else []
            report['groom_materials'] = [material.get_path_name() if material else None for material in groom_materials]
            check('groom_materials_exist', bool(groom_materials) and all(groom_materials), 'Native Groom has a missing material assignment.')
            attach_parent = component.get_attach_parent() if component else None
            check('groom_attached_to_hero_mesh', isinstance(attach_parent, unreal.SkeletalMeshComponent)
                  and package_name(attach_parent.get_skeletal_mesh_asset()) == '/Game/Leon/Mesh/SK_Leon_Preview', 'Native Groom is not attached to the retained preview mesh.')

    shader_paths = (
        ROOT / 'Shaders/LeonREHairGuideWrite.usf',
        ROOT / 'Plugins/LeonPreviewTools/Shaders/Private/LeonREHairGuide.ush',
    )
    report['shader_files'] = [{'path': str(path), 'exists': path.is_file()} for path in shader_paths]
    check('runtime_shader_files_exist', all(path.is_file() for path in shader_paths), 'A runtime RE hair shader source file is missing.')

    for row in package_rows:
        path = Path(row['file']) if row['file'] else None
        if path and path.is_file():
            row['sha256_after'] = digest(path)
    check('loaded_packages_unchanged', all(row.get('sha256_before') == row.get('sha256_after') and row.get('sha256_before') is not None for row in package_rows), 'A loaded package file changed during read-only verification.')
    report['success'] = bool(report['checks']) and all(report['checks'].values()) and not report['errors']
except Exception:
    report['errors'].append(traceback.format_exc())
    report['success'] = False

OUTPUT.parent.mkdir(parents=True, exist_ok=True)
OUTPUT.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
unreal.log('LEON_VIDEO_CLEANUP_VERIFICATION success=%s checks=%s errors=%s report=%s' %
           (report['success'], len(report['checks']), len(report['errors']), OUTPUT))
if not report['success']:
    raise RuntimeError('Leon video cleanup verification failed: ' + '; '.join(report['errors']))
