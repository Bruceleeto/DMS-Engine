"""Build texture-free city meshes; make assets converts these GLBs to DMS."""
import json
import math
from pathlib import Path
import struct

OUT = Path(__file__).resolve().parent / 'assets' / 'city'


class Mesh:
    def __init__(self):
        self.positions, self.normals, self.colours = [], [], []
        self.groups = [[], []]

    def face(self, points, colour, lit=False):
        a, b, c = points[:3]
        u = [b[i] - a[i] for i in range(3)]
        v = [c[i] - a[i] for i in range(3)]
        n = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]
        length = math.sqrt(sum(x*x for x in n))
        if length < 1e-10:
            return
        n = [x / length for x in n]
        shade = 1 if lit else 0.65 + 0.35 * max(0, n[1]*0.8 - n[0]*0.4 + n[2]*0.3)
        rgb = [((colour >> shift) & 255) / 255 * shade for shift in (16, 8, 0)]
        for i in range(1, len(points)-1):
            for p in (points[0], points[i], points[i+1]):
                self.groups[int(lit)].append(len(self.positions))
                self.positions.append(p)
                self.normals.append(n)
                self.colours.append(rgb + [1])

    def tower(self, ring, bottom, top, colour, taper=1, windows=0, lit=False):
        for i, a in enumerate(ring):
            b = ring[(i+1) % len(ring)]
            self.face([(a[0], bottom, a[1]), (a[0]*taper, top, a[1]*taper),
                       (b[0]*taper, top, b[1]*taper), (b[0], bottom, b[1])], colour, lit)
            # Actual recessed-looking window panels, offset from the wall.
            if windows:
                dx, dz = b[0]-a[0], b[1]-a[1]
                length = math.hypot(dx, dz)
                ox, oz = dz/length*0.002, -dx/length*0.002
                for row in range(windows):
                    for col in range(2):
                        pts = []
                        for t, h in ((0.12+col*0.46, 0.18), (0.12+col*0.46, 0.70),
                                     (0.42+col*0.46, 0.70), (0.42+col*0.46, 0.18)):
                            f = (row+h)/windows
                            scale = 1 + (taper-1)*f
                            pts.append(((a[0]+dx*t)*scale+ox, bottom+(top-bottom)*f,
                                        (a[1]+dz*t)*scale+oz))
                        glow = (0x55BDD1, 0xD8A564, 0x182B40)[(i+row*5+col*7) % 3]
                        self.face(pts, glow, glow != 0x182B40)
        self.face([(x*taper, top, z*taper) for x, z in reversed(ring)], colour, lit)

    def box(self, x, y, z, w, h, d, colour, lit=False):
        ring = [(x-w/2,z-d/2),(x+w/2,z-d/2),(x+w/2,z+d/2),(x-w/2,z+d/2)]
        self.tower(ring, y, y+h, colour, lit=lit)

    def save(self, name):
        binary = bytearray()
        views, accessors = [], []
        for values, kind in ((self.positions,'VEC3'), (self.normals,'VEC3'), (self.colours,'VEC4')):
            flat = [v for p in values for v in p]
            data = struct.pack('<'+'f'*len(flat), *flat)
            views.append({'buffer':0, 'byteOffset':len(binary), 'byteLength':len(data)})
            binary.extend(data)
            acc = {'bufferView':len(views)-1, 'componentType':5126, 'count':len(values), 'type':kind}
            if len(accessors) == 0:
                acc.update(min=[min(p[i] for p in values) for i in range(3)],
                           max=[max(p[i] for p in values) for i in range(3)])
            accessors.append(acc)
        primitives = []
        for material, group in enumerate(self.groups):
            if not group:
                continue
            indices = struct.pack('<'+'I'*len(group), *group)
            views.append({'buffer':0, 'byteOffset':len(binary), 'byteLength':len(indices)})
            binary.extend(indices)
            accessors.append({'bufferView':len(views)-1, 'componentType':5125,
                              'count':len(group), 'type':'SCALAR'})
            primitives.append({'attributes':{'POSITION':0,'NORMAL':1,'COLOR_0':2},
                               'indices':len(accessors)-1,'material':material})
        doc = {'asset':{'version':'2.0','generator':'DMS procedural city'},
               'scene':0,'scenes':[{'nodes':[0]}], 'nodes':[{'mesh':0}],
               'meshes':[{'primitives':primitives}],
               'materials':[{'name':name,'doubleSided':True,
                   'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'metallicFactor':0}},
                   {'name':name+'_lights','doubleSided':True,'emissiveFactor':[1,1,1],
                    'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'metallicFactor':0}}],
               'buffers':[{'byteLength':len(binary)}], 'bufferViews':views, 'accessors':accessors}
        js = json.dumps(doc,separators=(',',':')).encode()
        js += b' ' * (-len(js) % 4)
        OUT.mkdir(parents=True, exist_ok=True)
        with (OUT / (name+'.glb')).open('wb') as f:
            f.write(struct.pack('<III',0x46546C67,2,28+len(js)+len(binary)))
            f.write(struct.pack('<II',len(js),0x4E4F534A)); f.write(js)
            f.write(struct.pack('<II',len(binary),0x004E4942)); f.write(binary)
        print(name, len(self.positions)//3, 'triangles')


def ring(n, radius=0.5):
    return [(radius*math.cos(i*2*math.pi/n),radius*math.sin(i*2*math.pi/n)) for i in range(n)]


def build_tower(style):
    m = Mesh()
    bevel = [(-.35,-.5),(.35,-.5),(.5,-.35),(.5,.35),(.35,.5),(-.35,.5),(-.5,.35),(-.5,-.35)]
    outline = ring(12) if style in (1,4) else bevel
    colour = (0x253B57,0x244755,0x39394D,0x29464E,0x293D5A,0x43414E)[style]
    m.box(0,0,0,1,.07,1,0x475568)
    if style in (2,5):
        for level, (width, low, high) in enumerate(((1,.07,.45),(.78,.45,.76),(.55,.76,1))):
            shape = [(x*width,z*width) for x,z in outline]
            m.tower(shape,low,high,colour,windows=2)
            m.tower(shape,high,high+.008,0x379BAE,lit=True)
    else:
        taper = .60 if style == 4 else 1
        m.tower(outline,.07,.94,colour,taper,windows=6)
        top = [(x*taper,z*taper) for x,z in outline]
        m.tower(top,.94,1,0x456879,.75)
        m.tower(top,.94,.948,0x3EA4B4,lit=True)
        if style == 3:
            # Sloped crown and a narrow antenna.
            m.tower([(x*.75,z*.75) for x,z in outline],1,1.18,0x527A8B,.08)
    m.box(-.14,1,.05,.22,.055,.25,0x80909C)
    m.box(.16,1,-.08,.16,.035,.18,0x65747E)
    if style in (0,1,3):
        m.tower(ring(6,.018),1.04,1.24,0xA0B9C0)
        m.tower(ring(6,.035),1.24,1.26,0xFF7963,lit=True)
    m.save('tower'+str(style))


for style in range(6):
    build_tower(style)

m = Mesh()
m.box(0,-.5,0,100,.4,28,0x172132)
for side in (-1,1):
    m.box(side*14,-.1,0,2,.35,28,0x596374)
    m.box(side*12.85,0,0,.18,.38,28,0x91A6AE)
    for z in (-8,8):
        m.box(side*14,.25,z,.24,4.2,.24,0x889BA4)
        m.box(side*13.4,4.45,z,1.5,.14,.3,0x8EABB4)
        m.box(side*12.8,4.3,z,.5,.16,.55,0xFFE3A0,lit=True)
for z in (-9,0,9):
    m.box(0,-.085,z,.20,.02,4,0xB3B783)
# Long inset blue lane lights, split into short sections.
for side in (-1,1):
    for z in (-9,0,9):
        m.box(side*11.8,-.075,z,.10,.025,5,0x327D92,lit=True)
m.save('street')
