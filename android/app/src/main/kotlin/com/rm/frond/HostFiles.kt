package com.rm.frond

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import java.io.IOException
import java.io.OutputStream

// A picked document tree as a folder that card files and folders are copied into.
class DocumentHost(private val resolver: ContentResolver, private val tree: Uri, private val id: String) : CardFiles.HostFolder {
    private fun create(mime: String, name: String): Uri =
        DocumentsContract.createDocument(resolver, DocumentsContract.buildDocumentUriUsingTree(tree, id), mime, name)
            ?: throw IOException("Couldn't create $name there.")

    override fun folder(name: String): CardFiles.HostFolder =
        DocumentHost(resolver, tree, DocumentsContract.getDocumentId(create(DocumentsContract.Document.MIME_TYPE_DIR, name)))

    override fun file(name: String): OutputStream {
        val uri = create("application/octet-stream", name)
        return resolver.openOutputStream(uri, "wt") ?: throw IOException("Couldn't write $name there.")
    }

    companion object {
        fun root(resolver: ContentResolver, tree: Uri) = DocumentHost(resolver, tree, DocumentsContract.getTreeDocumentId(tree))
    }
}

object HostFiles {
    fun displayName(resolver: ContentResolver, uri: Uri): String? = try {
        resolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    } ?: uri.lastPathSegment?.substringAfterLast('/')

    fun size(resolver: ContentResolver, uri: Uri): Long? = try {
        resolver.query(uri, arrayOf(android.provider.OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else null
        }
    } catch (e: Exception) {
        null
    }
}
