#include <stdlib.h>
#include <string>
#include <mutex>
#include <stdint.h>

#include <jni.h>
#include <android/bitmap.h>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
    #include <libavcodec/jni.h>
};

#include "jni_utils.h"
#include "log.h"

extern "C" {
    jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension);
    jni_func(void, setThumbnailJavaVM, jobject appctx);
};

// ============================================================================
// FAST THUMBNAIL GENERATION USING DIRECT FFMPEG API
// Bypasses MPV entirely, uses FFmpeg API directly
// Expected performance: 50-100ms per thumbnail
// ============================================================================

static JavaVM *g_thumb_vm = nullptr;
static jobject g_thumb_appctx = nullptr;
static std::mutex g_thumb_mutex;

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

// Convert AVFrame to Android Bitmap
static jobject frame_to_bitmap(JNIEnv *env, AVFrame *frame, int target_dimension) {
    // Ensure JNI cache is initialized
    init_methods_cache(env);
    
    // Create SwsContext for scaling and format conversion
    // Android Bitmap.Config.ARGB_8888 expects BGRA byte order (little-endian)
    // Use SWS_POINT for maximum speed (nearest neighbor - fastest possible)
    // Alternative: SWS_FAST_BILINEAR for better quality with slight speed cost
    struct SwsContext *sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        target_dimension, target_dimension, AV_PIX_FMT_BGRA,
        SWS_POINT,  // Fastest algorithm (nearest neighbor)
        NULL, NULL, NULL
    );
    
    if (!sws_ctx) {
        ALOGE("grabThumbnailFast: Failed to create SwsContext");
        return NULL;
    }
    
    // Allocate output buffer
    jintArray arr = env->NewIntArray(target_dimension * target_dimension);
    if (!arr) {
        ALOGE("grabThumbnailFast: Failed to allocate int array");
        sws_freeContext(sws_ctx);
        return NULL;
    }
    
    jint *pixels = env->GetIntArrayElements(arr, NULL);
    if (!pixels) {
        ALOGE("grabThumbnailFast: Failed to get array elements");
        env->DeleteLocalRef(arr);
        sws_freeContext(sws_ctx);
        return NULL;
    }
    
    // Setup destination buffer
    uint8_t *dst_data[4] = { (uint8_t*)pixels };
    int dst_linesize[4] = { target_dimension * 4 };
    
    // Scale and convert
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
              dst_data, dst_linesize);
    
    sws_freeContext(sws_ctx);
    
    // Create Android Bitmap
    env->ReleaseIntArrayElements(arr, pixels, 0);
    
    // Get the ARGB_8888 config object (not the field ID!)
    jobject bitmap_config = env->GetStaticObjectField(
        android_graphics_Bitmap_Config, 
        android_graphics_Bitmap_Config_ARGB_8888
    );
    
    if (!bitmap_config) {
        ALOGE("grabThumbnailFast: Failed to get Bitmap.Config.ARGB_8888");
        env->DeleteLocalRef(arr);
        return NULL;
    }
    
    jobject bitmap = env->CallStaticObjectMethod(
        android_graphics_Bitmap, 
        android_graphics_Bitmap_createBitmap,
        arr, target_dimension, target_dimension, bitmap_config
    );
    
    if (env->ExceptionCheck()) {
        ALOGE("grabThumbnailFast: Exception while creating bitmap");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(bitmap_config);
    
    return bitmap;
}

jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension) {
    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    
    // Ensure JNI cache is initialized
    init_methods_cache(env);
    
    // Validate parameters
    if (dimension <= 0 || dimension > 4096) {
        ALOGE("grabThumbnailFast: invalid dimension %d (must be 1-4096)", dimension);
        return NULL;
    }
    
    if (position < 0.0) {
        ALOGE("grabThumbnailFast: invalid position %.2f (must be >= 0)", position);
        return NULL;
    }
    
    const char *path = env->GetStringUTFChars(jpath, NULL);
    if (!path) {
        ALOGE("grabThumbnailFast: invalid path");
        return NULL;
    }
    
    ALOGV("grabThumbnailFast: Opening %s at position %.2f", path, position);
    
    // ========================================================================
    // STEP 1: Open video file with optimizations
    // ========================================================================
    AVFormatContext *format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        ALOGE("grabThumbnailFast: Could not allocate format context");
        env->ReleaseStringUTFChars(jpath, path);
        return NULL;
    }
    
    // Speed optimizations: limit probing
    format_ctx->probesize = 5000000;           // Limit probe size (5MB instead of default)
    format_ctx->max_analyze_duration = 5000000; // Limit analysis time (5 seconds)
    format_ctx->fps_probe_size = 3;            // Only probe 3 frames for FPS
    
    if (avformat_open_input(&format_ctx, path, NULL, NULL) < 0) {
        ALOGE("grabThumbnailFast: Could not open file: %s", path);
        env->ReleaseStringUTFChars(jpath, path);
        avformat_free_context(format_ctx);
        return NULL;
    }
    
    env->ReleaseStringUTFChars(jpath, path);
    
    // Find stream information with minimal analysis
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "analyzeduration", "2000000", 0);  // 2 seconds max
    av_dict_set(&opts, "probesize", "5000000", 0);        // 5MB max
    
    if (avformat_find_stream_info(format_ctx, &opts) < 0) {
        ALOGE("grabThumbnailFast: Could not find stream info");
        av_dict_free(&opts);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    av_dict_free(&opts);
    
    // ========================================================================
    // STEP 2: Find video stream
    // ========================================================================
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
        ALOGE("grabThumbnailFast: Could not find video stream");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVStream *video_stream = format_ctx->streams[video_stream_idx];
    
    // ========================================================================
    // STEP 3: Initialize codec
    // ========================================================================
    const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        ALOGE("grabThumbnailFast: Codec not found");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        ALOGE("grabThumbnailFast: Could not allocate codec context");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        ALOGE("grabThumbnailFast: Could not copy codec params");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // OPTIMIZATION: Configure for maximum software decoding speed
    codec_ctx->thread_count = 0;  // Auto-detect CPU cores
    codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;  // Enable both frame and slice threading
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    
    // OPTIMIZATION: Aggressive speed settings (software decoding)
    codec_ctx->skip_loop_filter = AVDISCARD_ALL;      // Skip loop filter (faster, slightly lower quality)
    codec_ctx->skip_idct = AVDISCARD_BIDIR;           // Skip some IDCT operations
    codec_ctx->skip_frame = AVDISCARD_NONREF;         // Skip non-reference frames
    codec_ctx->lowres = 0;                             // Set to 1-3 for even faster low-res decoding
    
    // Hardware acceleration DISABLED for consistent performance
    // Software decoding is often faster for single-frame extraction due to:
    // - No GPU context initialization overhead
    // - No memory transfer between CPU and GPU
    // - Better for batch operations on multi-core CPUs
    ALOGV("grabThumbnailFast: Using optimized software decoding");
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        ALOGE("grabThumbnailFast: Could not open codec");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // ========================================================================
    // STEP 4: Seek to position
    // ========================================================================
    if (position > 0.0 && position < INT64_MAX / AV_TIME_BASE) {
        int64_t timestamp = (int64_t)(position * AV_TIME_BASE);
        
        // Seek to nearest keyframe before position
        if (av_seek_frame(format_ctx, -1, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            ALOGW("grabThumbnailFast: Seek failed, using first frame");
        }
        
        // Flush codec buffers after seek
        avcodec_flush_buffers(codec_ctx);
    }
    
    // ========================================================================
    // STEP 5: Decode frame at position
    // ========================================================================
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        ALOGE("grabThumbnailFast: Failed to allocate packet");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        ALOGE("grabThumbnailFast: Failed to allocate frame");
        av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    AVFrame *rgb_frame = NULL;
    jobject bitmap = NULL;
    
    bool frame_found = false;
    int frames_decoded = 0;
    const int MAX_FRAMES = 300;  // Safety limit
    
    while (av_read_frame(format_ctx, packet) >= 0 && frames_decoded < MAX_FRAMES) {
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
                    
                    // Check if we've reached the desired position
                    if (position == 0.0 || frame_time >= position - 0.5) {
                        ALOGV("grabThumbnailFast: Found frame at %.2fs (target: %.2fs)", 
                              frame_time, position);
                        
                        // Convert and create bitmap
                        bitmap = frame_to_bitmap(env, frame, dimension);
                        if (bitmap) {
                            frame_found = true;
                        } else {
                            ALOGE("grabThumbnailFast: Failed to convert frame to bitmap");
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
    
    // Cleanup
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    
    if (!frame_found) {
        ALOGE("grabThumbnailFast: Could not find frame at position");
        return NULL;
    }
    
    ALOGV("grabThumbnailFast: Successfully generated thumbnail");
    return bitmap;
}

// ============================================================================
// OPTIMIZATION NOTES:
//
// This implementation is optimized for maximum speed with software decoding:
// 
// 1. Direct API access - No MPV initialization overhead
// 2. Minimal decoding - Only decode frames we need
// 3. SOFTWARE decoding - HW acceleration DISABLED for better single-frame performance
//    - No GPU context initialization overhead
//    - No CPU-GPU memory transfer latency
//    - Better for batch operations on multi-core CPUs
//    - More consistent performance across devices
// 4. Aggressive codec optimizations:
//    - Skip loop filter (AVDISCARD_ALL)
//    - Skip non-reference frames
//    - Dual threading (frame + slice)
//    - Fast decoding flags
// 5. Limited stream probing:
//    - 5MB probesize (vs default 50MB)
//    - 5s analysis duration
//    - 3 frames FPS probe
// 6. Fastest scaling - SWS_POINT (nearest neighbor)
// 7. Fast seeking - Seeks to keyframe, then decode forward
// 8. Thread parallelism - Multi-threaded frame + slice decoding
//
// Expected performance (software decoding):
// - H.264 1080p: 30-60ms
// - H.264 720p:  20-40ms
// - HEVC 1080p:  40-80ms
// - VP9:         50-100ms
//
// Performance varies by:
// - Device CPU cores and speed
// - Video codec and bitrate
// - File I/O speed (local vs network)
// ============================================================================
