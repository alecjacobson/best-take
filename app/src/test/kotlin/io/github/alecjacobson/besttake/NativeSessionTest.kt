package io.github.alecjacobson.besttake

import io.github.alecjacobson.besttake.engine.EngineJson
import io.github.alecjacobson.besttake.engine.FrameRecord
import io.github.alecjacobson.besttake.engine.NativeSession
import io.github.alecjacobson.besttake.engine.RegionLoader
import kotlinx.serialization.builtins.ListSerializer
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Drives the real JNI bridge and engine (built for the host) with a
 * procedural photo set: two people, three frames, each person blinking in a
 * different frame.
 */
class NativeSessionTest {
    private val dir = File(System.getProperty("besttake.fixture")!!)
    private val frames: List<FrameRecord> =
        EngineJson.decodeFromString(ListSerializer(FrameRecord.serializer()), File(dir, "frames.json").readText())
    private val rgb = frames.indices.map { File(dir, "frame$it.rgb").readBytes() }
    private var loads = 0

    /** Pixels in ARGB_8888 memory order (red in the low byte), box-filtered for reduced scales. */
    private val loader = RegionLoader { frame, x0, y0, x1, y1, sample ->
        loads++
        val w = frames[frame].width
        val h = frames[frame].height
        val data = rgb[frame]
        IntArray((x1 - x0) * (y1 - y0)) { i ->
            val px = x0 + i % (x1 - x0)
            val py = y0 + i / (x1 - x0)
            val sum = IntArray(3)
            var n = 0
            for (sy in py * sample until minOf(h, (py + 1) * sample)) for (sx in px * sample until minOf(w, (px + 1) * sample)) {
                for (c in 0..2) sum[c] += data[(sy * w + sx) * 3 + c].toInt() and 0xff
                n++
            }
            (0xff shl 24) or ((sum[2] / n) shl 16) or ((sum[1] / n) shl 8) or (sum[0] / n)
        }
    }

    private fun pixel(frame: Int, x: Int, y: Int): Int {
        val o = (y * frames[frame].width + x) * 3
        val d = rgb[frame]
        return (0xff shl 24) or ((d[o + 2].toInt() and 0xff) shl 16) or ((d[o + 1].toInt() and 0xff) shl 8) or
            (d[o].toInt() and 0xff)
    }

    @Test
    fun proposesBuildsAndRendersThroughTheBridge() {
        NativeSession(frames, loader).use { session ->
            val association = session.association
            assertEquals(2, association.personCount)
            assertEquals(3, association.personOf.size)

            val selection = session.propose()
            assertTrue(selection.valid)
            assertTrue(selection.conflicts.isEmpty())
            assertTrue(selection.base == 0 || selection.base == 1)
            assertEquals(1, selection.source.count { it != selection.base })

            val recipe = session.buildRecipe(selection.base, selection.source)
            val rendered = session.render(recipe, 1)
            assertEquals(800 * 400, rendered.pixels.size)
            assertEquals(1, rendered.quality.size)
            assertTrue(rendered.quality.single().ok)

            // Pixels far from any face are the base photo, bit for bit; the edited face differs.
            val base = selection.base
            assertEquals(pixel(base, 5, 5), rendered.pixels[5 * 800 + 5])
            assertEquals(pixel(base, 795, 395), rendered.pixels[395 * 800 + 795])
            val changed = rendered.pixels.indices.count { rendered.pixels[it] != pixel(base, it % 800, it / 800) }
            assertTrue("changed pixels: $changed", changed in 500..40000)

            // The same recipe renders the same pixels again, and at preview scale.
            assertArrayEquals(rendered.pixels, session.render(recipe, 1).pixels)
            assertEquals(400 * 200, session.render(recipe, 2).pixels.size)
            assertTrue(loads > 0)
        }
    }

    @Test
    fun candidatesAndCandidatePatches() {
        NativeSession(frames, loader).use { session ->
            val person = session.association.personOf[0][0]
            val candidates = session.candidates(person, 0)
            assertEquals(3, candidates.size)
            assertTrue(candidates[0].isBase)
            assertEquals(0, candidates[0].frame)
            assertEquals(1, candidates[1].frame)
            assertTrue(candidates[1].feasible)

            val patch = session.renderCandidate(person, 1, 0, 1)
            assertTrue(patch.passesValidation)
            assertEquals(patch.rect.width * patch.rect.height, patch.pixels.size)
            assertTrue(patch.rect.x0 >= 0 && patch.rect.x1 <= 400)  // the left person's face
        }
    }

    @Test
    fun locksAreHonored() {
        NativeSession(frames, loader).use { session ->
            val person = session.association.personOf[0][0]
            val selection = session.propose(locks = mapOf(person to 2), lockedBase = 0)
            assertEquals(0, selection.base)
            assertEquals(2, selection.source[person])
        }
    }

    @Test
    fun invalidRequestsThrowInsteadOfSubstituting() {
        NativeSession(frames, loader).use { session ->
            assertThrows(IllegalArgumentException::class.java) { session.buildRecipe(0, listOf(1)) }
            assertThrows(IllegalArgumentException::class.java) { session.buildRecipe(9, listOf(0, 0)) }
            assertThrows(IllegalArgumentException::class.java) { session.candidates(5, 0) }
            assertThrows(IllegalArgumentException::class.java) { session.render("{}", 1) }
            val recipe = session.buildRecipe(0, listOf(0, 0))
            assertThrows(IllegalArgumentException::class.java) { session.render(recipe, 3) }
            assertThrows(IllegalArgumentException::class.java) { session.renderCandidate(0, 0, 0, 1) }
        }
        assertThrows(IllegalArgumentException::class.java) { NativeSession(emptyList(), loader) }
    }

    @Test
    fun loaderFailuresPropagate() {
        val failing = RegionLoader { _, _, _, _, _, _ -> throw java.io.IOException("source photo is missing") }
        NativeSession(frames, failing).use { session ->
            val e = assertThrows(java.io.IOException::class.java) { session.propose() }
            assertEquals("source photo is missing", e.message)
        }
        val wrongSize = RegionLoader { _, _, _, _, _, _ -> IntArray(3) }
        NativeSession(frames, wrongSize).use { session ->
            assertThrows(IllegalArgumentException::class.java) { session.propose() }
            assertFalse(session.association.personOf.isEmpty())  // the session stays usable
        }
    }
}
