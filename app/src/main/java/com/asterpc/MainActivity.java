package com.asterpc;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.graphics.Typeface;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;

public class MainActivity extends Activity {
    private static final int PICK_EXE = 41;
    private static final int MAX_EXE_SIZE = 64 * 1024 * 1024;
    private TextView output;
    private boolean mapRequested = false;

    private void pickExecutable() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, PICK_EXE);
    }

    static { System.loadLibrary("asterpc"); }
    private native String runSelfTest();
    private native String inspectPe(byte[] fileBytes);
    private native String mapPeImage(byte[] fileBytes);

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
        status.setText("\nStatus: prototype 0.5.0\nHost ABI: " + android.os.Build.SUPPORTED_ABIS[0]
                + "\nCPU: experimental x86-64 interpreter\nMemory: bounded guest address-space prototype\nGPU: native RGBA framebuffer + OpenGL ES 2.0");
        status.setTextColor(0xFFE4ECF5);
        status.setTextSize(15);
        root.addView(status);

        Button gpu = new Button(this);
        gpu.setText("Open GPU framebuffer test");
        root.addView(gpu);
        gpu.setOnClickListener(v -> startActivity(new Intent(this, GpuActivity.class)));

        Button test = new Button(this);
        test.setText("Run CPU interpreter self-test");
        root.addView(test);

        Button pe = new Button(this);
        pe.setText("Inspect Windows EXE (PE x64)");
        root.addView(pe);

        Button map = new Button(this);
        map.setText("Map PE sections into temporary guest buffer");
        root.addView(map);
        pe.setOnClickListener(v -> { pickExecutable(); });
        map.setOnClickListener(v -> { mapRequested = true; pickExecutable(); });

        output = new TextView(this);
        output.setTextColor(0xFF8FE3B0);
        output.setTextSize(14);
        output.setTypeface(Typeface.MONOSPACE);
        output.setPadding(0, 20, 0, 0);
        root.addView(output);

        TextView warning = new TextView(this);
        warning.setText("\nImportant: PE mapping and import recognition are diagnostic prototypes. IAT addresses are placeholders; Windows APIs and DLLs are not executed. No Windows program can run yet. GPU output is a test framebuffer, not DirectX/Vulkan translation.");
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

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_EXE || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        output.setText("Reading executable...");
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri);
                 ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                if (in == null) throw new IllegalStateException("Cannot open selected file.");
                byte[] chunk = new byte[8192];
                int count, total = 0;
                while ((count = in.read(chunk)) != -1) {
                    total += count;
                    if (total > MAX_EXE_SIZE) throw new IllegalArgumentException("File exceeds 64 MiB limit.");
                    out.write(chunk, 0, count);
                }
                byte[] bytes = out.toByteArray();
                final boolean doMap = mapRequested;
                mapRequested = false;
                String result = doMap ? mapPeImage(bytes) : inspectPe(bytes);
                runOnUiThread(() -> output.setText(result));
            } catch (Exception e) {
                runOnUiThread(() -> output.setText("PE inspection failed: " + e.getMessage()));
            }
        }).start();
    }
}