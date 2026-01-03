package `is`.xyz.mpv

import android.content.Context
import android.graphics.Bitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Fast thumbnail generation using direct FFmpeg API.
 * 
 * High-performance thumbnail generation (30-80ms per thumbnail) that bypasses MPV 
 * and uses optimized FFmpeg software decoding for maximum speed.
 * 
 * Features:
 * - Optimized software decoding (HW acceleration disabled for better single-frame performance)
 * - Multi-threaded frame + slice decoding
 * - Aggressive codec optimizations (skip loop filter, non-ref frames)
 * - Limited stream probing for faster initialization
 * - Fastest scaling algorithm (nearest neighbor)
 * - Smart keyframe seeking
 * - Minimal overhead (no MPV initialization)
 * 
 * Usage:
 * ```kotlin
 * // Initialize once (in Application.onCreate)
 * FastThumbnails.initialize(applicationContext)
 * 
 * // Generate thumbnails
 * val bitmap = FastThumbnails.generate("/path/video.mp4", 10.0, 256)
 * 
 * // Or async with coroutines
 * val bitmap = FastThumbnails.generateAsync("/path/video.mp4", 10.0, 256)
 * ```
 */
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
     * Generate thumbnail using fast FFmpeg direct API (30-80ms).
     * Uses optimized software decoding for maximum speed.
     * 
     * @param path File path or URL to the video
     * @param position Time position in seconds (default: 0.0)
     * @param dimension Size of the square thumbnail (default: 256)
     * @return Bitmap thumbnail, or null if generation fails
     * @throws IllegalStateException if not initialized
     */
    @JvmStatic
    @JvmOverloads
    fun generate(
        path: String,
        position: Double = 0.0,
        dimension: Int = 256
    ): Bitmap? {
        check(initialized.get()) {
            "FastThumbnails not initialized. Call initialize(context) first."
        }
        
        return try {
            MPVLib.grabThumbnailFast(path, position, dimension)
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
     * @param dimension Thumbnail size (default: 256)
     * @return Bitmap or null
     */
    suspend fun generateAsync(
        path: String,
        position: Double = 0.0,
        dimension: Int = 256
    ): Bitmap? = withContext(Dispatchers.IO) {
        generate(path, position, dimension)
    }
    
    /**
     * Generate multiple thumbnails at different positions.
     * 
     * @param path File path
     * @param positions List of time positions
     * @param dimension Thumbnail size (default: 256)
     * @return List of bitmaps (may contain nulls)
     */
    @JvmStatic
    @JvmOverloads
    fun generateMultiple(
        path: String,
        positions: List<Double>,
        dimension: Int = 256
    ): List<Bitmap?> {
        return positions.map { position ->
            generate(path, position, dimension)
        }
    }
    
    /**
     * Generate multiple thumbnails asynchronously.
     * 
     * @param path File path
     * @param positions List of positions
     * @param dimension Thumbnail size (default: 256)
     * @return List of bitmaps
     */
    suspend fun generateMultipleAsync(
        path: String,
        positions: List<Double>,
        dimension: Int = 256
    ): List<Bitmap?> = withContext(Dispatchers.IO) {
        positions.map { position ->
            generate(path, position, dimension)
        }
    }
    
    /**
     * Performance benchmark helper.
     * Generates a thumbnail and measures time taken.
     * 
     * @return Pair of (bitmap, time in milliseconds)
     */
    @JvmStatic
    fun benchmark(path: String, position: Double = 0.0, dimension: Int = 256): Pair<Bitmap?, Long> {
        val start = System.currentTimeMillis()
        val bitmap = generate(path, position, dimension)
        val elapsed = System.currentTimeMillis() - start
        return Pair(bitmap, elapsed)
    }
}
