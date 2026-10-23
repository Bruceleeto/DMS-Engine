# Toyota: 43fps, not 60

The Toyota (police demo, `TOYOTA 1`) takes about 21.6ms a frame. 60fps needs 16.6ms, so about 5ms has to go.
Sega's Ninja draws about 20k more vertices a frame than we do at 60fps.

## Where the time goes (Oleg's GCC, 21.58ms)

| Pass | Vertices | Time | Per vertex |
|---|---|---|---|
| Lit (plain sun) | 19.8k | 9.26ms | ~400-450ns |
| Env paint (one pass) | 20.2k | 8.80ms | ~435ns |
| Reflect (second pass) | 4.0k | 2.18ms | ~540ns |
| Chip-wait stall (in the above) | | ~6ms | |

With stock GCC the frame is 22.48ms: lit 8.55, env 9.88, reflect 2.79.
The reference elf and log are in `police/old/`.

The reflect pass covers the meshes that can't take the single env pass:
- the chrome mirrors: `reflect`, `Material.001`, `reflect.001` (roughness below 0.25)
- the glossy non-metals: `glass`, `glass.001`, `lights11`
- the textured `numberplate`

## The issue

GCC won't produce the vertex loop Ninja has.

`njDirectDraw` (Ninja2.lib) is hand-written SH-4 assembly with 38 instructions a vertex. These show it isn't compiler output:
- a branch on a `dt` sitting in the delay slot of the previous `bra`
- its own register interface
- `fschg` switched in the middle of the function

It does four things GCC doesn't:
- **Loads:** paired 64-bit loads with post-increment (`fmov @r+`).
- **Stores:** each vertex is written last word first, with `@-r`.
- **Light register:** the light stays in fv12 for the whole loop.
- **Int to float:** a word goes into a float register through `lds.l @r+,fpul` / `fsts`, never through the stack.

Our C loops come out at 71-81 instructions a vertex, and none of what we tried got there:

| Tried | Result |
|---|---|
| Stock GCC | 72 instructions; the addresses are built by hand and the loads aren't paired. |
| Volatile inline asm stores | 24.19ms: every `asm volatile` is a wall GCC won't move work across. |
| Oleg's GCC (sh-elf-testing) | 21.58ms. It pairs moves only for vector types, and moves int/float words through the stack. |
| Ninja's 32-byte layout read as two vec4s, on Oleg's GCC | 23.10ms, worse, reverted. The loop was 71 instructions with 7 stack round trips a vertex; the light was copied through the stack into fv12 every vertex. |

## What would get 60

The lit and env loops written in assembly, the way Ninja's is. At Ninja's cost of about 200-250ns a vertex (an estimate, not measured), the two save about 7-9ms.

1. The plain sun lit loop in a `.S` file, first as a test with Ninja's layout: x, y, z, nx, ny, nz, uv16, command word, intensity header with 16-bit UV.
2. If it measures well, the env loop the same way.

Smaller wins that don't get there on their own:
- Env paint without the sun: about 1ms.
- Moving chrome and glass onto the single env pass: at most about 1-2ms.
