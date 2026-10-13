"""Reload saved Native Groom assets and record actual editor properties."""
import json
from pathlib import Path
import unreal

groom=unreal.load_asset('/Game/Leon/Groom/Leon_Hair_Native')
binding=unreal.load_asset('/Game/Leon/Groom/GB_Leon_Native_Hair')
instance=unreal.load_asset('/Game/Leon/Groom/MI_Leon_NativeHair')
report={'errors':[],'groups':[],'rendering':[],'parameters':{}}
for name,obj in [('groom',groom),('binding',binding),('material',instance)]:
    if not obj: report['errors'].append('Missing '+name)
    else: report[name]=obj.get_path_name()
if groom:
    for group in groom.get_editor_property('hair_groups_info'):
        report['groups'].append({k:group.get_editor_property(k) for k in
            ['group_id','num_curves','num_guides','num_curve_vertices','max_curve_length']})
    for group in groom.get_editor_property('hair_groups_rendering'):
        geometry=group.get_editor_property('geometry_settings')
        shadow=group.get_editor_property('shadow_settings')
        advanced=group.get_editor_property('advanced_settings')
        report['rendering'].append({'width_cm':geometry.get_editor_property('hair_width'),
            'width_override':geometry.get_editor_property('hair_width_override'),
            'root_scale':geometry.get_editor_property('hair_root_scale'),
            'tip_scale':geometry.get_editor_property('hair_tip_scale'),
            'voxelize':shadow.get_editor_property('voxelize'),
            'stable_rasterization':advanced.get_editor_property('use_stable_rasterization')})
    report['global_interpolation']=groom.get_editor_property('enable_global_interpolation')
if binding:
    target=binding.get_editor_property('target_skeletal_mesh')
    report['target_mesh']=target.get_path_name() if target else None
    report['binding_groups']=[{'render_roots':g.get_editor_property('ren_root_count'),
        'guide_roots':g.get_editor_property('sim_root_count'),
        'render_lods':g.get_editor_property('ren_lod_count')} for g in binding.get_editor_property('group_infos')]
    if not report['binding_groups'] or any(g['render_roots']<=0 or g['guide_roots']<=0 for g in report['binding_groups']):
        report['errors'].append('Binding root data not saved')
if instance:
    report['hair_color']=str(unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(instance,'HairColor'))
    report['parameters']={name:unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(instance,name)
        for name in ['DryRoughness','WetRoughness','WetAmount','Specular','Scatter','Backlit','AmbientOcclusion']}
report['success']=not report['errors']
(Path(unreal.Paths.project_dir())/'Scripts/native_groom_reload_verification.json').write_text(
    json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
unreal.log('NATIVE_GROOM_RELOADED %s'%report)
unreal.SystemLibrary.quit_editor()
