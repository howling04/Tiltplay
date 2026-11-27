"""Import the current editable art, or audit the current runtime dependency closure."""
import json,re,unreal
from pathlib import Path
root=Path(unreal.Paths.project_dir());lib=unreal.EditorAssetLibrary;mel=unreal.MaterialEditingLibrary;assets=unreal.AssetToolsHelpers.get_asset_tools()
out=root/'Saved/Verification';out.mkdir(parents=True,exist_ok=True)
if '-LunaAudit' in unreal.SystemLibrary.get_command_line():
    registry=unreal.AssetRegistryHelpers.get_asset_registry();registry.search_all_assets(True)
    roots={'/Game/LunaArt/Maps/L_WanderingLuna'}
    for file in (root/'Source/LunaMVP').glob('*.cpp'):
        roots.update(re.findall(r'/Game/[A-Za-z0-9_/]+',file.read_text(encoding='utf-8')))
    roots.update(p.split('.')[0] for p in json.loads((root/'Content/LunaArt/Data/narration.json').read_text()).values())
    options=unreal.AssetRegistryDependencyOptions(include_soft_package_references=True,include_hard_package_references=True,include_searchable_names=False,include_soft_management_references=False,include_hard_management_references=False)
    keep=set();todo=list(roots)
    while todo:
        p=str(todo.pop()).split('.')[0]
        if p in keep or not p.startswith('/Game/'):continue
        assert lib.does_asset_exist(p),p
        keep.add(p);todo.extend(str(x) for x in registry.get_dependencies(p,options))
    all_assets={str(a.package_name) for a in registry.get_assets_by_path('/Game',recursive=True)}
    report={'roots':sorted(roots),'keep':sorted(keep),'unused':sorted(all_assets-keep),'passed':True}
    (out/'dependencies.json').write_text(json.dumps(report,indent=2));unreal.log('LUNA_DEPENDENCY_AUDIT '+str(len(keep)))
else:
    path='/Game/LunaArt/Materials/M_LunarUnified'
    m=lib.load_asset(path) if lib.does_asset_exist(path) else assets.create_asset('M_LunarUnified','/Game/LunaArt/Materials',unreal.Material,unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(m)
    def node(cls):return mel.create_material_expression(m,cls)
    def scalar(v):
        e=node(unreal.MaterialExpressionConstant);e.set_editor_property('r',float(v));return e
    def vec(v):
        e=node(unreal.MaterialExpressionConstant3Vector);e.set_editor_property('constant',unreal.LinearColor(*v,1));return e
    def connect(a,b,pin,output=''):
        if pin in ('Input','Position'):
            names=mel.get_material_expression_input_names(b)
            if names:pin=names[0]
        ok=mel.connect_material_expressions(a,output,b,pin)
        if not ok and pin=='Input':ok=mel.connect_material_expressions(a,output,b,'')
        assert ok,(b.get_class().get_name(),pin)
    def op(cls,a,b):
        e=node(cls);connect(a,e,'A');connect(b,e,'B');return e
    def lerp(a,b,alpha):
        e=node(unreal.MaterialExpressionLinearInterpolate);connect(a,e,'A');connect(b,e,'B');connect(alpha,e,'Alpha');return e
    def tex(name,sampler):
        e=node(unreal.MaterialExpressionTextureSample);e.set_editor_property('texture',lib.load_asset('/Game/LunaArt/Textures/'+name));e.set_editor_property('sampler_type',sampler);return e
    world=node(unreal.MaterialExpressionWorldPosition)
    anchor=node(unreal.MaterialExpressionVectorParameter);anchor.set_editor_property('parameter_name','SurfaceAnchor');anchor.set_editor_property('use_custom_primitive_data',True);anchor.set_editor_property('primitive_data_index',0)
    radius=node(unreal.MaterialExpressionScalarParameter);radius.set_editor_property('parameter_name','DetailRadius');radius.set_editor_property('default_value',1);radius.set_editor_property('use_custom_primitive_data',True);radius.set_editor_property('primitive_data_index',4)
    relative=op(unreal.MaterialExpressionSubtract,world,anchor)
    distance=op(unreal.MaterialExpressionDistance,world,anchor)
    alpha=op(unreal.MaterialExpressionDivide,op(unreal.MaterialExpressionSubtract,radius,distance),op(unreal.MaterialExpressionMultiply,radius,scalar(1/7)))
    clamp=node(unreal.MaterialExpressionSaturate);connect(alpha,clamp,'Input')
    smooth=op(unreal.MaterialExpressionMultiply,op(unreal.MaterialExpressionMultiply,clamp,clamp),op(unreal.MaterialExpressionSubtract,scalar(3),op(unreal.MaterialExpressionMultiply,clamp,scalar(2))))
    noise=node(unreal.MaterialExpressionNoise);noise.set_editor_property('levels',3);noise.set_editor_property('quality',1);connect(op(unreal.MaterialExpressionMultiply,op(unreal.MaterialExpressionDivide,relative,radius),scalar(8400)),noise,'Position')
    grain=op(unreal.MaterialExpressionAdd,scalar(.68),op(unreal.MaterialExpressionMultiply,noise,scalar(.32)))
    local_color=op(unreal.MaterialExpressionMultiply,vec((.32,.31,.295)),grain)
    color=tex('Moon_BaseColor_4K',unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    normal=tex('Moon_Normal_4K',unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    rough=tex('Moon_Roughness_4K',unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    rough_scalar=node(unreal.MaterialExpressionComponentMask);rough_scalar.set_editor_property('r',True);connect(rough,rough_scalar,'Input')
    for n,prop in [(lerp(color,local_color,smooth),unreal.MaterialProperty.MP_BASE_COLOR),(lerp(normal,vec((0,0,1)),smooth),unreal.MaterialProperty.MP_NORMAL),(lerp(rough_scalar,scalar(.96),smooth),unreal.MaterialProperty.MP_ROUGHNESS)]:assert mel.connect_material_property(n,'',prop)
    m.set_editor_property('two_sided',True);mel.recompile_material(m);lib.save_loaded_asset(m)
    materials={'LunarSurface':m,'Rocks':lib.load_asset('/Game/LunaArt/Materials/M_LocalRocks')}
    for slot,name in [('Ceramic','M_EngineCeramic'),('Steel','M_EngineSteel'),('DarkSteel','M_EngineDarkSteel'),('Light','M_EngineLight')]:
        mat=lib.load_asset('/Game/LunaArt/Materials/'+name);mat.set_editor_property('used_with_instanced_static_meshes',True);mel.recompile_material(mat);lib.save_loaded_asset(mat);materials[slot]=mat
    report={}
    files=[] if '-LunaMaterialsOnly' in unreal.SystemLibrary.get_command_line() else (root/'Saved/BuildAssets').glob('*.obj')
    for file in files:
        task=unreal.AssetImportTask();task.set_editor_property('filename',str(file));task.set_editor_property('destination_path','/Game/LunaArt/Meshes');task.set_editor_property('destination_name',file.stem);task.set_editor_property('automated',True);task.set_editor_property('save',True);task.set_editor_property('replace_existing',True)
        opts=unreal.FbxImportUI();opts.set_editor_property('import_mesh',True);opts.set_editor_property('import_materials',False);opts.set_editor_property('import_textures',False);opts.set_editor_property('mesh_type_to_import',unreal.FBXImportType.FBXIT_STATIC_MESH)
        for k,v in [('combine_meshes',True),('generate_lightmap_u_vs',False),('remove_degenerates',False),('auto_generate_collision',False),('normal_import_method',unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)]:opts.static_mesh_import_data.set_editor_property(k,v)
        task.set_editor_property('options',opts);task.set_editor_property('factory',unreal.FbxFactory());assets.import_asset_tasks([task]);assert task.imported_object_paths,str(file)
        mesh=lib.load_asset(task.imported_object_paths[0]);slots=[]
        for i,slot in enumerate(mesh.get_editor_property('static_materials')):
            name=str(slot.get_editor_property('imported_material_slot_name'));assert name in materials,name;mesh.set_material(i,materials[name]);slots.append(name)
        lib.save_loaded_asset(mesh);report[file.stem]={'materials':slots,'extent':str(mesh.get_bounds().box_extent)}
    if report:(out/'art.json').write_text(json.dumps(report,indent=2))
    unreal.log('LUNA_CURRENT_ART_SAVED '+json.dumps(report))
