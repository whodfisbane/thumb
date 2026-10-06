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
