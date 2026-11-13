package org.defjamffny.android;

import android.content.Context;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/** Floating, collapsible editor. Keeps SDL's surface and the saved layout intact. */
public final class ControlsEditorPanel extends FrameLayout {
    private final VirtualControlsView view;
    private final MenuControlsEditorView menuPreview;
    private boolean menuEditing;
    private Button menuToggle,mapAction,visibleAction;
    private final LinearLayout panel;
    private TextView hint,sizeValue,opacityValue;
    private SeekBar sizeSlider,opacitySlider;
    private Button selectedControl,layoutChoice,fightStyleChoice;
    private boolean syncing;
    private int dp(float n) { return PortUi.dp(getContext(),n); }
    private LinearLayout column() {
        LinearLayout l=new LinearLayout(getContext());l.setOrientation(LinearLayout.VERTICAL);return l;
    }
    private LinearLayout buttons(View...items) {
        LinearLayout row=new LinearLayout(getContext());row.setGravity(Gravity.CENTER_VERTICAL);
        for(int i=0;i<items.length;i++) {
            LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(0,-2,1);
            if(i>0)p.leftMargin=dp(6);row.addView(items[i],p);
        }
        return row;
    }
    private void gap(LinearLayout parent,View child,int top) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(top);parent.addView(child,p);
    }
    public ControlsEditorPanel(Context context,Runnable onSaved,Runnable onCanceled) {
        super(context);
        view=new VirtualControlsView(context,null,true);
        addView(view,new FrameLayout.LayoutParams(-1,-1));
        menuPreview=new MenuControlsEditorView(context,view.menuSettings);
        addView(menuPreview,new FrameLayout.LayoutParams(-1,-1));menuPreview.setVisibility(GONE);
        panel=column();panel.setPadding(dp(14),dp(12),dp(14),dp(12));
        panel.setBackground(PortUi.shape(context,0xfa111720,PortUi.LINE,20));panel.setElevation(dp(12));
        LinearLayout header=new LinearLayout(context);header.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout title=column();
        TextView eyebrow=PortUi.text(context,"⋮⋮   CONTATG / TOUCH",9,PortUi.GOLD,true);
        eyebrow.setLetterSpacing(.12f);title.addView(eyebrow);
        TextView heading=PortUi.text(context,"Touch editor",17,PortUi.TEXT,true);
        heading.setPadding(0,dp(4),0,dp(4));title.addView(heading);
        title.addView(PortUi.text(context,"Drag this header to move the panel",10,PortUi.MUTED,false));
        header.addView(title,new LinearLayout.LayoutParams(0,-2,1));
        Button fold=PortUi.button(context,"Minimize",false);
        header.addView(fold,new LinearLayout.LayoutParams(dp(92),-2));panel.addView(header);
        LinearLayout body=column();
        body.setPadding(0,0,dp(2),dp(8));
        layoutChoice=PortUi.button(context,"Layout: Automatic",false);gap(body,layoutChoice,10);
        fightStyleChoice=PortUi.button(context,"Fight controls",false);gap(body,fightStyleChoice,6);
        selectedControl=PortUi.button(context,"Select control",false);gap(body,selectedControl,6);
        hint=PortUi.text(context,"Drag a button to move it",11,PortUi.MUTED,false);gap(body,hint,7);
        sizeSlider=new SeekBar(context);sizeSlider.setMax(170);
        opacitySlider=new SeekBar(context);opacitySlider.setMax(100);
        sizeValue=PortUi.text(context,"100%",12,PortUi.GOLD,true);
        opacityValue=PortUi.text(context,"58%",12,PortUi.GOLD,true);
        gap(body,slider("Size",sizeValue,sizeSlider),8);
        gap(body,slider("Opacity",opacityValue,opacitySlider),5);
        Button map=PortUi.button(context,"Remap button",false),visible=PortUi.button(context,"Show/hide",false);
        gap(body,buttons(map,visible),6);
        mapAction=map;visibleAction=visible;
        Button menu=PortUi.button(context,"Menu buttons",false),reset=PortUi.button(context,"Default",false);menuToggle=menu;
        gap(body,buttons(menu,reset),6);
        Button fixKeys=PortUi.button(context,UiText.tr("Restore fight buttons","Restaurar botões de luta","Restaurar botones de lucha"),false);
        gap(body,fixKeys,6);
        if(context instanceof DefJamActivity)
            gap(body,PortUi.text(context,"Pause the game before editing.",10,PortUi.MUTED,false),8);
        ScrollView scroll=new ScrollView(context);scroll.setFillViewport(false);scroll.setOverScrollMode(OVER_SCROLL_NEVER);scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(body);panel.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        LinearLayout footer=buttons(PortUi.button(context,"Cancel",false),PortUi.button(context,"Save",true));
        View divider=new View(context);divider.setBackgroundColor(PortUi.LINE);
        LinearLayout.LayoutParams dividerParams=new LinearLayout.LayoutParams(-1,dp(1));
        dividerParams.topMargin=dp(8);panel.addView(divider,dividerParams);
        gap(panel,footer,10);
        FrameLayout.LayoutParams pos=new FrameLayout.LayoutParams(dp(350),dp(320),Gravity.TOP|Gravity.CENTER_HORIZONTAL);
        pos.topMargin=dp(12);addView(panel,pos);
        // Header dragging never intercepts the canvas, sliders, or action buttons.
        title.setOnTouchListener(new OnTouchListener() {
            private int owner=-1;
            private float x,y,tx,ty;
            public boolean onTouch(View v,MotionEvent e) {
                int action=e.getActionMasked();
                if(action==MotionEvent.ACTION_DOWN) {
                    owner=e.getPointerId(0);
                    x=e.getRawX();y=e.getRawY();tx=panel.getTranslationX();ty=panel.getTranslationY();
                } else if(action==MotionEvent.ACTION_MOVE) {
                    int pointer=e.findPointerIndex(owner);
                    if(pointer>=0)positionPanel(tx+e.getRawX(pointer)-x,ty+e.getRawY(pointer)-y);
                } else if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL||
                        (action==MotionEvent.ACTION_POINTER_UP&&e.getPointerId(e.getActionIndex())==owner)){
                    owner=-1;
                }
                return true;
            }
        });
        fold.setOnClickListener(v->{
            boolean collapsed=scroll.getVisibility()==GONE;
            int visibility=collapsed?VISIBLE:GONE;
            scroll.setVisibility(visibility);footer.setVisibility(visibility);divider.setVisibility(visibility);
            fold.setText(UiText.t(collapsed?"Minimize":"Expand"));
            android.view.ViewGroup.LayoutParams p=panel.getLayoutParams();
            p.height=collapsed?panelHeight():-2;panel.setLayoutParams(p);
        });
        layoutChoice.setOnClickListener(v->PortUi.show(PortUi.dialog(context).setTitle(UiText.t("Touch layout"))
                .setSingleChoiceItems(layoutNames(),view.getLayoutMode(),(dialog,which)->{
                    view.setLayoutMode(which);dialog.dismiss();
                })));
        fightStyleChoice.setOnClickListener(v->PortUi.show(PortUi.dialog(context).setTitle(UiText.t("Fight controls"))
                .setSingleChoiceItems(fightStyleNames(),view.getFightStyle(),(dialog,which)->{
                    view.setFightStyle(which);dialog.dismiss();
                })));
        selectedControl.setOnClickListener(v->PortUi.show(PortUi.dialog(context).setTitle(UiText.t("Select control"))
                .setItems(menuEditing?MenuControlsEditorView.names():VirtualControlsView.controlNames(),(d,which)->{
                    if(menuEditing)menuPreview.selectChoice(which);else view.selectControlChoice40(which);
                })));
        map.setOnClickListener(v->view.cycleKey());visible.setOnClickListener(v->view.toggleVisible());
        fixKeys.setOnClickListener(v->PortUi.show(PortUi.dialog(context)
            .setTitle(UiText.tr("Restore button mappings?","Restaurar comandos?","¿Restaurar comandos?"))
            .setMessage(UiText.tr("Restores fight buttons and makes LT/RT visible. Positions and sizes stay unchanged; tap Save to keep it.","Restaura os comandos de luta e mostra LT/RT. Posições e tamanhos continuam iguais; toque em Salvar para guardar.","Restaura los botones de lucha y muestra LT/RT. Las posiciones y tamaños no cambian; toca Guardar."))
            .setNegativeButton(UiText.t("Cancel"),null).setPositiveButton(UiText.tr("Restore","Restaurar","Restaurar"),(d,w)->{view.restoreFightMappings35();updateHint();})));
        menu.setOnClickListener(v->{
            menuEditing=!menuEditing;view.setVisibility(menuEditing?GONE:VISIBLE);
            fixKeys.setEnabled(!menuEditing);
            menuPreview.setVisibility(menuEditing?VISIBLE:GONE);updateHint();
        });
        reset.setOnClickListener(v->PortUi.show(PortUi.dialog(context).setTitle(UiText.t("Reset controls?"))
                .setMessage(UiText.t("Restore positions, appearance and button bindings?"))
                .setNegativeButton(UiText.t("Cancel"),null).setPositiveButton(UiText.t("Default"),(d,w)->{if(menuEditing){view.menuSettings.reset();menuPreview.appearanceChanged();}else view.resetAll();})));
        footer.getChildAt(0).setOnClickListener(v->onCanceled.run());
        footer.getChildAt(1).setOnClickListener(v->{
            if(view.save(context.getSharedPreferences("launcher",Context.MODE_PRIVATE)))onSaved.run();
            else PortUi.show(PortUi.dialog(context).setMessage(UiText.t("Could not save. Please try again."))
                    .setPositiveButton(UiText.t("Close"),null));
        });
        sizeSlider.setProgressTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        opacitySlider.setProgressTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        sizeSlider.setThumbTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        opacitySlider.setThumbTintList(android.content.res.ColorStateList.valueOf(PortUi.GOLD));
        sizeSlider.setOnSeekBarChangeListener(listener(value->{if(!syncing){
            if(menuEditing){view.menuSettings.size[menuPreview.selected()]=(value+50)/100f;menuPreview.appearanceChanged();}
            else view.setSelectedSize(value+50);
        }}));
        opacitySlider.setOnSeekBarChangeListener(listener(value->{if(!syncing){
            if(menuEditing){int index=menuPreview.selected();view.menuSettings.alpha[index]=Math.max(index==MenuTouchSettings.ONLINE?.25f:0,value/100f);menuPreview.appearanceChanged();}
            else view.setSelectedOpacity(value);
        }}));
        menuPreview.setChanged(this::updateHint);view.setOnSelectionChanged(this::updateHint);view.selectControl(VirtualControlsView.SOCO);
    }
    private String[] layoutNames() { return new String[]{UiText.t("Automatic"),UiText.t("Fight"),UiText.t("Menu")}; }
    private String[] fightStyleNames(){return new String[]{UiText.t("Custom artwork"),UiText.t("Xbox layout"),UiText.t("PlayStation layout")};}
    private int panelHeight() { return Math.max(1,Math.min(dp(320),getHeight()-dp(24))); }
    private void positionPanel(float x,float y) {
        int margin=dp(6);
        panel.setTranslationX(Math.max(margin-panel.getLeft(),Math.min(getWidth()-panel.getRight()-margin,x)));
        panel.setTranslationY(Math.max(margin-panel.getTop(),Math.min(getHeight()-panel.getBottom()-margin,y)));
    }
    @Override protected void onLayout(boolean changed,int left,int top,int right,int bottom) {
        super.onLayout(changed,left,top,right,bottom);
        positionPanel(panel.getTranslationX(),panel.getTranslationY());
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh) {
        super.onSizeChanged(w,h,oldw,oldh);
        FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)panel.getLayoutParams();
        p.width=Math.max(1,Math.min(dp(350),w-dp(24)));
        if(p.height!=-2)p.height=panelHeight();panel.setLayoutParams(p);
    }
    private LinearLayout slider(String title,TextView value,SeekBar bar) {
        LinearLayout box=column();LinearLayout labels=new LinearLayout(getContext());
        TextView label=PortUi.text(getContext(),title,12,PortUi.TEXT,false);label.setSingleLine(true);
        value.setSingleLine(true);value.setGravity(Gravity.RIGHT);
        labels.addView(label,new LinearLayout.LayoutParams(0,-2,1));
        labels.addView(value,new LinearLayout.LayoutParams(-2,-2));box.addView(labels);
        LinearLayout.LayoutParams bp=new LinearLayout.LayoutParams(-1,dp(28));bp.topMargin=dp(3);box.addView(bar,bp);
        return box;
    }
    private static SeekBar.OnSeekBarChangeListener listener(java.util.function.IntConsumer changed) {
        return new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s,int p,boolean user){if(user)changed.accept(p);}
            public void onStartTrackingTouch(SeekBar s){}public void onStopTrackingTouch(SeekBar s){}
        };
    }
    private void updateHint() {
        syncing=true;
        boolean selected=menuEditing||view.hasSelection();
        sizeSlider.setEnabled(selected);opacitySlider.setEnabled(selected);
        int index=menuPreview.selected();
        int size=menuEditing?Math.round(view.menuSettings.size[index]*100):view.selectedSizePercent();
        int alpha=menuEditing?Math.round(view.menuSettings.alpha[index]*100):view.selectedOpacityPercent();
        sizeSlider.setProgress(size-50);opacitySlider.setProgress(alpha);
        sizeValue.setText(size+"%");opacityValue.setText(alpha+"%");
        String info=menuEditing?menuPreview.selectedName():view.selectedInfo();
        selectedControl.setText(info==null?UiText.t("Select control"):info+"  ▾");
        mapAction.setEnabled(!menuEditing);visibleAction.setEnabled(!menuEditing);
        fightStyleChoice.setEnabled(!menuEditing);
        fightStyleChoice.setText(UiText.t("Fight controls")+": "+fightStyleNames()[view.getFightStyle()]+"  ▾");
        menuToggle.setText(UiText.t(menuEditing?"Fight buttons":"Menu buttons"));
        hint.setText(UiText.t("Drag a button to move it"));
        layoutChoice.setText(UiText.t("Layout")+": "+layoutNames()[view.getLayoutMode()]+"  ▾");
        syncing=false;
    }
}
