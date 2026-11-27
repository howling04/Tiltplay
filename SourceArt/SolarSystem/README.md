# Luna solar-system assets

The `Solar System` folder in `L_WanderingLuna` contains the eight planets, Pluto, 21 natural satellites, and the four giant planets' ring meshes. LUNA is the player-controlled Moon. Frame a body in the editor to inspect its material at close range. From LUNA, some discs span less than a pixel because body dimensions and distances use the same 62.05 km per Unreal unit.

## Scene coordinates

`ephemeris.json` and `satellite_ephemeris.json` record geometric ICRF vectors from the Moon's centre at 2026-11-20 00:00 UTC. Radii and pole directions come from the NAIF planetary constants kernel. The importer preserves the saved Sun anchor and uses the projected departure point and the actual ecliptic north to orient the flight course. Layer 0 lies on that plane in training and free flight. The bodies form a fixed scene. Each material takes its light direction from the Sun disc and compresses the exposure range for close viewing. [JPL Horizons](https://ssd.jpl.nasa.gov/horizons/) · [NAIF PCK](https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/pck00011.tpc)

## Texture credits

- Mercury, Venus cloud tops, Saturn, Saturn rings, Uranus, and Neptune use Solar System Scope textures by INOVE. The project resizes the maps, softens the Venus and Neptune color grades, and crops the Saturn ring texture to its radial profile. [Original textures](https://www.solarsystemscope.com/textures/) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
- Mars uses the Viking mosaic processed by USGS and distributed by Caltech/JPL. The project resamples the mosaic for Unreal. [JPL Mars texture](https://space.jpl.nasa.gov/tmaps/mars.html)
- Jupiter uses the Cassini cylindrical mosaic PIA07782 by NASA, JPL, and the Space Science Institute. The project downsamples the mosaic. [NASA Jupiter map](https://science.nasa.gov/photojournal/cassinis-best-maps-of-jupiter-cylindrical-map/)
- Pluto uses the New Horizons color mosaic PIA11707 by NASA, JHUAPL, and SwRI. The unobserved southern region blends into a neutral fill. [NASA Pluto map](https://science.nasa.gov/photojournal/pluto-color-map/)
- Earth uses NASA Goddard's MODIS Blue Marble mosaic. Mars, Jupiter and Uranus's moons and Iapetus use JPL/Caltech and USGS mission mosaics. [NASA Blue Marble](https://svs.gsfc.nasa.gov/2915/) · [JPL Solar System Simulator texture archive](https://space.jpl.nasa.gov/tmaps/)
- Mimas, Enceladus, Tethys, Dione and Rhea use the Cassini enhanced-color cylindrical maps. Triton uses Paul Schenk's Voyager 2 color reconstruction. The project blends unobserved areas of these six maps and the five major Uranian moons toward a smooth, neutral albedo. [Cassini PIA13423](https://science.nasa.gov/photojournal/enhanced-color-maps-of-saturn-inner-moons/) · [Voyager 2 PIA18668](https://science.nasa.gov/resource/map-of-triton/)
- Titan uses a warm-toned crop of the Cassini 938 nm mosaic. Charon uses the New Horizons cylindrical mosaic with its unobserved region blended to gray. Proteus uses Phil Stooke's Voyager 2 shaded relief reconstruction. [Titan PIA19658](https://science.nasa.gov/photojournal/titan-global-map-june-2015/) · [Charon PIA19866](https://science.nasa.gov/photojournal/global-map-of-plutos-moon-charon/) · [Proteus cartographic source](https://solarviews.com/cap/nep/proteus2.htm)
- Jupiter, Uranus and Neptune use radial textures baked from PDS band radii, widths and optical depths. Opacity is tone-mapped to make faint rings readable. Very narrow bands span a few texels. [Jupiter bands](https://pds-rings.seti.org/jupiter/jupiter_rings_table.html) · [Uranus bands](https://pds-rings.seti.org/uranus/uranus_rings_table.html) · [Neptune bands](https://pds-rings.seti.org/neptune/neptune_rings_table.html)

`SourceArt/import_solar_navigation.py` rebuilds the satellite actors, giant-planet rings, flight frame and map catalog through Unreal's Python commandlet. The surface maps are editable in the material instances under `Content/LunaArt/SolarSystem`.
