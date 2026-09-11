"""Validated legacy Doom II Win32-port saves -> AntiDoom save 2. No gameplay."""
import argparse, json, struct, hashlib, datetime, shutil, os, subprocess, tempfile
from pathlib import Path

if not __debug__:
    raise SystemExit('Run without -O: record validation must stay enabled.')

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wad',type=Path,required=True,help='The exact Doom II WAD used for these saves')
parser.add_argument('--save-dir',type=Path,default=Path('.'))
parser.add_argument('--compiler',default='gcc',help='GCC used to build the port')
parser.add_argument('--dry-run',action='store_true',help='Validate only; leave save files unchanged')
args=parser.parse_args()
ROOT=args.save_dir.resolve()
WAD=args.wad.resolve()
TOOLS=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='antidoom-save-layout-') as tmp:
    exe=Path(tmp)/'save_layout.exe'
    subprocess.run([args.compiler,'-I',str(TOOLS.parent/'linuxdoom-1.10'),str(TOOLS/'save_layout.c'),'-o',str(exe)],check=True)
    LAYOUT=json.loads(subprocess.check_output([str(exe)],text=True))
data=WAD.read_bytes()
magic,n,offset=struct.unpack_from('<4sii',data)
assert magic==b'IWAD'
lumps=[struct.unpack_from('<ii8s',data,offset+i*16) for i in range(n)]
FNV=14695981039346656037
def hash_bytes(h,b):
    for v in b: h=((h^v)*1099511628211)&0xffffffffffffffff
    return h
wad_hash=FNV
for off,size,name in lumps:
    wad_hash=hash_bytes(wad_hash,name+struct.pack('<Q',size)+data[off:off+size])
abi_names=['pointer','player_t','mobj_t','ceiling_t','vldoor_t','floormove_t','plat_t','lightflash_t','strobe_t','glow_t','NUMSTATES','NUMMOBJTYPES']
abi_hash=hash_bytes(FNV,b''.join(struct.pack('<Q',LAYOUT[k]) for k in abi_names))
def lump(name,start=0):
    idx=next(i for i in range(start,len(lumps)) if lumps[i][2].rstrip(b'\0')==name.encode())
    off,size,_=lumps[idx]
    return idx,data[off:off+size]
def field(b,t,f,fmt='<Q'): return struct.unpack_from(fmt,b,LAYOUT[t+'.'+f])[0]

def convert(src):
    raw=src.read_bytes()
    assert raw[24:40]==b'version 110\0\0\0\0\0', 'not a legacy version 110 save'
    skill,episode,mapnum=raw[40:43]
    assert skill<=4 and episode==1 and 1<=mapnum<=32
    assert list(raw[43:47])==[1,0,0,0], 'unexpected player setup'
    m,_=lump(f'MAP{mapnum:02}')
    _,sectors=lump('SECTORS',m); _,lines=lump('LINEDEFS',m)
    ns=len(sectors)//26
    lines=list(struct.iter_unpack('<7H',lines))
    pos=50
    out=bytearray(raw[:24]+b'AntiDoom save 2\0'+raw[40:50])
    assert len(out)==50
    def take(size):
        nonlocal pos
        assert pos+size<=len(raw), 'truncated record'
        b=raw[pos:pos+size];pos+=size;return b
    def record(t):
        nonlocal pos
        pos=(pos+3)&~3
        out.extend(bytes((-len(out))%8))
        b=take(LAYOUT[t]);out.extend(b);return b
    p=record('player_t')
    for j in range(2):
        state=struct.unpack_from('<Q',p,LAYOUT['player_t.psprites']+j*LAYOUT['pspdef_t'])[0]
        assert state<LAYOUT['NUMSTATES'], 'player state index'
    assert 0<=field(p,'player_t','readyweapon','<i')<9
    health=field(p,'player_t','health','<i')
    armor=field(p,'player_t','armorpoints','<i')
    # Tags and line-side record counts tie the legacy stream to this map.
    world=take(ns*14)
    for i in range(ns):
        assert struct.unpack_from('<h',world,i*14+12)[0]==struct.unpack_from('<h',sectors,i*26+24)[0], f'sector tag {i}'
    out.extend(world)
    for i,li in enumerate(lines):
        b=take(6)
        assert struct.unpack_from('<H',b,4)[0]==li[4], f'line tag {i}'
        out.extend(b)
        for side in li[5:7]:
            if side!=65535: out.extend(take(10))
    actors=0;player_objects=0;position=None
    while True:
        tag=take(1)[0];out.append(tag)
        if tag==0: break
        assert tag==1, 'unknown thinker record'
        b=record('mobj_t');actors+=1
        assert field(b,'mobj_t','state')<LAYOUT['NUMSTATES']
        assert 0<=field(b,'mobj_t','type','<i')<LAYOUT['NUMMOBJTYPES']
        player=field(b,'mobj_t','player')
        assert player in [0,1]
        if player:
            player_objects+=1
            position=[field(b,'mobj_t',f,'<i') for f in ['x','y','z']]
    assert player_objects==1, 'missing/duplicate player actor'
    classes=['ceiling_t','vldoor_t','floormove_t','plat_t','lightflash_t','strobe_t','glow_t']
    specials=0
    while True:
        tag=take(1)[0];out.append(tag)
        if tag==7: break
        assert tag<len(classes), 'unknown special record'
        t=classes[tag];b=record(t);specials+=1
        assert field(b,t,'sector')<ns, 'special sector index'
    assert take(1)==b'\x1d' and pos==len(raw), 'bad final marker/record size'
    out.append(0x1d)
    length=len(out)
    out.extend(b'ADSAVE2\0'+struct.pack('<QQQQ',wad_hash,abi_hash,length,hash_bytes(FNV,out)))
    assert len(out)==length+40
    return bytes(out),dict(file=src.name,map=f'MAP{mapnum:02}',health=health,armor=armor,
        position_fixed=position,leveltime=int.from_bytes(raw[47:50],'big'),actors=actors,specials=specials,
        original_bytes=len(raw),converted_bytes=len(out),sha256_original=hashlib.sha256(raw).hexdigest(),
        sha256_converted=hashlib.sha256(out).hexdigest())

converted=[]
for src in sorted(ROOT.glob('doomsav*.dsg')):
    if src.read_bytes()[24:40]==b'AntiDoom save 2\0':
        print('SKIP (already version 2)',src.name)
        continue
    output,info=convert(src)
    converted.append((src,output,info))
    print(json.dumps(info))
if args.dry_run or not converted:
    print('Validation complete; no save files changed.')
    raise SystemExit(0)
# No original is modified unless every slot passed full stream validation.
backup=ROOT/'save-backups'/('before-v2-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
backup.mkdir(parents=True)
for src,output,info in converted:
    shutil.copy2(src,backup/src.name)
    assert hashlib.sha256((backup/src.name).read_bytes()).hexdigest()==info['sha256_original']
    (backup/(src.name+'.v2')).write_bytes(output)
(backup/'manifest.json').write_text(json.dumps(dict(wad=str(WAD),wad_hash=f'{wad_hash:016x}',abi_hash=f'{abi_hash:016x}',saves=[i for _,_,i in converted]),indent=2))
for src,output,info in converted:
    tmp=src.with_suffix('.dsg.migrating')
    tmp.write_bytes(output)
    assert hashlib.sha256(tmp.read_bytes()).hexdigest()==info['sha256_converted']
    os.replace(tmp,src)
print('BACKUP',backup)
