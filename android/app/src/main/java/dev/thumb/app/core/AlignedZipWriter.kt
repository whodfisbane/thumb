package dev.thumb.app.core

import java.io.FilterOutputStream
import java.io.OutputStream
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * ZipOutputStream that aligns the data of uncompressed (STORED) entries, like
 * `zipalign`: 4 bytes in general, 16 KB for native libraries. Padding goes into
 * an extra field (id 0xD935, the one zipalign uses).
 */
class AlignedZipWriter(out: OutputStream) : AutoCloseable {
    private class Counting(out: OutputStream) : FilterOutputStream(out) {
        var count = 0L
        override fun write(b: Int) {
            out.write(b)
            count++
        }
        override fun write(b: ByteArray, off: Int, len: Int) {
            out.write(b, off, len)
            count += len
        }
    }

    private val counting = Counting(out)
    private val zip = ZipOutputStream(counting)

    fun addStored(name: String, data: ByteArray, time: Long) {
        val alignment = if (name.endsWith(".so")) 16384 else 4
        val entry = ZipEntry(name)
        entry.method = ZipEntry.STORED
        entry.size = data.size.toLong()
        entry.compressedSize = data.size.toLong()
        entry.crc = CRC32().also { it.update(data) }.value
        entry.time = time
        val nameLen = name.toByteArray(Charsets.UTF_8).size
        val headerEnd = counting.count + 30 + nameLen
        var pad = ((alignment - (headerEnd % alignment)) % alignment).toInt()
        if (pad in 1..3) pad += alignment // an extra field needs at least 4 bytes
        if (pad > 0) {
            val extra = ByteArray(pad)
            extra[0] = 0x35; extra[1] = 0xD9.toByte()
            val payload = pad - 4
            extra[2] = (payload and 0xFF).toByte(); extra[3] = (payload shr 8).toByte()
            entry.extra = extra
        }
        zip.putNextEntry(entry)
        zip.write(data)
        zip.closeEntry()
    }

    fun addDeflated(name: String, data: ByteArray, time: Long) {
        val entry = ZipEntry(name)
        entry.method = ZipEntry.DEFLATED
        entry.time = time
        zip.putNextEntry(entry)
        zip.write(data)
        zip.closeEntry()
    }

    override fun close() = zip.close()
}
