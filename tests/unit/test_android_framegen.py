"""Pure interpolation and production replay orchestration; Vulkan calls are mocks."""
from pathlib import Path
import os, shutil, subprocess
ROOT=Path(__file__).resolve().parents[2]

def compile_run(tmp_path, body):
    src=tmp_path/'fg.c';out=tmp_path/'fg';src.write_text(body)
    subprocess.run([shutil.which('cc') or 'cc','-std=gnu11','-Wall','-Wextra','-Werror',
        '-Wno-missing-field-initializers','-fsanitize=address,undefined','-g','-O1',
        '-I'+str(ROOT/'tools/xboxrecomp/src/d3d'),str(src),'-lm','-o',str(out)],
        check=True,capture_output=True,text=True)
    subprocess.run([str(out)],check=True,capture_output=True,text=True,timeout=20,
                   env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

def test_framegen_interpolation_and_fallback(tmp_path):
    compile_run(tmp_path,r'''
#include "d3d8_framegen.h"
#include <assert.h>
int main(void) {
    FgHistory h={0};float values[FG_FLOATS]={0};
    unsigned char ubo[sizeof(values)*2+32];
    assert(fg_history_alloc(&h));
    fg_history_record(&h,101,16,values);assert(!fg_history_ready(&h));
    fg_history_finish(&h,1);values[0]=0.5f;
    fg_history_record(&h,101,16,values);assert(fg_history_ready(&h));
    memset(ubo,0xa5,sizeof ubo);fg_history_write(&h,ubo,sizeof ubo,1);
    assert(((float*)(ubo+16))[0]==0.25f);assert(ubo[15]==0xa5);
    fg_history_write(&h,ubo,sizeof ubo,0);assert(!memcmp(ubo+16,values,sizeof values));
    fg_history_finish(&h,1);
    /* Cuts, reordered geometry, NaNs and mostly unmatched frames skip. */
    values[0]=99;fg_history_record(&h,101,16,values);assert(!fg_history_ready(&h));
    fg_history_finish(&h,1);values[0]=100;
    fg_history_record(&h,102,16,values);assert(!fg_history_ready(&h));
    fg_history_finish(&h,1);values[0]=NAN;
    fg_history_record(&h,102,16,values);assert(!fg_history_ready(&h));
    fg_history_finish(&h,0);assert(h.previous_n==0);
    memset(values,0,sizeof values);
    for(unsigned k=0;k<4;++k)fg_history_record(&h,k,k*sizeof values,values);
    fg_history_finish(&h,1);values[0]=.25f;
    for(unsigned k=0;k<4;++k)fg_history_record(&h,k?100+k:k,0,values);
    assert(!fg_history_ready(&h));
    fg_history_finish(&h,0);
    for(unsigned k=0;k<FG_DRAWS+1;++k)fg_history_record(&h,k,0,values);
    assert(h.failed&&!fg_history_ready(&h));
    fg_history_finish(&h,0);fg_history_free(&h);
    assert(!h.previous&&!h.current);
}
''')
