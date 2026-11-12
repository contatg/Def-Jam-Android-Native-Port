"""Production Java argument builder against a stale simulated preference cache.

Bundle/Intent/preferences are minimal value containers, not Android IPC tests.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_launch_snapshot_overrides_stale_process_preferences(tmp_path):
    code = (ROOT / 'android/app/src/main/java/org/defjamffny/android/DefJamActivity.java').read_text()
    start = code.index('    private Bundle launchSettings()')
    methods = code[start:code.index('    private void forceLandscape()', start)]
    java = r'''
import java.io.*;
import java.util.*;
class Values {
    final Map<String,Object> data=new HashMap<>();
    boolean containsKey(String k){return data.containsKey(k);}
    int getInt(String k,int d){return data.containsKey(k)?(Integer)data.get(k):d;}
    boolean getBoolean(String k,boolean d){return data.containsKey(k)?(Boolean)data.get(k):d;}
    String getString(String k,String d){return data.containsKey(k)?(String)data.get(k):d;}
}
class Bundle extends Values {}
class Intent {Bundle bundle;Bundle getBundleExtra(String k){return bundle;}}
class Info {String nativeLibraryDir="/native-libs";}
abstract class Parent {protected abstract String[] getArguments();}
public class LaunchSnapshot extends Parent {
    static final int MODE_PRIVATE=0;
    Intent intent=new Intent();Values stale=new Values();File directory;
    Intent getIntent(){return intent;}
    Values getSharedPreferences(String k,int mode){return stale;}
    File root(){return directory;}
    File getFilesDir(){return directory;}
    Info getApplicationInfo(){return new Info();}
''' + methods + r'''
    static Set<String> arguments(LaunchSnapshot s){return new HashSet<>(Arrays.asList(s.getArguments()));}
    public static void main(String[] argv) throws Exception {
        LaunchSnapshot s=new LaunchSnapshot();s.directory=new File(argv[0]);
        s.stale.data.put("guest_cores",1);s.stale.data.put("native_cores",1);
        s.stale.data.put("parallel_vertices",false);s.stale.data.put("fps_limit",165);
        s.stale.data.put("async_render",false);
        Bundle launch=new Bundle();s.intent.bundle=launch;
        launch.data.put("guest_cores",2);launch.data.put("native_cores",8);
        launch.data.put("parallel_vertices",true);launch.data.put("fps_limit",30);
        launch.data.put("async_render",true);
        Set<String> a=arguments(s);
        assert a.contains("--guest-cores=1") && a.contains("--native-cores=0");
        assert a.contains("--vertex-threads=1") && a.contains("--fps-limit=30") && a.contains("--vk-async=1");
        s.stale.data.put("guest_cores",7);assert arguments(s).contains("--guest-cores=1");
        launch.data.put("native_cores",0);launch.data.put("guest_cores",1);
        a=arguments(s);assert a.contains("--vertex-threads=1") && a.contains("--guest-cores=1");
        // Exercise every combination of legacy settings. Both fixed options
        // ignore the snapshot, while FPS/driver configuration still uses it.
        for (boolean oldAsync : new boolean[]{false,true}) {
            for (boolean oldParallel : new boolean[]{false,true}) {
                launch.data.put("async_render",oldAsync);
                launch.data.put("parallel_vertices",oldParallel);
                a=arguments(s);
                assert a.contains("--vertex-threads=1") && a.contains("--vk-async=1");
            }
        }
        File driver=new File(s.directory,"gpu_driver/current/custom.so");driver.getParentFile().mkdirs();
        assert driver.createNewFile();
        launch.data.put("use_custom_driver",true);launch.data.put("driver_library","custom.so");
        assert arguments(s).contains("--driver-lib=custom.so");assert driver.delete();
        assert !arguments(s).contains("--driver-lib=custom.so");
        launch.data.put("guest_cores",99);launch.data.put("native_cores",99);launch.data.put("fps_limit",29);
        a=arguments(s);assert a.contains("--guest-cores=1") && a.contains("--native-cores=0") && a.contains("--fps-limit=60");
        s.intent.bundle=null;
        a=arguments(s);assert a.contains("--guest-cores=1") && a.contains("--native-cores=0") && a.contains("--fps-limit=165") && a.contains("--vk-async=1");
    }
}
'''
    source = tmp_path / 'LaunchSnapshot.java'
    source.write_text(java)
    compiler = ['javac'] if shutil.which('javac') else ['java', 'com.sun.tools.javac.Main']
    subprocess.run(compiler + [str(source)], check=True, capture_output=True, text=True)
    subprocess.run(['java', '-ea', '-cp', str(tmp_path), 'LaunchSnapshot', str(tmp_path)],
                   check=True, capture_output=True, text=True)
