package io.github.alecjacobson.besttake.data

import android.content.Context
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.OpenableColumns
import androidx.exifinterface.media.ExifInterface
import io.github.alecjacobson.besttake.imaging.Orientation
import io.github.alecjacobson.besttake.imaging.SourceImage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.text.DateFormat
import java.util.Date

/** Thrown when a project-owned source copy can no longer be read. */
class SourceMissingException(val displayName: String) : IOException("Source photo is missing: $displayName")

/** Projects, their owned source copies, and their recipe revisions. */
class ProjectRepository(private val context: Context) {
    private val dao = AppDatabase.get(context).projects()

    val projects: Flow<List<ProjectEntity>> = dao.observeProjects()

    private fun projectDir(id: Long) = File(context.filesDir, "projects/$id")

    /**
     * Create a project from the selected photos, copying each into app
     * storage so the project can be reopened without the original grant.
     * Either the whole project is created or nothing is left behind.
     */
    suspend fun createProject(uris: List<Uri>): Long = withContext(Dispatchers.IO) {
        require(uris.size in MIN_PHOTOS..MAX_PHOTOS) { "A project needs $MIN_PHOTOS to $MAX_PHOTOS photos" }
        val staging = File(context.filesDir, "projects/staging-${System.nanoTime()}").apply { mkdirs() }
        try {
            val staged = uris.mapIndexed { index, uri -> stage(uri, index, staging) }
            val now = System.currentTimeMillis()
            val name = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(now))
            val id = dao.createProject(ProjectEntity(name = name, createdAtMillis = now)) { id ->
                staged.map { it.copy(projectId = id) }
            }
            val dir = projectDir(id)
            if (!staging.renameTo(dir)) {
                dao.deleteProject(id)
                throw IOException("Cannot create the project folder")
            }
            id
        } catch (e: Throwable) {
            staging.deleteRecursively()
            throw e
        }
    }

    private fun stage(uri: Uri, index: Int, dir: File): SourceEntity {
        val displayName = context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { if (it.moveToFirst()) it.getString(0) else null } ?: "photo ${index + 1}"
        val file = File(dir, "source_$index")
        val digest = MessageDigest.getInstance("SHA-256")
        (context.contentResolver.openInputStream(uri) ?: throw IOException("Cannot open $displayName")).use { input ->
            file.outputStream().use { output ->
                val buffer = ByteArray(1 shl 16)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    digest.update(buffer, 0, n)
                    output.write(buffer, 0, n)
                }
            }
        }
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) throw IOException("$displayName is not a supported photo")
        val exif = ExifInterface(file).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
        return SourceEntity(
            projectId = 0,
            position = index,
            fileName = file.name,
            displayName = displayName,
            sha256 = digest.digest().joinToString("") { "%02x".format(it) },
            encodedWidth = bounds.outWidth,
            encodedHeight = bounds.outHeight,
            orientation = if (exif in 1..8) exif else Orientation.NORMAL,
        )
    }

    suspend fun project(id: Long): ProjectEntity? = dao.project(id)

    /** Open the project's sources; throws [SourceMissingException] naming the first missing one. */
    suspend fun openSources(id: Long): List<Pair<SourceEntity, SourceImage>> = withContext(Dispatchers.IO) {
        dao.sources(id).map { entity ->
            val file = File(projectDir(id), entity.fileName)
            if (!file.canRead()) throw SourceMissingException(entity.displayName)
            entity to SourceImage(file, entity.orientation, entity.encodedWidth, entity.encodedHeight)
        }
    }

    suspend fun setAnalysis(id: Long, json: String) = dao.setAnalysis(id, json)

    suspend fun revision(id: Long, revision: Int): RevisionEntity? = dao.revision(id, revision)

    suspend fun latestRevision(id: Long): Int = dao.latestRevision(id) ?: 0

    suspend fun appendRevision(revision: RevisionEntity) = dao.appendRevision(revision)

    suspend fun setCurrentRevision(id: Long, revision: Int) = dao.setCurrentRevision(id, revision)

    suspend fun deleteProject(id: Long) = withContext(Dispatchers.IO) {
        dao.deleteProject(id)
        projectDir(id).deleteRecursively()
    }

    companion object {
        const val MIN_PHOTOS = 2
        const val MAX_PHOTOS = 20
    }
}
