#include <stdlib.h>
#include <string>
#include <mutex>
#include <stdint.h>

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
    jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension, jboolean use_hw_dec);
    jni_func(void, setThumbnailJavaVM, jobject appctx);
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
    CHECK_MPV_INIT();
    
    // Ensure JNI cache is initialized
    init_methods_cache(env);

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
        if (mpv_command_node(g_mpv, &c, &result) < 0) {
            ALOGE("screenshot-raw command failed");
            return NULL;
        }
    }

    // extract relevant property data from the node map mpv returns
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
        ALOGE("extracting data failed");
        mpv_free_node_contents(&result);
        return NULL;
    }
    ALOGV("screenshot w:%d h:%d stride:%d", w, h, stride);

    // crop to square
    int crop_left = 0, crop_top = 0;
    int new_w = w, new_h = h;
    if (w > h) {
        crop_left = (w - h) / 2;
        new_w = h;
    } else {
        crop_top = (h - w) / 2;
        new_h = w;
    }
    ALOGV("cropped w:%u h:%u", new_w, new_h);

    uint8_t *new_data = reinterpret_cast<uint8_t*>(data->data);
    new_data += crop_left * sizeof(uint32_t); // move begin rightwards
    new_data += stride * crop_top; // move begin downwards

    // convert & scale to appropriate size
    struct SwsContext *ctx = sws_getContext(
        new_w, new_h, AV_PIX_FMT_BGR0,
        dimension, dimension, AV_PIX_FMT_RGB32,
        SWS_BICUBIC, NULL, NULL, NULL);
    if (!ctx) {
        mpv_free_node_contents(&result);
        return NULL;
    }

    jintArray arr = env->NewIntArray(dimension * dimension);
    jint *scaled = env->GetIntArrayElements(arr, NULL);

    uint8_t *src_p[4] = { new_data }, *dst_p[4] = { (uint8_t*) scaled };
    int src_stride[4] = { stride },
        dst_stride[4] = { (int) sizeof(jint) * dimension };
    sws_scale(ctx, src_p, src_stride, 0, new_h, dst_p, dst_stride);
    sws_freeContext(ctx);

    mpv_free_node_contents(&result); // frees data->data

    // create android.graphics.Bitmap
    env->ReleaseIntArrayElements(arr, scaled, 0);

    jobject bitmap_config =
        env->GetStaticObjectField(android_graphics_Bitmap_Config, android_graphics_Bitmap_Config_ARGB_8888);
    jobject bitmap =
        env->CallStaticObjectMethod(android_graphics_Bitmap, android_graphics_Bitmap_createBitmap,
        arr, dimension, dimension, bitmap_config);
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(bitmap_config);

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

    // Create SwsContext for scaling and format conversion
    // Android Bitmap.Config.ARGB_8888 expects BGRA byte order (little-endian)
    struct SwsContext *sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        width, height, AV_PIX_FMT_BGRA,
        SWS_POINT,  // Fastest algorithm - good quality for thumbnails
        NULL, NULL, NULL
    );
    
    if (!sws_ctx) {
        ALOGE("grabThumbnailFast: Failed to create SwsContext");
        return NULL;
    }
    
    // Allocate output buffer
    jintArray arr = env->NewIntArray(width * height);
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
    int dst_linesize[4] = { width * 4 };
    
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
        arr, width, height, bitmap_config
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

jni_func(jobject, grabThumbnailFast, jstring jpath, jdouble position, jint dimension, jboolean use_hw_dec) {
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
    // STEP 1: Open video file
    // ========================================================================
    AVFormatContext *format_ctx = NULL;
    if (avformat_open_input(&format_ctx, path, NULL, NULL) < 0) {
        ALOGE("grabThumbnailFast: Could not open file: %s", path);
        env->ReleaseStringUTFChars(jpath, path);
        return NULL;
    }
    
    env->ReleaseStringUTFChars(jpath, path);
    
    // Find stream information (with limited analysis for speed)
    // Directly set properties on context instead of passing dictionary array
    format_ctx->max_analyze_duration = 1000000; // 1 second max analysis
    format_ctx->probesize = 5000000;            // 5MB max probe size
    
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        ALOGE("grabThumbnailFast: Could not find stream info");
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
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
    
    // OPTIMIZATION: Configure for speed
    codec_ctx->thread_count = 2;  // 2-4 threads optimal for thumbnails
    codec_ctx->thread_type = FF_THREAD_SLICE;  // Slice threading faster for single frames
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    
    // OPTIMIZATION: Enable hardware decoding if requested
    if (use_hw_dec) {
        enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
        
        // Try to find hardware decoder (Android MediaCodec)
        hw_type = av_hwdevice_find_type_by_name("mediacodec");
        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            AVBufferRef *hw_device_ctx = NULL;
            if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0) >= 0) {
                codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                // Release the original reference (av_buffer_ref incremented it)
                av_buffer_unref(&hw_device_ctx);
                ALOGV("grabThumbnailFast: Hardware decoding enabled");
            }
        }
    } else {
        ALOGV("grabThumbnailFast: Hardware decoding disabled by request");
    }
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        ALOGE("grabThumbnailFast: Could not open codec");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }
    
    // ========================================================================
    // STEP 4: Seek to position (optimized strategy)
    // ========================================================================
    if (position > 0.0 && position < INT64_MAX / AV_TIME_BASE) {
        int64_t timestamp = (int64_t)(position * AV_TIME_BASE);
        
        // Smart seeking: use BACKWARD for accuracy, ANY for speed on short seeks
        int seek_flags = AVSEEK_FLAG_BACKWARD;
        if (position < 5.0) {  // For positions < 5s, seek directly to any frame
            seek_flags = AVSEEK_FLAG_ANY;
        }
        
        // Seek to target frame using video stream index for better precision
        if (av_seek_frame(format_ctx, video_stream_idx, 
                          timestamp * video_stream->time_base.den / video_stream->time_base.num / AV_TIME_BASE,
                          seek_flags) < 0) {
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
                    
                    // OPTIMIZATION: Skip frames that are too early (saves decoding time)
                    if (position > 0.0 && frame_time < position - 1.5) {
                        // Still far from target, skip this frame
                        av_frame_unref(frame);
                        continue;
                    }
                    
                    // Check if we've reached the desired position (with tolerance)
                    if (position == 0.0 || frame_time >= position - 1.0) {
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
