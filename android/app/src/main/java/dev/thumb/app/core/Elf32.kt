package dev.thumb.app.core

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Minimal 32-bit ELF reader for armeabi-v7a libraries. Uses program headers
 * only (like the THUMB loader), so it also works on packed libraries whose
 * section headers are mangled.
 */
class Elf32(private val data: ByteArray) {
    class Info(val needed: List<String>, val imports: Map<String, Boolean /* weak */>, val exports: Set<String>)

    private val buf: ByteBuffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    private val loads = ArrayList<Triple<Long, Int, Int>>() // vaddr, offset, filesz
    private var dynamic = -1L

    init {
        require(data.size > 52 && data[0] == 0x7f.toByte() && data[1] == 'E'.code.toByte() && data[4] == 1.toByte()) {
            "not a 32-bit ELF"
        }
        val phoff = u32At(0x1C).toInt()
        val phentsize = u16At(0x2A)
        val phnum = u16At(0x2C)
        for (i in 0 until phnum) {
            val p = phoff + i * phentsize
            val type = u32At(p)
            val offset = u32At(p + 4).toInt()
            val vaddr = u32At(p + 8)
            val filesz = u32At(p + 16).toInt()
            when (type) {
                1L -> loads += Triple(vaddr, offset, filesz)
                2L -> dynamic = vaddr
            }
        }
        require(dynamic >= 0) { "no PT_DYNAMIC" }
    }

    private fun u16At(off: Int) = buf.getShort(off).toInt() and 0xFFFF
    private fun u32At(off: Int) = buf.getInt(off).toLong() and 0xFFFFFFFFL

    private fun off(vaddr: Long): Int {
        for ((v, o, sz) in loads) if (vaddr >= v && vaddr < v + sz) return (o + (vaddr - v)).toInt()
        throw IllegalArgumentException("address 0x${vaddr.toString(16)} not in file")
    }

    private fun u32(vaddr: Long) = u32At(off(vaddr))

    private fun cstr(vaddr: Long): String {
        val start = off(vaddr)
        var end = start
        while (end < data.size && data[end] != 0.toByte()) end++
        return String(data, start, end - start, Charsets.UTF_8)
    }

    fun parse(): Info {
        val tags = HashMap<Long, Long>()
        val neededOffsets = ArrayList<Long>()
        var a = dynamic
        while (true) {
            val tag = buf.getInt(off(a)).toLong()
            val value = u32At(off(a) + 4)
            if (tag == 0L) break
            if (tag == 1L) neededOffsets += value else tags[tag] = value
            a += 8
        }
        val strtab = tags[5L] ?: error("no DT_STRTAB")
        val symtab = tags[6L] ?: error("no DT_SYMTAB")
        val nsyms: Long = tags[4L]?.let { u32(it + 4) } ?: gnuHashCount(tags[0x6FFFFEF5L] ?: error("no symbol hash"))

        val imports = HashMap<String, Boolean>()
        val exports = HashSet<String>()
        for (i in 1 until nsyms) {
            val s = off(symtab + i * 16)
            val nameOff = u32At(s)
            if (nameOff == 0L) continue
            val info = data[s + 12].toInt() and 0xFF
            val shndx = u16At(s + 14)
            val name = cstr(strtab + nameOff)
            if (shndx == 0) imports[name] = (info shr 4) == 2 else exports += name
        }
        return Info(neededOffsets.map { cstr(strtab + it) }, imports, exports)
    }

    private fun gnuHashCount(gh: Long): Long {
        val nb = u32(gh)
        val symoff = u32(gh + 4)
        val bloom = u32(gh + 8)
        val buckets = gh + 16 + bloom * 4
        var last = 0L
        for (i in 0 until nb) last = maxOf(last, u32(buckets + 4 * i))
        if (last < symoff) return symoff
        val chains = buckets + nb * 4
        while (u32(chains + (last - symoff) * 4) and 1L == 0L) last++
        return last + 1
    }
}
