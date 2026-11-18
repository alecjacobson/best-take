package io.github.alecjacobson.besttake.imaging

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapRegionDecoder
import android.graphics.ColorSpace
import android.graphics.Matrix
import android.graphics.Rect
import java.io.File
import java.nio.IntBuffer

/**
 * A project-owned source photo, addressed in upright original coordinates.
 * Pixels are decoded by region so a full-resolution image never has to be
 * held for every source at once.
 */
class SourceImage(
    val file: File,
    val orientation: Int,
    val encodedWidth: Int,
    val encodedHeight: Int,
) : AutoCloseable {
    val width = Orientation.uprightWidth(orientation, encodedWidth, encodedHeight)
    val height = Orientation.uprightHeight(orientation, encodedWidth, encodedHeight)

    /** Color space pixels are decoded into and results are tagged with. */
    val colorSpace: ColorSpace by lazy {
        val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, options)
        val source = options.outColorSpace
        // The engine assumes the sRGB transfer curve; Display P3 shares it.
        if (source != null && source.model == ColorSpace.Model.RGB &&
            (source.isSrgb || source == ColorSpace.get(ColorSpace.Named.DISPLAY_P3))
        ) {
            source
        } else {
            ColorSpace.get(ColorSpace.Named.SRGB)
        }
    }

    private var decoder: BitmapRegionDecoder? = null

    private fun decoder(): BitmapRegionDecoder =
        decoder ?: (BitmapRegionDecoder.newInstance(file.path)
            ?: throw java.io.IOException("Cannot decode ${file.name}")).also { decoder = it }

    /** Size of the upright image at 1 / [sampleSize]. */
    fun scaledWidth(sampleSize: Int) = (width + sampleSize - 1) / sampleSize
    fun scaledHeight(sampleSize: Int) = (height + sampleSize - 1) / sampleSize

    /**
     * Decode the upright rectangle [rect], given in pixels at 1 / [sampleSize],
     * as an opaque ARGB_8888 bitmap of exactly that size in [colorSpace].
     */
    @Synchronized
    fun decodeRegion(rect: PixelRect, sampleSize: Int): Bitmap {
        require(sampleSize == 1 || sampleSize == 2 || sampleSize == 4 || sampleSize == 8) {
            "sampleSize must be 1, 2, 4, or 8"
        }
        require(!rect.isEmpty && rect.x0 >= 0 && rect.y0 >= 0 &&
            rect.x1 <= scaledWidth(sampleSize) && rect.y1 <= scaledHeight(sampleSize)) {
            "Region $rect is outside ${file.name} at 1/$sampleSize"
        }
        val upright = PixelRect(
            rect.x0 * sampleSize, rect.y0 * sampleSize,
            minOf(width, rect.x1 * sampleSize), minOf(height, rect.y1 * sampleSize),
        )
        val encoded = Orientation.uprightToEncoded(orientation, upright, encodedWidth, encodedHeight)
        val options = BitmapFactory.Options().apply {
            inSampleSize = sampleSize
            inPreferredConfig = Bitmap.Config.ARGB_8888
            inPreferredColorSpace = colorSpace
        }
        val raw = decoder().decodeRegion(Rect(encoded.x0, encoded.y0, encoded.x1, encoded.y1), options)
            ?: throw java.io.IOException("Cannot decode a region of ${file.name}")
        val (degrees, flip) = Orientation.rotationAndFlip(orientation)
        var result = raw
        if (degrees != 0 || flip) {
            val matrix = Matrix()
            matrix.postRotate(degrees.toFloat())
            if (flip) matrix.postScale(-1f, 1f)
            result = Bitmap.createBitmap(raw, 0, 0, raw.width, raw.height, matrix, true)
            if (result !== raw) raw.recycle()
        }
        if (result.width != rect.width || result.height != rect.height) {
            // Sub-sampled decodes may round the size differently by one pixel.
            val exact = Bitmap.createScaledBitmap(result, rect.width, rect.height, true)
            if (exact !== result) result.recycle()
            result = exact
        }
        return result
    }

    /** Raw ARGB_8888 memory of [decodeRegion], one int per pixel. */
    fun decodeRegionPixels(rect: PixelRect, sampleSize: Int): IntArray {
        val bitmap = decodeRegion(rect, sampleSize)
        val pixels = IntArray(rect.width * rect.height)
        bitmap.copyPixelsToBuffer(IntBuffer.wrap(pixels))
        bitmap.recycle()
        return pixels
    }

    /** The whole upright image at 1 / [sampleSize]. */
    fun decodeFull(sampleSize: Int): Bitmap =
        decodeRegion(PixelRect(0, 0, scaledWidth(sampleSize), scaledHeight(sampleSize)), sampleSize)

    /** Wrap engine output (raw ARGB_8888 memory) in a bitmap tagged with [colorSpace]. */
    fun bitmapFromPixels(pixels: IntArray, width: Int, height: Int): Bitmap {
        require(pixels.size == width * height) { "Pixel count does not match ${width}x$height" }
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888, false, colorSpace)
        bitmap.copyPixelsFromBuffer(IntBuffer.wrap(pixels))
        return bitmap
    }

    @Synchronized
    override fun close() {
        decoder?.recycle()
        decoder = null
    }

    companion object {
        /** Smallest supported sample size whose long side is at most [maxLongSide]. */
        fun sampleSizeFor(width: Int, height: Int, maxLongSide: Int): Int {
            var sample = 1
            while (sample < 8 && maxOf(width, height) / sample > maxLongSide) sample *= 2
            return sample
        }
    }
}
