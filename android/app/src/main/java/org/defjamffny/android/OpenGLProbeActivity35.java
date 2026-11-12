package org.defjamffny.android;

import android.app.Activity;
import android.graphics.Color;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Environment;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.charset.StandardCharsets;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** A real GLES rendering probe. This is deliberately not presented as a game backend. */
public final class OpenGLProbeActivity35 extends Activity {
    private GLSurfaceView surface;private TextView status;
    private String tr(String en,String pt,String es){return UiText.isPortuguese()?pt:UiText.isSpanish()?es:en;}
    private int dp(int v){return Math.round(v*getResources().getDisplayMetrics().density);}
    @Override public void onCreate(Bundle state){super.onCreate(state);
        UiText.setLanguage(getSharedPreferences("launcher",MODE_PRIVATE).getString("ui_language","en"));
        surface=new GLSurfaceView(this);surface.setEGLContextClientVersion(2);surface.setRenderer(new Probe());
        FrameLayout content=new FrameLayout(this);content.setBackgroundColor(Color.BLACK);content.addView(surface,new FrameLayout.LayoutParams(-1,-1));
        status=new TextView(this);status.setTextColor(Color.WHITE);status.setBackgroundColor(0xdd101318);status.setTextSize(14);status.setPadding(dp(14),dp(10),dp(14),dp(10));
        status.setText(tr("OpenGL compatibility test. The game renderer is still in development.\nStarting graphics…","Teste de compatibilidade OpenGL. O render do jogo ainda está em desenvolvimento.\nIniciando os gráficos…","Prueba de compatibilidad OpenGL. El render del juego sigue en desarrollo.\nIniciando gráficos…"));
        FrameLayout.LayoutParams label=new FrameLayout.LayoutParams(-1,-2,Gravity.TOP);content.addView(status,label);
        Button back=new Button(this);back.setAllCaps(false);back.setText(tr("Back to launcher","Voltar ao launcher","Volver al launcher"));
        FrameLayout.LayoutParams button=new FrameLayout.LayoutParams(dp(185),dp(48),Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);button.bottomMargin=dp(12);content.addView(back,button);back.setOnClickListener(v->finish());setContentView(content);
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(()->{if(!finished35&&!isFinishing())status.append(tr("\nOpenGL context did not start.","\nO contexto OpenGL não iniciou.","\nEl contexto OpenGL no inició."));},5000);
    }
    private volatile boolean finished35;
    @Override protected void onPause(){if(surface!=null)surface.onPause();super.onPause();}
    @Override protected void onResume(){super.onResume();if(surface!=null)surface.onResume();}
    private void result(String details){
        finished35=true;String path="";
        try{File dir=new File(Environment.getExternalStorageDirectory(),"DefJamFFNY/logs");
            if(!dir.isDirectory()&&!dir.mkdirs()){File ext=getExternalFilesDir(null);dir=new File(ext==null?getFilesDir():ext,"logs");if(!dir.isDirectory())dir.mkdirs();}
            File file=new File(dir,"opengl-probe35.txt");try(FileOutputStream out=new FileOutputStream(file)){out.write(details.getBytes(StandardCharsets.UTF_8));}path=file.getAbsolutePath();
        }catch(Exception e){android.util.Log.w("GLPROBE35","Could not save probe",e);}
        String shown=details+"\n"+path;
        runOnUiThread(()->{if(!isFinishing()&&!isDestroyed())status.setText(tr("OpenGL TEST — game backend not implemented","TESTE OPENGL — render do jogo não implementado","PRUEBA OPENGL — render del juego no implementado")+"\n"+shown);});
    }
    private final class Probe implements GLSurfaceView.Renderer {
        final float[] vertices={-.8f,-.72f,.8f,-.72f,0f,.86f};final FloatBuffer data;
        int program,attribute;boolean valid;
        Probe(){data=ByteBuffer.allocateDirect(vertices.length*4).order(ByteOrder.nativeOrder()).asFloatBuffer();data.put(vertices).position(0);}
        private int shader(int type,String code){int s=GLES20.glCreateShader(type);if(s==0)return 0;GLES20.glShaderSource(s,code);GLES20.glCompileShader(s);int[] ok={0};GLES20.glGetShaderiv(s,GLES20.GL_COMPILE_STATUS,ok,0);if(ok[0]==0){android.util.Log.e("GLPROBE35",GLES20.glGetShaderInfoLog(s));GLES20.glDeleteShader(s);return 0;}return s;}
        @Override public void onSurfaceCreated(GL10 unused,EGLConfig config){
            int vs=shader(GLES20.GL_VERTEX_SHADER,"attribute vec2 p; varying vec2 uv; void main(){uv=p*0.5+0.5;gl_Position=vec4(p,0.0,1.0);}");
            int fs=shader(GLES20.GL_FRAGMENT_SHADER,"precision mediump float; varying vec2 uv; void main(){gl_FragColor=vec4(uv.x,uv.y,0.35,1.0);}");
            boolean framebuffer=false;
            if(vs!=0&&fs!=0){program=GLES20.glCreateProgram();GLES20.glAttachShader(program,vs);GLES20.glAttachShader(program,fs);GLES20.glLinkProgram(program);int[] ok={0};GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,ok,0);attribute=GLES20.glGetAttribLocation(program,"p");valid=ok[0]!=0&&attribute>=0;}
            if(vs!=0)GLES20.glDeleteShader(vs);if(fs!=0)GLES20.glDeleteShader(fs);
            int[] tex={0},fbo={0};GLES20.glGenTextures(1,tex,0);GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,tex[0]);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_NEAREST);GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_NEAREST);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D,0,GLES20.GL_RGBA,16,16,0,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,null);
            GLES20.glGenFramebuffers(1,fbo,0);GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER,fbo[0]);GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER,GLES20.GL_COLOR_ATTACHMENT0,GLES20.GL_TEXTURE_2D,tex[0],0);
            if(GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER)==GLES20.GL_FRAMEBUFFER_COMPLETE){
                GLES20.glViewport(0,0,16,16);GLES20.glClearColor(.25f,.5f,.75f,1f);GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);ByteBuffer px=ByteBuffer.allocateDirect(4);
                GLES20.glReadPixels(8,8,1,1,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,px);framebuffer=(px.get(0)&255)>40&&(px.get(1)&255)>100&&(px.get(2)&255)>150;
            }
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER,0);GLES20.glDeleteFramebuffers(1,fbo,0);GLES20.glDeleteTextures(1,tex,0);
            int[] max={0};GLES20.glGetIntegerv(GLES20.GL_MAX_TEXTURE_SIZE,max,0);int error=GLES20.glGetError();
            result("Vendor: "+GLES20.glGetString(GLES20.GL_VENDOR)+"\nRenderer: "+GLES20.glGetString(GLES20.GL_RENDERER)+"\nVersion: "+GLES20.glGetString(GLES20.GL_VERSION)+"\nGLSL: "+GLES20.glGetString(GLES20.GL_SHADING_LANGUAGE_VERSION)+"\nShader: "+(valid?"OK":"FAILED")+" | Framebuffer/readback: "+(framebuffer?"OK":"FAILED")+" | GL error: 0x"+Integer.toHexString(error)+"\nMax texture: "+max[0]);
        }
        @Override public void onSurfaceChanged(GL10 unused,int w,int h){GLES20.glViewport(0,0,w,h);}
        @Override public void onDrawFrame(GL10 unused){
            GLES20.glClearColor(.035f,.04f,.06f,1);GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            if(!valid)return;GLES20.glUseProgram(program);data.position(0);GLES20.glEnableVertexAttribArray(attribute);GLES20.glVertexAttribPointer(attribute,2,GLES20.GL_FLOAT,false,0,data);GLES20.glDrawArrays(GLES20.GL_TRIANGLES,0,3);GLES20.glDisableVertexAttribArray(attribute);
        }
    }
}
