"""Production touch entry: menu passes through; the custom wrench opens the editor."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_menu_wrench_routes_without_capturing_navigation(tmp_path):
    source = (ROOT / 'android/app/src/main/java/org/defjamffny/android/VirtualControlsView.java').read_text()
    begin = source.index('    @Override\n    public boolean onTouchEvent(MotionEvent e)')
    method = source[begin:source.index('    private void syncKeys()', begin)]
    java = r'''
import java.util.*;
class MotionEvent {
    static final int ACTION_DOWN=0,ACTION_UP=1,ACTION_MOVE=2,ACTION_CANCEL=3,
                     ACTION_POINTER_DOWN=5,ACTION_POINTER_UP=6;
    int action;float x,y;
    MotionEvent(int a,float x,float y){action=a;this.x=x;this.y=y;}
    int getActionMasked(){return action;}int getActionIndex(){return 0;}
    float getX(int i){return x;}float getY(int i){return y;}
    int getPointerId(int i){return 0;}int getPointerCount(){return 1;}
}
class Parent {public boolean onTouchEvent(MotionEvent e){return false;}}
class Activity {int opened;void openControlsEditor(){++opened;}}
public class WrenchTouch extends Parent {
    static final int CONFIG=9;boolean edit,menuOnly;
    boolean[] visible=new boolean[14];Activity activity=new Activity();
    float[] knobX=new float[2],knobY=new float[2];
    Map<Integer,Integer> stickOwner=new HashMap<>();
    Map<Integer,Set<Integer>> pointerKeys=new HashMap<>(),pointerCtls=new HashMap<>();
    int evaluated,released,synced,layouts;
    void geometry(){++layouts;}
    boolean inside(int c,float x,float y){return x>=40&&x<=60&&y>=40&&y<=60;}
    boolean editTouch(MotionEvent e){return true;}
    void releaseAll(){++released;pointerKeys.clear();pointerCtls.clear();}
    void evaluate(int pid,float x,float y,Set<Integer> keys,Set<Integer> ctls){++evaluated;}
    void syncKeys(){++synced;}
''' + method + r'''
    public static void main(String[] args) {
        WrenchTouch v=new WrenchTouch();v.visible[CONFIG]=true;v.menuOnly=true;
        assert !v.onTouchEvent(new MotionEvent(MotionEvent.ACTION_DOWN,200,200));
        assert v.evaluated==0&&v.activity.opened==0; // A/B/D-pad go to view below
        assert v.onTouchEvent(new MotionEvent(MotionEvent.ACTION_DOWN,50,50));
        assert v.activity.opened==1&&v.released==1;
        v.menuOnly=false;
        assert v.onTouchEvent(new MotionEvent(MotionEvent.ACTION_DOWN,200,200));
        assert v.evaluated==1&&v.synced==1;
        assert v.onTouchEvent(new MotionEvent(MotionEvent.ACTION_POINTER_DOWN,50,50));
        assert v.activity.opened==2&&v.released==2;
        assert v.onTouchEvent(new MotionEvent(MotionEvent.ACTION_CANCEL,200,200));
        assert v.released==3&&v.layouts==5;
    }
}
'''
    path = tmp_path / 'WrenchTouch.java'
    path.write_text(java)
    compiler = ['javac'] if shutil.which('javac') else ['java', 'com.sun.tools.javac.Main']
    subprocess.run(compiler+[str(path)], check=True, capture_output=True, text=True)
    subprocess.run(['java','-ea','-cp',str(tmp_path),'WrenchTouch'],
                   check=True,capture_output=True,text=True)
