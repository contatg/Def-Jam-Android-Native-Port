from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
def test_dynamic_state_changes(tmp_path):
    source=tmp_path/'state.c';out=tmp_path/'state'
    source.write_text(r'''
#include "d3d8_draw_state.h"
#include <assert.h>
int main(void){
 D3D8DrawState s={0};uint32_t offsets[3]={0,256,512},stencil[3]={2,255,255};float blend[4]={1,1,1,1};
 assert(d3d8_draw_state_changes(&s,7,offsets,blend,1,stencil)==31);
 for(unsigned i=0;i<100;++i)assert(d3d8_draw_state_changes(&s,7,offsets,blend,1,stencil)==0);
 offsets[1]+=256;assert(d3d8_draw_state_changes(&s,7,offsets,blend,1,stencil)==DRAW_BIND_SET);
 assert(d3d8_draw_state_changes(&s,8,offsets,blend,1,stencil)==DRAW_BIND_SET);
 blend[3]=.5f;assert(d3d8_draw_state_changes(&s,8,offsets,blend,1,stencil)==DRAW_BLEND);
 stencil[0]++;assert(d3d8_draw_state_changes(&s,8,offsets,blend,1,stencil)==DRAW_STENCIL_REF);
 stencil[1]--;assert(d3d8_draw_state_changes(&s,8,offsets,blend,0,stencil)==0);
 assert(d3d8_draw_state_changes(&s,8,offsets,blend,1,stencil)==DRAW_STENCIL_COMPARE);
 stencil[2]--;assert(d3d8_draw_state_changes(&s,8,offsets,blend,1,stencil)==DRAW_STENCIL_WRITE);
 s.valid=0;assert(d3d8_draw_state_changes(&s,8,offsets,blend,1,stencil)==31);
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'tools/xboxrecomp/src/d3d'),str(source),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True)
