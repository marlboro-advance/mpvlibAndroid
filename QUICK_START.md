# Quick Start: Fast Thumbnail Generation

## 30-Second Setup

### 1. Initialize (in `Application.onCreate()`)
```kotlin
class MyApp : Application() {
    override fun onCreate() {
        super.onCreate()
        FastThumbnails.initialize(this)
    }
}
```

### 2. Generate Thumbnails

#### Kotlin Coroutines (Recommended)
```kotlin
lifecycleScope.launch {
    val bitmap = FastThumbnails.generateAsync(
        path = "/sdcard/video.mp4",
        position = 10.0,  // seconds
        dimension = 256,  // Max dimension (aspect ratio preserved)
        useHwDec = true   // Enable hardware acceleration (default: true)
    )
    imageView.setImageBitmap(bitmap)
}
```

#### Java
```java
// In a background thread
Bitmap bitmap = FastThumbnails.generate(
    "/sdcard/video.mp4",
    10.0,   // seconds
    256,    // Max dimension
    true    // Enable hardware acceleration (default: true)
);
```

## Common Use Cases

### Video Gallery
```kotlin
class VideoGalleryAdapter : RecyclerView.Adapter<ViewHolder>() {
    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val video = videos[position]
        holder.scope.launch {
            val thumb = FastThumbnails.generate(video.path)
            holder.thumbnail.setImageBitmap(thumb)
        }
    }
}
```

### Seek Bar Preview
```kotlin
seekBar.setOnSeekBarChangeListener(object : OnSeekBarChangeListener {
    override fun onProgressChanged(seekBar: SeekBar, progress: Int, fromUser: Boolean) {
        if (fromUser) {
            lifecycleScope.launch {
                val position = progressToSeconds(progress)
                val preview = FastThumbnails.generate(videoPath, position, 128)
                previewImage.setImageBitmap(preview)
            }
        }
    }
})
```

## API Reference

| Method | Description | Returns |
|--------|-------------|---------|
| `initialize(context)` | One-time setup | `Unit` |
| `generate(path, pos, dim, hw)` | Sync generation | `Bitmap?` |
| `generateAsync(path, pos, dim, hw)` | Async with coroutines | `Bitmap?` |
| `generateMultiple(path, positions, dim, hw)` | Multiple positions | `List<Bitmap?>` |

### Parameters

- **path**: File path or URL (String)
- **position**: Time in seconds (Double, default: 0.0)
- **dimension**: Max dimension in pixels (Int, default: 512). Preserves aspect ratio.
- **useHwDec**: Enable hardware acceleration (Boolean, default: true)

### Returns
- `Bitmap?` - Success returns bitmap, failure returns `null`

## Best Practices

### ✅ DO
- Call `initialize()` once in `Application.onCreate()`
- Use `generateAsync()` for UI operations
- Cache thumbnails if generating multiple times
- Handle `null` returns gracefully
- Use appropriate dimensions (128-512 px)

### ❌ DON'T
- Call `generate()` on main thread
- Generate unnecessarily large thumbnails
- Forget to initialize before use
- Ignore null returns

## Error Handling

```kotlin
lifecycleScope.launch {
    try {
        val bitmap = FastThumbnails.generateAsync(path, position, useHwDec = true)
        if (bitmap != null) {
            imageView.setImageBitmap(bitmap)
        } else {
            // Show placeholder or error
            imageView.setImageResource(R.drawable.error_placeholder)
        }
    } catch (e: Exception) {
        Log.e("Thumbnail", "Failed to generate", e)
    }
}
```

## Performance Tips

1. **Smaller = Faster**: 128px thumbnails generate faster than 512px
2. **Cache Results**: Store bitmaps in `LruCache` for reuse
3. **Background Thread**: Always use `Dispatchers.IO` or `generateAsync()`
4. **Position Selection**: Use 10-20% into video to avoid intros

## Common Issues

| Problem | Solution |
|---------|----------|
| Returns `null` | Check file exists and is valid video |
| `IllegalStateException` | Call `initialize()` first |
| Slow generation | Reduce dimension, check file size/codec |
| Out of memory | Use smaller dimension, implement caching |

## Minimum Example

```kotlin
// Application.kt
class App : Application() {
    override fun onCreate() {
        super.onCreate()
        FastThumbnails.initialize(this)
    }
}

// Activity.kt
class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val imageView = ImageView(this)
        setContentView(imageView)
        
        lifecycleScope.launch {
            val bitmap = FastThumbnails.generateAsync("/sdcard/video.mp4", useHwDec = true)
            imageView.setImageBitmap(bitmap)
        }
    }
}
```

That's it! You're ready to generate fast thumbnails. 🚀

**Performance**: 50-100ms per thumbnail with hardware acceleration

For more examples, see [`THUMBNAIL_EXAMPLE.kt`](THUMBNAIL_EXAMPLE.kt)  
For detailed docs, see [`THUMBNAIL_GUIDE.md`](THUMBNAIL_GUIDE.md)
