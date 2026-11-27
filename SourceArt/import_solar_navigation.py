"""Add the observed satellite systems and align the saved flight lane to J2000."""
import json
import math
from pathlib import Path
import re
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_flight_scene as art


WORK = art.PROJECT / "Saved/Verification/SolarFlight"
WORK.mkdir(parents=True, exist_ok=True)
AU_KM = 149597870.7


def xyz(v):
    return [v.x, v.y, v.z]


def catalog_entry(name, location, radius, parent="", rings=0.0):
    return dict(name=name, position_uu=xyz(location), radius_uu=radius, parent=parent, ring_radius_uu=rings)


def orbit_pole(body, align):
    ra, dec = map(math.radians, body["pole_icrf_deg"])
    return align(unreal.Vector(math.cos(dec)*math.cos(ra), math.cos(dec)*math.sin(ra), math.sin(dec)))


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    assert levels.load_level(art.LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    existing = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
    snapshot = json.loads((art.SOURCE / "ephemeris.json").read_text(encoding="utf-8"))
    satellites = json.loads((art.SOURCE / "satellite_ephemeris.json").read_text(encoding="utf-8"))
    rings = json.loads((art.SOURCE / "ring_profiles.json").read_text(encoding="utf-8"))
    km_per_uu = art.scene_km_per_uu()
    lunar_origin, sun, align, north = art.solar_frame(snapshot, existing)
    plane_origin = lunar_origin - north * (lunar_origin-sun).dot(north)
    flight_header = (art.PROJECT/"Source/LunaMVP/LunaFlightPlan.h").read_text(encoding="utf-8")
    plane_z = float(re.search(r"PlaneZ = ([0-9.]+)", flight_header)[1])
    forward = (plane_origin-sun).normal()
    orient = unreal.MathLibrary.make_rot_from_xz(forward, north)
    course = existing["LUNA | Asteroid navigation and holographic volumes"]
    departure = existing["LUNA | Departure"]
    course.modify()
    departure.modify()
    course.set_actor_location(plane_origin-north*plane_z, False, True)
    course.set_actor_rotation(orient, True)
    departure.set_actor_location(plane_origin, False, True)
    departure.set_actor_rotation(orient, True)
    assert abs((departure.get_actor_location()-sun).dot(north)) < .001

    globe = art.LIB.load_asset("/Engine/BasicShapes/Sphere")
    globe_radius = globe.get_bounds().box_extent.x
    surface = art.LIB.load_asset(art.ART+"/M_PlanetSurface")
    placements = {name: existing["LUNA | "+name] for name in snapshot["bodies"]}
    for name, body in satellites.items():
        source_texture = art.SOURCE / "Satellites" / (name+".jpg")
        body_map = art.texture(source_texture, art.ART+"/Textures")
        center = lunar_origin+align(unreal.Vector(*body["moon_centered_km"]))/km_per_uu
        to_sun = sun-center
        scale = unreal.Vector(*body["radii_km"])/(km_per_uu*globe_radius)
        rim = {"Earth":(.035,.093,.19), "Titan":(.12,.055,.014), "Triton":(.009,.012,.018)}.get(name,(0,0,0))
        mat = art.material_instance("MI_"+name, surface, to_sun.normal(),
            (AU_KM/(to_sun.length()*km_per_uu))**.38, planet_map=body_map, rim=rim)
        rotation = unreal.MathLibrary.make_rot_from_z(orbit_pole(body,align))
        actor = art.place_body(actors, existing, name, globe, mat, center, rotation, scale)
        if body["parent"]:
            actor.set_folder_path("Solar System/"+body["parent"]+" system")
        placements[name] = actor
        unreal.log(f"LUNA_SOLAR_BODY {name} {center}")

    ring_maps = {name:art.texture(art.SOURCE/(name+"Rings.png"),art.ART+"/Textures",radial=True)
                 for name in rings}
    ring_master = art.rings_material(ring_maps["Jupiter"],"M_GiantRingProfile")
    for name, values in rings.items():
        body = placements[name]
        center = body.get_actor_location()
        to_sun = sun-center
        equator = snapshot["bodies"][name]["radii_km"][0]/km_per_uu
        inner,outer = [r/km_per_uu for r in values["radial_range_km"]]
        file = WORK / ("SM_"+name+"Rings.obj")
        art.write_lathe(file, 128, [(inner,0,0),(outer,0,1)], ring=True)
        mesh = art.import_file(file,art.ART,mesh=True)
        assert abs(mesh.get_bounds().box_extent.z)<.001 and abs(mesh.get_bounds().box_extent.x-outer)<.1
        mat = art.material_instance("MI_"+name+"Rings",ring_master,to_sun.normal(),
            (AU_KM/(to_sun.length()*km_per_uu))**.38, radius=equator)
        art.MEL.set_material_instance_texture_parameter_value(mat,"RingProfile",ring_maps[name])
        art.save(mat)
        mesh.set_material(0,mat)
        art.save(mesh)
        ring = art.place_body(actors,existing,name+" Rings",mesh,mat,center,
                             body.get_actor_rotation(),unreal.Vector(1,1,1))
        ring.set_folder_path("Solar System/"+name+" system")
        unreal.log(f"LUNA_SOLAR_RING {name} inner={inner:.4f} outer={outer:.4f}")

    sun_component = next(c for c in existing["LUNA | Sun Disc"].get_components_by_class(unreal.StaticMeshComponent)
                         if c.get_name()=="SunSphere")
    # Use the mesh radius since the component bounds' sphere can include box corners.
    sun_radius = sun_component.get_editor_property("static_mesh").get_bounds().box_extent.x * sun_component.get_world_scale().x
    catalog = [catalog_entry("Sun",sun,sun_radius)]
    all_bodies = snapshot["bodies"] | satellites
    outer_limits = {"Saturn":snapshot["rings_km"][1]} | {name:r["radial_range_km"][1] for name,r in rings.items()}
    order = ["Mercury","Venus","Earth","Mars","Jupiter","Saturn","Uranus","Neptune","Pluto"]
    for name in order + [n for n,data in satellites.items() if data["parent"]]:
        values = all_bodies[name]
        catalog.append(catalog_entry(name, placements[name].get_actor_location(),
            max(values["radii_km"])/km_per_uu, values.get("parent",""), outer_limits.get(name,0.0)/km_per_uu))
    manifest = dict(epoch_utc=snapshot["epoch_utc"], frame="J2000 ecliptic through the saved Sun",
                    ecliptic_north_ue=xyz(north), departure_ue=xyz(plane_origin),
                    course_forward_ue=xyz(forward), bodies=catalog)
    assert unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True,False)
    assert not unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()
    file = art.PROJECT/"Content/LunaArt/Data/solar_navigation.json"
    file.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    unreal.log(f"LUNA_SOLAR_SAVED catalog={len(catalog)} satellites={len(satellites)-1} rings={len(rings)+1}")


if __name__=="__main__":
    main()
