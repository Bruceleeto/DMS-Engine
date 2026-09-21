# DMS-Engine

A 3D engine for the Sega Dreamcast, built on KallistiOS and sh4zam. Models are
converted from `.glb` to the engine's `.dms` format with the converter in
`converter/`.

## Building an example

Each example is its own folder with a `Makefile`, a `main.c` and an `assets/`
folder of `.glb` files.

| Command | What it does |
|---|---|
| `make assets` | Converts `assets/*/*.glb` into `pc/` |
| `make` | Builds `test.elf`. Assets are read over dcload from `/pc/` (run `dc-tool` with `-c pc`) |
| `make disc` | Builds `test.elf` reading from `/cd/`, and a `.cdi` named after the folder with `pc/` as the disc root (needs `mkdcdisc`) |

## Examples

### picophysics

![picophysics](picophysics/resources/example.png)

Rigid body physics.
(`dms/picophysics.h`). Bodies collide with the level through the engine's
collision grid. A crate and a ball drop at the start, and a wrecking ball
swings on a chain of 14 jointed links.

| Button | Action |
|---|---|
| A | Throw a ball |
| X | Throw a crate |
| Y | Reset |
| Start | Exit |

Credits:
- Level: [GameCube - Mario Kart Double Dash - Block City](https://sketchfab.com/3d-models/gamecube-mario-kart-double-dash-block-city-9d8002b53a884d0290759f05694a49c3)
  by [jkimmel694](https://sketchfab.com/jkimmel694), CC Attribution 4.0
- Ball, crate, chain: to add

### race_track

![race_track](race_track/resources/example.png)

A large racing track with a car, third-person camera and collision. The
track is close to a worst case for per-mesh overhead: many materials spread
over many small meshes.

Credits:
- Track: [nurburgring (race driver grid ds)](https://sketchfab.com/3d-models/nurburgring-race-driver-grid-ds-fc6393b88aa64fc98e63ee65846d3fc3)
  by [amogusstrikesback2](https://sketchfab.com/amogusstrikesback2), CC Attribution 4.0
- Car: to add

### clipping_stresstest

First-person walk through a level with collision. Stresses the near-plane
clipper, since walls and floors are always crossing the camera.

Credits: to add

### animation_stresstest

A skinned, animated character (stand, walk, attack on X) in a level, with a
third-person camera and collision.

Credits: to add

### sonic_city_stress

Free camera over a city level. A general draw and culling stress test.

Credits: to add

### highpoly_stresstest

Free camera over a high-poly model, to find the polygon rate limit. This one is
limited by the PVR, not the CPU. To be replaced.

Credits: to add
