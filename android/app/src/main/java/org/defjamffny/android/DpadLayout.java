package org.defjamffny.android;

/** Pure geometry for one grouped D-pad, independent of Android rendering. */
final class DpadLayout {
    float x = .15f, y = .75f, scale = 1f;
    boolean visible = true, positioned;
    final float[] cx = new float[4], cy = new float[4];
    float cell, left, top, right, bottom;
    private static float clamp(float v, float low, float high) {
        return Float.isNaN(v) || Float.isInfinite(v) ? low : Math.max(low, Math.min(high, v));
    }
    void layout(float width, float height, float baseCell) {
        if (width <= 0 || height <= 0) return;
        scale = clamp(scale, .5f, 2.2f);
        // Three cells with a small seam, always clamped as ONE bounding box.
        cell = Math.min(baseCell * scale, Math.min(width, height) / 3.2f);
        float step = cell * 1.04f, extent = step + cell * .5f;
        float midX = clamp(x * width, extent, width - extent);
        float midY = clamp(y * height, extent, height - extent);
        x = midX / width; y = midY / height;
        cx[0] = cx[1] = midX; cy[0] = midY - step; cy[1] = midY + step;
        cx[2] = midX - step; cx[3] = midX + step; cy[2] = cy[3] = midY;
        left = midX - extent; right = midX + extent;
        top = midY - extent; bottom = midY + extent;
    }
    boolean contains(float px, float py) {
        return cell > 0 && px >= left && px <= right && py >= top && py <= bottom;
    }
    int directionsAt(float px, float py) {
        if (!visible || !contains(px, py)) return 0;
        float dx = px - cx[0], dy = py - cy[2], dead = cell * .32f;
        return (dy < -dead ? 1 : dy > dead ? 2 : 0)
                | (dx < -dead ? 4 : dx > dead ? 8 : 0);
    }
}
