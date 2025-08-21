#include <jni.h>
#include <string>

// OpenGL ES 2.0 code
#include <GLES2/gl2.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/sensor.h>
#include <dlfcn.h>
#include <jni.h>

#define MINIAUDIO_IMPLEMENTATION

#include "miniaudio.h"

#include <cassert>
#include <cstdint>
#include <string>

#define LOG_TAG "therecell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define DEVICE_FORMAT       ma_format_f32
#define DEVICE_CHANNELS     2
#define DEVICE_SAMPLE_RATE  48000

const int LOOPER_ID_USER = 3;
const int SENSOR_HISTORY_LENGTH = 100;
const int SENSOR_REFRESH_RATE_HZ = 100;
constexpr int32_t SENSOR_REFRESH_PERIOD_US =
        int32_t(1000000 / SENSOR_REFRESH_RATE_HZ);
const float SENSOR_FILTER_ALPHA = 0.1f;

/*
 * AcquireASensorManagerInstance(void)
 *    Workaround AsensorManager_getInstance() deprecation false alarm
 *    for Android-N and before, when compiling with NDK-r15
 */
#include <dlfcn.h>

const char *kPackageName = "com.android.therecell";

ASensorManager *AcquireASensorManagerInstance(void) {
    typedef ASensorManager *(*PF_GETINSTANCEFORPACKAGE)(const char *name);
    void *androidHandle = dlopen("libandroid.so", RTLD_NOW);
    PF_GETINSTANCEFORPACKAGE getInstanceForPackageFunc =
            (PF_GETINSTANCEFORPACKAGE) dlsym(androidHandle,
                                             "ASensorManager_getInstanceForPackage");
    if (getInstanceForPackageFunc) {
        return getInstanceForPackageFunc(kPackageName);
    }

    typedef ASensorManager *(*PF_GETINSTANCE)();
    PF_GETINSTANCE getInstanceFunc =
            (PF_GETINSTANCE) dlsym(androidHandle, "ASensorManager_getInstance");
    // by all means at this point, ASensorManager_getInstance should be available
    assert(getInstanceFunc);
    return getInstanceFunc();
}

class sensorgraph {
    std::string vertexShaderSource;
    std::string fragmentShaderSource;
    ASensorManager *sensorManager;
    const ASensor *accelerometer;
    const ASensor *gyroscope;                 // NEW
    ASensorEventQueue *accelerometerEventQueue;
    ASensorEventQueue *gyroscopeEventQueue;   // NEW
    ALooper *looper;

    GLuint shaderProgram;
    GLuint vPositionHandle;
    GLuint vSensorValueHandle;
    GLuint uFragColorHandle;
    GLfloat xPos[SENSOR_HISTORY_LENGTH];


    struct Vec3 {
        GLfloat x, y, z;
    };

    // --- Accelerometer buffers ---
    Vec3 accelData[SENSOR_HISTORY_LENGTH * 2]{}; // duplicated buffer trick
    Vec3 accelFilter{0.f, 0.f, 0.f};
    int accelIndex = 0;

    // --- Gyroscope buffers ---
    Vec3 gyroData[SENSOR_HISTORY_LENGTH * 2]{};  // duplicated buffer trick
    Vec3 gyroFilter{0.f, 0.f, 0.f};
    int gyroIndex = 0;

    ma_waveform sineWave;
    ma_device_config deviceConfig;
    ma_device device;
    bool audioInitialized = false;

public:
    sensorgraph() = default;

    void init(AAssetManager *assetManager) {
        AAsset *vertexShaderAsset =
                AAssetManager_open(assetManager, "shader.glslv", AASSET_MODE_BUFFER);
        assert(vertexShaderAsset != nullptr);
        const void *vertexShaderBuf = AAsset_getBuffer(vertexShaderAsset);
        assert(vertexShaderBuf != NULL);
        off_t vertexShaderLength = AAsset_getLength(vertexShaderAsset);
        vertexShaderSource =
                std::string((const char *) vertexShaderBuf, (size_t) vertexShaderLength);
        AAsset_close(vertexShaderAsset);

        AAsset *fragmentShaderAsset =
                AAssetManager_open(assetManager, "shader.glslf", AASSET_MODE_BUFFER);
        assert(fragmentShaderAsset != NULL);
        const void *fragmentShaderBuf = AAsset_getBuffer(fragmentShaderAsset);
        assert(fragmentShaderBuf != NULL);
        off_t fragmentShaderLength = AAsset_getLength(fragmentShaderAsset);
        fragmentShaderSource = std::string((const char *) fragmentShaderBuf,
                                           (size_t) fragmentShaderLength);
        AAsset_close(fragmentShaderAsset);

        // --- Sensors setup ---
        sensorManager = AcquireASensorManagerInstance();
        assert(sensorManager != nullptr);

        // Use linear acceleration (gravity removed). You can switch to ASENSOR_TYPE_ACCELEROMETER if desired.
        accelerometer = ASensorManager_getDefaultSensor(sensorManager, ASENSOR_TYPE_LINEAR_ACCELERATION);
        assert(accelerometer != nullptr);

        gyroscope = ASensorManager_getDefaultSensor(sensorManager, ASENSOR_TYPE_GYROSCOPE); // NEW
        // gyroscope can be null on some devices; code handles absence gracefully.

        looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        assert(looper != nullptr);

        // Accel queue
        accelerometerEventQueue = ASensorManager_createEventQueue(
                sensorManager, looper, LOOPER_ID_USER, NULL, NULL);
        assert(accelerometerEventQueue != nullptr);
        {
            auto status = ASensorEventQueue_enableSensor(accelerometerEventQueue, accelerometer);
            assert(status >= 0);
            status = ASensorEventQueue_setEventRate(accelerometerEventQueue, accelerometer, SENSOR_REFRESH_PERIOD_US);
            assert(status >= 0);
            (void)status;
        }

        // Gyro queue (NEW)
        if (gyroscope) {
            gyroscopeEventQueue = ASensorManager_createEventQueue(
                    sensorManager, looper, LOOPER_ID_USER + 1, NULL, NULL);
            assert(gyroscopeEventQueue != nullptr);
            auto status = ASensorEventQueue_enableSensor(gyroscopeEventQueue, gyroscope);
            assert(status >= 0);
            status = ASensorEventQueue_setEventRate(gyroscopeEventQueue, gyroscope, SENSOR_REFRESH_PERIOD_US);
            assert(status >= 0);
            (void)status;
        }

        generateXPos();
    }

    static void
    data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
        ma_waveform *pSineWave = (ma_waveform *) pDevice->pUserData;
        ma_waveform_read_pcm_frames(pSineWave, pOutput, frameCount, nullptr);
        (void) pInput;
    }

    void initAudio() {
        if (audioInitialized) return;

        deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format = DEVICE_FORMAT;
        deviceConfig.playback.channels = DEVICE_CHANNELS;
        deviceConfig.sampleRate = DEVICE_SAMPLE_RATE;
        deviceConfig.dataCallback = data_callback;
        deviceConfig.pUserData = &sineWave;

        if (ma_device_init(nullptr, &deviceConfig, &device) != MA_SUCCESS) {
            LOGI("Failed to initialize audio device");
            return;
        }

        ma_waveform_config sineWaveConfig = ma_waveform_config_init(
                device.playback.format,
                device.playback.channels,
                device.sampleRate,
                ma_waveform_type_sine,
                0.2f,    // amplitude
                220.0f   // frequency
        );
        ma_waveform_init(&sineWaveConfig, &sineWave);

        if (ma_device_start(&device) != MA_SUCCESS) {
            LOGI("Failed to start audio device");
            ma_device_uninit(&device);
            return;
        }

        audioInitialized = true;
        LOGI("Audio device started!");
    }

    void surfaceCreated() {
        LOGI("GL_VERSION: %s", glGetString(GL_VERSION));
        LOGI("GL_VENDOR: %s", glGetString(GL_VENDOR));
        LOGI("GL_RENDERER: %s", glGetString(GL_RENDERER));
        LOGI("GL_EXTENSIONS: %s", glGetString(GL_EXTENSIONS));

        shaderProgram = createProgram(vertexShaderSource, fragmentShaderSource);
        assert(shaderProgram != 0);
        GLint getPositionLocationResult =
                glGetAttribLocation(shaderProgram, "vPosition");
        assert(getPositionLocationResult != -1);
        vPositionHandle = (GLuint) getPositionLocationResult;
        GLint getSensorValueLocationResult =
                glGetAttribLocation(shaderProgram, "vSensorValue");
        assert(getSensorValueLocationResult != -1);
        vSensorValueHandle = (GLuint) getSensorValueLocationResult;
        GLint getFragColorLocationResult =
                glGetUniformLocation(shaderProgram, "uFragColor");
        assert(getFragColorLocationResult != -1);
        uFragColorHandle = (GLuint) getFragColorLocationResult;
    }

    void surfaceChanged(int w, int h) { glViewport(0, 0, w, h); }

    void generateXPos() {
        for (auto i = 0; i < SENSOR_HISTORY_LENGTH; i++) {
            float t =
                    static_cast<float>(i) / static_cast<float>(SENSOR_HISTORY_LENGTH - 1);
            xPos[i] = -1.f * (1.f - t) + 1.f * t;
        }
    }

    GLuint createProgram(const std::string &pVertexSource,
                         const std::string &pFragmentSource) {
        GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
        GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
        GLuint program = glCreateProgram();
        assert(program != 0);
        glAttachShader(program, vertexShader);
        glAttachShader(program, pixelShader);
        glLinkProgram(program);
        GLint programLinked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &programLinked);
        assert(programLinked != 0);
        glDeleteShader(vertexShader);
        glDeleteShader(pixelShader);
        return program;
    }

    GLuint loadShader(GLenum shaderType, const std::string &pSource) {
        GLuint shader = glCreateShader(shaderType);
        assert(shader != 0);
        const char *sourceBuf = pSource.c_str();
        glShaderSource(shader, 1, &sourceBuf, NULL);
        glCompileShader(shader);
        GLint shaderCompiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &shaderCompiled);
        assert(shaderCompiled != 0);
        return shader;
    }

    void update() {
        ALooper_pollOnce(0, NULL, NULL, NULL);
        ASensorEvent event;
        float a = SENSOR_FILTER_ALPHA;

        // --- Accelerometer ---
        while (ASensorEventQueue_getEvents(accelerometerEventQueue, &event, 1) > 0) {
            if (event.type == ASENSOR_TYPE_ACCELEROMETER ||
                event.type == ASENSOR_TYPE_LINEAR_ACCELERATION) {
                accelFilter.x = a * event.acceleration.x + (1.0f - a) * accelFilter.x;
                accelFilter.y = a * event.acceleration.y + (1.0f - a) * accelFilter.y;
                accelFilter.z = a * event.acceleration.z + (1.0f - a) * accelFilter.z;
            }
        }
        // write to circular buffer twice (wrapping trick for contiguous slice)
        accelData[accelIndex] = accelFilter;
        accelData[SENSOR_HISTORY_LENGTH + accelIndex] = accelFilter;
        accelIndex = (accelIndex + 1) % SENSOR_HISTORY_LENGTH;

        // --- Gyroscope (rad/s) ---
        if (gyroscope && gyroscopeEventQueue) {
            while (ASensorEventQueue_getEvents(gyroscopeEventQueue, &event, 1) > 0) {
                if (event.type == ASENSOR_TYPE_GYROSCOPE) {
                    // event.vector.{x,y,z} are angular velocities in rad/s
                    gyroFilter.x = a * event.vector.x + (1.0f - a) * gyroFilter.x;
                    gyroFilter.y = a * event.vector.y + (1.0f - a) * gyroFilter.y;
                    gyroFilter.z = a * event.vector.z + (1.0f - a) * gyroFilter.z;
                }
            }
            gyroData[gyroIndex] = gyroFilter;
            gyroData[SENSOR_HISTORY_LENGTH + gyroIndex] = gyroFilter;
            gyroIndex = (gyroIndex + 1) % SENSOR_HISTORY_LENGTH;
        }

        // Map x acceleration to a reasonable frequency range
        // Example: map -10..10 m/s² to 200Hz..1000Hz
        if (audioInitialized) {
            float gyroZ = std::fabs(gyroFilter.z);     // rad/s
            // Map 0..5 rad/s → 0.05..0.6 (clamped)
            float gMin = 0.0f, gMax = 5.0f;
            float minFreq = 200.f, maxFreq = 1000.f;
            float t = (std::fmin(std::fmax(gyroZ, gMin), gMax) - gMin) / (gMax - gMin);
            float freq = minFreq + t * (maxFreq - minFreq);
            ma_waveform_set_frequency(&sineWave, freq);
        }
    }

    void render() {
        glClearColor(0.f, 0.f, 0.f, 1.0f);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

        glUseProgram(shaderProgram);

        glEnableVertexAttribArray(vPositionHandle);
        glVertexAttribPointer(vPositionHandle, 1, GL_FLOAT, GL_FALSE, 0, xPos);

        glEnableVertexAttribArray(vSensorValueHandle);

        // --- Accelerometer: X, Y, Z ---
        glVertexAttribPointer(vSensorValueHandle, 1, GL_FLOAT, GL_FALSE,
                              sizeof(Vec3),
                              &accelData[accelIndex].x);
        glUniform4f(uFragColorHandle, 1.0f, 1.0f, 0.0f, 1.0f); // yellow
        glDrawArrays(GL_LINE_STRIP, 0, SENSOR_HISTORY_LENGTH);

        glVertexAttribPointer(vSensorValueHandle, 1, GL_FLOAT, GL_FALSE,
                              sizeof(Vec3),
                              &accelData[accelIndex].y);
        glUniform4f(uFragColorHandle, 1.0f, 0.0f, 1.0f, 1.0f); // magenta
        glDrawArrays(GL_LINE_STRIP, 0, SENSOR_HISTORY_LENGTH);

        glVertexAttribPointer(vSensorValueHandle, 1, GL_FLOAT, GL_FALSE,
                              sizeof(Vec3),
                              &accelData[accelIndex].z);
        glUniform4f(uFragColorHandle, 0.0f, 1.0f, 1.0f, 1.0f); // cyan
        glDrawArrays(GL_LINE_STRIP, 0, SENSOR_HISTORY_LENGTH);

        // --- Gyroscope: X, Y, Z (different tints) ---
        if (gyroscope) {
            glVertexAttribPointer(vSensorValueHandle, 1, GL_FLOAT, GL_FALSE,
                                  sizeof(Vec3),
                                  &gyroData[gyroIndex].x);
            glUniform4f(uFragColorHandle, 0.6f, 0.6f, 0.0f, 1.0f); // darker yellow
            glDrawArrays(GL_LINE_STRIP, 0, SENSOR_HISTORY_LENGTH);

            glVertexAttribPointer(vSensorValueHandle, 1, GL_FLOAT, GL_FALSE,
                                  sizeof(Vec3),
                                  &gyroData[gyroIndex].y);
            glUniform4f(uFragColorHandle, 0.6f, 0.0f, 0.6f, 1.0f); // darker magenta
            glDrawArrays(GL_LINE_STRIP, 0, SENSOR_HISTORY_LENGTH);

            glVertexAttribPointer(vSensorValueHandle, 1, GL_FLOAT, GL_FALSE,
                                  sizeof(Vec3),
                                  &gyroData[gyroIndex].z);
            glUniform4f(uFragColorHandle, 0.0f, 0.6f, 0.6f, 1.0f); // darker cyan
            glDrawArrays(GL_LINE_STRIP, 0, SENSOR_HISTORY_LENGTH);
        }
    }

    void pause() {
        if (accelerometerEventQueue && accelerometer) {
            ASensorEventQueue_disableSensor(accelerometerEventQueue, accelerometer);
        }
        if (gyroscopeEventQueue && gyroscope) {
            ASensorEventQueue_disableSensor(gyroscopeEventQueue, gyroscope);
        }
    }

    void resume() {
        if (accelerometerEventQueue && accelerometer) {
            ASensorEventQueue_enableSensor(accelerometerEventQueue, accelerometer);
            auto status = ASensorEventQueue_setEventRate(
                    accelerometerEventQueue, accelerometer, SENSOR_REFRESH_PERIOD_US);
            assert(status >= 0);
            (void)status;
        }
        if (gyroscopeEventQueue && gyroscope) {
            ASensorEventQueue_enableSensor(gyroscopeEventQueue, gyroscope);
            auto status = ASensorEventQueue_setEventRate(
                    gyroscopeEventQueue, gyroscope, SENSOR_REFRESH_PERIOD_US);
            assert(status >= 0);
            (void)status;
        }
    }
};


sensorgraph gSensorGraph;

extern "C" {
JNIEXPORT void JNICALL Java_com_example_therecell_MainActivity_init(
        JNIEnv *env, jobject type, jobject assetManager) {
    (void) type;
    AAssetManager *nativeAssetManager = AAssetManager_fromJava(env, assetManager);
    gSensorGraph.init(nativeAssetManager);
}

JNIEXPORT void JNICALL Java_com_example_therecell_MainActivity_surfaceCreated(
        JNIEnv *env, jobject type) {
    (void) env;
    (void) type;
    gSensorGraph.surfaceCreated();
}

JNIEXPORT void JNICALL Java_com_example_therecell_MainActivity_surfaceChanged(
        JNIEnv *env, jobject type, jint width, jint height) {
    (void) env;
    (void) type;
    gSensorGraph.surfaceChanged(width, height);
}

JNIEXPORT void JNICALL
Java_com_example_therecell_MainActivity_drawFrame(JNIEnv *env, jobject type) {
    (void) env;
    (void) type;
    gSensorGraph.update();
    gSensorGraph.render();
}

JNIEXPORT void JNICALL
Java_com_example_therecell_MainActivity_pause(JNIEnv *env, jobject type) {
    (void) env;
    (void) type;
    gSensorGraph.pause();
}

JNIEXPORT void JNICALL
Java_com_example_therecell_MainActivity_resume(JNIEnv *env, jobject type) {
    (void) env;
    (void) type;
    gSensorGraph.resume();
}

JNIEXPORT void JNICALL
Java_com_example_therecell_MainActivity_initAudio(JNIEnv *env, jobject obj) {
    gSensorGraph.initAudio();
}

}

