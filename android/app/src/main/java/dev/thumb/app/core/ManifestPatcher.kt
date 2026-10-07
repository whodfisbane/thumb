package dev.thumb.app.core

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * In-place edits of a binary AndroidManifest.xml (AXML). Only changes values
 * of existing attributes, so no chunk sizes move.
 */
object ManifestPatcher {
    private const val RES_XML_START_ELEMENT = 0x0102
    private const val RES_XML_RESOURCE_MAP = 0x0180
    private const val RES_STRING_POOL = 0x0001
    private const val ATTR_TARGET_SDK = 0x01010270
    private const val ATTR_MIN_SDK = 0x0101020c
    private const val TYPE_INT_DEC = 0x10
    private const val TYPE_INT_BOOLEAN = 0x12
    private const val ATTR_EXTRACT_NATIVE_LIBS = 0x010104ea

    data class Result(val bytes: ByteArray, val oldTarget: Int?, val newTarget: Int?, val forcedExtractNativeLibs: Boolean = false)

    /**
     * Applies all THUMB edits: raise targetSdkVersion to [minTarget] and set
     * android:extractNativeLibs to true (THUMB's stub loads the original
     * library from a file next to it, so libs must be extracted).
     */
    fun patch(axml: ByteArray, minTarget: Int): Result {
        val r = raiseTargetSdk(axml, minTarget)
        val forced = forceExtractNativeLibs(r.bytes)
        return r.copy(forcedExtractNativeLibs = forced)
    }

    /** Flips an existing android:extractNativeLibs="false" to true, in place. */
    fun forceExtractNativeLibs(out: ByteArray): Boolean {
        var changed = false
        forEachAttribute(out) { b, a, resId, dataType ->
            if (resId == ATTR_EXTRACT_NATIVE_LIBS && dataType == TYPE_INT_BOOLEAN && b.getInt(a + 16) == 0) {
                b.putInt(a + 16, -1)
                changed = true
            }
        }
        return changed
    }

    private const val RES_XML_END_ELEMENT = 0x0103
    private const val ATTR_NAME = 0x01010003
    private const val TYPE_STRING = 0x03

    /**
     * Removes <uses-permission> (and -sdk-23) elements whose android:name is in
     * [names]. Element chunks are dropped and the document size fixed up; the
     * string pool is untouched (unused strings are harmless).
     */
    fun removePermissions(axml: ByteArray, names: Set<String>): Pair<ByteArray, List<String>> {
        val b = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN)
        var strings: List<String> = emptyList()
        var resMap = IntArray(0)
        val keep = java.io.ByteArrayOutputStream(axml.size)
        val headerSize = b.getShort(2).toInt() and 0xFFFF
        keep.write(axml, 0, headerSize)
        val removed = ArrayList<String>()
        var skipping = false
        var pos = headerSize
        while (pos + 8 <= axml.size) {
            val type = b.getShort(pos).toInt() and 0xFFFF
            val chunkHeader = b.getShort(pos + 2).toInt() and 0xFFFF
            val size = b.getInt(pos + 4)
            if (size <= 0) break
            var drop = false
            when (type) {
                RES_STRING_POOL -> strings = readStringPool(b, pos)
                RES_XML_RESOURCE_MAP -> resMap = IntArray((size - chunkHeader) / 4) { b.getInt(pos + chunkHeader + it * 4) }
                RES_XML_START_ELEMENT -> {
                    val ext = pos + chunkHeader
                    val element = strings.getOrNull(b.getInt(ext + 4))
                    if (element == "uses-permission" || element == "uses-permission-sdk-23") {
                        val attrStart = b.getShort(ext + 8).toInt() and 0xFFFF
                        val attrSize = b.getShort(ext + 10).toInt() and 0xFFFF
                        val attrCount = b.getShort(ext + 12).toInt() and 0xFFFF
                        for (i in 0 until attrCount) {
                            val a = ext + attrStart + i * attrSize
                            if (resMap.getOrNull(b.getInt(a + 4)) != ATTR_NAME) continue
                            val value = if ((axml[a + 15].toInt() and 0xFF) == TYPE_STRING) strings.getOrNull(b.getInt(a + 16))
                                else strings.getOrNull(b.getInt(a + 8))
                            if (value != null && value in names) {
                                drop = true
                                skipping = true
                                removed += value
                            }
                        }
                    }
                }
                RES_XML_END_ELEMENT -> if (skipping) {
                    drop = true // the matching end of a removed (childless) element
                    skipping = false
                }
            }
            if (!drop) keep.write(axml, pos, size)
            pos += size
        }
        val out = keep.toByteArray()
        ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN).putInt(4, out.size)
        return out to removed
    }

    /** Calls [fn] for every attribute of every element (buffer, attr offset, resource id, data type). */
    private fun forEachAttribute(out: ByteArray, fn: (ByteBuffer, Int, Int, Int) -> Unit) {
        val b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
        var resMap = IntArray(0)
        var pos = b.getShort(2).toInt() and 0xFFFF
        while (pos + 8 <= out.size) {
            val type = b.getShort(pos).toInt() and 0xFFFF
            val headerSize = b.getShort(pos + 2).toInt() and 0xFFFF
            val size = b.getInt(pos + 4)
            if (size <= 0) break
            if (type == RES_XML_RESOURCE_MAP) resMap = IntArray((size - headerSize) / 4) { b.getInt(pos + headerSize + it * 4) }
            if (type == RES_XML_START_ELEMENT) {
                val ext = pos + headerSize
                val attrStart = b.getShort(ext + 8).toInt() and 0xFFFF
                val attrSize = b.getShort(ext + 10).toInt() and 0xFFFF
                val attrCount = b.getShort(ext + 12).toInt() and 0xFFFF
                for (i in 0 until attrCount) {
                    val a = ext + attrStart + i * attrSize
                    val resId = resMap.getOrNull(b.getInt(a + 4)) ?: continue
                    fn(b, a, resId, out[a + 15].toInt() and 0xFF)
                }
            }
            pos += size
        }
    }

    /**
     * Raises android:targetSdkVersion on <uses-sdk> to at least [minTarget]
     * (Android 14+ refuses to install apps targeting very old SDKs).
     */
    fun raiseTargetSdk(axml: ByteArray, minTarget: Int): Result {
        val out = axml.copyOf()
        val b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
        var strings: List<String> = emptyList()
        var resMap = IntArray(0)
        var oldTarget: Int? = null
        var newTarget: Int? = null

        var pos = b.getShort(2).toInt() and 0xFFFF // skip the XML chunk header
        while (pos + 8 <= out.size) {
            val type = b.getShort(pos).toInt() and 0xFFFF
            val headerSize = b.getShort(pos + 2).toInt() and 0xFFFF
            val size = b.getInt(pos + 4)
            if (size <= 0) break
            when (type) {
                RES_STRING_POOL -> strings = readStringPool(b, pos)
                RES_XML_RESOURCE_MAP -> resMap = IntArray((size - headerSize) / 4) { b.getInt(pos + headerSize + it * 4) }
                RES_XML_START_ELEMENT -> {
                    val ext = pos + headerSize
                    val name = strings.getOrNull(b.getInt(ext + 4))
                    if (name == "uses-sdk") {
                        val attrStart = b.getShort(ext + 8).toInt() and 0xFFFF
                        val attrSize = b.getShort(ext + 10).toInt() and 0xFFFF
                        val attrCount = b.getShort(ext + 12).toInt() and 0xFFFF
                        for (i in 0 until attrCount) {
                            val a = ext + attrStart + i * attrSize
                            val nameIdx = b.getInt(a + 4)
                            val resId = resMap.getOrNull(nameIdx) ?: continue
                            val dataType = out[a + 15].toInt() and 0xFF
                            if (resId == ATTR_TARGET_SDK && dataType == TYPE_INT_DEC) {
                                oldTarget = b.getInt(a + 16)
                                if (oldTarget < minTarget) {
                                    b.putInt(a + 16, minTarget)
                                    newTarget = minTarget
                                }
                            }
                        }
                    }
                }
            }
            pos += size
        }
        return Result(out, oldTarget, newTarget)
    }

    private const val ATTR_LABEL = 0x01010001

    /**
     * Renames the app: appends [label] to the string pool and points
     * android:label at it on <application> and on every <activity> /
     * <activity-alias> that had the same label (the launcher shows the
     * launcher activity's label). Returns null if the manifest has a layout
     * this doesn't handle (styled string pool, no application label).
     */
    fun setLabel(axml: ByteArray, label: String): ByteArray? {
        val src = ByteBuffer.wrap(axml).order(ByteOrder.LITTLE_ENDIAN)
        val docHeader = src.getShort(2).toInt() and 0xFFFF
        val poolPos = docHeader
        if ((src.getShort(poolPos).toInt() and 0xFFFF) != RES_STRING_POOL) return null
        val poolHeader = src.getShort(poolPos + 2).toInt() and 0xFFFF
        val poolSize = src.getInt(poolPos + 4)
        val count = src.getInt(poolPos + 8)
        val styleCount = src.getInt(poolPos + 12)
        val flags = src.getInt(poolPos + 16)
        val stringsStart = src.getInt(poolPos + 20)
        if (styleCount != 0) return null
        val utf8 = flags and (1 shl 8) != 0

        // New pool: one more offset, the old string data, then the new string.
        val oldData = axml.copyOfRange(poolPos + stringsStart, poolPos + poolSize)
        val encoded = java.io.ByteArrayOutputStream().apply {
            if (utf8) {
                val bytes = label.toByteArray(Charsets.UTF_8)
                fun len(n: Int) = if (n > 0x7F) { write(0x80 or (n shr 8)); write(n and 0xFF) } else write(n)
                len(label.length); len(bytes.size); write(bytes); write(0)
            } else {
                val chars = label.toByteArray(Charsets.UTF_16LE)
                val n = label.length
                if (n > 0x7FFF) { write(0x80 or ((n shr 24) and 0x7F)); write((n shr 16) and 0xFF) } // never for a label
                write(n and 0xFF); write((n shr 8) and 0xFF); write(chars); write(0); write(0)
            }
        }.toByteArray()
        // Old string data may end in padding; the new string starts after it.
        var newOffset = oldData.size
        val dataSize = (oldData.size + encoded.size + 3) and 3.inv()
        val newStringsStart = stringsStart + 4
        val newPoolSize = newStringsStart + dataSize
        val pool = ByteBuffer.allocate(newPoolSize).order(ByteOrder.LITTLE_ENDIAN)
        pool.put(axml, poolPos, poolHeader)
        pool.putInt(4, newPoolSize)
        pool.putInt(8, count + 1)
        pool.putInt(20, newStringsStart)
        for (i in 0 until count) pool.putInt(poolHeader + i * 4, src.getInt(poolPos + poolHeader + i * 4))
        pool.putInt(poolHeader + count * 4, newOffset)
        pool.position(newStringsStart)
        pool.put(oldData)
        pool.put(encoded)
        val newIndex = count

        val rest = axml.copyOfRange(poolPos + poolSize, axml.size)
        val out = ByteArray(docHeader + newPoolSize + rest.size)
        System.arraycopy(axml, 0, out, 0, docHeader)
        System.arraycopy(pool.array(), 0, out, docHeader, newPoolSize)
        System.arraycopy(rest, 0, out, docHeader + newPoolSize, rest.size)
        val b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(4, out.size)

        // Point the labels at it.
        val strings = readStringPool(b, docHeader)
        var resMap = IntArray(0)
        var appLabel: Pair<Int, Int>? = null // (dataType, data) of the original application label
        var pos = docHeader
        var changed = 0
        while (pos + 8 <= out.size) {
            val type = b.getShort(pos).toInt() and 0xFFFF
            val headerSize = b.getShort(pos + 2).toInt() and 0xFFFF
            val size = b.getInt(pos + 4)
            if (size <= 0) break
            when (type) {
                RES_XML_RESOURCE_MAP -> resMap = IntArray((size - headerSize) / 4) { b.getInt(pos + headerSize + it * 4) }
                RES_XML_START_ELEMENT -> {
                    val ext = pos + headerSize
                    val name = strings.getOrNull(b.getInt(ext + 4))
                    if (name == "application" || name == "activity" || name == "activity-alias") {
                        val attrStart = b.getShort(ext + 8).toInt() and 0xFFFF
                        val attrSize = b.getShort(ext + 10).toInt() and 0xFFFF
                        val attrCount = b.getShort(ext + 12).toInt() and 0xFFFF
                        for (i in 0 until attrCount) {
                            val a = ext + attrStart + i * attrSize
                            if (resMap.getOrNull(b.getInt(a + 4)) != ATTR_LABEL) continue
                            val current = (out[a + 15].toInt() and 0xFF) to b.getInt(a + 16)
                            if (name == "application") appLabel = current
                            else if (current != appLabel) continue
                            b.putInt(a + 8, newIndex)
                            b.putShort(a + 12, 8)
                            out[a + 14] = 0
                            out[a + 15] = TYPE_STRING.toByte()
                            b.putInt(a + 16, newIndex)
                            changed++
                        }
                    }
                }
            }
            pos += size
        }
        return if (appLabel != null && changed > 0) out else null
    }

    private fun readStringPool(b: ByteBuffer, pos: Int): List<String> {
        val headerSize = b.getShort(pos + 2).toInt() and 0xFFFF
        val count = b.getInt(pos + 8)
        val flags = b.getInt(pos + 16)
        val stringsStart = b.getInt(pos + 20)
        val utf8 = flags and (1 shl 8) != 0
        return List(count) { i ->
            val off = pos + stringsStart + b.getInt(pos + headerSize + i * 4)
            if (utf8) {
                var p = off
                p += if (b.get(p).toInt() and 0x80 != 0) 2 else 1 // utf16 length
                var len = b.get(p).toInt() and 0xFF
                p++
                if (len and 0x80 != 0) {
                    len = ((len and 0x7F) shl 8) or (b.get(p).toInt() and 0xFF)
                    p++
                }
                String(b.array(), p, len, Charsets.UTF_8)
            } else {
                var len = b.getShort(off).toInt() and 0xFFFF
                var p = off + 2
                if (len and 0x8000 != 0) {
                    len = ((len and 0x7FFF) shl 16) or (b.getShort(p).toInt() and 0xFFFF)
                    p += 2
                }
                String(b.array(), p, len * 2, Charsets.UTF_16LE)
            }
        }
    }
}
