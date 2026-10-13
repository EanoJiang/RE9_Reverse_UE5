"""Fresh-editor verification of saved map component settings; no asset writes.

Run after the preview owner has saved the final map, in a fresh editor process.
This verifies persistence and does not request GPU rendering or alter assets.
"""
import json
from pathlib import Path
import unreal

project = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
report = {'success': False, 'errors': [], 'asset_writes': False}
try:
    map_path = '/Game/Leon/Preview/LeonVideoPreview'
    if not unreal.EditorLoadingAndSavingUtils.load_map(map_path):
        raise RuntimeError('Cannot load saved preview map')
    actors = list(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
    hairs = [actor for actor in actors if isinstance(actor, unreal.GroomActor)
             and 'Native' in actor.get_actor_label()]
    if len(hairs) != 1:
        raise RuntimeError('Expected exactly one native GroomActor, found %s' % len(hairs))
    hair = hairs[0]
    component = hair.groom_component
    report['groom_actor'] = hair.get_path_name()
    report['groom_asset'] = component.get_editor_property('groom_asset').get_path_name()
    binding = component.get_editor_property('binding_asset')
    report['binding_asset'] = binding.get_path_name() if binding else None
    report['binding_groups'] = [
        {'render_roots': group.get_editor_property('ren_root_count'),
         'guide_roots': group.get_editor_property('sim_root_count')}
        for group in binding.get_editor_property('group_infos')] if binding else []
    body = component.get_attach_parent()
    if not isinstance(body, unreal.SkeletalMeshComponent):
        raise RuntimeError('Groom attachment is not a SkeletalMeshComponent')
    report['target_mesh'] = body.get_skeletal_mesh_asset().get_path_name()
    report['original_mesh_physics_asset'] = body.get_skeletal_mesh_asset().get_editor_property('physics_asset').get_path_name()
    report['relative_location_cm'] = list(component.get_editor_property('relative_location').to_tuple())
    report['relative_rotation_degrees'] = list(component.get_editor_property('relative_rotation').to_tuple())
    report['relative_scale'] = list(component.get_editor_property('relative_scale3d').to_tuple())
    report['inspection'] = json.loads(unreal.LeonGroomPhysicsLibrary.inspect_native_groom_simulation(component))
    inspection = report['inspection']
    shapes = inspection.get('collision_shapes_world', [])
    checks = {
        'native_groom': report['groom_asset'].startswith('/Game/Leon/Groom/Leon_Hair_Native.'),
        'native_binding': bool(binding) and report['binding_asset'].startswith('/Game/Leon/Groom/GB_Leon_Native_Hair.'),
        'binding_render_roots': sum(group['render_roots'] for group in report['binding_groups']) == 20214,
        'binding_guide_roots': sum(group['guide_roots'] for group in report['binding_groups']) == 2021,
        'target_preview_mesh': report['target_mesh'].startswith('/Game/Leon/Mesh/SK_Leon_Preview.'),
        'original_mesh_pa_preserved': report['original_mesh_physics_asset'].startswith('/Game/Leon/Mesh/SK_Leon_Preview_PhysicsAsset.'),
        'unit_location': all(abs(value) < 0.001 for value in report['relative_location_cm']),
        'unit_rotation': all(abs(value) < 0.001 for value in report['relative_rotation_degrees']),
        'unit_scale': all(abs(value - 1.0) < 0.001 for value in report['relative_scale']),
        'head_only_override': inspection.get('physics_asset', '').startswith('/Game/Leon/Groom/PA_Leon_NativeGroomHead.') and len(shapes) == 1 and shapes[0]['bone'] == 'Head_0',
        'head_radius_cm': len(shapes) == 1 and all(7.6 < value < 7.8 for value in shapes[0].get('radius_world_axes_cm', [])),
        'solver_capability': inspection.get('asset_solver_capability', False),
        'component_override': inspection.get('component_override', False),
        'simulation_default_off': not inspection.get('component_solver_flag', True) and all(not group['effective_lod0_simulation'] for group in inspection.get('groups', [])),
        'no_duplicate_collision_source': inspection.get('collision_components') == 0,
    }
    report['checks'] = checks
    report['success'] = all(checks.values())
except Exception as exc:
    report['errors'].append(str(exc))
(project / 'Scripts/native_groom_saved_video_map_verification.json').write_text(
    json.dumps(report, indent=2), encoding='utf-8')
unreal.log('NATIVE_GROOM_SAVED_MAP_VERIFICATION %s' % report)
unreal.SystemLibrary.quit_editor()
