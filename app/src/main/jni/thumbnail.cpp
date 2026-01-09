#include <stdlib.h>
#include <string>
#include <mutex>
#include <stdint.h>
#include <chrono>
#include <unordered_map>

#include <jni.h>
#include <android/bitmap.h>
#include <mpv/client.h>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
    #include <libavcodec/jni.h>
};

#include "jni_utils.h"
#include "globals.h"
#include "log.h"

extern "C" {
    jni_func(jobject, grabThumbnail, jint dimension);
    jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension, jboolean use_hw_dec, jint quality);
    jni_func(void, setThumbnailJavaVM, jobject appctx);
    jni_func(void, clearThumbnailCache);
};

// ============================================================================
// MPV-BASED THUMBNAIL GENERATION
// Takes a snapshot of the currently playing video in MPV
// ============================================================================

static inline mpv_node make_node_str(const char *s)
{
    mpv_node r{};
    r.format = MPV_FORMAT_STRING;
    r.u.string = const_cast<char*>(s);
    return r;
}

jni_func(jobject, grabThumbnail, jint dimension) {
    auto total_start = std::chrono::high_resolution_clock::now();
    
    CHECK_MPV_INIT();
    
    // Ensure JNI cache is initialized
    init_methods_cache(env);
    
    ALOGI("════════════════════════════════════════════════════════════════");
    ALOGI("Thumbnail (MPV) | Starting snapshot from current playback");
    ALOGI("Thumbnail (MPV) | Dimension: %dpx", dimension);

    mpv_node result{};
    {
        mpv_node c{}, c_args[2];
        mpv_node_list c_array{};
        c_args[0] = make_node_str("screenshot-raw");
        c_args[1] = make_node_str("video");
        c_array.num = 2;
        c_array.values = c_args;
        c.format = MPV_FORMAT_NODE_ARRAY;
        c.u.list = &c_array;
        
        ALOGD("Thumbnail (MPV) | Executing screenshot-raw command...");
        if (mpv_command_node(g_mpv, &c, &result) < 0) {
            ALOGE("Thumbnail (MPV) | ✗ screenshot-raw command failed");
            ALOGI("════════════════════════════════════════════════════════════════");
            return NULL;
        }
    }
    
    ALOGD("Thumbnail (MPV) | ✓ Screenshot command completed");

    // extract relevant property data from the node map mpv returns
    ALOGD("Thumbnail (MPV) | Extracting frame data from response...");
    int w = 0, h = 0, stride = 0;
    bool format_ok = false;
    struct mpv_byte_array *data = NULL;
    do {
        if (result.format != MPV_FORMAT_NODE_MAP)
            break;
        for (int i = 0; i < result.u.list->num; i++) {
            std::string key(result.u.list->keys[i]);
            const mpv_node *val = &result.u.list->values[i];
            if (key == "w" || key == "h" || key == "stride") {
                if (val->format != MPV_FORMAT_INT64)
                    break;
                if (key == "w")
                    w = val->u.int64;
                else if (key == "h")
                    h = val->u.int64;
                else
                    stride = val->u.int64;
            } else if (key == "format") {
                if (val->format != MPV_FORMAT_STRING)
                    break;
                format_ok = !strcmp(val->u.string, "bgr0");
            } else if (key == "data") {
                if (val->format != MPV_FORMAT_BYTE_ARRAY)
                    break;
                data = val->u.ba;
            }
        }
    } while (0);
    if (!w || !h || !stride || !format_ok || !data) {
        ALOGE("Thumbnail (MPV) | ✗ Failed to extract frame data from response");
        mpv_free_node_contents(&result);
        ALOGI("════════════════════════════════════════════════════════════════");
        return NULL;
    }
    ALOGD("Thumbnail (MPV) | Frame data | Size: %dx%d | Stride: %d | Format: bgr0", w, h, stride);

    // crop to square
    ALOGD("Thumbnail (MPV) | Cropping to square...");
    int crop_left = 0, crop_top = 0;
    int new_w = w, new_h = h;
    if (w > h) {
        crop_left = (w - h) / 2;
        new_w = h;
        ALOGV("Thumbnail (MPV) | Cropping: removing %dpx from left/right", crop_left);
    } else if (h > w) {
        crop_top = (h - w) / 2;
        new_h = w;
        ALOGV("Thumbnail (MPV) | Cropping: removing %dpx from top/bottom", crop_top);
    }
    ALOGD("Thumbnail (MPV) | Cropped dimensions: %dx%d", new_w, new_h);

    uint8_t *new_data = reinterpret_cast<uint8_t*>(data->data);
    new_data += crop_left * sizeof(uint32_t); // move begin rightwards
    new_data += stride * crop_top; // move begin downwards

    // convert & scale to appropriate size
    ALOGD("Thumbnail (MPV) | Scaling %dx%d → %dx%d using BICUBIC...", new_w, new_h, dimension, dimension);
    struct SwsContext *ctx = sws_getContext(
        new_w, new_h, AV_PIX_FMT_BGR0,
        dimension, dimension, AV_PIX_FMT_RGB32,
        SWS_BICUBIC, NULL, NULL, NULL);
    if (!ctx) {
        ALOGE("Thumbnail (MPV) | ✗ Failed to create scaling context");
        mpv_free_node_contents(&result);
        ALOGI("════════════════════════════════════════════════════════════════");
        return NULL;
    }

    jintArray arr = env->NewIntArray(dimension * dimension);
    jint *scaled = env->GetIntArrayElements(arr, NULL);

    uint8_t *src_p[4] = { new_data }, *dst_p[4] = { (uint8_t*) scaled };
    int src_stride[4] = { stride },
        dst_stride[4] = { (int) sizeof(jint) * dimension };
    
    auto scale_start = std::chrono::high_resolution_clock::now();
    sws_scale(ctx, src_p, src_stride, 0, new_h, dst_p, dst_stride);
    auto scale_end = std::chrono::high_resolution_clock::now();
    auto scale_duration = std::chrono::duration_cast<std::chrono::milliseconds>(scale_end - scale_start);
    ALOGD("Thumbnail (MPV) | ✓ Scaling completed in %lld ms", (long long)scale_duration.count());
    
    sws_freeContext(ctx);

    mpv_free_node_contents(&result); // frees data->data

    // create android.graphics.Bitmap
    ALOGD("Thumbnail (MPV) | Creating Android Bitmap...");
    env->ReleaseIntArrayElements(arr, scaled, 0);

    jobject bitmap_config =
        env->GetStaticObjectField(android_graphics_Bitmap_Config, android_graphics_Bitmap_Config_ARGB_8888);
    jobject bitmap =
        env->CallStaticObjectMethod(android_graphics_Bitmap, android_graphics_Bitmap_createBitmap,
        arr, dimension, dimension, bitmap_config);
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(bitmap_config);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start);
    ALOGI("Thumbnail (MPV) | ✓ SUCCESS | Total time: %lld ms | Size: %dx%d", 
          (long long)total_duration.count(), dimension, dimension);
    ALOGI("════════════════════════════════════════════════════════════════");

    return bitmap;
}

// ============================================================================
// FAST THUMBNAIL GENERATION USING DIRECT FFMPEG API
// Bypasses MPV entirely, uses FFmpeg API directly
// Expected performance: 50-100ms per thumbnail
// ============================================================================

static JavaVM *g_thumb_vm = nullptr;
static jobject g_thumb_appctx = nullptr;
static std::mutex g_thumb_mutex;

// Codec cache for faster initialization
struct CodecCacheEntry {
    AVCodecID codec_id;
    const AVCodec *codec;
    std::chrono::steady_clock::time_point last_used;
};

static std::unordered_map<AVCodecID, CodecCacheEntry> g_codec_cache;
static std::mutex g_codec_cache_mutex;

// Hardware device context cache (expensive to create)
static AVBufferRef *g_hw_device_ctx = nullptr;
static std::mutex g_hw_ctx_mutex;
static bool g_hw_ctx_initialized = false;
static bool g_hw_ctx_available = false;

// Get codec from cache or find it
static const AVCodec* get_cached_codec(AVCodecID codec_id) {
    std::lock_guard<std::mutex> lock(g_codec_cache_mutex);
    
    auto it = g_codec_cache.find(codec_id);
    if (it != g_codec_cache.end()) {
        it->second.last_used = std::chrono::steady_clock::now();
        ALOGV("Thumbnail | Codec found in cache: %s", avcodec_get_name(codec_id));
        return it->second.codec;
    }
    
    // Not in cache, find it
    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (codec) {
        g_codec_cache[codec_id] = {codec_id, codec, std::chrono::steady_clock::now()};
        ALOGV("Thumbnail | Codec added to cache: %s", codec->name);
    }
    
    return codec;
}

// Initialize hardware device context once and reuse it
static bool init_hw_device_context() {
    std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
    
    if (g_hw_ctx_initialized) {
        return g_hw_ctx_available;
    }
    
    g_hw_ctx_initialized = true;
    
    enum AVHWDeviceType hw_type = av_hwdevice_find_type_by_name("mediacodec");
    if (hw_type == AV_HWDEVICE_TYPE_NONE) {
        ALOGD("Thumbnail | MediaCodec not found, HW accel unavailable");
        g_hw_ctx_available = false;
        return false;
    }
    
    if (av_hwdevice_ctx_create(&g_hw_device_ctx, hw_type, NULL, NULL, 0) < 0) {
        ALOGD("Thumbnail | Failed to create HW device context");
        g_hw_ctx_available = false;
        return false;
    }
    
    ALOGI("Thumbnail | Hardware device context initialized successfully");
    g_hw_ctx_available = true;
    return true;
}

// Automatic cleanup on library unload
static void cleanup_thumbnail_resources() __attribute__((destructor));
static void cleanup_thumbnail_resources() {
    ALOGI("Thumbnail | Library unloading, cleaning up resources...");
    
    // Clear codec cache
    {
        std::lock_guard<std::mutex> lock(g_codec_cache_mutex);
        g_codec_cache.clear();
    }
    
    // Release hardware context
    {
        std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
        if (g_hw_device_ctx) {
            av_buffer_unref(&g_hw_device_ctx);
            g_hw_device_ctx = nullptr;
        }
    }
    
    // Release JNI global references
    {
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        if (g_thumb_appctx && g_thumb_vm) {
            JNIEnv* env = nullptr;
            if (g_thumb_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
                env->DeleteGlobalRef(g_thumb_appctx);
                g_thumb_appctx = nullptr;
            }
        }
    }
    
    ALOGI("Thumbnail | Cleanup completed");
}

jni_func(void, setThumbnailJavaVM, jobject appctx) {
    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    
    if (g_thumb_appctx) {
        env->DeleteGlobalRef(g_thumb_appctx);
        g_thumb_appctx = nullptr;
    }
    
    if (!env->GetJavaVM(&g_thumb_vm) && g_thumb_vm) {
        av_jni_set_java_vm(g_thumb_vm, NULL);
    }
    
    if (appctx) {
        g_thumb_appctx = env->NewGlobalRef(appctx);
        if (g_thumb_appctx) {
            av_jni_set_android_app_ctx(g_thumb_appctx, NULL);
        }
    }
}

// Clear codec cache and hardware context (call on low memory or app termination)
jni_func(void, clearThumbnailCache) {
    ALOGI("Thumbnail | Clearing codec cache...");
    
    // Clear codec cache (safe - just pointers managed by FFmpeg)
    {
        std::lock_guard<std::mutex> lock(g_codec_cache_mutex);
        size_t cache_size = g_codec_cache.size();
        g_codec_cache.clear();
        ALOGD("Thumbnail | Cleared %zu codec entries from cache", cache_size);
    }
    
    // Clear hardware context (properly release AVBufferRef)
    {
        std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
        if (g_hw_device_ctx) {
            // av_buffer_unref will free the buffer when refcount reaches 0
            av_buffer_unref(&g_hw_device_ctx);
            g_hw_device_ctx = nullptr;
            ALOGD("Thumbnail | Hardware device context released");
        }
        g_hw_ctx_initialized = false;
        g_hw_ctx_available = false;
    }
    
    ALOGI("Thumbnail | Cache cleared successfully");
}

// Quality level constants
enum ThumbnailQuality {
    QUALITY_FAST = 0,   // Fast extraction - lower quality
    QUALITY_NORMAL = 1, // Normal quality (default)
    QUALITY_HQ = 2      // High quality
};

// Helper function to get quality name for logging
static const char* get_quality_name(int quality) {
    switch (quality) {
        case QUALITY_FAST: return "FAST";
        case QUALITY_HQ: return "HQ";
        case QUALITY_NORMAL:
        default: return "NORMAL";
    }
}

// Helper function to get scaling algorithm name for logging
static const char* get_scaling_algorithm_name(int algorithm) {
    switch (algorithm) {
        case SWS_FAST_BILINEAR: return "FAST_BILINEAR";
        case SWS_LANCZOS: return "LANCZOS";
        case SWS_POINT: return "POINT";
        default: return "UNKNOWN";
    }
}

// Convert AVFrame to Android Bitmap
static jobject frame_to_bitmap(JNIEnv *env, AVFrame *frame, int target_dimension, int quality) {
    auto conversion_start = std::chrono::high_resolution_clock::now();
    
    // Ensure JNI cache is initialized
    init_methods_cache(env);
    
    ALOGI("Thumbnail | Converting frame to bitmap | Source: %dx%d | Target: %dpx | Quality: %s",
          frame->width, frame->height, target_dimension, get_quality_name(quality));
    
    // Calculate scaled dimensions while preserving aspect ratio
    // target_dimension is the maximum size for the largest side
    int width = frame->width;
    int height = frame->height;
    
    if (width > 0 && height > 0) {
        float scale = 1.0f;
        if (width >= height) {
            // Landscape or square: constrain width
            if (width > target_dimension) {
                scale = (float)target_dimension / width;
            }
        } else {
            // Portrait: constrain height
            if (height > target_dimension) {
                scale = (float)target_dimension / height;
            }
        }
        
        width = (int)(width * scale);
        height = (int)(height * scale);
    }
    
    // Ensure minimum dimensions
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    
    float scale_factor = (float)width / frame->width;
    ALOGD("Thumbnail | Scaling dimensions | Original: %dx%d → Scaled: %dx%d (%.1f%%)",
          frame->width, frame->height, width, height, scale_factor * 100);

    // Select scaling algorithm based on quality
    // FAST: SWS_FAST_BILINEAR - Fastest, lower quality
    // NORMAL: SWS_POINT - Good balance (current default)
    // HQ: SWS_LANCZOS - Best quality, slower
    int sws_algorithm;
    switch (quality) {
        case QUALITY_FAST:
            sws_algorithm = SWS_FAST_BILINEAR;
            break;
        case QUALITY_HQ:
            sws_algorithm = SWS_LANCZOS;
            break;
        case QUALITY_NORMAL:
        default:
            sws_algorithm = SWS_POINT;
            break;
    }
    
    ALOGD("Thumbnail | Using scaling algorithm: %s", get_scaling_algorithm_name(sws_algorithm));

    // Create SwsContext for scaling and format conversion
    // Android Bitmap.Config.ARGB_8888 expects BGRA byte order (little-endian)
    struct SwsContext *sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        width, height, AV_PIX_FMT_BGRA,
        sws_algorithm,
        NULL, NULL, NULL
    );
    
    if (!sws_ctx) {
        ALOGE("Thumbnail | ✗ Failed to create SwsContext");
        return NULL;
    }
    
    // Allocate output buffer
    int pixel_count = width * height;
    ALOGV("Thumbnail | Allocating buffer for %d pixels (%d bytes)", 
          pixel_count, pixel_count * 4);
    
    jintArray arr = env->NewIntArray(pixel_count);
    if (!arr) {
        ALOGE("Thumbnail | ✗ Failed to allocate int array for %d pixels", pixel_count);
        sws_freeContext(sws_ctx);
        return NULL;
    }
    
    jint *pixels = env->GetIntArrayElements(arr, NULL);
    if (!pixels) {
        ALOGE("Thumbnail | ✗ Failed to get array elements");
        env->DeleteLocalRef(arr);
        sws_freeContext(sws_ctx);
        return NULL;
    }
    
    // Setup destination buffer
    uint8_t *dst_data[4] = { (uint8_t*)pixels };
    int dst_linesize[4] = { width * 4 };
    
    ALOGV("Thumbnail | Starting pixel format conversion and scaling...");
    auto scale_start = std::chrono::high_resolution_clock::now();
    
    // Scale and convert
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
              dst_data, dst_linesize);
    
    auto scale_end = std::chrono::high_resolution_clock::now();
    auto scale_duration = std::chrono::duration_cast<std::chrono::milliseconds>(scale_end - scale_start);
    ALOGD("Thumbnail | Scaling completed in %lld ms", (long long)scale_duration.count());
    
    sws_freeContext(sws_ctx);
    
    // Create Android Bitmap
    env->ReleaseIntArrayElements(arr, pixels, 0);
    
    ALOGV("Thumbnail | Creating Android Bitmap object...");
    
    // Get the ARGB_8888 config object (not the field ID!)
    jobject bitmap_config = env->GetStaticObjectField(
        android_graphics_Bitmap_Config, 
        android_graphics_Bitmap_Config_ARGB_8888
    );
    
    if (!bitmap_config) {
        ALOGE("Thumbnail | ✗ Failed to get Bitmap.Config.ARGB_8888");
        env->DeleteLocalRef(arr);
        return NULL;
    }
    
    jobject bitmap = env->CallStaticObjectMethod(
        android_graphics_Bitmap, 
        android_graphics_Bitmap_createBitmap,
        arr, width, height, bitmap_config
    );
    
    if (env->ExceptionCheck()) {
        ALOGE("Thumbnail | ✗ Exception while creating bitmap");
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(arr);
        env->DeleteLocalRef(bitmap_config);
        return NULL;
    }
    
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(bitmap_config);
    
    auto conversion_end = std::chrono::high_resolution_clock::now();
    auto conversion_duration = std::chrono::duration_cast<std::chrono::milliseconds>(conversion_end - conversion_start);
    ALOGI("Thumbnail | ✓ Bitmap conversion successful | Time: %lld ms | Size: %dx%d",
          (long long)conversion_duration.count(), width, height);
    
    return bitmap;
}

jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension, jboolean use_hw_dec, jint quality) {
    auto total_start = std::chrono::high_resolution_clock::now();
    
    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    
    // Ensure JNI cache is initialized
    init_methods_cache(env);
    
    // Validate parameters
    if (dimension <= 0 || dimension > 4096) {
        ALOGE("Thumbnail | ✗ Invalid dimension %d (must be 1-4096)", dimension);
        return NULL;
    }
    
    if (position < 0.0) {
        ALOGE("Thumbnail | ✗ Invalid position %.2f (must be >= 0)", position);
        return NULL;
    }
    
    // Validate quality parameter
    if (quality < 0 || quality > 2) {
        ALOGW("Thumbnail | Invalid quality %d, using NORMAL (1)", quality);
        quality = 1;
    }
    
    const char *path = env->GetStringUTFChars(jpath, NULL);
    if (!path) {
        ALOGE("Thumbnail | ✗ Invalid path");
        return NULL;
    }
    
    ALOGI("════════════════════════════════════════════════════════════════");
    ALOGI("Thumbnail | Starting extraction");
    ALOGI("Thumbnail | Position: %.2fs | Dimension: %dpx | Quality: %s | HW Decode: %s",
          position, dimension, get_quality_name(quality), use_hw_dec ? "ON" : "OFF");
    ALOGD("Thumbnail | File: %s", path);
    
    // ========================================================================
    // STEP 1: Open video file
    // ========================================================================
    ALOGI("Thumbnail | [1/5] Opening video file...");
    auto step_start = std::chrono::high_resolution_clock::now();
    
    AVFormatContext *format_ctx = NULL;
    if (avformat_open_input(&format_ctx, path, NULL, NULL) < 0) {
        ALOGE("Thumbnail | ✗ Could not open file: %s", path);
        env->ReleaseStringUTFChars(jpath, path);
        return NULL;
    }
    
    auto step_end = std::chrono::high_resolution_clock::now();
    auto step_duration = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start);
    ALOGD("Thumbnail | ✓ File opened successfully in %lld ms", (long long)step_duration.count());
    
    env->ReleaseStringUTFChars(jpath, path);
    
    // Find stream information (analysis duration based on quality)
    // Directly set properties on context instead of passing dictionary array
    ALOGI("Thumbnail | [1/5] Analyzing stream info...");
    step_start = std::chrono::high_resolution_clock::now();
    
    switch (quality) {
        case QUALITY_FAST:
            // Minimal analysis for speed
            format_ctx->max_analyze_duration = 500000;   // 0.5 second max analysis
            format_ctx->probesize = 2000000;             // 2MB max probe size
            ALOGD("Thumbnail | Analysis params: duration=0.5s, probesize=2MB (FAST mode)");
            break;
        case QUALITY_HQ:
            // More thorough analysis for quality
            format_ctx->max_analyze_duration = 5000000;  // 5 seconds max analysis
            format_ctx->probesize = 10000000;            // 10MB max probe size
            ALOGD("Thumbnail | Analysis params: duration=5s, probesize=10MB (HQ mode)");
            break;
        case QUALITY_NORMAL:
        default:
            // Balanced analysis (current default)
            format_ctx->max_analyze_duration = 1000000;  // 1 second max analysis
            format_ctx->probesize = 5000000;             // 5MB max probe size
            ALOGD("Thumbnail | Analysis params: duration=1s, probesize=5MB (NORMAL mode)");
            break;
    }
    
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        ALOGE("Thumbnail | ✗ Could not find stream info");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    step_end = std::chrono::high_resolution_clock::now();
    step_duration = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start);
    ALOGD("Thumbnail | ✓ Stream info analyzed in %lld ms", (long long)step_duration.count());
    
    // ========================================================================
    // STEP 2: Find video stream
    // ========================================================================
    ALOGI("Thumbnail | [2/5] Finding video stream...");
    
    int video_stream_idx = -1;
    AVCodecParameters *codec_params = NULL;
    
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            codec_params = format_ctx->streams[i]->codecpar;
            break;
        }
    }
    
    if (video_stream_idx == -1) {
        ALOGE("Thumbnail | ✗ Could not find video stream (total streams: %d)", format_ctx->nb_streams);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVStream *video_stream = format_ctx->streams[video_stream_idx];
    
    ALOGD("Thumbnail | ✓ Video stream found | Index: %d | Resolution: %dx%d | Format: %s",
          video_stream_idx, codec_params->width, codec_params->height,
          avcodec_get_name(codec_params->codec_id));
    
    // ========================================================================
    // STEP 3: Initialize codec
    // ========================================================================
    ALOGI("Thumbnail | [3/5] Initializing codec...");
    step_start = std::chrono::high_resolution_clock::now();
    
    // Use cached codec lookup for faster initialization
    const AVCodec *codec = get_cached_codec(codec_params->codec_id);
    if (!codec) {
        ALOGE("Thumbnail | ✗ Codec not found for codec_id: %d", codec_params->codec_id);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    ALOGD("Thumbnail | Codec: %s (%s)", codec->name, codec->long_name);
    
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        ALOGE("Thumbnail | ✗ Could not allocate codec context");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        ALOGE("Thumbnail | ✗ Could not copy codec params");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // OPTIMIZATION: Configure decoder based on quality level
    ALOGD("Thumbnail | Configuring decoder for %s quality...", get_quality_name(quality));
    switch (quality) {
        case QUALITY_FAST:
            // Maximize speed, minimize quality
            codec_ctx->thread_count = 0;  // Let FFmpeg auto-select (often faster than 1)
            codec_ctx->thread_type = FF_THREAD_SLICE;
            codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
            codec_ctx->skip_frame = AVDISCARD_NONREF;  // Skip non-reference frames
            codec_ctx->skip_idct = AVDISCARD_BIDIR;    // Skip some decoding steps
            codec_ctx->skip_loop_filter = AVDISCARD_ALL;  // Skip loop filter
            // Additional fast mode optimizations
            codec_ctx->export_side_data = 0;  // Don't export side data
            codec_ctx->err_recognition = 0;   // Disable error recognition for speed
            ALOGD("Thumbnail | Decoder: auto threads, slice threading, aggressive optimizations");
            break;
            
        case QUALITY_HQ:
            // Maximize quality, accept slower speed
            codec_ctx->thread_count = 4;  // More threads for better quality processing
            codec_ctx->thread_type = FF_THREAD_FRAME;  // Frame threading for quality
            // Don't set fast flags for HQ
            codec_ctx->skip_frame = AVDISCARD_NONE;
            codec_ctx->skip_idct = AVDISCARD_NONE;
            codec_ctx->skip_loop_filter = AVDISCARD_NONE;
            ALOGD("Thumbnail | Decoder: 4 threads, frame threading, full quality decode");
            break;
            
        case QUALITY_NORMAL:
        default:
            // Balanced settings (current default)
            codec_ctx->thread_count = 2;  // 2 threads optimal for thumbnails
            codec_ctx->thread_type = FF_THREAD_SLICE;  // Slice threading faster for single frames
            codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
            ALOGD("Thumbnail | Decoder: 2 threads, slice threading, balanced settings");
            break;
    }
    
    // OPTIMIZATION: Enable hardware decoding if requested (using cached HW context)
    if (use_hw_dec) {
        ALOGD("Thumbnail | Attempting hardware acceleration...");
        
        // Use cached hardware device context (much faster than creating new one)
        if (init_hw_device_context()) {
            std::lock_guard<std::mutex> lock(g_hw_ctx_mutex);
            if (g_hw_device_ctx) {
                codec_ctx->hw_device_ctx = av_buffer_ref(g_hw_device_ctx);
                ALOGI("Thumbnail | ✓ Hardware acceleration enabled (cached MediaCodec context)");
            }
        } else {
            ALOGW("Thumbnail | Hardware acceleration unavailable, using software decode");
        }
    } else {
        ALOGD("Thumbnail | Hardware decoding disabled by request");
    }
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        ALOGE("Thumbnail | ✗ Could not open codec");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    step_end = std::chrono::high_resolution_clock::now();
    step_duration = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start);
    ALOGD("Thumbnail | ✓ Codec initialized in %lld ms", (long long)step_duration.count());
    
    // ========================================================================
    // STEP 4: Seek to position (strategy based on quality)
    // ========================================================================
    ALOGI("Thumbnail | [4/5] Seeking to position %.2fs...", position);
    step_start = std::chrono::high_resolution_clock::now();
    
    if (position > 0.0 && position < INT64_MAX / AV_TIME_BASE) {
        int64_t timestamp = (int64_t)(position * AV_TIME_BASE);
        
        // Smart seeking based on quality level
        int seek_flags;
        const char *seek_strategy;
        switch (quality) {
            case QUALITY_FAST:
                // Fastest seeking - any frame
                seek_flags = AVSEEK_FLAG_ANY;
                seek_strategy = "ANY (fastest)";
                break;
            case QUALITY_HQ:
                // Most accurate seeking - always backward to keyframe
                seek_flags = AVSEEK_FLAG_BACKWARD;
                seek_strategy = "BACKWARD (accurate)";
                break;
            case QUALITY_NORMAL:
            default:
                // Balanced: use BACKWARD for accuracy, ANY for speed on short seeks
                seek_flags = position < 5.0 ? AVSEEK_FLAG_ANY : AVSEEK_FLAG_BACKWARD;
                seek_strategy = position < 5.0 ? "ANY (short seek)" : "BACKWARD (long seek)";
                break;
        }
        
        ALOGD("Thumbnail | Seek strategy: %s", seek_strategy);
        
        // Seek to target frame using video stream index for better precision
        if (av_seek_frame(format_ctx, video_stream_idx, 
                          timestamp * video_stream->time_base.den / video_stream->time_base.num / AV_TIME_BASE,
                          seek_flags) < 0) {
            ALOGW("Thumbnail | Seek failed, using first available frame");
        } else {
            step_end = std::chrono::high_resolution_clock::now();
            step_duration = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start);
            ALOGD("Thumbnail | ✓ Seek completed in %lld ms", (long long)step_duration.count());
        }
        
        // Flush codec buffers after seek
        avcodec_flush_buffers(codec_ctx);
    } else {
        ALOGD("Thumbnail | Extracting from start of video (position %.2fs)", position);
    }
    
    // ========================================================================
    // STEP 5: Decode frame at position
    // ========================================================================
    ALOGI("Thumbnail | [5/5] Decoding frame...");
    step_start = std::chrono::high_resolution_clock::now();
    
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        ALOGE("Thumbnail | ✗ Failed to allocate packet");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        ALOGE("Thumbnail | ✗ Failed to allocate frame");
        av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVFrame *rgb_frame = NULL;
    jobject bitmap = NULL;
    
    bool frame_found = false;
    int frames_decoded = 0;
    int packets_read = 0;
    const int MAX_FRAMES = 300;  // Safety limit
    
    while (av_read_frame(format_ctx, packet) >= 0 && frames_decoded < MAX_FRAMES) {
        packets_read++;
        
        if (packet->stream_index == video_stream_idx) {
            // Send packet to decoder
            if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                // Receive decoded frame
                while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    frames_decoded++;
                    
                    // Calculate frame timestamp
                    double frame_time = 0.0;
                    if (frame->pts != AV_NOPTS_VALUE) {
                        frame_time = frame->pts * av_q2d(video_stream->time_base);
                    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        frame_time = frame->best_effort_timestamp * av_q2d(video_stream->time_base);
                    }
                    
                    // OPTIMIZATION: Skip frames that are too early (saves decoding time)
                    // Tolerance varies by quality
                    double skip_tolerance, match_tolerance;
                    switch (quality) {
                        case QUALITY_FAST:
                            skip_tolerance = 3.0;   // More aggressive skipping
                            match_tolerance = 2.0;  // Less precise matching
                            break;
                        case QUALITY_HQ:
                            skip_tolerance = 0.5;   // Minimal skipping
                            match_tolerance = 0.5;  // Precise matching
                            break;
                        case QUALITY_NORMAL:
                        default:
                            skip_tolerance = 1.5;   // Balanced skipping
                            match_tolerance = 1.0;  // Balanced matching
                            break;
                    }
                    
                    if (position > 0.0 && frame_time < position - skip_tolerance) {
                        // Still far from target, skip this frame
                        ALOGV("Thumbnail | Skipping frame at %.2fs (too early, target: %.2fs)",
                              frame_time, position);
                        av_frame_unref(frame);
                        continue;
                    }
                    
                    // Check if we've reached the desired position (with tolerance)
                    if (position == 0.0 || frame_time >= position - match_tolerance) {
                        ALOGI("Thumbnail | ✓ Found matching frame at %.2fs (target: %.2fs, tolerance: ±%.1fs)", 
                              frame_time, position, match_tolerance);
                        ALOGD("Thumbnail | Frame info | Type: %s | Size: %dx%d | Format: %d",
                              frame->key_frame ? "KEYFRAME" : "REGULAR",
                              frame->width, frame->height, frame->format);
                        
                        // Convert and create bitmap
                        bitmap = frame_to_bitmap(env, frame, dimension, quality);
                        if (bitmap) {
                            frame_found = true;
                        } else {
                            ALOGE("Thumbnail | ✗ Failed to convert frame to bitmap");
                        }
                        break;
                    }
                    
                    av_frame_unref(frame);
                }
            }
            
            if (frame_found) {
                av_packet_unref(packet);
                break;
            }
        }
        
        av_packet_unref(packet);
    }
    
    step_end = std::chrono::high_resolution_clock::now();
    step_duration = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start);
    ALOGD("Thumbnail | Decode stats | Packets: %d | Frames decoded: %d | Time: %lld ms",
          packets_read, frames_decoded, (long long)step_duration.count());
    
    // Cleanup
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    
    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start);
    
    if (!frame_found) {
        ALOGE("Thumbnail | ✗ FAILED | Could not find frame at position %.2fs", position);
        ALOGE("Thumbnail | Total time: %lld ms | Frames decoded: %d",
              (long long)total_duration.count(), frames_decoded);
        ALOGI("════════════════════════════════════════════════════════════════");
        return NULL;
    }
    
    ALOGI("Thumbnail | ✓ SUCCESS | Total time: %lld ms", (long long)total_duration.count());
    ALOGI("════════════════════════════════════════════════════════════════");
    return bitmap;
}
