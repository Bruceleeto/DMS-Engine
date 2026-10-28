# rail_shooter

On-rails shooter prototype inspired by `test.jpg`: a textured jet
flies through an endless generated city. Six procedural building types include
beveled, round, tapered and stepped towers, window panels, rooftop equipment
and antennas. Streets have sidewalks, curbs and lamps. Orange cubes are targets;
Windows, roof trim and lane lights use emissive materials with bloom. The
night sky follows the camera pitch; the jet has directional and rim lighting,
and street lamps and engine exhausts use coronas. B toggles bloom.
shoot them for points or dodge them to preserve five shield points. Buildings
are scenery outside the flight corridor. The rail gently curves through the
city; the camera follows your movement with a little lag and looks ahead into
turns. The jet accelerates, banks and pitches as you steer; the camera stays
level. You can fly low between buildings or
climb above the rooftops (altitude 3–48). Boost and brake ease the forward
speed, camera distance and field of view. Three green aiming frames show the
straight firing line independently of the moving camera.

| Button | Action |
|---|---|
| Stick / D-pad | Move the jet horizontally and vertically |
| A (hold) | Fire |
| R / L trigger | Boost / brake |
| Y | Restart the same generated course |
| X | Start a new generated course |
| Start | Exit |

`make` converts the assets into `pc/` and builds `build/rail_shooter/rail_shooter.elf`; `make disc` builds a self-contained `build/rail_shooter/rail_shooter.cdi`. `generate_city.py` builds the city meshes automatically, and `assets/jet/jet.glb` keeps its textures without VQ compression (`-no-vq`). The HUD shows FPS at the top right and the rendered triangle count at the left.
The course seed is shown in the HUD. `rail_shooter/main.c` holds generation,
movement, shooting and drawing; `SPEED`, `SPACING` and `SECTIONS` tune the course.
Scenery reuses seven city models and fixed pools, with recycled sections
keeping memory use and world coordinates bounded.

See the [top-level README](../../README.md) for building.
