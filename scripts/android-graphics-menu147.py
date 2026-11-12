#!/usr/bin/env python3
"""Add GRAPHICS to the original USA Options movie in the player's own dump.
No game bytes are shipped. Unrecognized movies/modified overrides are refused.
"""
import argparse,hashlib,importlib.util,os,struct,sys,uuid,shutil
from pathlib import Path

def load():
 p=Path(__file__).with_name('android-online-menu-apt.py');s=importlib.util.spec_from_file_location('apt147',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def build(apt,const):
 m=load()
 if hashlib.sha256(apt).hexdigest()!=APT_HASH or hashlib.sha256(const).hexdigest()!=CONST_HASH:raise RuntimeError('Unsupported original Options movie. No menu was modified.')
 out=bytearray(apt)
 def put(data):
  while len(out)%4:out.append(0)
  at=len(out);out.extend(data);return at
 def string(s):return put(s.encode('ascii')+b'\0')
 names={s:string(s)for s in ['oGraphics147','graphicsText','graphicsPos','GRAPHICS','PortGraphics.Open()']}
 ins=m.decode(apt,0x6434);pool=m.pool_slots(apt,ins);strings=m.const_strings(const);slots={strings[c]:i for i,c in enumerate(pool)}
 def var(a,n):a.op(m.OP_PUSHVAR,slots[n])
 def c(a,n):a.op(m.OP_PUSHCONST,slots[n])
 def text(a,n):a.op_u32(m.OP_PUSHSTRING,names[n])
 # A confirm on the new row calls the host screen, then returns to the same movie.
 a=m.Asm(0x6718);var(a,'aOptionsMenuItems');var(a,'nSelectedIndex');a.op(0x4e,m.OP_GETMEMBER,slots['sName']);text(a,'oGraphics147');a.op(m.OP_EQUALS2,m.OP_NOT);branch=a.op_u32(m.OP_IF,0);after=len(a.buf)
 m.asm_funccall_const(a,names['PortGraphics.Open()'],slots);a.op(0x76,0x3e);a.patch_u32(branch,len(a.buf)-after);a.pad_to_multiple_of_4();prefix=bytes(a.buf)
 # Create a normal CMenuItem using the original constructor and highlight style.
 a=m.Asm(0x7121);text(a,'oGraphics147');a.op(0x75);text(a,'graphicsText');var(a,'HIGHLIGHTBAR_SMALL');text(a,'graphicsPos');text(a,'oGraphics147');a.op(m.OP_PUSHBYTE,5);c(a,'CMenuItem');a.op(0x40,0x1d)
 var(a,'menu');text(a,'graphicsText');a.op(0x4e);c(a,'text');text(a,'GRAPHICS');a.op(m.OP_SETMEMBER);a.pad_to_multiple_of_4();init=bytes(a.buf)
 a=m.Asm(0x7174);text(a,'oGraphics147');a.op(0x1c);a.pad_to_multiple_of_4();array=bytes(a.buf)
 edits=[(0x6718,prefix),(0x7121,init),(0x7174,array)];fix=[]
 for point,data in edits:
  for i in m.branches_crossing(ins,point):fix.append((m.align(i.addr+1,4),len(data)))
 key=m.find_function(ins,apt,'keyX');assert key.body==(0x6718,444)
 fix.append((key.vals[4],len(prefix)))
 block=bytearray(apt[0x6434:ins[-1].nxt]);assert apt[0x7182:0x7184]==bytes([m.OP_PUSHBYTE,8]);block[0x7183-0x6434]=9
 modified=m.apply_edits(block,0x6434,fix,edits);action=put(modified)
 # The copy starts on the same alignment; all relative branches and function sizes are checked.
 newins=m.decode(out,action);m.verify_block(out,action)
 ptrs=[o+4 for o in range(0,len(apt)-8,4)if m.u32(apt,o)==1 and m.u32(apt,o+4)==0x6434];assert len(ptrs)==1
 struct.pack_into('<I',out,ptrs[0],action)
 # Clone the original EXIT text and its invisible highlight anchor into the free line.
 chars=m.u32(apt,m.u32(const,0x14)+24);sprite=m.u32(apt,chars+88*4);frames=m.u32(apt,sprite+12);record=frames+2*8;n,arr=struct.unpack_from('<II',apt,record);items=[m.u32(apt,arr+i*4)for i in range(n)]
 clones=[]
 for oldname,newname,depth,y in [('exitText','graphicsText',90,350.0),('highLightPos14','graphicsPos',91,360.7)]:
  matches=[i for i in items if m.u32(apt,i)==3 and m.u32(apt,i+4)&0x20 and m.cstr(apt,m.u32(apt,i+52))==oldname];assert len(matches)==1
  w=list(struct.unpack_from('<16I',apt,matches[0]));w[2]=depth;w[13]=names[newname];w[9]=struct.unpack('<I',struct.pack('<f',y))[0];clones.append(put(struct.pack('<16I',*w)))
 newarr=put(struct.pack('<%dI'%(n+2),*(items+clones)));struct.pack_into('<II',out,record,n+2,newarr)
 # Original prefix changes only the action pointer and this frame's pointer/count.
 allowed=set(range(ptrs[0],ptrs[0]+4))|set(range(record,record+8))
 assert all(a==b or i in allowed for i,(a,b)in enumerate(zip(apt,out)))
 return bytes(out)

APT_HASH='30a830a976820e9547b6426e3ade524c30714ade052b02807b651270a4409dc3'
CONST_HASH='7ea2f652b56e0b4a839f8dd8ce41ec7b549afdf1f9feef0ce7cc8b81b20fa922'
AFTER_HASH='933eeb943430c5a066682b7907ed8497620ad95adab9c538b8a8f5d2d30df0ff'
ENTRY='screens/options/options.big'

def prepare(data):
 m=load();source=data/'extracted/screens/screens.viv';target=data/'override/screens/screens.viv'
 if not source.is_file():raise RuntimeError('Missing extracted/screens/screens.viv')
 def patch(raw,extras=None):
  payload=m.refpack_decode(raw);files=m.inner_files(payload);a,c=files['options.apt'],files['options.const'];new=build(a,c);packed=m.rebuild_bigf(payload,{'options.apt':new});verified=m.inner_files(packed)
  assert all(verified[k]==v for k,v in files.items()if k!='options.apt');return m.refpack_encode_literal(packed),['[GRAPHICS147] original Options -> GRAPHICS appended; existing rows retained']
 # First generate from original; then only retain a known existing ONLINE override.
 plan=m.Plan(str(source),entry_name=ENTRY,patcher=patch,extra_entries=[],more=[])
 if target.exists():
  with target.open('rb') as f:
   head=f.read(16);h=struct.unpack_from('>I',head,12)[0]
   if not 16<=h<=32*1024*1024:raise RuntimeError('Invalid existing override directory')
   f.seek(0);records=m.big_records(f.read(h));found=[r for r in records if r[0]==ENTRY]
   if len(found)!=1:raise RuntimeError('Existing override Options entry missing/ambiguous')
   _,_,offset,size=found[0]
   if size>16*1024*1024:raise RuntimeError('Oversized existing menu entry')
   f.seek(offset);files=m.inner_files(m.refpack_decode(f.read(size)))
   if hashlib.sha256(files.get('options.apt',b'')).hexdigest()==AFTER_HASH and hashlib.sha256(files.get('options.const',b'')).hexdigest()==CONST_HASH:
    print('Original GRAPHICS menu already prepared.');return
  if not m.compare_override(plan,str(target)):print('Original GRAPHICS menu already prepared.');return
  online=m.Plan(str(source))
  if m.compare_override(online,str(target)):raise RuntimeError('Existing override was modified. Kept untouched; send the override for merging.')
  # Start from the validated existing ONLINE archive and append only Options.
  plan=m.Plan(str(target),entry_name=ENTRY,patcher=patch,extra_entries=[],more=[])
  backup=target.with_name(target.name+'.pre-graphics147-'+uuid.uuid4().hex[:10]);shutil.copy2(target,backup)
 m.write_override(plan,str(target));print('[GRAPHICS147] prepared original Options -> GRAPHICS from local dump; original files/saves retained')

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--data',required=True);args=p.parse_args()
 try:prepare(Path(args.data).resolve());return 0
 except Exception as e:print('Graphics menu setup failed: '+str(e),file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
