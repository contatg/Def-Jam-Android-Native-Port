// SPDX-License-Identifier: GPL-3.0-or-later
/* Adapted from the GPL-3.0-or-later Armageddon hakuX frontend.
 * See assets/hakux36/licenses/hakuX-COPYING.txt. */
package org.defjamffny.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.SparseIntArray;
import android.view.MotionEvent;
import android.view.View;
import com.rfandango.haku_x.NativeCoreActivity;

final class HakuXControls36 extends View {
    private static final int DIRECTION = 0;
    private static final int MENU = 11;
    private static final int RIGHT_STICK = 12;
    private static final int GOLD = 0xffe9b44b;
    private static final int IVORY = 0xfff2f5fa;
    private static final int[] DIGITAL = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x10, 0};
    private static final int[] ANALOG = {0, 4, 8, 1, 2, 32, 16, 0, 0, 0, 0, 0};
    private static final int[] DIRECTIONS = {1, 8, 2, 4};
    private static final int LT = 7;
    private static final int RT = 8;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path shape = new Path();
    private final SparseIntArray fingers = new SparseIntArray();
    private final RectF[] bounds = new RectF[RIGHT_STICK + 1];
    private final Runnable openMenu;
    private final String[] labels = {
        "", "X", "Y", "A", "B", "LB", "RB", "LT", "RT", "BACK", "START", "☰"
    };
    private int held;
    private int dpad;
    private float axisX;
    private float axisY;
    private float rightX, rightY;
    private float unit;
    private boolean shown = true;
    private boolean stick;
    private boolean rightVisible=true;
    private int opacity = 0;
    private int sentButtons;
    private int sentAnalog;
    private int sentTriggers;
    private float sentX;
    private float sentY;
    private float sentRightX,sentRightY;

    HakuXControls36(Context context, Runnable openMenu) {
        super(context);
        this.openMenu = openMenu;
        for (int i = 0; i < bounds.length; i++) bounds[i] = new RectF();
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
    }

    void setShown(boolean value) {
        shown = value;
        reset();
    }

    boolean controlsVisible() {
        return shown;
    }

    void setStick(boolean value) {
        stick = value;
        reset();
    }

    boolean isStick() {
        return stick;
    }

    void setRightStick(boolean value){rightVisible=value;reset();}
    boolean isRightStickVisible(){return rightVisible;}

    void setOpacity(int value) {
        opacity = value;
        invalidate();
    }

    void reset() {
        fingers.clear();
        held = 0;
        dpad = 0;
        axisX = axisY = rightX = rightY = 0;
        publish();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        unit = Math.min(h / 9.5f, w / 16f);
        float u = unit;
        float attackX = w - 2.45f * u;
        float attackY = h - 2.75f * u;
        setBounds(DIRECTION, 2.45f * u, attackY, 1.72f * u, 1.72f * u);
        setBounds(1, attackX - 1.28f * u, attackY, .66f * u, .66f * u);
        setBounds(2, attackX, attackY - 1.28f * u, .66f * u, .66f * u);
        setBounds(3, attackX, attackY + 1.28f * u, .66f * u, .66f * u);
        setBounds(4, attackX + 1.28f * u, attackY, .66f * u, .66f * u);
        setBounds(5, 1.72f * u, 1.65f * u, 1.15f * u, .42f * u);
        setBounds(6, w - 1.72f * u, 1.65f * u, 1.15f * u, .42f * u);
        setBounds(LT, 1.72f * u, .65f * u, 1.15f * u, .38f * u);
        setBounds(RT, w - 1.72f * u, .65f * u, 1.15f * u, .38f * u);
        setBounds(9, w / 2f - 1.10f * u, h - .67f * u, .93f * u, .34f * u);
        setBounds(10, w / 2f + 1.10f * u, h - .67f * u, .93f * u, .34f * u);
        setBounds(MENU, w / 2f, .66f * u, .45f * u, .42f * u);
        setBounds(RIGHT_STICK, w * .64f, h - 3.0f * u, .90f * u, .90f * u);
        reset();
    }

    private void setBounds(int id, float x, float y, float rx, float ry) {
        bounds[id].set(x - rx, y - ry, x + rx, y + ry);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (shown) {
            drawDirection(canvas);
            for (int id = 1; id < MENU; id++) {
                drawButton(canvas, id);
            }
            if(rightVisible)drawRightStick(canvas);
        }
        drawButton(canvas, MENU);
    }

    private void drawRightStick(Canvas c){
        RectF b=bounds[RIGHT_STICK];float x=b.centerX(),y=b.centerY(),r=b.width()/2;
        fill(false);c.drawCircle(x,y,r,paint);stroke();c.drawCircle(x,y,r,paint);
        fill((held&(1<<RIGHT_STICK))!=0);c.drawCircle(x+rightX*r*.55f,y+rightY*r*.55f,r*.38f,paint);
        stroke();c.drawCircle(x+rightX*r*.55f,y+rightY*r*.55f,r*.38f,paint);
    }

    private void drawDirection(Canvas canvas) {
        RectF box = bounds[DIRECTION];
        float x = box.centerX();
        float y = box.centerY();
        float u = unit;
        if (stick) {
            fill(false);
            canvas.drawCircle(x, y, u * 1.54f, paint);
            stroke();
            canvas.drawCircle(x, y, u * 1.54f, paint);
            fill((held & 1) != 0);
            canvas.drawCircle(x + axisX * u, y + axisY * u, u * .55f, paint);
            stroke();
            canvas.drawCircle(x + axisX * u, y + axisY * u, u * .55f, paint);
            return;
        }
        for (int i = 0; i < 4; i++) {
            canvas.save();
            canvas.rotate(i * 90f, x, y);
            shape.reset();
            shape.moveTo(x, y - 1.58f * u);
            shape.lineTo(x + .54f * u, y - 1.04f * u);
            shape.lineTo(x + .38f * u, y - .42f * u);
            shape.lineTo(x - .38f * u, y - .42f * u);
            shape.lineTo(x - .54f * u, y - 1.04f * u);
            shape.close();
            fill((dpad & DIRECTIONS[i]) != 0);
            canvas.drawPath(shape, paint);
            stroke();
            canvas.drawPath(shape, paint);
            canvas.restore();
        }
    }

    private void drawButton(Canvas canvas, int id) {
        RectF b = bounds[id];
        boolean diamond = id <= 4 || id == MENU;
        if (diamond) {
            float inset = b.width() * .17f;
            shape.reset();
            shape.moveTo(b.centerX(), b.top);
            shape.lineTo(b.right - inset, b.top + inset);
            shape.lineTo(b.right, b.centerY());
            shape.lineTo(b.right - inset, b.bottom - inset);
            shape.lineTo(b.centerX(), b.bottom);
            shape.lineTo(b.left + inset, b.bottom - inset);
            shape.lineTo(b.left, b.centerY());
            shape.lineTo(b.left + inset, b.top + inset);
            shape.close();
        }
        fill((held & (1 << id)) != 0);
        if (diamond) canvas.drawPath(shape, paint);
        else canvas.drawRoundRect(b, unit * .20f, unit * .20f, paint);
        stroke();
        if (diamond) canvas.drawPath(shape, paint);
        else canvas.drawRoundRect(b, unit * .20f, unit * .20f, paint);

        paint.setStyle(Paint.Style.FILL);
        paint.setColor(IVORY);
        paint.setAlpha(opacity == 0 ? 205 : 255);
        paint.setTypeface(Typeface.DEFAULT_BOLD);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(unit * (id <= 4 ? .32f : .26f));
        canvas.drawText(labels[id], b.centerX(), b.centerY() - (paint.ascent() + paint.descent()) / 2f, paint);
    }

    private void fill(boolean pressed) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(pressed ? 0xff715128 : 0xff11161d);
        paint.setAlpha(pressed ? 224 : opacity == 0 ? 132 : 214);
    }

    private void stroke() {
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(GOLD);
        paint.setAlpha(opacity == 0 ? 173 : 240);
        paint.setStrokeWidth(Math.max(1f, unit * .027f));
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        if (action == MotionEvent.ACTION_CANCEL) {
            reset();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int target = targetAt(event.getX(index), event.getY(index));
            if (target >= 0) fingers.put(event.getPointerId(index), target);
        }
        boolean tappedMenu = false;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int pointer = event.getPointerId(index);
            tappedMenu = fingers.get(pointer, -1) == MENU
                    && targetAt(event.getX(index), event.getY(index)) == MENU;
            fingers.delete(pointer);
        }
        int before = held;
        int previousDpad = dpad;
        float previousX = axisX;
        float previousY = axisY;
        float previousRightX=rightX,previousRightY=rightY;
        held = dpad = 0;
        axisX = axisY = rightX = rightY = 0;
        for (int i = 0; i < fingers.size(); i++) {
            int id = fingers.valueAt(i);
            int at = event.findPointerIndex(fingers.keyAt(i));
            if (at < 0) continue;
            if (id == DIRECTION && (held & 1) == 0) {
                direction(event.getX(at), event.getY(at));
            }
            if(id==RIGHT_STICK&&(held&(1<<RIGHT_STICK))==0){
                float r=bounds[RIGHT_STICK].width()/2f;
                float dx=(event.getX(at)-bounds[RIGHT_STICK].centerX())/r;
                float dy=(event.getY(at)-bounds[RIGHT_STICK].centerY())/r;
                float distance=(float)Math.hypot(dx,dy);
                if(distance>.17f){float scale=Math.min(1f,(distance-.17f)/.83f)/distance;rightX=dx*scale;rightY=dy*scale;}
            }
            held |= 1 << id;
        }
        if (held != before || dpad != previousDpad || axisX != previousX || axisY != previousY || rightX!=previousRightX || rightY!=previousRightY) {
            publish();
            postInvalidateOnAnimation();
        }
        if (tappedMenu) {
            performClick();
            openMenu.run();
        }
        return true;
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    private int targetAt(float x, float y) {
        if (bounds[MENU].contains(x, y)) return MENU;
        if (!shown) return -1;
        if(rightVisible&&bounds[RIGHT_STICK].contains(x,y))return RIGHT_STICK;
        for (int id = 0; id < MENU; id++) {
            if (bounds[id].contains(x, y)) return id;
        }
        return -1;
    }

    private void direction(float x, float y) {
        float dx = (x - bounds[DIRECTION].centerX()) / unit;
        float dy = (y - bounds[DIRECTION].centerY()) / unit;
        if (stick) {
            float distance = (float) Math.hypot(dx, dy);
            if (distance > .17f) {
                float scale = Math.min(1f, (distance - .17f) / .83f) / distance;
                axisX = dx * scale;
                axisY = dy * scale;
            }
        } else {
            if (Math.abs(dx) > .24f && Math.abs(dx) >= Math.abs(dy) * .48f) dpad |= dx < 0 ? 4 : 8;
            if (Math.abs(dy) > .24f && Math.abs(dy) >= Math.abs(dx) * .48f) dpad |= dy < 0 ? 1 : 2;
        }
    }

    private void publish() {
        int buttons = dpad;
        int analog = 0;
        for (int id = 1; id < MENU; id++) {
            if ((held & (1 << id)) != 0) {
                buttons |= DIGITAL[id];
                analog |= ANALOG[id];
            }
        }
        int triggers = (held >> LT) & 3;
        if (buttons == sentButtons && analog == sentAnalog && triggers == sentTriggers
                && axisX == sentX && axisY == sentY && rightX==sentRightX && rightY==sentRightY) return;
        sentButtons = buttons;
        sentAnalog = analog;
        sentTriggers = triggers;
        sentX = axisX;
        sentY = axisY;
        sentRightX=rightX;sentRightY=rightY;
        NativeCoreActivity.nativeSetOverlayPad(buttons, analog, axisX, axisY, rightX, rightY,
                triggers & 1, (triggers >> 1) & 1);
    }
}
