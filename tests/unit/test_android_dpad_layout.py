"""Compile and execute the production D-pad geometry without Android SDK stubs."""
from pathlib import Path
import shutil
import subprocess
import pytest
ROOT = Path(__file__).resolve().parents[2]


def test_grouped_dpad_geometry(tmp_path):
    java = shutil.which('java')
    if not java:
        pytest.skip('Java required')
    modules = subprocess.run([java, '--list-modules'], capture_output=True, text=True)
    if 'jdk.compiler@' not in modules.stdout:
        pytest.skip('Java compiler module required')
    harness = tmp_path / 'DpadCheck.java'
    harness.write_text('''package org.defjamffny.android;
public class DpadCheck {
 static void near(float a,float b) { if(Math.abs(a-b)>.02f)throw new AssertionError(a+" != "+b); }
 static void check(boolean b) { if(!b)throw new AssertionError(); }
 public static void main(String[] args) {
  DpadLayout d=new DpadLayout(); d.x=.3f;d.y=.7f;d.layout(1000,500,40);
  near(d.cx[0],d.cx[1]); near(d.cy[2],d.cy[3]);
  near(d.cx[3]-d.cx[0],d.cy[1]-d.cy[2]);
  near(d.cx[0]-d.cx[2],d.cy[2]-d.cy[0]);
  for(int i=0;i<4;i++)check(d.directionsAt(d.cx[i],d.cy[i])==(1<<i));
  check(d.directionsAt(d.cx[0],d.cy[2])==0);
  check(d.directionsAt(d.right,d.top)==(1|8));
  check(d.directionsAt(d.left,d.bottom)==(2|4));
  check(d.directionsAt(-1,-1)==0);
  float oldX=d.cx[2],oldY=d.cy[0];
  d.x+=.1f;d.y-=.1f;d.layout(1000,500,40);
  near(d.cx[2]-oldX,100);near(d.cy[0]-oldY,-50);
  float gap=d.cx[3]-d.cx[2];
  d.scale=2;d.layout(1000,500,40);near(d.cx[3]-d.cx[2],gap*2);
  d.x=0;d.y=1;d.layout(1000,500,40);
  check(d.left>=0 && d.top>=0 && d.right<=1000 && d.bottom<=500.01f);
  near(d.cx[0]-d.cx[2],d.cy[2]-d.cy[0]);
  d.visible=false;check(d.directionsAt(d.cx[0],d.cy[0])==0);
  d.visible=true;d.scale=99;d.layout(90,50,100);
  check(d.left>=0 && d.top>=0 && d.right<=90 && d.bottom<=50.01f);
  d.x=Float.NaN;d.y=Float.POSITIVE_INFINITY;d.scale=Float.NaN;d.layout(1000,500,40);
  check(Float.isFinite(d.x)&&Float.isFinite(d.y)&&Float.isFinite(d.scale));
 }
}''')
    source=ROOT/'android/app/src/main/java/org/defjamffny/android/DpadLayout.java'
    subprocess.run([java,'--module','jdk.compiler/com.sun.tools.javac.Main','-d',str(tmp_path),str(source),str(harness)],check=True,capture_output=True,text=True)
    subprocess.run([java,'-cp',str(tmp_path),'org.defjamffny.android.DpadCheck'],check=True,capture_output=True,text=True)
