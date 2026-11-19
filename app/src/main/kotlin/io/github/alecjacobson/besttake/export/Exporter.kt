package io.github.alecjacobson.besttake.export

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import androidx.exifinterface.media.ExifInterface
import java.io.File
import java.io.IOException

/** Saves a finished composite as a new image; never overwrites a source. */
object Exporter {
    /** Capture-time tags carried from the base photo; nothing else (no location) is copied. */
    private val dateTags = listOf(
        ExifInterface.TAG_DATETIME_ORIGINAL, ExifInterface.TAG_OFFSET_TIME_ORIGINAL,
        ExifInterface.TAG_SUBSEC_TIME_ORIGINAL, ExifInterface.TAG_DATETIME_DIGITIZED,
        ExifInterface.TAG_OFFSET_TIME_DIGITIZED, ExifInterface.TAG_SUBSEC_TIME_DIGITIZED,
        ExifInterface.TAG_DATETIME, ExifInterface.TAG_OFFSET_TIME, ExifInterface.TAG_SUBSEC_TIME,
    )

    /**
     * Write [bitmap] as a JPEG copy in the camera folder (DCIM/Camera), so it
     * appears in the phone's main photo library, dated like [basePhoto] so it
     * sits beside the originals. The entry becomes visible only after it is
     * completely written; on failure nothing is left.
     */
    fun saveCopy(context: Context, bitmap: Bitmap, displayName: String, basePhoto: File): Uri {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, displayName)
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_DCIM + "/Camera")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        val baseExif = ExifInterface(basePhoto)
        val collection = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        val uri = resolver.insert(collection, values) ?: throw IOException("Cannot create the output image")
        try {
            (resolver.openOutputStream(uri) ?: throw IOException("Cannot open the output image")).use { out ->
                if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 95, out)) throw IOException("Cannot encode the image")
            }
            (resolver.openFileDescriptor(uri, "rw") ?: throw IOException("Cannot open the output image")).use { fd ->
                val exif = ExifInterface(fd.fileDescriptor)
                for (tag in dateTags) baseExif.getAttribute(tag)?.let { exif.setAttribute(tag, it) }
                // Pixels are already upright.
                exif.setAttribute(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL.toString())
                exif.setAttribute(ExifInterface.TAG_SOFTWARE, "Best Take")
                exif.saveAttributes()
            }
            values.clear()
            baseExif.dateTimeOriginal?.let { values.put(MediaStore.Images.Media.DATE_TAKEN, it) }
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            if (resolver.update(uri, values, null, null) != 1) throw IOException("Cannot finish the output image")
            return uri
        } catch (e: Throwable) {
            resolver.delete(uri, null, null)
            throw e
        }
    }
}
