"""The bolt pickup: a round slotted head on a threaded shank, in the N64
Screw.glb's footprint (base at the origin, 0.416 tall, head 0.43 across).
Run with Blender:  blender -b -P tools/make_bolt.py -- out.glb preview.png
The texture is tools/Screw.png. """
import bpy, bmesh, math, sys, os

args = sys.argv[sys.argv.index('--') + 1:]
OUT, PREVIEW = args[0], args[1] if len(args) > 1 else None
HERE = os.path.dirname(os.path.abspath(__file__))
TEX = os.path.join(HERE, 'Screw.png')

# The N64 model: node scale 0.7627 on a head y 0.388..0.545 r 0.282, shank
# r 0.105 from 0.101, a cone to 0. The same sizes here, the scale baked in.
K = 0.7627
HEAD_Y0, HEAD_Y1, HEAD_R = 0.388 * K, 0.545 * K, 0.282 * K
SHANK_R, SHANK_Y0 = 0.105 * K, 0.101 * K
CHAMFER = 0.025 * K                     # the head's top edge is eased off
SLOT_W, SLOT_D = 0.045 * K, 0.035 * K   # the screwdriver slot
THREAD_IN = 0.012 * K                   # the thread's valleys
N_HEAD, N_SHANK = 16, 8
N_THREAD = 10                           # rings down the shank, every other one a valley

bpy.ops.wm.read_factory_settings(use_empty=True)
bm = bmesh.new()
uv_layer = bm.loops.layers.uv.new('UVMap')

def ring(n, r, y):
    return [bm.verts.new((r * math.cos(2 * math.pi * i / n), y, r * math.sin(2 * math.pi * i / n))) for i in range(n)]

def quad(a, b, c, d, uvs=None, smooth=False):
    f = bm.faces.new((a, b, c, d))
    f.smooth = smooth
    if uvs:
        for l, uv in zip(f.loops, uvs): l[uv_layer].uv = uv
    return f

def tri(a, b, c, uvs=None):
    f = bm.faces.new((a, b, c))
    if uvs:
        for l, uv in zip(f.loops, uvs): l[uv_layer].uv = uv
    return f

def planar(v):                          # the top, bottom and slot: straight down onto the texture
    return ((v.co.x / HEAD_R) * 0.5 + 0.5, (v.co.z / HEAD_R) * 0.5 + 0.5)

def bridge(lo, hi, v0, v1, smooth=True, u_rep=1.0):
    n = len(lo)
    for i in range(n):
        j = (i + 1) % n
        u0, u1 = u_rep * i / n, u_rep * (i + 1) / n
        quad(lo[i], lo[j], hi[j], hi[i], [(u0, v0), (u1, v0), (u1, v1), (u0, v1)], smooth)

# ---- The head: side, chamfer, bottom ----
side_lo = ring(N_HEAD, HEAD_R, HEAD_Y0)
side_hi = ring(N_HEAD, HEAD_R, HEAD_Y1 - CHAMFER)
top_rim = ring(N_HEAD, HEAD_R - CHAMFER, HEAD_Y1)
bridge(side_lo, side_hi, 0.0, 0.5, True, 2.0)
bridge(side_hi, top_rim, 0.5, 0.6, False, 2.0)
centre_lo = bm.verts.new((0, HEAD_Y0, 0))
for i in range(N_HEAD):                 # the underside, seen as it spins
    j = (i + 1) % N_HEAD
    tri(side_lo[j], side_lo[i], centre_lo, [planar(side_lo[j]), planar(side_lo[i]), (0.5, 0.5)])

# ---- The top with the slot across it (along x): the two halves, the slot's
# walls and floor. The rim points nearest the slot are moved onto its edge
# so the slot runs right out to the chamfer. ----
def slot_edge_x(z):
    return math.sqrt(max((HEAD_R - CHAMFER) ** 2 - z * z, 0.0))

top_uv = lambda v: planar(v)
for sign in (1, -1):                    # +z half then -z half
    zs = sign * SLOT_W / 2
    # rim verts on this side, in order of angle from the slot's +x end to its -x end
    pts = [v for v in top_rim if sign * v.co.z > SLOT_W / 2]
    pts.sort(key=lambda v: math.atan2(v.co.z, v.co.x))   # +x round to -x
    # the slot lip: from +x to -x on this side, at y = top
    lip_a = bm.verts.new((slot_edge_x(zs), HEAD_Y1, zs))
    lip_b = bm.verts.new((-slot_edge_x(zs), HEAD_Y1, zs))
    # the slot floor corners under them
    flo_a = bm.verts.new((slot_edge_x(zs), HEAD_Y1 - SLOT_D, zs))
    flo_b = bm.verts.new((-slot_edge_x(zs), HEAD_Y1 - SLOT_D, zs))
    # the half disc: a fan from the lip's midpoint
    mid = bm.verts.new((0, HEAD_Y1, zs))
    chain = [lip_a] + (pts if sign > 0 else list(reversed(pts))) + [lip_b]
    for i in range(len(chain) - 1):
        a, b = chain[i], chain[i + 1]
        if sign > 0: tri(a, mid, b, [top_uv(a), top_uv(mid), top_uv(b)])
        else:        tri(b, mid, a, [top_uv(b), top_uv(mid), top_uv(a)])
    # the slot wall, facing in, darker by its uv on the texture's lower band
    if sign > 0: quad(lip_a, lip_b, flo_b, flo_a, [(0.0, 0.05), (1.0, 0.05), (1.0, 0.0), (0.0, 0.0)])
    else:        quad(lip_b, lip_a, flo_a, flo_b, [(0.0, 0.05), (1.0, 0.05), (1.0, 0.0), (0.0, 0.0)])
    if sign > 0: floor_p = (flo_a, flo_b); lips_p = (lip_a, lip_b)
    else:        floor_n = (flo_a, flo_b); lips_n = (lip_a, lip_b)
quad(floor_p[0], floor_p[1], floor_n[1], floor_n[0], [(0.0, 0.0), (1.0, 0.0), (1.0, 0.05), (0.0, 0.05)])
# the slot's ends, closed, so there is no looking into the head
quad(lips_p[0], lips_n[0], floor_n[0], floor_p[0], [(0.0, 0.05), (1.0, 0.05), (1.0, 0.0), (0.0, 0.0)])
quad(lips_n[1], lips_p[1], floor_p[1], floor_n[1], [(0.0, 0.05), (1.0, 0.05), (1.0, 0.0), (0.0, 0.0)])

# ---- The shank: rings alternating radius for the thread, then the tip ----
ys = [HEAD_Y0]
pitch = (HEAD_Y0 - SHANK_Y0) / N_THREAD
for i in range(N_THREAD): ys.append(HEAD_Y0 - pitch * (i + 1))
rings = []
for i, yy in enumerate(ys):
    r = SHANK_R - (THREAD_IN if i % 2 == 1 else 0.0)
    rings.append(ring(N_SHANK, r, yy))
for i in range(len(rings) - 1):
    v0 = 1.0 - i / (len(rings) - 1) * 2.0
    v1 = 1.0 - (i + 1) / (len(rings) - 1) * 2.0
    bridge(rings[i + 1], rings[i], v1, v0, True, 1.0)
tip = bm.verts.new((0, 0, 0))
last = rings[-1]
for i in range(N_SHANK):
    j = (i + 1) % N_SHANK
    tri(last[j], last[i], tip, [(j / N_SHANK, -1.0), (i / N_SHANK, -1.0), (0.5, -1.2)])

# Built with y up as the game has it; Blender is z up, and the exporter turns
# z up into glTF's y up, so turn it over here
import mathutils
bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=mathutils.Matrix.Rotation(math.radians(90), 3, 'X'))
bm.normal_update()
# Every face on the top must face up; the exporter marks the material
# double-sided unless culling is on, so a wrong one would show dark in the game
top = [f for f in bm.faces if all(abs(v.co.z - HEAD_Y1) < 1e-6 for v in f.verts)]
assert top and all(f.normal.z > 0.99 for f in top), [f.normal.z for f in top]
mesh = bpy.data.meshes.new('Screw')
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('Screw', mesh)
bpy.context.scene.collection.objects.link(obj)

# ---- Material: the brushed steel, lit ----
mat = bpy.data.materials.new('Screw.png')
mat.use_nodes = True
bsdf = mat.node_tree.nodes['Principled BSDF']
texn = mat.node_tree.nodes.new('ShaderNodeTexImage')
texn.image = bpy.data.images.load(TEX)
texn.interpolation = 'Closest'
mat.node_tree.links.new(texn.outputs['Color'], bsdf.inputs['Base Color'])
# Metallic 0.5 or more reflects DMS's environment image (env_gold in the
# levels); roughness under 0.25 would make it a mirror. Sheen is the rim
# light: edges turned from the camera catch this colour (Fresnel).
bsdf.inputs['Metallic'].default_value = 0.8
bsdf.inputs['Roughness'].default_value = 0.5
bsdf.inputs['Sheen Weight'].default_value = 1.0
bsdf.inputs['Sheen Tint'].default_value = (0.55, 0.6, 0.75, 1.0)
if 'Specular IOR Level' in bsdf.inputs: bsdf.inputs['Specular IOR Level'].default_value = 0.6
mat.use_backface_culling = True        # single-sided in the glb: DMS culls the backs
mesh.materials.append(mat)
print('faces', len(mesh.polygons), 'tris', sum(len(p.vertices) - 2 for p in mesh.polygons))

bpy.ops.object.select_all(action='DESELECT')
obj.select_set(True)
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=True, export_apply=True,
                          export_image_format='AUTO', export_yup=True)

# ---- A picture ----
if PREVIEW:
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE_NEXT' if hasattr(bpy.types, 'SceneEEVEE') and 'BLENDER_EEVEE_NEXT' in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items] else 'BLENDER_EEVEE'
    scene.render.resolution_x = scene.render.resolution_y = 512
    scene.render.film_transparent = False
    world = bpy.data.worlds.new('W'); scene.world = world; world.use_nodes = True
    world.node_tree.nodes['Background'].inputs[0].default_value = (0.18, 0.2, 0.24, 1)
    world.node_tree.nodes['Background'].inputs[1].default_value = 0.6
    cam = bpy.data.objects.new('Cam', bpy.data.cameras.new('Cam'))
    scene.collection.objects.link(cam); scene.camera = cam
    cam.location = (0.8, -0.8, 0.75)
    tgt = (0, 0, 0.2)
    d = mathutils.Vector(tgt) - cam.location
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    cam.data.lens = 60
    for loc, e in (((1.5, -1.0, 2.0), 500), ((-1.5, -1.0, 1.0), 150), ((0, 2.0, 0.5), 120)):
        l = bpy.data.objects.new('L', bpy.data.lights.new('L', 'POINT'))
        l.data.energy = e; l.location = loc
        scene.collection.objects.link(l)
    scene.render.filepath = PREVIEW
    bpy.ops.render.render(write_still=True)
