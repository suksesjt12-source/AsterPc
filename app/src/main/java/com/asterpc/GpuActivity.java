package com.asterpc;

import android.app.Activity;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.TextView;

public class GpuActivity extends Activity {
    private GLSurfaceView surface;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        surface = new GLSurfaceView(this);
        surface.setEGLContextClientVersion(2);
        surface.setRenderer(new GpuRenderer());
        surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);

        FrameLayout frame = new FrameLayout(this);
        frame.addView(surface, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        TextView label = new TextView(this);
        label.setText("ASTERPC GPU TEST  |  OpenGL ES 2.0\nAnimated shader scene • rendered by device GPU");
        label.setTextColor(0xFFFFFFFF);
        label.setTextSize(14);
        label.setPadding(18, 18, 18, 18);
        label.setBackgroundColor(0x990B1420);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        frame.addView(label, lp);
        setContentView(frame);
    }

    @Override protected void onPause() {
        super.onPause();
        if (surface != null) surface.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (surface != null) surface.onResume();
    }
}