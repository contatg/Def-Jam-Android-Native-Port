"""Compiled, read-only legacy-save compatibility with synthetic metadata."""
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope='module')
def save_tool(tmp_path_factory):
    folder = tmp_path_factory.mktemp('save-compat-native')
    driver = folder / 'driver.c'
    manual = (ROOT / 'src/recomp_manual.c').read_text(encoding='utf-8')
    match = re.search(r'void sub_001F8360\(void\)\s*\{', manual)
    end, depth = match.end(), 1
    while depth:
        depth += (manual[end] == '{') - (manual[end] == '}')
        end += 1
    wrapper = manual[match.start():end]
    prelude = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "save_compat.h"
#ifdef _WIN32
#include <windows.h>
#else
typedef long LONG;
static LONG InterlockedIncrement(volatile LONG*p){return ++*p;}
#endif
static unsigned char memory[4096];
uint32_t g_esp,g_eax,g_ebx,g_esi,g_edi,g_ebp;
#define MEM32(a) (*(uint32_t*)(memory+(a)))
#define XBOX_PTR(a) (memory+(a))
'''
    driver.write_text(prelude + wrapper + r'''
static int run(const char*data,FILE*f,int use_wrapper){
    unsigned char bytes[512];uint16_t name[257];char out[13],root[1024];size_t n,i;
    n=fread(bytes,1,sizeof(bytes),f);fclose(f);
    if(n&1)return 3;
    for(i=0;i<n/2;i++)name[i]=(uint16_t)(bytes[2*i]|(bytes[2*i+1]<<8));
    name[n/2]=0;snprintf(root,sizeof(root),"%s/UserData",data);defjam_save_compat_init(root);
    if(use_wrapper){
        memset(memory,0xA5,sizeof(memory));memcpy(memory+0x100,name,(n/2+1)*2);
        g_esp=0x800;g_ebx=11;g_esi=22;g_edi=33;g_ebp=44;
        MEM32(g_esp+4)=0x100;MEM32(g_esp+8)=0x600;MEM32(g_esp+12)=13;
        sub_001F8360();
        if(g_esp!=0x810||g_ebx!=11||g_esi!=22||g_edi!=33||g_ebp!=44
           ||memory[0x5ff]!=0xA5||memory[0x60d]!=0xA5||memory[0x60c]!=0
           ||g_eax!=memory[0x600])return 4;
        printf("%s 0\n",memory+0x600);return 0;
    }
    i=defjam_save_directory_name(name,out);printf("%s %u\n",out,(unsigned)i);
    return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t**argv){
    char data[1024];FILE*f;
    if((argc!=3&&argc!=4)||!WideCharToMultiByte(CP_UTF8,0,argv[1],-1,data,sizeof(data),0,0)
       ||!(f=_wfopen(argv[2],L"rb")))return 2;
    return run(data,f,argc==4);
}
#else
int main(int argc,char**argv){
    FILE*f;if((argc!=3&&argc!=4)||!(f=fopen(argv[2],"rb")))return 2;
    return run(argv[1],f,argc==4);
}
#endif
''', encoding='utf-8')
    executable = folder / 'save_compat.exe'
    include = ROOT / 'src/hooks'
    source = include / 'save_compat.c'
    compiler = shutil.which('cl')
    if compiler:
        command = [compiler, '/nologo', '/O2', '/TC', '/I'+str(include), str(driver),
                   str(source), '/Fe:'+str(executable)]
    else:
        compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not compiler:
            pytest.skip('native C compiler unavailable')
        command = [compiler, '-O2', '-I'+str(include), str(driver), str(source), '-o', str(executable)]
    result = subprocess.run(command, cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return executable


def hashes(name):
    # Arbitrary-precision reference, independent of native fixed-width shifts.
    value = 0
    units = name.encode('utf-16-le')
    for i in range(0, len(units), 2):
        value = (value * 65536 + int.from_bytes(units[i:i+2], 'little')) % (2**48 - 59)
    correct = f'{value:012X}'
    legacy = correct[:-1] + f'{(value | (value >> 32)) & 15:X}'
    return correct, legacy


def request(tool, root, name, wrapper=False):
    input_file = root / 'request.bin'
    input_file.write_bytes(name.encode('utf-16-le'))
    command = [str(tool), str(root), str(input_file)] + (['wrapper'] if wrapper else [])
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    folder, aliased = result.stdout.strip().split()
    return folder, int(aliased)


def metadata(root, folder, name):
    path = root / 'UserData/45410049' / folder / 'SaveMeta.xbx'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\xff\xfe' + ('Name='+name+'\r\n').encode('utf-16-le'))
    return path


@pytest.mark.parametrize('name', ['VY2', 'VY3', 'Options', 'Alpha', 'Another profile', 'Zoë 🎮'])
def test_correct_hash_without_existing_folder(save_tool, tmp_path, name):
    assert request(save_tool, tmp_path, name) == (hashes(name)[0], 0)


def test_exact_legacy_metadata_preserves_existing_folder_read_only(save_tool, tmp_path):
    name = 'VY2'
    canonical, legacy = hashes(name)
    path = metadata(tmp_path, legacy, name)
    before = path.read_bytes()
    assert request(save_tool, tmp_path, name) == (legacy, 1)
    assert path.read_bytes() == before
    assert not (path.parent.parent / canonical).exists()


def test_canonical_folder_wins_when_both_exist(save_tool, tmp_path):
    canonical, legacy = hashes('Options')
    metadata(tmp_path, legacy, 'Options')
    metadata(tmp_path, canonical, 'Options')
    assert request(save_tool, tmp_path, 'Options') == (canonical, 0)


@pytest.mark.parametrize('contents', [
    b'Name=VY2\r\n', b'\xff\xfeN',
    b'\xff\xfe'+ 'Name=VY3\r\n'.encode('utf-16-le'),
    b'\xff\xfe'+ 'Other=VY2\r\n'.encode('utf-16-le'),
    b'\xff\xfe'+ 'Name=VY2suffix\r\n'.encode('utf-16-le'),
])
def test_unproven_or_colliding_legacy_folder_is_rejected(save_tool, tmp_path, contents):
    canonical, legacy = hashes('VY2')
    path = metadata(tmp_path, legacy, 'VY2')
    path.write_bytes(contents)
    assert request(save_tool, tmp_path, 'VY2') == (canonical, 0)
    assert path.read_bytes() == contents


def test_colliding_legacy_spelling_requires_exact_name(save_tool, tmp_path):
    assert hashes('VY0')[1] == hashes('VY2')[1]
    metadata(tmp_path, hashes('VY2')[1], 'VY2')
    assert request(save_tool, tmp_path, 'VY0') == (hashes('VY0')[0], 0)


def test_unicode_save_root_and_metadata_boundary(save_tool, tmp_path):
    root = tmp_path / 'données'
    root.mkdir()
    name = 'Z'*127+'é'
    canonical, legacy = hashes(name)
    assert canonical != legacy
    path = metadata(root, legacy, name)
    assert request(save_tool, root, name) == (legacy, 1)
    path.write_bytes(path.read_bytes()[:-4])  # no complete delimiter after Name
    assert request(save_tool, root, name) == (canonical, 0)


def test_manual_wrapper_stack_preserved_registers_and_output_canaries(save_tool, tmp_path):
    name = 'VY3'
    canonical, legacy = hashes(name)
    assert request(save_tool, tmp_path, name, wrapper=True) == (canonical, 0)
    metadata(tmp_path, legacy, name)
    assert request(save_tool, tmp_path, name, wrapper=True) == (legacy, 0)


@pytest.mark.parametrize('length', [127, 128, 129])
def test_metadata_length_limit(save_tool, tmp_path, length):
    name = next('A'*(length-1)+chr(c) for c in range(32, 127)
                if len(set(hashes('A'*(length-1)+chr(c)))) == 2)
    canonical, legacy = hashes(name)
    assert canonical != legacy
    metadata(tmp_path, legacy, name)
    assert request(save_tool, tmp_path, name) == ((legacy, 1) if length<=128 else (canonical, 0))
