package io.github.alecjacobson.besttake.data

import android.content.Context
import androidx.room.Dao
import androidx.room.Database
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.Transaction
import kotlinx.coroutines.flow.Flow

@Entity(tableName = "projects")
data class ProjectEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val name: String,
    val createdAtMillis: Long,
    /** Analyzer output (JSON list of frames); null until analysis completes. */
    val analysisJson: String? = null,
    /** Revision currently shown; 0 before the first recipe exists. */
    val currentRevision: Int = 0,
)

/** A project-owned copy of one selected photo. */
@Entity(tableName = "sources", primaryKeys = ["projectId", "position"])
data class SourceEntity(
    val projectId: Long,
    val position: Int,
    val fileName: String,
    val displayName: String,
    val sha256: String,
    val encodedWidth: Int,
    val encodedHeight: Int,
    val orientation: Int,
)

/** An immutable recipe revision: the user's choices and the engine recipe that renders them. */
@Entity(tableName = "revisions", primaryKeys = ["projectId", "revision"])
data class RevisionEntity(
    val projectId: Long,
    val revision: Int,
    val base: Int,
    /** JSON list: donor frame per person. */
    val sourcesJson: String,
    /** JSON list of persons whose choice the user made explicitly. */
    val locksJson: String,
    /** JSON list: the automatic recommendation per person, for marking it in the chooser. */
    val recommendedJson: String,
    val recipeJson: String,
    val createdAtMillis: Long,
)

@Dao
abstract class ProjectDao {
    @Query("SELECT * FROM projects ORDER BY createdAtMillis DESC")
    abstract fun observeProjects(): Flow<List<ProjectEntity>>

    @Query("SELECT * FROM projects WHERE id = :id")
    abstract suspend fun project(id: Long): ProjectEntity?

    @Query("SELECT * FROM sources WHERE projectId = :id ORDER BY position")
    abstract suspend fun sources(id: Long): List<SourceEntity>

    @Query("SELECT * FROM revisions WHERE projectId = :id AND revision = :revision")
    abstract suspend fun revision(id: Long, revision: Int): RevisionEntity?

    @Query("SELECT MAX(revision) FROM revisions WHERE projectId = :id")
    abstract suspend fun latestRevision(id: Long): Int?

    @Insert
    protected abstract suspend fun insertProject(project: ProjectEntity): Long

    @Insert
    protected abstract suspend fun insertSources(sources: List<SourceEntity>)

    @Insert
    protected abstract suspend fun insertRevision(revision: RevisionEntity)

    @Query("UPDATE projects SET analysisJson = :json WHERE id = :id")
    abstract suspend fun setAnalysis(id: Long, json: String)

    @Query("UPDATE projects SET currentRevision = :revision WHERE id = :id")
    abstract suspend fun setCurrentRevision(id: Long, revision: Int)

    @Query("DELETE FROM revisions WHERE projectId = :id AND revision > :after")
    protected abstract suspend fun deleteRevisionsAfter(id: Long, after: Int)

    @Query("DELETE FROM revisions WHERE projectId = :id")
    protected abstract suspend fun deleteRevisions(id: Long)

    @Query("DELETE FROM sources WHERE projectId = :id")
    protected abstract suspend fun deleteSources(id: Long)

    @Query("DELETE FROM projects WHERE id = :id")
    protected abstract suspend fun deleteProjectRow(id: Long)

    @Transaction
    open suspend fun createProject(project: ProjectEntity, sources: (Long) -> List<SourceEntity>): Long {
        val id = insertProject(project)
        insertSources(sources(id))
        return id
    }

    /**
     * Append a revision after the project's current one, discarding any
     * undone revisions, and make it current.
     */
    @Transaction
    open suspend fun appendRevision(revision: RevisionEntity) {
        val project = project(revision.projectId) ?: error("Project ${revision.projectId} does not exist")
        require(revision.revision == project.currentRevision + 1) {
            "Revision ${revision.revision} does not follow current revision ${project.currentRevision}"
        }
        deleteRevisionsAfter(revision.projectId, project.currentRevision)
        insertRevision(revision)
        setCurrentRevision(revision.projectId, revision.revision)
    }

    @Transaction
    open suspend fun deleteProject(id: Long) {
        deleteRevisions(id)
        deleteSources(id)
        deleteProjectRow(id)
    }
}

@Database(entities = [ProjectEntity::class, SourceEntity::class, RevisionEntity::class], version = 1, exportSchema = false)
abstract class AppDatabase : RoomDatabase() {
    abstract fun projects(): ProjectDao

    companion object {
        @Volatile private var instance: AppDatabase? = null

        fun get(context: Context): AppDatabase = instance ?: synchronized(this) {
            instance ?: Room.databaseBuilder(context.applicationContext, AppDatabase::class.java, "besttake.db")
                .build().also { instance = it }
        }
    }
}
