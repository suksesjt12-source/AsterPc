package com.asterpc;

import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.Matrix;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class GpuRenderer implements GLSurfaceView.Renderer {
    private static final String VERTEX =
            "attribute vec2 aPosition; attribute vec3 aColor; uniform mat2 uRotation;" +
            "varying vec3 vColor; void main(){ vec2 p=uRotation*aPosition;" +
            "gl_Position=vec4(p,0.0,1.0); vColor=aColor; }";
    private static final String FRAGMENT =
            "precision mediump float; varying vec3 vColor;" +
            "void main(){ gl_FragColor=vec4(vColor,1.0); }";

    private final FloatBuffer vertices;
    private int program;
    private int positionHandle, colorHandle, rotationHandle;
    private long startNs;

    public GpuRenderer() {
        float[] data = {
            // x, y, r, g, b
             0.00f,  0.78f, 0.20f, 0.85f, 1.00f,
            -0.72f, -0.52f, 0.15f, 0.95f, 0.55f,
             0.72f, -0.52f, 1.00f, 0.55f, 0.18f
        };
        vertices = ByteBuffer.allocateDirect(data.length * 4)
                .order(ByteOrder.nativeOrder()).asFloatBuffer();
        vertices.put(data).position(0);
    }

    @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        GLES20.glClearColor(0.035f, 0.055f, 0.09f, 1.0f);
        GLES20.glDisable(GLES20.GL_DEPTH_TEST);
        int vs = compile(GLES20.GL_VERTEX_SHADER, VERTEX);
        int fs = compile(GLES20.GL_FRAGMENT_SHADER, FRAGMENT);
        program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, vs);
        GLES20.glAttachShader(program, fs);
        GLES20.glLinkProgram(program);
        int[] status = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0);
        if (status[0] == 0) {
            String log = GLES20.glGetProgramInfoLog(program);
            GLES20.glDeleteProgram(program);
            throw new RuntimeException("GPU shader link failed: " + log);
        }
        GLES20.glDeleteShader(vs);
        GLES20.glDeleteShader(fs);
        positionHandle = GLES20.glGetAttribLocation(program, "aPosition");
        colorHandle = GLES20.glGetAttribLocation(program, "aColor");
        rotationHandle = GLES20.glGetUniformLocation(program, "uRotation");
        startNs = System.nanoTime();
    }

    private int compile(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        int[] status = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0);
        if (status[0] == 0) {
            String log = GLES20.glGetShaderInfoLog(shader);
            GLES20.glDeleteShader(shader);
            throw new RuntimeException("GPU shader compile failed: " + log);
        }
        return shader;
    }

    @Override public void onSurfaceChanged(GL10 gl, int width, int height) {
        GLES20.glViewport(0, 0, width, height);
    }

    @Override public void onDrawFrame(GL10 gl) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glUseProgram(program);

        float seconds = (System.nanoTime() - startNs) / 1_000_000_000.0f;
        float angle = seconds * 0.65f;
        float c = (float)Math.cos(angle), s = (float)Math.sin(angle);
        float[] rotation = { c, -s, s, c };
        GLES20.glUniformMatrix2fv(rotationHandle, 1, false, rotation, 0);

        vertices.position(0);
        GLES20.glEnableVertexAttribArray(positionHandle);
        GLES20.glVertexAttribPointer(positionHandle, 2, GLES20.GL_FLOAT,
                false, 5 * 4, vertices);
        vertices.position(2);
        GLES20.glEnableVertexAttribArray(colorHandle);
        GLES20.glVertexAttribPointer(colorHandle, 3, GLES20.GL_FLOAT,
                false, 5 * 4, vertices);

        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        GLES20.glDisableVertexAttribArray(positionHandle);
        GLES20.glDisableVertexAttribArray(colorHandle);
    }
}