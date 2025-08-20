package com.example.theracell

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.widget.TextView
import com.example.theracell.databinding.ActivityMainBinding
import android.opengl.GLSurfaceView
import android.opengl.GLSurfaceView.Renderer
import android.opengl.GLES20
import android.content.res.AssetManager
import javax.microedition.khronos.opengles.GL10
import javax.microedition.khronos.egl.EGLConfig


class MainActivity : AppCompatActivity() {

    companion object {
        init { System.loadLibrary("theracell") }
    }

    private external fun init(assetManager: AssetManager)
    private external fun surfaceCreated()
    private external fun surfaceChanged(width: Int, height: Int)
    private external fun drawFrame()
    private external fun pause()
    private external fun resume()

    private lateinit var glSurfaceView: GLSurfaceView  // <-- declare it here

    override fun onCreate(savedInstanceState: Bundle?) {

        super.onCreate(savedInstanceState)
        System.loadLibrary("theracell")



        glSurfaceView = GLSurfaceView(this)
        glSurfaceView.setEGLContextClientVersion(2)
        glSurfaceView.setRenderer(object : GLSurfaceView.Renderer {
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) { surfaceCreated() }
            override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) { surfaceChanged(width, height) }
            override fun onDrawFrame(gl: GL10?) { drawFrame() }
        })

        setContentView(glSurfaceView)  // set the GLSurfaceView as the root view
        init(assets)

    }


    override fun onPause() {
        super.onPause()
        glSurfaceView.onPause()  // <- important!
        pause()
    }

    override fun onResume() {
        super.onResume()
        glSurfaceView.onResume() // <- important!
        resume()
    }
}

