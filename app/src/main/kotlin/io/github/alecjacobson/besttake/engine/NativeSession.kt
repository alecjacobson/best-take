package io.github.alecjacobson.besttake.engine

import androidx.annotation.Keep
import io.github.alecjacobson.besttake.imaging.PixelRect
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject

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

    /**
     * Render [recipeJson] at 1 / [sampleSize] onto [basePixels], the base
     * photo at that scale ([width] pixels per row), which is modified in
     * place. Only the edited faces are rendered natively, so no second copy
     * of the photo is needed and every other pixel stays exactly as decoded.
     */
    fun render(recipeJson: String, sampleSize: Int, basePixels: IntArray, width: Int): RenderedImage {
        require(width > 0 && basePixels.size % width == 0) { "Base pixels do not form rows of $width" }
        val height = basePixels.size / width
        val count = replacementCount(recipeJson)
        val reports = ArrayList<ReplacementQuality>(count)
        val rect = IntArray(4)
        val quality = arrayOfNulls<String>(1)
        for (index in 0 until count) {
            val patch = nativeRenderPatch(handle, recipeJson, index, sampleSize, rect, quality)
            val (x0, y0, x1, y1) = rect
            require(x0 >= 0 && y0 >= 0 && x1 <= width && y1 <= height && patch.size == (x1 - x0) * (y1 - y0)) {
                "Patch $index does not fit the base image"
            }
            for (y in y0 until y1) patch.copyInto(basePixels, y * width + x0, (y - y0) * (x1 - x0), (y - y0 + 1) * (x1 - x0))
            reports += EngineJson.decodeFromString(ListSerializer(ReplacementQuality.serializer()), quality[0]!!)
        }
        return RenderedImage(basePixels, reports)
    }

    private fun replacementCount(recipeJson: String): Int {
        val root = try {
            EngineJson.parseToJsonElement(recipeJson) as? JsonObject
        } catch (e: SerializationException) {
            null
        }
        val replacements = root?.get("replacements") as? JsonArray
        return requireNotNull(replacements) { "Not a recipe" }.size
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
        @JvmStatic external fun nativeRenderPatch(
            handle: Long, recipeJson: String, index: Int, sampleSize: Int, rectOut: IntArray, qualityOut: Array<String?>,
        ): IntArray
        @JvmStatic external fun nativeRenderCandidate(
            handle: Long, person: Int, donor: Int, base: Int, sampleSize: Int, rectOut: IntArray, okOut: BooleanArray,
        ): IntArray
    }
}
