"""Current Luna source art: embedded engines and a continuous, locally detailed Moon.

Dimensions are metres. The engine is 8 km tall; source Moon and controls remain intact.
The forefield samples the supplied HIGH mesh before adding local regolith detail.
"""
import json,math,struct
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'Saved/BuildAssets';OUT.mkdir(parents=True,exist_ok=True)

class Mesh:
    def __init__(self): self.v=[];self.n=[];self.uv=[];self.faces=[];self.mats=[]
    def tri(self,a,b,c,mat,uv=None):
        a,b,c=[np.asarray(p,dtype=float) for p in (a,b,c)]
        n=np.cross(b-a,c-a);n/=max(np.linalg.norm(n),1e-12)
        k=len(self.v);self.v.extend([a,b,c]);self.n.extend([n,n,n]);self.uv.extend(uv or [(0,0),(1,0),(1,1)])
        self.faces.append((k+1,k+2,k+3));self.mats.append(mat)
    def quad(self,a,b,c,d,mat,uv=None):
        uv=uv or [(0,0),(1,0),(1,1),(0,1)]
        self.tri(a,b,c,mat,[uv[i] for i in [0,1,2]]);self.tri(a,c,d,mat,[uv[i] for i in [0,2,3]])
    def beam(self,a,b,w,d,mat):
        a,b=np.array(a,float),np.array(b,float);z=(b-a)/np.linalg.norm(b-a)
        up=np.array([0,0,1] if abs(z[2])<.98 else [1,0,0]);x=np.cross(up,z);x*=w/2/np.linalg.norm(x);y=np.cross(z,x);y*=d/2/np.linalg.norm(y)
        vs=[p+i*x+j*y for p in [a,b] for i,j in [(-1,-1),(1,-1),(1,1),(-1,1)]]
        for ids in [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]: self.quad(*(vs[i] for i in ids),mat)
    def lathe(self,profile,mat,segments=96,close=False):
        pairs=list(zip(profile,profile[1:]+(profile[:1] if close else [])))
        for (r,z),(rr,zz) in pairs:
            for i in range(segments):
                a,b=2*math.pi*i/segments,2*math.pi*(i+1)/segments
                self.quad((r*math.cos(a),r*math.sin(a),z),(r*math.cos(b),r*math.sin(b),z),(rr*math.cos(b),rr*math.sin(b),zz),(rr*math.cos(a),rr*math.sin(a),zz),mat)
    def ring(self,r,z,w,h,mat): self.lathe([(r-w/2,z-h/2),(r+w/2,z-h/2),(r+w/2,z+h/2),(r-w/2,z+h/2)],mat,128,True)
    def export(self,name,scale=1,yup=True):
        v=np.asarray(self.v)*scale;n=np.asarray(self.n)
        if yup:v=v[:,[0,2,1]]*np.array([1,1,-1]);n=n[:,[0,2,1]]*np.array([1,1,-1])
        materials=list(dict.fromkeys(self.mats))
        (OUT/(name+'.mtl')).write_text('\n'.join(f'newmtl {m}\nKd 0.4 0.4 0.4' for m in materials))
        with (OUT/(name+'.obj')).open('w') as f:
            f.write(f'mtllib {name}.mtl\n')
            for p in v:f.write('v %.9f %.9f %.9f\n'%tuple(p))
            for uv in self.uv:f.write('vt %.7f %.7f\n'%tuple(uv))
            for p in n:f.write('vn %.7f %.7f %.7f\n'%tuple(p))
            last=None
            for tri,mat in zip(self.faces,self.mats):
                if mat!=last:f.write(f'usemtl {mat}\n');last=mat
                f.write('f '+' '.join(f'{i}/{i}/{i}' for i in tri)+'\n')
        return {'name':name,'vertices':len(v),'triangles':len(self.faces),'materials':materials,'min':v.min(0).tolist(),'max':v.max(0).tolist()}

engine=Mesh()
engine.lathe([(5800,0),(5800,140),(4200,380),(2800,650),(1850,1900),(1100,4300),(1700,6600),(1740,7040)],'Ceramic')
engine.lathe([(1750,6800),(2100,7380),(1950,7750),(1580,8000),(1250,8000),(1030,7300),(800,6500)],'DarkSteel')
for r,z,w,h in [(5700,120,100,120),(4100,360,180,180),(2600,700,240,240),(1640,2000,200,160),(1300,3450,220,160),(1380,4700,260,160),(1800,6500,250,200),(2080,7400,130,130),(1550,7930,160,140)]:engine.ring(r,z,w,h,'Steel')
engine.ring(1270,7970,90,60,'Light')
engine.ring(1560,7800,40,25,'Light')
def polar(r,a,z):return np.array([r*math.cos(a),r*math.sin(a),z])
for i in range(12):
    a=2*math.pi*i/12
    # Load-bearing radial ribs, separate triangular braces and deep foundation feet.
    engine.beam(polar(5200,a,140),polar(1780,a,6590),250,300,'Steel')
    engine.beam(polar(4300,a,260),polar(1280,a,4240),180,180,'DarkSteel')
    engine.beam(polar(5150,a,180),polar(2600,a,720),500,600,'Ceramic')
    for r,z in [(4900,450),(4000,2100),(3100,3900)]:
        engine.beam(polar(r,a-.025,z),polar(r-850,a+.04,z+1400),70,90,'Steel')
    # Horizontal truss members tie neighbouring ribs together.
    for r,z in [(4350,1500),(3550,3000),(2750,4500),(1950,6000)]:
        engine.beam(polar(r,a,z),polar(r,a+2*math.pi/12,z),75,110,'Steel')
        engine.beam(polar(r,a,z),polar(r-600,a+2*math.pi/12,z+1100),32,44,'Steel')
    for j in range(6):
        aa=a+(j-2.5)*.035
        engine.beam(polar(1920,aa,1850),polar(1230,aa,5000),30,70,'Steel')
        engine.beam(polar(1800,aa,6550),polar(2120,aa,7370),36,60,'Ceramic')
    # Service decks and radiator stacks create readable human-scale repetition.
    for j in range(5):
        z=1600+j*920;r=2000-j*110
        engine.beam(polar(r,a-.055,z),polar(r,a+.055,z),280,45,'Steel')
        engine.beam(polar(r+90,a-.055,z+36),polar(r+90,a+.055,z+36),10,12,'Light')
        for k in range(4):engine.beam(polar(r+70,a+(k-1.5)*.035,z+35),polar(r+70,a+(k-1.5)*.035,z+110),12,12,'Steel')
    for j in range(10):
        z=2400+j*300;r=1800-(z-2400)*.17
        engine.beam(polar(r,a-.07,z),polar(r,a+.07,z),120,30,'DarkSteel')
    for j in range(3):
        aa=a+(j-1)*.025
        engine.beam(polar(5400,aa,160),polar(5400,aa,440),100,160,'Ceramic')
        engine.beam(polar(5900+j*180,a,120),polar(6100+j*180,a,120),130,80,'Steel')
    # A second scale: small service buildings around kilometre-wide pylons.
    for j in range(5):
        aa=a+(j-2)*.018;r=6400+(j%2)*240
        engine.beam(polar(r,aa,10),polar(r,aa,70+j%3*15),60,100,'Ceramic')
    engine.beam(polar(6000,a,50),polar(6000,a,540),10,10,'Steel')
    engine.beam(polar(6000,a-.02,520),polar(6000,a+.02,520),12,12,'Steel')

# Foundations continue below the local grade, closing any gap on an uneven surface.
engine.lathe([(5800,0),(5800,-8000),(5300,-8000),(5300,0)],'Ceramic',96,True)
for i in range(12):
    a=2*math.pi*i/12
    engine.beam(polar(5200,a,-7500),polar(5200,a,200),700,900,'Steel')
report=[engine.export('SM_EngineMonument',.1)]
far=Mesh()
far.lathe([(5800,-8000),(5800,180),(2600,700),(1100,4300),(1750,6800),(2100,7400),(1580,8000)],'Ceramic',12)
for i in range(12):
    a=2*math.pi*i/12
    far.beam(polar(5200,a,140),polar(1780,a,6590),260,320,'Steel')
report.append(far.export('SM_EngineMonument_Far',.1))
raw=(ROOT/'SourceArt/Moon.glb').read_bytes()
n=struct.unpack_from('<I',raw,12)[0];doc=json.loads(raw[20:20+n]);data=raw[28+n:]
def acc(index):
    a=doc['accessors'][index];v=doc['bufferViews'][a['bufferView']];w={'SCALAR':1,'VEC2':2,'VEC3':3}[a['type']]
    return np.frombuffer(data,dtype={5126:'<f4',5125:'<u4',5123:'<u2'}[a['componentType']],count=a['count']*w,offset=v.get('byteOffset',0)+a.get('byteOffset',0)).reshape(-1,w)
pr=doc['meshes'][0]['primitives'][0]
g=acc(pr['attributes']['POSITION']).astype(float);ng=acc(pr['attributes']['NORMAL']).astype(float);uv=acc(pr['attributes']['TEXCOORD_0']).astype(float)
vertices=np.column_stack([g[:,0],-g[:,2],g[:,1]]);normals=np.column_stack([ng[:,0],-ng[:,2],ng[:,1]])
faces=acc(pr['indices']).reshape(-1,3).astype(np.int32)
p=ROOT/'Content/LunaArt/Data/thrusters.json';config=json.loads(p.read_text())
normal=np.array(config['hero']['normal']);normal/=np.linalg.norm(normal)
base=np.array(config['cinematic']['base_m']);tangent=np.cross([0,0,1],normal);tangent/=np.linalg.norm(tangent);bitangent=np.cross(normal,tangent)
local=np.column_stack([(vertices-base)@tangent,(vertices-base)@bitangent,(vertices-base)@normal])
rng=np.random.default_rng(568)
craters=[(rng.uniform(-48000,48000),rng.uniform(-48000,48000),rng.uniform(180,2400)) for _ in range(88)]
craters.extend([(24800,-900,680),(27900,1800,1100),(19200,-3600,1900),(29300,-3900,850),(22400,2200,490)])
def detail(x,y):
    x,y=np.asarray(x),np.asarray(y)
    h=12*np.sin(x/430)*np.cos(y/330)+5*np.sin(x/57+y/81)+2*np.cos(x/13-y/19)
    for cx,cy,r in craters:
        d=np.sqrt((x-cx)**2+(y-cy)**2)/r
        h+=r*(-.14*np.exp(-d**4*3.5)+.043*np.exp(-((d-.96)/.12)**2))
    edge=np.clip((42000-np.sqrt(x*x+y*y))/6000,0,1)
    edge=edge*edge*(3-2*edge)
    foundation=np.clip(np.sqrt(x*x+y*y)/7000,0,1)
    return h*edge*foundation+1.5*edge
# Spatial buckets index original triangles; the same source surface is used for every anchor.
centers=vertices[faces].mean(1);directions=centers/np.linalg.norm(centers,axis=1)[:,None]
lat=np.floor(np.degrees(np.arcsin(directions[:,2]))).astype(int)
lon=np.floor(np.degrees(np.arctan2(directions[:,1],directions[:,0]))).astype(int)
keys=(lat+90)*360+(lon+180)%360;order=np.argsort(keys);unique,starts=np.unique(keys[order],return_index=True)
buckets={int(k):order[a:b] for k,a,b in zip(unique,starts,np.r_[starts[1:],len(order)])}
def surface(direction):
    direction=np.array(direction,float);direction/=np.linalg.norm(direction)
    la=int(math.floor(math.degrees(math.asin(np.clip(direction[2],-1,1)))))
    lo=int(math.floor(math.degrees(math.atan2(direction[1],direction[0]))))
    for reach in [1,2,4]:
        ids=np.concatenate([buckets.get((yy+90)*360+(xx+180)%360,np.empty(0,dtype=int)) for yy in range(max(-90,la-reach),min(89,la+reach)+1) for xx in range(lo-reach,lo+reach+1)])
        t=vertices[faces[ids]];a,b,c=t[:,0],t[:,1],t[:,2];e1,e2=b-a,c-a;h=np.cross(direction,e2);det=np.einsum('ij,ij->i',e1,h)
        inv=np.divide(1.,det,out=np.zeros_like(det),where=np.abs(det)>1e-9);delta=-a;u=np.einsum('ij,ij->i',delta,h)*inv;q=np.cross(delta,e1);v=q@direction*inv;distance=np.einsum('ij,ij->i',e2,q)*inv
        valid=(np.abs(det)>1e-9)&(u>=-1e-7)&(v>=-1e-7)&(u+v<=1.0000001)&(distance>0)
        if valid.any():
            point=direction*distance[valid].min();d=point-base;x=float(d@tangent);y=float(d@bitangent)
            if d@normal>-5000:point+=normal*float(detail(x,y))
            return point
    raise RuntimeError(('No surface intersection',direction.tolist()))
base=surface(normal)
# One mesh: keep the global surface and refine its original triangles near the opening.
near_vertex=(np.linalg.norm(local[:,:2],axis=1)<46000)&(local[:,2]>-5000)
near=np.any(near_vertex[faces],axis=1)
N=64;weights=[];lookup={}
for i in range(N+1):
    for j in range(N+1-i):lookup[i,j]=len(weights);weights.append((1-(i+j)/N,i/N,j/N))
weights=np.array(weights);sub=[]
for i in range(N):
    for j in range(N-i):
        sub.append([lookup[i,j],lookup[i+1,j],lookup[i,j+1]])
        if j<N-i-1:sub.append([lookup[i+1,j],lookup[i+1,j+1],lookup[i,j+1]])
sub=np.array(sub,dtype=np.int32)
vs=[vertices];ns=[normals];uvs=[uv];fs=[faces[~near]];offset=len(vertices)
for face in faces[near]:
    pos=weights@vertices[face];u=weights@uv[face];norm=weights@normals[face]
    relative=pos-base;x=relative@tangent;y=relative@bitangent
    pos+=detail(x,y)[:,None]*normal
    eps=24.;dx=(detail(x+eps,y)-detail(x-eps,y))/(2*eps);dy=(detail(x,y+eps)-detail(x,y-eps))/(2*eps)
    norm-=dx[:,None]*tangent+dy[:,None]*bitangent;norm/=np.linalg.norm(norm,axis=1)[:,None]
    vs.append(pos);ns.append(norm);uvs.append(u);fs.append(sub+offset);offset+=len(pos)
v=np.concatenate(vs);nn=np.concatenate(ns);uu=np.concatenate(uvs);ff=np.concatenate(fs)
scale=100/1737400
with (OUT/'SM_MoonUnified.obj').open('w') as f:
    f.write('mtllib SM_MoonUnified.mtl\nusemtl LunarSurface\n')
    for point in v[:,[0,2,1]]*np.array([1,1,-1])*scale:f.write('v %.9f %.9f %.9f\n'%tuple(point))
    for point in uu:f.write('vt %.8f %.8f\n'%tuple(point))
    for point in nn[:,[0,2,1]]*np.array([1,1,-1]):f.write('vn %.8f %.8f %.8f\n'%tuple(point))
    for tri in ff+1:f.write('f '+' '.join(f'{i}/{i}/{i}' for i in tri)+'\n')
(OUT/'SM_MoonUnified.mtl').write_text('newmtl LunarSurface\nKd 0.5 0.5 0.5\n')
report.append({'name':'SM_MoonUnified','vertices':len(v),'triangles':len(ff),'refined_source_triangles':int(near.sum())})
# Retain local-origin boulders as geometry that naturally becomes subpixel during departure.
rocks=Mesh()
for i in range(550):
    x=rng.uniform(21500,28500);y=rng.uniform(-3700,3700)
    if (x-25000)**2+y*y<130**2:continue
    r=rng.uniform(3,23);point=surface(base+tangent*x+bitangent*y);center=point+normal*r*.2;points=[]
    for row in range(5):
        a=math.pi*row/4
        for col in range(8):
            b=2*math.pi*col/8;rad=r*(1+.13*math.sin(3*b+row));points.append(center+rad*(tangent*math.sin(a)*math.cos(b)+bitangent*math.sin(a)*math.sin(b)*.8+normal*math.cos(a)*.65)-base)
    for row in range(4):
        for col in range(8):
            a=row*8+col;b=row*8+(col+1)%8;c=a+8;d=b+8
            if row>0:rocks.tri(points[a],points[c],points[b],'Rocks')
            if row<3:rocks.tri(points[b],points[c],points[d],'Rocks')
report.append(rocks.export('SM_SurfaceRocks',.1))
original={x['id']:x['position_m'][:] for x in config['instances']}
max_shift=0
for item in config['instances']:
    point=surface(item['normal']);max_shift=max(max_shift,float(np.linalg.norm(point-np.array(item['position_m']))));item['position_m']=point.tolist()
hero=next(i for i in config['instances'] if i['id']==config['hero']['id'])
config['hero']=dict(hero);config['hero']['height_m']=8000
base=surface(normal);eye_ground=surface(base+tangent*25000);eye=eye_ground+normal*90
forward=base+normal*5100-eye;forward/=np.linalg.norm(forward);right=np.cross(forward,normal);right/=np.linalg.norm(right)
config['cinematic']={'base_m':base.tolist(),'eye_m':eye.tolist(),'look_m':(base+normal*5100-right*7500).tolist(),'eye_clearance_m':90,'hero_height_m':8000}
config['surface_detail']={'center_m':base.tolist(),'blend_inner_m':36000,'blend_outer_m':42000}
p.write_text(json.dumps(config,separators=(',',':')))
(ROOT/'Saved/BuildAssets/art.json').write_text(json.dumps({'meshes':report,'anchored_instances':len(config['instances']),'largest_anchor_correction_m':max_shift,'foundation_depth_m':8000,'continuous_surface':True},indent=2))
print(json.dumps(report))
