from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
def test_descriptor_cache(tmp_path):
    source=(ROOT/'tools/xboxrecomp/src/d3d/d3d8_vk.c').read_text()
    a=source.index('#define FRAME_SET_SLOTS');b=source.index('\n#endif',a)
    body='#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n#include <assert.h>\n#define STAGES 4\ntypedef uintptr_t VkImageView;typedef uintptr_t VkSampler;typedef uintptr_t VkDescriptorSet;\n'+source[a:b]+r'''
int main(void){
 VkImageView v[4]={1,2,3,4};VkSampler sm[4]={4,3,2,1};
 FrameSetEntry*e=frame_set_slot(v,sm);assert(e&&!e->set);memcpy(e->views,v,sizeof v);memcpy(e->samplers,sm,sizeof sm);e->set=99;
 for(int k=0;k<1000;++k)assert(frame_set_slot(v,sm)->set==99);
 for(unsigned k=0;k<FRAME_SET_SLOTS-1;++k){v[0]=100+k;e=frame_set_slot(v,sm);assert(e&&!e->set);memcpy(e->views,v,sizeof v);memcpy(e->samplers,sm,sizeof sm);e->set=100+k;}
 v[0]=9999;assert(!frame_set_slot(v,sm));v[0]=1;assert(frame_set_slot(v,sm)->set==99);
 memset(s_frame_sets,0,sizeof s_frame_sets);assert(frame_set_slot(v,sm)&&!frame_set_slot(v,sm)->set);
}
'''
    p=tmp_path/'cache.c';p.write_text(body);o=tmp_path/'cache'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p),'-o',str(o)],check=True)
    subprocess.run([str(o)],check=True)
