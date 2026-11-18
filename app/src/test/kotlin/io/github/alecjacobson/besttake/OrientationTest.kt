package io.github.alecjacobson.besttake

import io.github.alecjacobson.besttake.imaging.Orientation
import io.github.alecjacobson.besttake.imaging.PixelRect
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class OrientationTest {
    private val w = 40
    private val h = 30

    /** Apply "rotate clockwise by degrees, then flip horizontally" to an encoded point. */
    private fun rotateThenFlip(orientation: Int, x: Float, y: Float): Pair<Float, Float> {
        val (degrees, flip) = Orientation.rotationAndFlip(orientation)
        val (u, v, uprightWidth) = when (degrees) {
            0 -> Triple(x, y, w.toFloat())
            90 -> Triple(h - y, x, h.toFloat())
            180 -> Triple(w - x, h - y, w.toFloat())
            else -> Triple(y, w - x, h.toFloat())
        }
        return (if (flip) uprightWidth - u else u) to v
    }

    @Test
    fun rotationAndFlipAgreesWithPointMapping() {
        for (orientation in 1..8) {
            for ((x, y) in listOf(0f to 0f, 40f to 0f, 7.5f to 22.25f, 40f to 30f)) {
                val expected = Orientation.encodedToUpright(orientation, x, y, w, h)
                val actual = rotateThenFlip(orientation, x, y)
                assertEquals("orientation $orientation u", expected.first, actual.first, 1e-4f)
                assertEquals("orientation $orientation v", expected.second, actual.second, 1e-4f)
            }
        }
    }

    @Test
    fun uprightRectMapsToTheEncodedPixelsThatShowIt() {
        for (orientation in 1..8) {
            val uw = Orientation.uprightWidth(orientation, w, h)
            val uh = Orientation.uprightHeight(orientation, w, h)
            val upright = PixelRect(3, 5, 11, 9)
            val encoded = Orientation.uprightToEncoded(orientation, upright, w, h)
            assert(encoded.x0 >= 0 && encoded.y0 >= 0 && encoded.x1 <= w && encoded.y1 <= h)
            assertEquals(upright.width * upright.height, encoded.width * encoded.height)
            // Both corners of the encoded rectangle land on corners of the upright rectangle.
            val a = Orientation.encodedToUpright(orientation, encoded.x0.toFloat(), encoded.y0.toFloat(), w, h)
            val b = Orientation.encodedToUpright(orientation, encoded.x1.toFloat(), encoded.y1.toFloat(), w, h)
            assertEquals(upright.x0.toFloat(), minOf(a.first, b.first), 0f)
            assertEquals(upright.x1.toFloat(), maxOf(a.first, b.first), 0f)
            assertEquals(upright.y0.toFloat(), minOf(a.second, b.second), 0f)
            assertEquals(upright.y1.toFloat(), maxOf(a.second, b.second), 0f)
            // The whole upright image maps to the whole encoded image.
            assertEquals(PixelRect(0, 0, w, h), Orientation.uprightToEncoded(orientation, PixelRect(0, 0, uw, uh), w, h))
        }
    }

    @Test
    fun uprightSizeSwapsForQuarterTurns() {
        assertEquals(40, Orientation.uprightWidth(1, w, h))
        assertEquals(30, Orientation.uprightWidth(6, w, h))
        assertEquals(40, Orientation.uprightHeight(8, w, h))
    }

    @Test
    fun invalidOrientationIsRejected() {
        assertThrows(IllegalArgumentException::class.java) { Orientation.rotationAndFlip(0) }
        assertThrows(IllegalArgumentException::class.java) { Orientation.uprightToEncoded(9, PixelRect(0, 0, 1, 1), w, h) }
    }
}
