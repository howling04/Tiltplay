"""Rebuild Luna's exhaust assets and place the solar-system snapshot in the main map."""
import json
import math
from pathlib import Path
import re
import struct
import zlib

import unreal


PROJECT = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
SOURCE = PROJECT / "SourceArt/SolarSystem"
WORK = PROJECT / "Saved/Verification/LunaFlightUpdate"
WORK.mkdir(parents=True, exist_ok=True)
CONTENT = "/Game/LunaArt"
LEVEL = CONTENT + "/Maps/L_WanderingLuna"
ART = CONTENT + "/SolarSystem"
LIB = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()


def save(asset):
    assert LIB.save_loaded_asset(asset, only_if_is_dirty=False), asset.get_path_name()
    return asset


def get_or_create(name, directory, asset_type, factory):
    path = directory + "/" + name
    if LIB.does_asset_exist(path):
        return LIB.load_asset(path)
    return TOOLS.create_asset(name, directory, asset_type, factory)


def import_file(file, directory, *, mesh=False):
    task = unreal.AssetImportTask()
    task.set_editor_properties(dict(filename=str(file), destination_path=directory,
                                    destination_name=file.stem, automated=True,
                                    replace_existing=True, save=False))
    if mesh:
        options = unreal.FbxImportUI()
        options.set_editor_properties(dict(import_mesh=True, import_materials=False, import_textures=False,
                                           mesh_type_to_import=unreal.FBXImportType.FBXIT_STATIC_MESH))
        options.static_mesh_import_data.set_editor_properties(dict(
            combine_meshes=True, generate_lightmap_u_vs=False, auto_generate_collision=False,
            normal_import_method=unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS))
        task.set_editor_properties(dict(options=options, factory=unreal.FbxFactory()))
    TOOLS.import_asset_tasks([task])
    assert task.imported_object_paths, file
    return LIB.load_asset(task.imported_object_paths[0])


def texture(file, directory, *, linear=False, radial=False):
    asset = import_file(file, directory)
    asset.set_editor_properties(dict(srgb=not linear,
        address_x=unreal.TextureAddress.TA_CLAMP if radial else unreal.TextureAddress.TA_WRAP,
        address_y=unreal.TextureAddress.TA_WRAP if linear else unreal.TextureAddress.TA_CLAMP))
    if radial:
        asset.set_editor_property("never_stream", True)
    return save(asset)


def material(name, directory, blend=unreal.BlendMode.BLEND_OPAQUE, *, twosided=False, instanced=False):
    asset = get_or_create(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(asset)
    asset.set_editor_properties(dict(shading_model=unreal.MaterialShadingModel.MSM_UNLIT, blend_mode=blend,
                                     two_sided=twosided, used_with_instanced_static_meshes=instanced))
    return asset


def node(material_asset, cls, **props):
    expression = MEL.create_material_expression(material_asset, cls)
    expression.set_editor_properties(props)
    return expression


def graph_code(material_asset, code, inputs):
    pins = []
    for name in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", name)
        pins.append(pin)
    expression = node(material_asset, unreal.MaterialExpressionCustom, code=code,
                      output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, inputs=pins)
    for name, (source, output) in inputs.items():
        assert MEL.connect_material_expressions(source, output, expression, name), name
    return expression


def finish(material_asset, emission, opacity=None):
    assert MEL.connect_material_property(emission, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if opacity:
        assert MEL.connect_material_property(opacity, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    MEL.layout_material_expressions(material_asset)
    MEL.recompile_material(material_asset)
    return save(material_asset)


def write_lathe(file, sides, profile, *, ring=False):
    """Emit a Z-up OBJ, mirroring Y for Unreal's right-to-left handed import."""
    vertices, normals, coords, faces = [], [], [], []
    for row, (radius, height, v) in enumerate(profile):
        prev, nxt = profile[max(0, row-1)], profile[min(len(profile)-1, row+1)]
        tangent = (nxt[0]-prev[0], nxt[1]-prev[1])
        for col in range(sides+1):
            angle = math.tau * col / sides
            x, y = math.cos(angle), math.sin(angle)
            vertices.append((radius*x, -radius*y, height))
            normals.append((0, 0, 1) if ring else (tangent[1]*x, -tangent[1]*y, -tangent[0]))
            # Unreal's OBJ importer converts V from the file's bottom-up convention.
            coords.append((v, 1-col/sides) if ring else (col/sides, 1-v))
            if col < sides and row < len(profile)-1:
                a = row * (sides+1) + col + 1
                b = a + sides+1
                faces.extend([(a, b+1, a+1), (a, b, b+1)] if not ring
                             else [(a, a+1, b+1), (a, b+1, b)])
    with file.open("w") as obj:
        obj.writelines("v %.8f %.8f %.8f\n" % v for v in vertices)
        for normal in normals:
            length = math.sqrt(sum(v*v for v in normal))
            obj.write("vn %.8f %.8f %.8f\n" % tuple(x/length for x in normal))
        obj.writelines("vt %.8f %.8f\n" % uv for uv in coords)
        obj.writelines("f " + " ".join(f"{i}/{i}/{i}" for i in f) + "\n" for f in faces)


def write_flow_texture(file):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload)))

    size = 128
    rows = bytearray()
    for row in range(size):
        y = row/size*math.tau
        rows.append(0)
        for col in range(size):
            x = col/size*math.tau
            filament = math.sin(7*x + .6*math.sin(2*y+x))
            pulse = math.sin(3*y + .9*math.sin(3*x))
            turbulence = math.sin(5*x+2*y) * math.cos(3*x-5*y)
            rows.append(round(128 + 60*filament + 35*pulse + 29*turbulence))
    file.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 0, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def make_exhaust():
    file = WORK / "T_ExhaustFlow.png"
    write_flow_texture(file)
    flow = texture(file, CONTENT + "/Textures", linear=True)
    mat = material("M_ExhaustFlow", CONTENT + "/Materials", unreal.BlendMode.BLEND_ADDITIVE, instanced=True)
    emission = graph_code(mat, """
float h = saturate(UV.y);
float2 longFlow = float2(UV.x, h * 1.8 - TimeSeconds * 2.45 + InstancePhase);
float2 shortFlow = float2(UV.x * 2 + 0.31, h * 2.7 - TimeSeconds * 1.16 + InstancePhase);
float stream = Texture2DSample(Flow, FlowSampler, longFlow).r;
float shear = Texture2DSample(Flow, FlowSampler, shortFlow).r;
float flicker = smoothstep(0.12, 0.92, 0.72 * stream + 0.28 * shear);
float edge = pow(saturate(abs(dot(normalize(SurfaceNormal), normalize(ToViewer)))), 1.25);
float tail = 1 - smoothstep(0.38, 1, h);
float3 color = lerp(float3(0.055, 0.30, 1.0), float3(0.68, 0.88, 1.0),
                    saturate((0.9 - h) * edge + 0.26 * flicker));
return color * (0.23 + 2.1 * flicker) * tail * (0.16 + 0.84 * edge) * 4.3;
""", {
        "UV": (node(mat, unreal.MaterialExpressionTextureCoordinate), ""),
        "Flow": (node(mat, unreal.MaterialExpressionTextureObject, texture=flow,
                      sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR), ""),
        "TimeSeconds": (node(mat, unreal.MaterialExpressionTime), ""),
        "InstancePhase": (node(mat, unreal.MaterialExpressionPerInstanceRandom), ""),
        "SurfaceNormal": (node(mat, unreal.MaterialExpressionVertexNormalWS), ""),
        "ToViewer": (node(mat, unreal.MaterialExpressionCameraVectorWS), ""),
    })
    finish(mat, emission)
    profiles = {
        "Near": (24, [(100,0,0),(107,12,.12),(93,31,.31),(74,55,.55),(48,77,.77),(4,100,1)]),
        "Far": (8, [(100,0,0),(103,23,.23),(67,62,.62),(4,100,1)]),
    }
    for detail, (sides, profile) in profiles.items():
        path = WORK / f"SM_LunaExhaust_{detail}.obj"
        write_lathe(path, sides, profile)
        mesh = import_file(path, CONTENT + "/Meshes", mesh=True)
        bounds = mesh.get_bounds()
        assert abs(bounds.origin.z-50) < .1 and abs(bounds.box_extent.z-50) < .1, bounds
        mesh.set_material(0, mat)
        save(mesh)


def solar_material(default_map):
    mat = material("M_PlanetSurface", ART)
    emission = graph_code(mat, """
float3 n = normalize(NormalWS);
float sun = dot(n, normalize(SolarVector));
float day = pow(saturate((sun + 0.018) / 1.018), 0.82);
float limb = pow(saturate(1 - dot(n, normalize(ToViewer))), 4.0) * smoothstep(-0.15, 0.52, sun);
return MapColor * (0.004 + day * ExposureScale) + LimbColour * (limb * ExposureScale);
""", {
        "MapColor": (node(mat, unreal.MaterialExpressionTextureSampleParameter2D,
                          parameter_name="SurfaceMap", texture=default_map), "RGB"),
        "NormalWS": (node(mat, unreal.MaterialExpressionVertexNormalWS), ""),
        "ToViewer": (node(mat, unreal.MaterialExpressionCameraVectorWS), ""),
        "SolarVector": (node(mat, unreal.MaterialExpressionVectorParameter, parameter_name="SunDirection",
                            default_value=unreal.LinearColor(.3,.1,.95,1)), "RGB"),
        "LimbColour": (node(mat, unreal.MaterialExpressionVectorParameter, parameter_name="LimbColour"), "RGB"),
        "ExposureScale": (node(mat, unreal.MaterialExpressionScalarParameter, parameter_name="ExposureScale",
                              default_value=1.0), ""),
    })
    return finish(mat, emission)


def rings_material(ring_texture, name="M_SaturnRings"):
    mat = material(name, ART, unreal.BlendMode.BLEND_MASKED, twosided=True)
    mat.set_editor_property("opacity_mask_clip_value", .08)
    sample = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name="RingProfile", texture=ring_texture)
    relative = node(mat, unreal.MaterialExpressionSubtract)
    assert MEL.connect_material_expressions(node(mat, unreal.MaterialExpressionWorldPosition), "", relative, "A")
    assert MEL.connect_material_expressions(node(mat, unreal.MaterialExpressionObjectPositionWS), "", relative, "B")
    emission = graph_code(mat, """
float3 p = OffsetWS / max(RadiusUU, 1);
float3 sun = normalize(SolarVector);
float alongSun = dot(p, sun);
float shadow = alongSun < 0 ? smoothstep(0.965, 1.018, length(p - alongSun * sun)) : 1;
float incidence = abs(dot(normalize(NormalWS), sun));
return RingColor * (0.012 + shadow * sqrt(incidence) * ExposureScale) * saturate(0.15 + Density);
""", {
        "RingColor": (sample, "RGB"),
        "Density": (sample, "A"),
        "OffsetWS": (relative, ""),
        "NormalWS": (node(mat, unreal.MaterialExpressionVertexNormalWS), ""),
        "SolarVector": (node(mat, unreal.MaterialExpressionVectorParameter, parameter_name="SunDirection"), "RGB"),
        "RadiusUU": (node(mat, unreal.MaterialExpressionScalarParameter, parameter_name="RadiusUU"), ""),
        "ExposureScale": (node(mat, unreal.MaterialExpressionScalarParameter, parameter_name="ExposureScale"), ""),
    })
    return finish(mat, emission, sample)


def material_instance(name, parent, sun, exposure, *, planet_map=None, rim=None, radius=None):
    inst = get_or_create(name, ART, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(inst, parent)
    MEL.set_material_instance_vector_parameter_value(inst, "SunDirection", unreal.LinearColor(sun.x, sun.y, sun.z, 1))
    MEL.set_material_instance_scalar_parameter_value(inst, "ExposureScale", exposure)
    if planet_map:
        MEL.set_material_instance_texture_parameter_value(inst, "SurfaceMap", planet_map)
        MEL.set_material_instance_vector_parameter_value(inst, "LimbColour", unreal.LinearColor(*rim, 1))
    if radius:
        MEL.set_material_instance_scalar_parameter_value(inst, "RadiusUU", radius)
    return save(inst)


def place_body(subsystem, existing, name, mesh, mat, center, rot, scale):
    label = "LUNA | " + name
    actor = existing.get(label) or subsystem.spawn_actor_from_class(unreal.StaticMeshActor, center, rot)
    actor.set_actor_label(label)
    actor.set_folder_path("Solar System")
    component = actor.static_mesh_component
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    component.set_static_mesh(mesh)
    component.set_material(0, mat)
    component.set_collision_profile_name("NoCollision")
    component.set_editor_properties(dict(cast_shadow=False, visible_in_ray_tracing=False,
                                         affect_distance_field_lighting=False, receives_decals=False))
    actor.set_actor_location(center, False, True)
    actor.set_actor_rotation(rot, True)
    actor.set_actor_scale3d(scale)
    component.set_mobility(unreal.ComponentMobility.STATIC)
    return actor


def solar_frame(snapshot, existing):
    # The ephemeris is tied to the original lunar location even after the
    # training lane has been projected onto the heliocentric ecliptic.
    origin = unreal.Vector(*snapshot.get("original_lunar_origin_uu", [0,0,300]))
    sun_actor = existing["LUNA | Sun Disc"]
    sun = next(c for c in sun_actor.get_components_by_class(unreal.StaticMeshComponent)
               if c.get_name() == "SunSphere").get_world_location()

    source_x = unreal.Vector(*snapshot["sun_moon_centered_km"]).normal()
    obliquity = math.radians(84381.448/3600)
    ecliptic_north = unreal.Vector(0, -math.sin(obliquity), math.cos(obliquity))
    source_y = ecliptic_north.cross(source_x).normal()
    source_z = source_x.cross(source_y).normal()
    ue_x = (sun - origin).normal()
    ue_y = (unreal.Vector(0,1,0) - ue_x * ue_x.y).normal()
    ue_z = ue_y.cross(ue_x).normal()

    def align(vector):
        return ue_x * vector.dot(source_x) + ue_y * vector.dot(source_y) + ue_z * vector.dot(source_z)

    return origin, sun, align, align(ecliptic_north).normal()


def scene_km_per_uu():
    scale_header = (PROJECT / "Source/LunaMVP/LunaScale.h").read_text()
    return (float(re.search(r"RadiusKm=([0-9.]+)", scale_header)[1])
            / float(re.search(r"RadiusUU=([0-9.]+)", scale_header)[1]))


def place_solar_system(subsystem, existing):
    snapshot = json.loads((SOURCE / "ephemeris.json").read_text())
    km_per_uu = scene_km_per_uu()
    origin, sun, align, _ = solar_frame(snapshot, existing)

    body_maps = {name: texture(SOURCE / f"{name}.jpg", ART + "/Textures") for name in snapshot["bodies"]}
    surface = solar_material(body_maps["Mercury"])
    ring_map = texture(SOURCE / "SaturnRings.png", ART + "/Textures", radial=True)
    ring_base = rings_material(ring_map)
    globe = LIB.load_asset("/Engine/BasicShapes/Sphere")
    globe_radius = globe.get_bounds().box_extent.x
    rim_colors = dict(Venus=(.075,.059,.035), Mars=(.026,.009,.003), Jupiter=(.032,.027,.021),
                      Saturn=(.040,.033,.020), Uranus=(.021,.057,.063),
                      Neptune=(.026,.050,.068), Pluto=(.008,.015,.025))
    report = {}
    for name, body in snapshot["bodies"].items():
        center = origin + align(unreal.Vector(*body["moon_centered_km"])) / km_per_uu
        ra, dec = map(math.radians, body["pole_icrf_deg"])
        pole = align(unreal.Vector(math.cos(dec)*math.cos(ra), math.cos(dec)*math.sin(ra), math.sin(dec)))
        rotation = unreal.MathLibrary.make_rot_from_z(pole)
        radii = unreal.Vector(*body["radii_km"]) / km_per_uu
        to_sun = sun - center
        # Compress the inverse-square exposure range so outer planets retain readable close-ups.
        exposure = (149597870.7 / (to_sun.length() * km_per_uu)) ** .38
        mat = material_instance("MI_" + name, surface, to_sun.normal(), exposure,
                                planet_map=body_maps[name], rim=rim_colors.get(name, (0,0,0)))
        place_body(subsystem, existing, name, globe, mat, center, rotation, radii / globe_radius)
        report[name] = dict(position_uu=[center.x,center.y,center.z], radii_uu=[radii.x,radii.y,radii.z],
                            lunar_distance_km=(center-origin).length()*km_per_uu)

        if name == "Saturn":
            file = WORK / "SM_SaturnRings.obj"
            inner, outer = [radius / km_per_uu for radius in snapshot["rings_km"]]
            write_lathe(file, 96, [(inner,0,0),(outer,0,1)], ring=True)
            ring = import_file(file, ART, mesh=True)
            assert abs(ring.get_bounds().box_extent.x-outer)<.1
            ring_asset = material_instance("MI_SaturnRings", ring_base, to_sun.normal(), exposure, radius=radii.x)
            ring.set_material(0, ring_asset)
            save(ring)
            place_body(subsystem, existing, "Saturn Rings", ring, ring_asset, center, rotation, unreal.Vector(1,1,1))

    return report


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    assert levels.load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    existing = {actor.get_actor_label(): actor for actor in actors.get_all_level_actors()}
    course = existing["LUNA | Asteroid navigation and holographic volumes"]
    # Refresh the map's serialized course components with the current C++ layout.
    course.modify()
    course.set_editor_property("asteroid_density", course.get_editor_property("asteroid_density"))
    interlock = next(c for c in course.get_components_by_class(unreal.BoxComponent)
                     if c.get_name().startswith("PolarInterlock"))
    assert abs(interlock.get_editor_property("relative_location").x-15940)<.1, interlock
    make_exhaust()
    for file in sorted((WORK / "voices").glob("VO_Nano_*.wav")):
        save(import_file(file, CONTENT + "/Audio"))
    report = place_solar_system(actors, existing)
    assert unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, False)
    assert not unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()
    (WORK / "solar-system-saved.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    unreal.log(f"LUNA_FLIGHT_ASSETS_SAVED {len(report)} bodies")


if __name__ == "__main__":
    main()
