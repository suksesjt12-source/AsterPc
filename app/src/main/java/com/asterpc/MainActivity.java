package com.asterpc;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.graphics.Typeface;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    private TextView output;

    static { System.loadLibrary("asterpc"); }
    private native String runSelfTest();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(24, 28, 24, 24);
        root.setBackgroundColor(0xFF101722);

        TextView title = new TextView(this);
        title.setText("ASTERPC");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("Android PC-emulation research prototype");
        subtitle.setTextColor(0xFFB9C7D8);
        subtitle.setTextSize(14);
        root.addView(subtitle);

        TextView status = new TextView(this);
        status.setText("\nStatus: prototype 0.2.0\nHost ABI: " + android.os.Build.SUPPORTED_ABIS[0]
                + "\nCPU: experimental x86-64 interpreter\nGPU: OpenGL ES 2.0 renderer");
        status.setTextColor(0xFFE4ECF5);
        status.setTextSize(15);
        root.addView(status);

        Button gpu = new Button(this);
        gpu.setText("Open GPU rendering test");
        root.addView(gpu);
        gpu.setOnClickListener(v -> startActivity(new Intent(this, GpuActivity.class)));

        Button test = new Button(this);
        test.setText("Run CPU interpreter self-test");
        root.addView(test);

        output = new TextView(this);
        output.setTextColor(0xFF8FE3B0);
        output.setTextSize(14);
        output.setTypeface(Typeface.MONOSPACE);
        output.setPadding(0, 20, 0, 0);
        root.addView(output);

        TextView warning = new TextView(this);
        warning.setText("\nImportant: GPU test currently renders a native demo scene. It is not yet connected to guest Windows graphics, DirectX, Vulkan, or PC games.");
        warning.setTextColor(0xFFFFD28A);
        warning.setTextSize(13);
        root.addView(warning);

        test.setOnClickListener(v -> {
            try { output.setText(runSelfTest()); }
            catch (Throwable t) { output.setText("Native test failed: " + t.getMessage()); }
        });

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }
}