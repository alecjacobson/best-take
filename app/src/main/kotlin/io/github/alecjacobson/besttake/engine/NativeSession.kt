package io.github.alecjacobson.besttake.engine

import androidx.annotation.Keep
import io.github.alecjacobson.besttake.imaging.PixelRect
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.encodeToString

/** Supplies source pixels to the engine. Called on the thread using the session. */
@Keep
fun interface RegionLoader {
    /**
     * Raw ARGB_8888 pixels of [frame] for the upright rectangle
     * [x0, x1) x [y0, y1) expressed at 1 / [sampleSize] of the original.
     */
    fun loadRegion(frame: Int, x0: Int, y0: Int, x1: Int, y1: Int, sampleSize: Int): IntArray
}

class RenderedImage(val pixels: IntArray, val quality: List<ReplacementQuality>)

class RenderedPatch(val pixels: IntArray, val rect: PixelRect, val passesValidation: Boolean)

/**
 * The native editing session for one set of analyzed photos. Not thread-safe:
 * confine every call to a single thread. Invalid requests throw
 * [IllegalArgumentException]; the engine never substitutes a different source.
 */
class NativeSession(frames: List<FrameRecord>, loader: RegionLoader) : AutoCloseable {
    private var handle: Long =
        nativeCreate(EngineJson.encodeToString(ListSerializer(FrameRecord.serializer()), frames), loader)

    val association: Association
        get() = EngineJson.decodeFromString(Association.serializer(), nativeDescribe(handle))

    /** Recommended base and sources. [locks] maps person to donor frame; [lockedBase] is -1 for automatic. */
    fun propose(locks: Map<Int, Int> = emptyMap(), lockedBase: Int = -1): Selection {
        val persons = locks.keys.toIntArray()
        val frames = IntArray(persons.size) { locks.getValue(persons[it]) }
        return EngineJson.decodeFromString(Selection.serializer(), nativePropose(handle, persons, frames, lockedBase))
    }

    /** Every observation of [person], the base's own face first, then by increasing cost. */
    fun candidates(person: Int, base: Int): List<CandidateInfo> =
        EngineJson.decodeFromString(ListSerializer(CandidateInfo.serializer()), nativeCandidates(handle, person, base))

    /** Recipe (JSON) for explicit choices; [sources] holds one donor frame per person. */
    fun buildRecipe(base: Int, sources: List<Int>): String = nativeBuildRecipe(handle, base, sources.toIntArray())

    /** Render [recipeJson] at 1 / [sampleSize] of the base photo. */
    fun render(recipeJson: String, sampleSize: Int): RenderedImage {
        val quality = arrayOfNulls<String>(1)
        val pixels = nativeRender(handle, recipeJson, sampleSize, quality)
        val reports = EngineJson.decodeFromString(
            ListSerializer(ReplacementQuality.serializer()), quality[0] ?: "[]",
        )
        return RenderedImage(pixels, reports)
    }

    /** Render what placing [donor] for [person] into [base] would look like. */
    fun renderCandidate(person: Int, donor: Int, base: Int, sampleSize: Int): RenderedPatch {
        val rect = IntArray(4)
        val ok = BooleanArray(1)
        val pixels = nativeRenderCandidate(handle, person, donor, base, sampleSize, rect, ok)
        return RenderedPatch(pixels, PixelRect(rect[0], rect[1], rect[2], rect[3]), ok[0])
    }

    override fun close() {
        nativeDestroy(handle)
        handle = 0
    }

    private companion object {
        init {
            System.loadLibrary("besttake")
        }

        @JvmStatic external fun nativeCreate(framesJson: String, loader: RegionLoader): Long
        @JvmStatic external fun nativeDestroy(handle: Long)
        @JvmStatic external fun nativeDescribe(handle: Long): String
        @JvmStatic external fun nativeSetConstraints(handle: Long, constraintsJson: String)
        @JvmStatic external fun nativePropose(handle: Long, lockPersons: IntArray, lockFrames: IntArray, lockedBase: Int): String
        @JvmStatic external fun nativeCandidates(handle: Long, person: Int, base: Int): String
        @JvmStatic external fun nativeBuildRecipe(handle: Long, base: Int, sources: IntArray): String
        @JvmStatic external fun nativeRender(handle: Long, recipeJson: String, sampleSize: Int, qualityOut: Array<String?>): IntArray
        @JvmStatic external fun nativeRenderCandidate(
            handle: Long, person: Int, donor: Int, base: Int, sampleSize: Int, rectOut: IntArray, okOut: BooleanArray,
        ): IntArray
    }
}
