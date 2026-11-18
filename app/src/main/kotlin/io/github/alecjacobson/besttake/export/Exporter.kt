package io.github.alecjacobson.besttake.export

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import java.io.IOException

/** Saves a finished composite as a new image; never overwrites a source. */
object Exporter {
    /**
     * Write [bitmap] as a JPEG copy in Pictures/BestTake. The entry becomes
     * visible only after it is completely written; on failure nothing is left.
     */
    fun saveCopy(context: Context, bitmap: Bitmap, displayName: String): Uri {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, displayName)
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/BestTake")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        val collection = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        val uri = resolver.insert(collection, values) ?: throw IOException("Cannot create the output image")
        try {
            (resolver.openOutputStream(uri) ?: throw IOException("Cannot open the output image")).use { out ->
                if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 95, out)) throw IOException("Cannot encode the image")
            }
            values.clear()
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            if (resolver.update(uri, values, null, null) != 1) throw IOException("Cannot finish the output image")
            return uri
        } catch (e: Throwable) {
            resolver.delete(uri, null, null)
            throw e
        }
    }
}
