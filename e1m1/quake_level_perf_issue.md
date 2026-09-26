# Quake-style level perf issue (e1m1)

## What happens

e1m1 drops to ~35fps when most of the map is in view at once (flying high,
looking across it). In normal play it holds 60fps.

## Why

Per-mesh cost, not the vertex loop.

- The glb has 134 meshes and 134 materials: every brush texture is its own
  material, and 133 of them look different, so nothing merges by material.
- The converter makes one mesh per material per block, then cuts chunks.
  127 blocks x ~17 materials each = 2236 meshes, nearly all 32 tris or fewer
  (avg ~12 tris, ~23 strip verts).
- Every mesh pays a fixed cost before it draws anything.

Measured on hardware, whole map in view (FAR_Z 1000):

| | ms | each |
|---|---|---|
| draw total | 28.4 | 2226 meshes, 50.6k verts |
| vertex loops | ~17.9 | 353ns a vertex |
| headers + matrix reload | 2.58 | 1.2us (2138 sent: neighbours all differ) |
| per-mesh sphere tests | 1.87 | 0.84us |
| unaccounted | ~6 | ~2.7us a mesh |

The unaccounted part sits between the cull and the draw (vtxbuf check, rim,
lights_at, uv scroll, the stat timers, cache misses on the mesh structs).
Not pinned down yet.

In total ~4.7us a mesh, about 13 vertices' worth, on meshes of ~23 vertices:
roughly a third of the frame is per-mesh overhead. The PVR was fine
(render ~10ms); the CPU was the limit.

## Not specific to Quake

It hits any level with many materials in small patches, all visible at once.
A level with a few materials over big surfaces gets large meshes and the
per-mesh cost disappears into the vertex work.

## What was tried / found

- FAR_Z (dms/main.h) is 1000; tried at 200, where e1m1 holds 60fps
  throughout (worst frame ~10.5ms). The converter comment already assumed 200.
- Converter knobs on e1m1 (scratch build, not committed):

  | Setting | Meshes |
  |---|---|
  | current | 2236 |
  | CHUNK_MAX_SIZE 32 | 2100 |
  | CHUNK_MIN_TRIS 64 | 1863 |
  | BLOCK_MAX_SIZE 128 | 1710 |

  Each trades culling everywhere else, so none were taken.

## Open

- Does send_header really wipe XMTRX? If not, the MVP reload after each
  header can go.
- Find the ~2.7us a mesh that is unaccounted.
- Check what the other branch did (it did not drop to 30fps): FAR_Z and the
  converter block/chunk sizes.
- Real fix for this kind of level is visibility (PVS / portals), not a faster
  loop.
- The stat timers (cull, hdr, skin) and dc_frame_stats_log() in e1m1 and
  animation main.c are still in.
