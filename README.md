# DMS-Engine

A 3D engine for the Sega Dreamcast, built on KallistiOS and sh4zam. Models are
converted from `.glb` to the engine's `.dms` format with the converter in
`tools/converter/`.

## Layout

| Folder | What it is |
|---|---|
| `dms/` | The engine. Projects include it as `#include "dms/dc_engine.h"` |
| `examples/` | One small demo per folder: `main.c`, `assets/`, a 1–3 line `Makefile` |
| `games/` | Full games (multi-file `src/`) built the same way |
| `host/` | Linux/SDL3 desktop preview of the engine |
| `tools/converter/` | `strippy` (`.glb` → `.dms`) and the `dcaconv` audio submodule |
| `mk/example.mk` | The shared build rules every project Makefile includes |
| `build/` | All build output, one folder per project (git-ignored) |

## Building

Load the KOS environment (`source /opt/toolchains/dc/kos/environ.sh`), then from
the repo root:

| Command | What it does |
|---|---|
| `make teapot` | Converts `examples/teapot/assets/` into `examples/teapot/pc/` and builds `build/teapot/teapot.elf` |
| `make teapot/run` | Sends the ELF with `dc-tool`, serving `pc/` as `/pc/` |
| `make teapot/disc` | Builds `build/teapot/teapot.cdi`, reading from `/cd/` with `pc/` as the disc root (needs `mkdcdisc`) |
| `make teapot/assets` | Converts assets only |
| `make teapot/clean`, `make teapot/assets-clean` | Removes `build/teapot/` or `pc/` |
| `make all` / `make list` | Builds or lists every project |
| `make host EXAMPLE=teapot` | Runs the Linux desktop preview (needs SDL3) |

The same targets work inside a project folder: `cd examples/teapot && make run`.

### Adding a project

Make `examples/<name>/` with a `main.c` and `assets/<dir>/*.glb`, and a Makefile:

```make
BAKE = 1                    # optional: bake vertex lighting (AO, shadows)
ENGINE_EXTRA = dc_audio     # optional: dc_audio, dc_particles, dc_corona
include ../../mk/example.mk
```

Asset conversion is automatic: `.glb` → `.dms`, `.png` → `.dt`, `.wav`/`.mp3` →
`.dca` (files in `assets/music/` are converted for streaming) and `.bin` is copied.
Multi-file projects set `SRCS` (see `games/botboy/Makefile`).

## Examples

| Project | |
|---|---|
| [rail_shooter](examples/rail_shooter/README.md) | On-rails shooter prototype inspired by `test.jpg`: a textured jet flies through an endless generated city. |
| [picophysics](examples/picophysics/README.md) | Rigid body physics with `dms/picophysics.h`. |
| [bowling](examples/bowling/README.md) | Ten-pin bowling with picophysics. |
| [race_track](examples/race_track/README.md) | A large racing track with a car, third-person camera and collision. |
| [1st_person](examples/1st_person/README.md) | First-person walk through a level with collision. |
| [e1m1](examples/e1m1/README.md) | Free camera through Quake's first level, The Slipgate Complex. |
| [animation](examples/animation/README.md) | A skinned, animated character (stand, walk, attack on X) in a level, with a third-person camera and collision. |
| [sonic_streets](examples/sonic_streets/README.md) | Free camera over City Escape from Sonic Adventure 2. |
| [highpoly](examples/highpoly/README.md) | Free camera over a high-poly model (about 40,000 triangles on screen), to find the polygon rate limit: about 2.3 million polygons a second. |
| [projector](examples/projector/README.md) | First-person walk through a room with a TV that shows the room live, from a camera turning on the projector (render to texture). |
| [dragonfly](examples/dragonfly/README.md) | The dragonfly from Sega's Katana SDK demo, with motion blur. |
| [shady](examples/shady/README.md) | A wooden knot turning over a marble floor, with a flat shadow thrown away from a light you can move. |
| [particles](examples/particles/README.md) | A fountain of fire over a marble floor: 600 particles thrown up from a spot, pulled back down and bouncing when they land, grey-pink as they are born, red at their height, out as they die. |
| [vase](examples/vase/README.md) | The first DMS demo, redone on the engine. |
| [scanner](examples/scanner/README.md) | A dinosaur under an x-ray scanner: where the scanner window crosses its skin, the skin goes see-through and the skeleton underneath shows. |
| [fire](examples/fire/README.md) | Four gas flames on a burner lighting a head and torso, after Fire from Sega's Katana SDK. |
| [bloom](examples/bloom/README.md) | Bloom post-processing in a mine dungeon level. |
| [cel](examples/cel/README.md) | Cel shading. |
| [arcade](examples/arcade/README.md) | An arcade cabinet with a game playing on its screen. |
| [fresnel](examples/fresnel/README.md) | A rounded block drawn as a mirror of a small environment image, turning so the reflection slides over it. |
| [teapot](examples/teapot/README.md) | A chrome teapot over a blurred photo, with bloom: the bright parts of its reflection flare. |
| [stained](examples/stained/README.md) | A lamp behind a wall shines through a stained glass window onto the floor, after Katana's stained glass sample. |
| [police](examples/police/README.md) | A police car under a sun, with a paint highlight, a flashing siren, headlights and their beams on the road. |
| [botboy](games/botboy/README.md) | BotBoy!64 from the N64brew Game Jam 2025, being rewritten on DMS as a test of the engine on a whole game: the menu room, the levels and two player split screen. |

## Blender settings

Export as `.glb`. The converter reads these from the material (Principled
BSDF):

| Setting | What it does |
|---|---|
| Base Color, and its texture | The colour and texture of the mesh |
| Alpha, or a texture with alpha | Soft alpha makes the mesh transparent, hard alpha makes it a cutout |
| Backface Culling | Off means double sided |
| Metallic, 0.5 or more | The mesh reflects the environment image |
| Roughness, under 0.25 | With Metallic: a mirror |
| Emission | With `-bake`, the material lights the things around it |

IOR and the other settings are ignored.

### Reflections

Set Metallic to 1 on the material.

- A solid material with Roughness at 0 is a mirror: it shows the environment
  image in place of its own texture, tinted by its Base Color.
- A solid material with more Roughness keeps its texture and gets a shine
  over it.
- A see-through material (glass) gets a reflection, cut out by the alpha of
  its texture.

In the example, load the image to reflect and hand it to the engine once:

```c
DCImage* environment = dc_image_load(ASSETS "environment/environment.dt");
dc_set_environment(environment);
```

`make assets` turns `assets/<dir>/<name>.png` into the `.dt`. Without the
`dc_set_environment` call, metallic materials draw as normal. See `vase`.

### Screens (render to texture)

Give the screen its own material in Blender, with any name. It needs no
texture and no UVs: a flat rectangle gets the picture stretched across it,
upright, and at full brightness. In the example, make a target, put it on the
material, then draw into it like the screen:

```c
DCTarget* tv = dc_target_create(256, 256);
dc_target_show_on(tv, world, "RTT_effect");

/* every frame */
dc_set_target(tv);                 /* what follows goes into the TV's picture */
dc_set_camera(&security_cam);
dc_draw(world, origin);
dc_set_target(NULL);               /* back to the screen */
dc_set_camera(&camera);
dc_draw(world, origin);
```

What is drawn into a target is drawn a second time, and a 256x256 target takes
256KB of VRAM.

### Trails (motion blur)

A target drawn into itself is last frame's picture, put behind what is drawn
this frame. A little see-through, it fades frame after frame:

```c
DCTarget* trail = dc_target_create(256, 256);

/* every frame */
dc_set_target(trail);
dc_set_camera(&camera);
dc_draw(ship, pos);
dc_draw_target_ex(trail, &(DCTargetOpts){ .alpha = 0.92f });   /* how much stays */
dc_set_target(NULL);
dc_set_camera(&camera);
dc_draw(ship, pos);
dc_draw_target_ex(trail, &(DCTargetOpts){ .add = true });      /* added over the screen */
```

Only what is drawn into the trail is blurred. While a trail exists the PVR's
dithering is off (with it on, the trail never fades out fully). See
`dragonfly`.

### Flat shadows

A model's shadow on a level floor, thrown away from a light:

```c
DCShadow shadow = { .light = lamp_pos, .floor_y = 0.0f };
dc_draw_ex(knot, &(DCDrawOpts){ .pos = pos, .yaw = yaw, .shadow = &shadow });
```

`.sun = true` makes `light` the way the light shines instead of where it is.
`.dark` is how dark the shadow is (0 to 1, left out it is 0.5). Where the
shadow lies over itself it does not get darker. It only looks right on flat
ground, and it costs about as much as drawing the model again: more when the
shadow fills the screen. It shows on a real Dreamcast; Flycast draws a black
square around it. See `shady`.

### Particles

Fire, smoke, sparks, dust, a fountain. Say once what they look like and how
they move, then draw them every frame:

```c
DCParticles* fire = dc_particles_create(600, &(DCParticleOpts){
    .pos    = fire_place,
    .speed  = { 0, 14, 0 }, .speed_spread = { 2.5f, 6, 2.5f },
    .gravity = 9.8f,
    .life   = 4.0f, .size = 1.2f,
    .start  = 0x998080, .middle = 0xFF2000, .end = 0x000000,
    .rate   = 150.0f, .bounce = true,
});

/* every frame */
dc_particles_draw(fire);        /* moves them and draws them */
```

Each one is a flat square that faces the camera, added over what is behind it,
so black is invisible. `.smoke` mixes them in instead, for dark smoke and
dust. Without `.texture` they get a soft round glow made in code, which is
white, so `.start`, `.middle` and `.end` colour them. `dc_particles_burst()`
throws a lot at once (an explosion), `dc_particles_move()` moves where they
come from (an emitter that follows something), `dc_particles_scale()` turns the
whole puff up and down keeping its shape (a flame on a gas tap).

The whole puff is left out when it is off screen. They cost four vertices
each, so the count is cheap; what costs is the screen they cover, and a camera
inside a cloud of big ones is the slow case. See `particles`.

### X-ray windows (modifier volumes)

One mesh of a model is made into a shape. Wherever it covers another mesh of
the same model, that mesh goes see-through, so a third mesh drawn behind it
shows through the window. All three are named by their Blender material, all
out of the one `.glb`:

```c
dc_model_volume(scene, "Scanner", "Dinosaur", "Bones");
dc_model_volume_inside(scene, 0x40, 0xC8E6FF);   /* how much skin is left
                                                    inside, and its tint */
```

Said once at load; `dc_draw()` does the rest. The shape goes to the modifier
list, the mesh it works on is drawn after everything opaque with two sets of
parameters, and the mesh it reveals needs no handling at all — the window
simply stops covering it.

It has to be asked for at startup, because the PVR's tile bins are sized before
any model is loaded, and the extra list costs about 525KB of texture RAM:

```c
dc_init((DCInitParams){ .vram_size = 2 * 1024 * 1024, .volumes = true });
```

A pixel is inside the volume when an odd number of the shape's faces lie
between it and the camera, so the shape need not be closed — a flat disc makes
a good window. A closed solid marks only the region it hides behind itself, so
a ring or a tube used as its own shape covers everything it marks with its own
near wall. `scanner` draws its ring see-through for that reason
(`dc_model_see_through`), and B takes it away altogether.

Give the shape a material of its own: a material worn by two meshes names
neither, and the wrong mesh looks exactly like a broken effect. Call
`dc_model_materials(model)` on a new `.glb` to see what there is to ask for —
it prints every mesh with its material name, counts, texture and alpha mode.
Build with `-DDMS_VOLUME_DEBUG=1` for a line a second saying what actually
reached the hardware. See `scanner`.

### Lights

One light that can move, over everything drawn from then on:

```c
dc_set_light(&(DCLight){ .pos = torch, .range = 300.0f });
```

Left out, or set to NULL, nothing changes and nothing costs: the colours baked
in Blender go out as they are. With a light set they are multiplied by how much
of it each vertex catches, so the baking stays and the light is what moves.
`.sun = true` makes `.pos` the way it shines instead of where it is (no range,
no falloff). `.r`, `.g`, `.b` colour it, `.ambient` is how lit the side facing
away is. Static models only; one with a skeleton ignores it.

It is a second, lit copy of the vertex loop. The normal is already in the
32-byte vertex, and the light is moved into the model's own space once per
draw, so no normal is ever transformed. See `fire`.

### Markers

Where an effect made in code goes, said in Blender rather than typed in as
numbers. Give a mesh a material of its own, put it where the effect belongs,
and read its corners back out:

```c
shz_vec3_t point[8];
int n = dc_model_points(scene, "MFIRE", point, 8);
```

Every mesh wearing that material stops being drawn — asking for it says it is a
marker, not something to look at. Good for a flame over a burner, a muzzle, a
spawn, the place a door swings from. `fire` uses it for its four flames.
