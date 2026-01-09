package `is`.xyz.mpv

import android.content.Context
import android.graphics.Bitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean

object FastThumbnails {
    private val initialized = AtomicBoolean(false)
    
    /**
     * Initialize the fast thumbnail system.
     * Call this once before generating thumbnails (typically in Application.onCreate).
     * 
     * @param context Application context
     */
    @JvmStatic
    fun initialize(context: Context) {
        if (initialized.compareAndSet(false, true)) {
            MPVLib.setThumbnailJavaVM(context.applicationContext)
        }
    }
    
    /**
     * Check if initialized.
     */
    @JvmStatic
    fun isInitialized(): Boolean = initialized.get()
    
    /**
     * Clear internal codec cache and hardware context.
     * Call this when you want to free up memory (e.g., onLowMemory callback).
     * The cache will be rebuilt automatically on next thumbnail generation.
     * 
     * Note: Clearing the cache will make the next thumbnail generation slightly slower
     * as codecs need to be re-initialized, but subsequent calls will be fast again.
     */
    @JvmStatic
    fun clearCache() {
        if (initialized.get()) {
            MPVLib.clearThumbnailCache()
        }
    }
    
    /**
     * Generate thumbnail using fast FFmpeg direct API
     * 
     * @param path File path or URL to the video
     * @param position Time position in seconds (default: 0.0)
     * @param dimension Max dimension for longest side (width or height) in pixels (default: 512)
     * @param useHwDec Whether to use hardware acceleration if available (default: true)
     * @param quality Quality preset for thumbnail generation (default: NORMAL)
     * @return Bitmap thumbnail, or null if generation fails
     * @throws IllegalStateException if not initialized
     */
    @JvmStatic
    @JvmOverloads
    fun generate(
        path: String,
        position: Double = 0.0,
        dimension: Int = 512,
        useHwDec: Boolean = true,
        quality: ThumbnailQuality = ThumbnailQuality.NORMAL
    ): Bitmap? {
        check(initialized.get()) {
            "FastThumbnails not initialized. Call initialize(context) first."
        }
        
        require(dimension in 1..4096) {
            "Dimension must be between 1 and 4096 (got $dimension)"
        }
        
        return try {
            MPVLib.grabThumbnailFast(path, position, dimension, useHwDec, quality.value)
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }
    
    /**
     * Generate thumbnail asynchronously (IO dispatcher).
     * 
     * @param path File path or URL
     * @param position Time position in seconds (default: 0.0)
     * @param dimension Max dimension for longest side (width or height) in pixels (default: 512)
     * @param useHwDec Whether to use hardware acceleration if available (default: true)
     * @param quality Quality preset for thumbnail generation (default: NORMAL)
     * @return Bitmap thumbnail, or null
     */
    suspend fun generateAsync(
        path: String,
        position: Double = 0.0,
        dimension: Int = 512,
        useHwDec: Boolean = true,
        quality: ThumbnailQuality = ThumbnailQuality.NORMAL
    ): Bitmap? = withContext(Dispatchers.IO) {
        generate(path, position, dimension, useHwDec, quality)
    }
    
    /**
     * Generate multiple thumbnails at different positions.
     * 
     * @param path File path
     * @param positions List of time positions
     * @param dimension Max dimension for longest side (width or height) in pixels (default: 512)
     * @param useHwDec Whether to use hardware acceleration if available (default: true)
     * @param quality Quality preset for thumbnail generation (default: NORMAL)
     * @return List of bitmaps (may contain nulls)
     */
    @JvmStatic
    @JvmOverloads
    fun generateMultiple(
        path: String,
        positions: List<Double>,
        dimension: Int = 512,
        useHwDec: Boolean = true,
        quality: ThumbnailQuality = ThumbnailQuality.NORMAL
    ): List<Bitmap?> {
        return positions.map { position ->
            generate(path, position, dimension, useHwDec, quality)
        }
    }
    
    /**
     * Generate multiple thumbnails asynchronously.
     * 
     * @param path File path
     * @param positions List of positions
     * @param dimension Max dimension for longest side (width or height) in pixels (default: 512)
     * @param useHwDec Whether to use hardware acceleration if available (default: true)
     * @param quality Quality preset for thumbnail generation (default: NORMAL)
     * @return List of bitmaps
     */
    suspend fun generateMultipleAsync(
        path: String,
        positions: List<Double>,
        dimension: Int = 512,
        useHwDec: Boolean = true,
        quality: ThumbnailQuality = ThumbnailQuality.NORMAL
    ): List<Bitmap?> = withContext(Dispatchers.IO) {
        positions.map { position ->
            generate(path, position, dimension, useHwDec, quality)
        }
    }
    
    /**
     * Performance benchmark helper.
     * Generates a thumbnail and measures time taken.
     * 
     * @param path File path
     * @param position Time position in seconds (default: 0.0)
     * @param dimension Max dimension for longest side (width or height) in pixels (default: 512)
     * @param useHwDec Whether to use hardware acceleration if available (default: true)
     * @param quality Quality preset for thumbnail generation (default: NORMAL)
     * @return Pair of (bitmap, time in milliseconds)
     */
    @JvmStatic
    @JvmOverloads
    fun benchmark(
        path: String,
        position: Double = 0.0,
        dimension: Int = 512,
        useHwDec: Boolean = true,
        quality: ThumbnailQuality = ThumbnailQuality.NORMAL
    ): Pair<Bitmap?, Long> {
        val start = System.currentTimeMillis()
        val bitmap = generate(path, position, dimension, useHwDec, quality)
        val elapsed = System.currentTimeMillis() - start
        return Pair(bitmap, elapsed)
    }
}
