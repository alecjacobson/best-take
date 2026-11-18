package io.github.alecjacobson.besttake.ui

import android.app.Application
import android.graphics.Bitmap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import io.github.alecjacobson.besttake.R
import io.github.alecjacobson.besttake.analysis.FaceAnalyzer
import io.github.alecjacobson.besttake.data.ProjectRepository
import io.github.alecjacobson.besttake.data.RevisionEntity
import io.github.alecjacobson.besttake.data.SourceEntity
import io.github.alecjacobson.besttake.data.SourceMissingException
import io.github.alecjacobson.besttake.engine.Association
import io.github.alecjacobson.besttake.engine.EngineJson
import io.github.alecjacobson.besttake.engine.FrameRecord
import io.github.alecjacobson.besttake.engine.NativeSession
import io.github.alecjacobson.besttake.export.Exporter
import io.github.alecjacobson.besttake.imaging.PixelRect
import io.github.alecjacobson.besttake.imaging.SourceImage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.serializer

/** A tappable face of one person in the base photo, in upright original pixels. */
data class FaceMarker(val person: Int, val centerX: Float, val centerY: Float, val radius: Float, val changed: Boolean)

data class CandidateUi(
    val frame: Int,
    val isOriginal: Boolean,
    val feasible: Boolean,
    val rejection: String,
    val thumbnail: Bitmap?,
    val selected: Boolean,
    val recommended: Boolean,
)

sealed interface Busy {
    data class Analyzing(val index: Int, val count: Int) : Busy
    data object Choosing : Busy
    data object Rendering : Busy
    data object Saving : Busy
}

data class EditorState(
    val busy: Busy? = Busy.Choosing,
    val fatalError: String? = null,
    val message: String? = null,
    val preview: Bitmap? = null,
    val original: Bitmap? = null,
    val imageWidth: Int = 0,
    val imageHeight: Int = 0,
    val markers: List<FaceMarker> = emptyList(),
    val selectedPerson: Int? = null,
    val candidates: List<CandidateUi> = emptyList(),
    val edits: Int = 0,
    val canUndo: Boolean = false,
    val canRedo: Boolean = false,
)

/**
 * Editing state for one project. The recipe revision is authoritative; the
 * preview is a cache of it. Results for an obsolete request are discarded.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class EditorViewModel(application: Application, private val projectId: Long) : AndroidViewModel(application) {
    private val repository = ProjectRepository(application)
    private val engineThread = Dispatchers.Default.limitedParallelism(1)  // the native session is single-threaded
    private val intList = ListSerializer(Int.serializer())

    private val _state = MutableStateFlow(EditorState())
    val state: StateFlow<EditorState> = _state

    private var sources: List<Pair<SourceEntity, SourceImage>> = emptyList()
    private var frames: List<FrameRecord> = emptyList()
    private var session: NativeSession? = null
    private var association: Association? = null
    private var previewSample = 1

    // The accepted revision.
    private var revision = 0
    private var latestRevision = 0
    private var base = -1
    private var chosen: List<Int> = emptyList()
    private var locks: Set<Int> = emptySet()
    private var recommended: List<Int> = emptyList()
    private var recipeJson = ""

    private var work: Job? = null

    init {
        work = viewModelScope.launch { open() }
    }

    private fun string(id: Int, vararg args: Any) = getApplication<Application>().getString(id, *args)

    private suspend fun open() {
        try {
            val project = repository.project(projectId) ?: error("Project $projectId does not exist")
            sources = repository.openSources(projectId)
            var analysisJson = project.analysisJson
            if (analysisJson == null) {
                analysisJson = analyze()
                repository.setAnalysis(projectId, analysisJson)
            }
            frames = EngineJson.decodeFromString(ListSerializer(FrameRecord.serializer()), analysisJson)
            if (frames.none { it.faces.isNotEmpty() }) {
                _state.update { it.copy(busy = null, fatalError = string(R.string.no_faces)) }
                return
            }
            _state.update { it.copy(busy = Busy.Choosing) }
            latestRevision = repository.latestRevision(projectId)
            withContext(engineThread) {
                val s = NativeSession(frames) { frame, x0, y0, x1, y1, sample ->
                    sources[frame].second.decodeRegionPixels(PixelRect(x0, y0, x1, y1), sample)
                }
                session = s
                association = s.association
            }
            val stored = if (project.currentRevision > 0) repository.revision(projectId, project.currentRevision) else null
            if (stored != null) {
                adopt(stored)
            } else {
                val (selection, recipe) = withContext(engineThread) {
                    val selection = session!!.propose()
                    check(selection.valid) { "No photo can serve as the base" }
                    selection to session!!.buildRecipe(selection.base, selection.source)
                }
                val first = RevisionEntity(
                    projectId = projectId, revision = 1, base = selection.base,
                    sourcesJson = EngineJson.encodeToString(intList, selection.source),
                    locksJson = "[]",
                    recommendedJson = EngineJson.encodeToString(intList, selection.source),
                    recipeJson = recipe, createdAtMillis = System.currentTimeMillis(),
                )
                repository.appendRevision(first)
                latestRevision = 1
                adopt(first)
            }
            renderAccepted(resetOriginal = true)
        } catch (e: CancellationException) {
            throw e
        } catch (e: SourceMissingException) {
            _state.update { it.copy(busy = null, fatalError = string(R.string.source_missing, e.displayName)) }
        } catch (e: Exception) {
            _state.update { it.copy(busy = null, fatalError = e.message ?: e.toString()) }
        }
    }

    private suspend fun analyze(): String = withContext(Dispatchers.Default) {
        FaceAnalyzer.verifyAssets(getApplication())
        FaceAnalyzer(getApplication()).use { analyzer ->
            val records = sources.mapIndexed { index, (entity, image) ->
                _state.update { it.copy(busy = Busy.Analyzing(index + 1, sources.size)) }
                FrameRecord(
                    id = "source_${entity.position}", hash = entity.sha256,
                    width = image.width, height = image.height, faces = analyzer.analyze(image),
                )
            }
            EngineJson.encodeToString(ListSerializer(FrameRecord.serializer()), records)
        }
    }

    private fun adopt(entity: RevisionEntity) {
        revision = entity.revision
        base = entity.base
        chosen = EngineJson.decodeFromString(intList, entity.sourcesJson)
        locks = EngineJson.decodeFromString(intList, entity.locksJson).toSet()
        recommended = EngineJson.decodeFromString(intList, entity.recommendedJson)
        recipeJson = entity.recipeJson
    }

    private fun baseImage() = sources[base].second

    private fun markers(): List<FaceMarker> {
        val personOf = association!!.personOf[base]
        return frames[base].faces.mapIndexed { face, record ->
            var minX = Float.MAX_VALUE
            var maxX = -Float.MAX_VALUE
            var minY = Float.MAX_VALUE
            var maxY = -Float.MAX_VALUE
            for (i in record.landmarks.indices step 2) {
                minX = minOf(minX, record.landmarks[i]); maxX = maxOf(maxX, record.landmarks[i])
                minY = minOf(minY, record.landmarks[i + 1]); maxY = maxOf(maxY, record.landmarks[i + 1])
            }
            val person = personOf[face]
            FaceMarker(
                person, (minX + maxX) / 2, (minY + maxY) / 2, 0.6f * maxOf(maxX - minX, maxY - minY),
                changed = chosen[person] >= 0 && chosen[person] != base,
            )
        }
    }

    /** Render the accepted recipe and publish it. */
    private suspend fun renderAccepted(resetOriginal: Boolean) {
        _state.update { it.copy(busy = Busy.Rendering) }
        val image = baseImage()
        previewSample = SourceImage.sampleSizeFor(image.width, image.height, PREVIEW_LONG_SIDE)
        val w = image.scaledWidth(previewSample)
        val h = image.scaledHeight(previewSample)
        val preview = withContext(engineThread) {
            val pixels = image.decodeFullPixels(previewSample)
            image.bitmapFromPixels(session!!.render(recipeJson, previewSample, pixels, w).pixels, w, h)
        }
        val original = if (resetOriginal || _state.value.original == null) {
            withContext(Dispatchers.IO) { image.decodeFull(previewSample) }
        } else {
            _state.value.original
        }
        val edits = chosen.count { it >= 0 && it != base }
        _state.update {
            it.copy(
                busy = null, preview = preview, original = original,
                imageWidth = image.width, imageHeight = image.height, markers = markers(), edits = edits,
                canUndo = revision > 1, canRedo = revision < latestRevision,
            )
        }
        _state.value.selectedPerson?.let { loadCandidates(it) }
    }

    fun selectPerson(person: Int?) {
        _state.update { it.copy(selectedPerson = person, candidates = emptyList()) }
        if (person != null) viewModelScope.launch { loadCandidates(person) }
    }

    private suspend fun loadCandidates(person: Int) {
        val requestRevision = revision
        val current = chosen[person].let { if (it < 0) base else it }
        val marker = markers().firstOrNull { it.person == person } ?: return
        val list = withContext(engineThread) {
            val image = baseImage()
            session!!.candidates(person, base).map { c ->
                var thumbnail: Bitmap? = null
                var feasible = c.feasible
                var rejection = c.rejection
                if (c.isBase) {
                    val r = marker.radius * 1.2f
                    val rect = PixelRect(
                        ((marker.centerX - r) / previewSample).toInt().coerceAtLeast(0),
                        ((marker.centerY - r) / previewSample).toInt().coerceAtLeast(0),
                        ((marker.centerX + r) / previewSample).toInt().coerceAtMost(image.scaledWidth(previewSample)),
                        ((marker.centerY + r) / previewSample).toInt().coerceAtMost(image.scaledHeight(previewSample)),
                    )
                    if (!rect.isEmpty) thumbnail = image.decodeRegion(rect, previewSample)
                } else if (c.feasible) {
                    // The thumbnail is the actual composed face, not the donor's own photo.
                    val patch = session!!.renderCandidate(person, c.frame, base, previewSample)
                    if (patch.passesValidation) {
                        thumbnail = image.bitmapFromPixels(patch.pixels, patch.rect.width, patch.rect.height)
                    } else {
                        feasible = false
                        rejection = string(R.string.quality_failed)
                    }
                }
                CandidateUi(
                    frame = c.frame, isOriginal = c.isBase, feasible = feasible, rejection = rejection,
                    thumbnail = thumbnail, selected = c.frame == current,
                    recommended = !c.isBase && recommended.getOrNull(person) == c.frame,
                )
            }
        }
        // Discard results computed for a revision or selection that is no longer current.
        if (requestRevision == revision && _state.value.selectedPerson == person) {
            _state.update { it.copy(candidates = list) }
        }
    }

    /** Choose [frame] as the expression source for [person]; the choice becomes a lock. */
    fun choose(person: Int, frame: Int) = edit {
        val next = chosen.toMutableList().also { it[person] = frame }
        commit(next, if (frame == base) locks - person else locks + person)
    }

    fun resetPerson(person: Int) = edit {
        commit(chosen.toMutableList().also { it[person] = base }, locks - person)
    }

    fun resetAll() = edit {
        commit(chosen.map { if (it >= 0) base else it }, emptySet())
    }

    private fun edit(block: suspend () -> Unit) {
        if (_state.value.busy != null) return
        work = viewModelScope.launch {
            try {
                block()
            } catch (e: CancellationException) {
                throw e
            } catch (e: IllegalArgumentException) {
                // The engine refused the request; the accepted revision stays as it was.
                _state.update { it.copy(busy = null, message = e.message) }
            } catch (e: Exception) {
                _state.update { it.copy(busy = null, message = e.message ?: e.toString()) }
            }
        }
    }

    private suspend fun commit(nextChosen: List<Int>, nextLocks: Set<Int>) {
        if (nextChosen == chosen) return
        _state.update { it.copy(busy = Busy.Rendering) }
        val image = baseImage()
        val (recipe, rendered) = withContext(engineThread) {
            val recipe = session!!.buildRecipe(base, nextChosen)
            val pixels = image.decodeFullPixels(previewSample)
            recipe to session!!.render(recipe, previewSample, pixels, image.scaledWidth(previewSample))
        }
        if (rendered.quality.any { !it.ok }) {
            // Keep the valid accepted image rather than showing a defective composite.
            _state.update { it.copy(busy = null, message = string(R.string.quality_failed)) }
            return
        }
        val entity = RevisionEntity(
            projectId = projectId, revision = revision + 1, base = base,
            sourcesJson = EngineJson.encodeToString(intList, nextChosen),
            locksJson = EngineJson.encodeToString(intList, nextLocks.sorted()),
            recommendedJson = EngineJson.encodeToString(intList, recommended),
            recipeJson = recipe, createdAtMillis = System.currentTimeMillis(),
        )
        repository.appendRevision(entity)
        latestRevision = entity.revision
        adopt(entity)
        val preview = image.bitmapFromPixels(
            rendered.pixels, image.scaledWidth(previewSample), image.scaledHeight(previewSample),
        )
        _state.update {
            it.copy(
                busy = null, preview = preview, markers = markers(),
                edits = chosen.count { c -> c >= 0 && c != base },
                canUndo = revision > 1, canRedo = revision < latestRevision,
            )
        }
        _state.value.selectedPerson?.let { loadCandidates(it) }
    }

    fun undo() = step(-1)

    fun redo() = step(+1)

    private fun step(delta: Int) = edit {
        val target = revision + delta
        if (target < 1 || target > latestRevision) return@edit
        val entity = repository.revision(projectId, target) ?: error("Revision $target is missing")
        val baseChanged = entity.base != base
        repository.setCurrentRevision(projectId, target)
        adopt(entity)
        renderAccepted(resetOriginal = baseChanged)
    }

    /** Render the accepted revision at full resolution and save it as a new image. */
    fun saveCopy() = edit {
        _state.update { it.copy(busy = Busy.Saving) }
        val image = baseImage()
        val savedRevision = revision
        val recipe = recipeJson
        withContext(engineThread) {
            val rendered = session!!.render(recipe, 1, image.decodeFullPixels(1), image.width)
            // Export uses exactly the accepted recipe; a full-resolution defect returns to review.
            require(rendered.quality.all { it.ok }) { string(R.string.quality_failed) }
            val bitmap = image.bitmapFromPixels(rendered.pixels, image.width, image.height)
            try {
                Exporter.saveCopy(getApplication(), bitmap, "BestTake_${projectId}_r$savedRevision.jpg")
            } finally {
                bitmap.recycle()
            }
        }
        _state.update { it.copy(busy = null, message = string(R.string.saved)) }
    }

    fun dismissMessage() = _state.update { it.copy(message = null) }

    override fun onCleared() {
        val s = session
        val opened = sources
        session = null
        // Release native state after any in-flight engine call has finished.
        kotlinx.coroutines.GlobalScope.launch(engineThread) {
            s?.close()
            opened.forEach { it.second.close() }
        }
    }

    class Factory(private val application: Application, private val projectId: Long) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = EditorViewModel(application, projectId) as T
    }

    private companion object {
        const val PREVIEW_LONG_SIDE = 2400
    }
}
