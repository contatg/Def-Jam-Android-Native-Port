"""Native launcher inventory and application-timing behavior, without ImGui or game data."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[2]
DRIVER=r'''
#include "pc_ui_model.h"
#include <iostream>
int main(int argc,char**argv){
    auto list=pcui::inventory(std::filesystem::u8path(argv[1]),argv[2]);
    for(const auto&p:list)std::cout<<p.name<<"|"<<p.selected<<"|"<<p.problem<<"\n";
    std::cout<<"chosen="<<pcui::selected_packs(list)<<"\n";
    std::cout<<"timing="<<pcui::aspect_next_launch(0,1)<<pcui::aspect_next_launch(1,0)
        <<pcui::aspect_next_launch(0,2)<<pcui::aspect_next_launch(2,0)
        <<pcui::aspect_next_launch(2,3)<<pcui::aspect_next_launch(3,3)<<"\n";
}
'''

@pytest.fixture(scope='module')
def inventory_tool(tmp_path_factory):
    folder=tmp_path_factory.mktemp('launcher-model')
    source=folder/'main.cpp';source.write_text(DRIVER)
    compiler=shutil.which('cl') or shutil.which('clang++') or shutil.which('g++')
    if not compiler:pytest.skip('C++ compiler required')
    exe=folder/('inventory.exe' if Path(compiler).stem.lower()=='cl' else 'inventory')
    if Path(compiler).stem.lower()=='cl':
        cmd=[compiler,'/nologo','/std:c++17','/EHsc','/I'+str(ROOT/'src/hooks'),str(source),'/Fe:'+str(exe)]
    else:cmd=[compiler,'-std=c++17','-I'+str(ROOT/'src/hooks'),str(source),'-o',str(exe)]
    result=subprocess.run(cmd,cwd=folder,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return exe

def run(tool,mods,chosen):
    result=subprocess.run([str(tool),str(mods),chosen],capture_output=True,text=True,check=True)
    return result.stdout

def pack(mods,name,manifest='[pack]\nschema = 1\n[textures]\n'):
    folder=mods/name;folder.mkdir(parents=True)
    if manifest is not None:(folder/'manifest.ini').write_text(manifest,encoding='utf-8')

def test_missing_packs_and_priority_are_preserved(inventory_tool,tmp_path):
    pack(tmp_path,'first');pack(tmp_path,'last')
    out=run(inventory_tool,tmp_path,'last;missing;first')
    assert 'chosen=last;missing;first\n' in out
    assert 'missing|1|Not installed' in out
    assert 'last|1|\n' in out

def test_inventory_adds_available_packs_without_enabling_them(inventory_tool,tmp_path):
    pack(tmp_path,'new');pack(tmp_path,'chosen')
    out=run(inventory_tool,tmp_path,'chosen')
    assert 'chosen|1|\n' in out and 'new|0|\n' in out
    assert 'chosen=chosen\n' in out

def test_invalid_names_and_unsupported_headers_are_reported(inventory_tool,tmp_path):
    pack(tmp_path,'future','[pack]\nschema=2\n');pack(tmp_path,'broken',None)
    out=run(inventory_tool,tmp_path,'../outside;future;broken')
    assert '../outside|1|Invalid pack folder name' in out
    assert 'future|1|Unsupported manifest schema' in out
    assert 'broken|1|Missing manifest.ini' in out

def test_bom_and_whitespace_headers_and_duplicate_selection(inventory_tool,tmp_path):
    pack(tmp_path,'valid','\ufeff[pack]\n  schema = 1 \n[textures]\n')
    out=run(inventory_tool,tmp_path,' valid ;valid; ;valid')
    assert 'valid|1|\n' in out and 'chosen=valid\n' in out

def test_missing_mods_directory_is_not_an_error(inventory_tool,tmp_path):
    out=run(inventory_tool,tmp_path/'absent','')
    assert out.startswith('chosen=\n')

def test_aspect_timing_distinguishes_live_shapes_from_widescreen(inventory_tool,tmp_path):
    assert 'timing=001110\n' in run(inventory_tool,tmp_path,'')

def test_duplicate_pack_list_preserves_last_override_priority(inventory_tool, tmp_path):
    pack(tmp_path, 'first')
    pack(tmp_path, 'second')
    out = run(inventory_tool, tmp_path, 'first;second;first')
    assert 'chosen=second;first\n' in out
