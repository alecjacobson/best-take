package io.github.alecjacobson.besttake.imaging

/** Integer pixel rectangle, half-open: [x0, x1) x [y0, y1). */
data class PixelRect(val x0: Int, val y0: Int, val x1: Int, val y1: Int) {
    val width get() = x1 - x0
    val height get() = y1 - y0
    val isEmpty get() = x1 <= x0 || y1 <= y0
}

/**
 * Mapping between the encoded pixel grid of a file and its upright
 * presentation, for the eight EXIF orientations.
 */
object Orientation {
    const val NORMAL = 1

    private fun requireValid(orientation: Int) =
        require(orientation in 1..8) { "EXIF orientation must be in 1..8, got $orientation" }

    /** True when the upright image swaps the encoded width and height. */
    fun swapsAxes(orientation: Int): Boolean {
        requireValid(orientation)
        return orientation >= 5
    }

    fun uprightWidth(orientation: Int, encodedWidth: Int, encodedHeight: Int) =
        if (swapsAxes(orientation)) encodedHeight else encodedWidth

    fun uprightHeight(orientation: Int, encodedWidth: Int, encodedHeight: Int) =
        if (swapsAxes(orientation)) encodedWidth else encodedHeight

    /**
     * Rectangle of the encoded image that shows [upright], a rectangle of the
     * upright image. [encodedWidth] and [encodedHeight] are the stored size.
     */
    fun uprightToEncoded(orientation: Int, upright: PixelRect, encodedWidth: Int, encodedHeight: Int): PixelRect {
        requireValid(orientation)
        val w = encodedWidth
        val h = encodedHeight
        val (u0, v0, u1, v1) = upright
        return when (orientation) {
            1 -> PixelRect(u0, v0, u1, v1)
            2 -> PixelRect(w - u1, v0, w - u0, v1)
            3 -> PixelRect(w - u1, h - v1, w - u0, h - v0)
            4 -> PixelRect(u0, h - v1, u1, h - v0)
            5 -> PixelRect(v0, u0, v1, u1)
            6 -> PixelRect(v0, h - u1, v1, h - u0)
            7 -> PixelRect(w - v1, h - u1, w - v0, h - u0)
            else -> PixelRect(w - v1, u0, w - v0, u1)
        }
    }

    /**
     * Upright position of the encoded pixel-space point ([x], [y]).
     * Returned as (u, v) in upright coordinates.
     */
    fun encodedToUpright(orientation: Int, x: Float, y: Float, encodedWidth: Int, encodedHeight: Int): Pair<Float, Float> {
        requireValid(orientation)
        val w = encodedWidth.toFloat()
        val h = encodedHeight.toFloat()
        return when (orientation) {
            1 -> x to y
            2 -> (w - x) to y
            3 -> (w - x) to (h - y)
            4 -> x to (h - y)
            5 -> y to x
            6 -> (h - y) to x
            7 -> (h - y) to (w - x)
            else -> y to (w - x)
        }
    }

    /**
     * Rotation in degrees (clockwise) followed by an optional horizontal flip
     * that turns encoded pixels upright.
     */
    fun rotationAndFlip(orientation: Int): Pair<Int, Boolean> {
        requireValid(orientation)
        return when (orientation) {
            1 -> 0 to false
            2 -> 0 to true
            3 -> 180 to false
            4 -> 180 to true
            5 -> 90 to true
            6 -> 90 to false
            7 -> 270 to true
            else -> 270 to false
        }
    }
}
