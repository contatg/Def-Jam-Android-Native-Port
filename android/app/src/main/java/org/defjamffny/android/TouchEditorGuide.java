package org.defjamffny.android;

import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;

final class TouchEditorGuide {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    TouchEditorGuide(float density) {
        paint.setColor(0x99e9bd65);
        paint.setStrokeWidth(density);
        paint.setPathEffect(new DashPathEffect(new float[]{5 * density, 5 * density}, 0));
    }

    void draw(Canvas canvas, float width, float height, float x, float y) {
        canvas.drawLine(0, y, width, y, paint);
        canvas.drawLine(x, 0, x, height, paint);
    }
}
