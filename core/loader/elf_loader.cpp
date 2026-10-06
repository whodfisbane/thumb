#include "loader/elf_loader.h"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>

#include "cpu/cpu.h"
#include "memory/arena.h"
#include "thunks/thunks.h"

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif

namespace h32 {

namespace {

std::recursive_mutex g_modules_mutex;
std::vector<std::unique_ptr<Module>> g_modules;
// Directory of the library being loaded (siblings come from the same place).
thread_local std::string g_loading_dir;

struct DynInfo {
    gaddr symtab = 0, strtab = 0, hash = 0, gnu_hash = 0;
    gaddr rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    uint32_t pltrel = DT_REL;
    std::vector<uint32_t> needed;  // strtab offsets
};

uint32_t symbol_count(const Arena& m, const DynInfo& d) {
    if (d.hash) return m.read<uint32_t>(d.hash + 4);  // nchain
    if (d.gnu_hash) {
        // Walk GNU hash buckets/chains to find the highest symbol index.
        uint32_t nbuckets = m.read<uint32_t>(d.gnu_hash);
        uint32_t symoffset = m.read<uint32_t>(d.gnu_hash + 4);
        uint32_t bloom_size = m.read<uint32_t>(d.gnu_hash + 8);
        gaddr buckets = d.gnu_hash + 16 + bloom_size * 4;
        gaddr chains = buckets + nbuckets * 4;
        uint32_t last = 0;
        for (uint32_t b = 0; b < nbuckets; b++) last = std::max(last, m.read<uint32_t>(buckets + b * 4));
        if (last < symoffset) return symoffset;
        while (!(m.read<uint32_t>(chains + (last - symoffset) * 4) & 1)) last++;
        return last + 1;
    }
    return 0;
}

}  // namespace

std::optional<gaddr> Module::find(std::string_view sym) const {
    auto it = exports.find(std::string(sym));
    if (it == exports.end()) return std::nullopt;
    return it->second;
}

Module* load_module(const std::vector<uint8_t>& bytes, const std::string& name) {
    auto& m = mem();
    if (bytes.size() < sizeof(Elf32_Ehdr)) return nullptr;
    Elf32_Ehdr eh;
    std::memcpy(&eh, bytes.data(), sizeof eh);
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_machine != EM_ARM) {
        H32_ERROR("%s: not a 32-bit ARM ELF", name.c_str());
        return nullptr;
    }
    if (eh.e_phoff + size_t(eh.e_phnum) * sizeof(Elf32_Phdr) > bytes.size()) return nullptr;
    std::vector<Elf32_Phdr> ph(eh.e_phnum);
    std::memcpy(ph.data(), bytes.data() + eh.e_phoff, ph.size() * sizeof(Elf32_Phdr));

    uint32_t lo = UINT32_MAX, hi = 0;
    for (auto& p : ph)
        if (p.p_type == PT_LOAD) {
            lo = std::min(lo, p.p_vaddr & ~0xFFFu);
            hi = std::max(hi, p.p_vaddr + p.p_memsz);
        }
    if (lo >= hi) return nullptr;

    auto mod = std::make_unique<Module>();
    mod->name = name;
    gaddr start = m.alloc_image(hi - lo);
    mod->base = start - lo;
    mod->size = hi;
    const gaddr bias = mod->base;

    gaddr dynamic = 0;
    for (auto& p : ph) {
        if (p.p_type == PT_LOAD) {
            if (p.p_offset + p.p_filesz > bytes.size()) {
                H32_ERROR("%s: segment past end of file", name.c_str());
                return nullptr;
            }
            std::memcpy(m.ptr<uint8_t>(bias + p.p_vaddr), bytes.data() + p.p_offset, p.p_filesz);
            // .bss is already zero: fresh arena pages.
        } else if (p.p_type == PT_DYNAMIC) {
            dynamic = bias + p.p_vaddr;
        } else if (p.p_type == PT_ARM_EXIDX) {
            mod->exidx = bias + p.p_vaddr;
            mod->exidx_count = p.p_memsz / 8;
        }
    }
    if (!dynamic) {
        H32_ERROR("%s: no PT_DYNAMIC", name.c_str());
        return nullptr;
    }

    DynInfo d;
    for (gaddr a = dynamic;; a += 8) {
        int32_t tag = m.read<int32_t>(a);
        uint32_t val = m.read<uint32_t>(a + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_SYMTAB: d.symtab = bias + val; break;
        case DT_STRTAB: d.strtab = bias + val; break;
        case DT_HASH: d.hash = bias + val; break;
        case DT_GNU_HASH: d.gnu_hash = bias + val; break;
        case DT_REL: d.rel = bias + val; break;
        case DT_RELSZ: d.relsz = val; break;
        case DT_JMPREL: d.jmprel = bias + val; break;
        case DT_PLTRELSZ: d.pltrelsz = val; break;
        case DT_PLTREL: d.pltrel = val; break;
        case DT_INIT: mod->dt_init = bias + val; break;
        case DT_FINI: mod->dt_fini = bias + val; break;
        case DT_INIT_ARRAY: mod->init_array = bias + val; break;
        case DT_INIT_ARRAYSZ: mod->init_count = val / 4; break;
        case DT_FINI_ARRAY: mod->fini_array = bias + val; break;
        case DT_FINI_ARRAYSZ: mod->fini_count = val / 4; break;
        case DT_NEEDED: d.needed.push_back(val); break;
        case DT_RELA:
        case DT_RELASZ:
            H32_ERROR("%s: RELA relocations are not used on ARM32; file is unusual", name.c_str());
            break;
        default: break;
        }
    }
    if (!d.symtab || !d.strtab) return nullptr;

    // Exported symbols.
    const uint32_t nsyms = symbol_count(m, d);
    for (uint32_t i = 1; i < nsyms; i++) {
        Elf32_Sym s = m.read<Elf32_Sym>(d.symtab + i * sizeof(Elf32_Sym));
        if (s.st_shndx == SHN_UNDEF || !s.st_name) continue;
        const char* sn = m.str(d.strtab + s.st_name);
        mod->exports.emplace(sn, bias + s.st_value);
        if (ELF32_ST_TYPE(s.st_info) == STT_FUNC) mod->sorted_symbols.emplace_back((bias + s.st_value) & ~1u, sn);
    }
    std::sort(mod->sorted_symbols.begin(), mod->sorted_symbols.end());

    // Sibling libraries the app ships itself must be loaded first so their
    // exports can satisfy our imports. System libraries are served by thunks.
    for (uint32_t off : d.needed) {
        std::string dep = m.str(d.strtab + off);
        if (g_loading_dir.empty() || find_module(dep)) {
            if (Module* already = find_module(dep)) mod->deps.push_back(already);
            continue;
        }
        if (Module* sib = load_sibling(g_loading_dir, dep)) {
            H32_DEBUG("%s needs %s: loaded from the app", name.c_str(), dep.c_str());
            mod->deps.push_back(sib);
        }
    }

    // Symbol resolution: own definition, then other modules, then thunks.
    std::unordered_map<uint32_t, gaddr> cache;
    size_t unresolved = 0;
    auto resolve_sym = [&](uint32_t idx) -> gaddr {
        if (auto it = cache.find(idx); it != cache.end()) return it->second;
        Elf32_Sym s = m.read<Elf32_Sym>(d.symtab + idx * sizeof(Elf32_Sym));
        const char* sn = m.str(d.strtab + s.st_name);
        gaddr v = 0;
        if (s.st_shndx != SHN_UNDEF) {
            v = bias + s.st_value;
        } else {
            bool found = false;
            {
                std::lock_guard<std::recursive_mutex> lk(g_modules_mutex);
                for (auto& other : g_modules)
                    if (auto a = other->find(sn)) {
                        v = *a;
                        found = true;
                        break;
                    }
            }
            if (!found) {
                bool weak = ELF32_ST_BIND(s.st_info) == STB_WEAK;
                bool is_func = ELF32_ST_TYPE(s.st_info) == STT_FUNC;
                if (weak && !thunks::lookup(sn)) {
                    v = 0;  // unresolved weak: null, as the real linker does
                } else {
                    v = thunks::resolve(sn, is_func || ELF32_ST_TYPE(s.st_info) == STT_NOTYPE);
                    if (!v) unresolved++;
                }
            }
        }
        cache.emplace(idx, v);
        return v;
    };

    auto apply = [&](gaddr table, uint32_t size) {
        for (gaddr r = table; r < table + size; r += 8) {
            uint32_t off = m.read<uint32_t>(r);
            uint32_t info = m.read<uint32_t>(r + 4);
            uint32_t type = ELF32_R_TYPE(info), sym = ELF32_R_SYM(info);
            gaddr where = bias + off;
            switch (type) {
            case R_ARM_RELATIVE: m.write<uint32_t>(where, m.read<uint32_t>(where) + bias); break;
            case R_ARM_ABS32: m.write<uint32_t>(where, m.read<uint32_t>(where) + resolve_sym(sym)); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: m.write<uint32_t>(where, resolve_sym(sym)); break;
            case 0: break;  // R_ARM_NONE
            default: H32_ERROR("%s: unsupported relocation type %u at 0x%x", name.c_str(), type, off); break;
            }
        }
    };
    if (d.rel) apply(d.rel, d.relsz);
    if (d.jmprel) {
        if (d.pltrel != DT_REL) H32_ERROR("%s: DT_PLTREL is not REL", name.c_str());
        apply(d.jmprel, d.pltrelsz);
    }

    H32_INFO("loaded %s at 0x%08x (%u KB, %zu exports, %zu unresolved imports)", name.c_str(), start, (hi - lo) / 1024,
             mod->exports.size(), unresolved);

    std::lock_guard<std::recursive_mutex> lk(g_modules_mutex);
    g_modules.push_back(std::move(mod));
    return g_modules.back().get();
}

Module* load_module_file(const std::string& path, const std::string& name_override) {
    std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
    std::string saved = g_loading_dir;
    g_loading_dir = dir.empty() ? "." : dir;
    struct Restore {
        std::string& ref;
        std::string val;
        ~Restore() { ref = val; }
    } restore{g_loading_dir, saved};
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        H32_ERROR("cannot open %s", path.c_str());
        return nullptr;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string name = name_override.empty() ? path.substr(path.find_last_of('/') + 1) : name_override;
    Module* mod = load_module(bytes, name);
    if (mod) mod->dir = g_loading_dir;
    return mod;
}

Module* load_sibling(const std::string& dir, const std::string& name) {
    if (Module* m = find_module(name)) return m;
    // Libraries that need each other: break the cycle (imports then resolve to thunks).
    thread_local std::vector<std::string> loading;
    if (std::find(loading.begin(), loading.end(), name) != loading.end()) return nullptr;
    loading.push_back(name);
    struct Pop {
        std::vector<std::string>& v;
        ~Pop() { v.pop_back(); }
    } pop{loading};
    std::string base = name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0 ? name.substr(0, name.size() - 3) : name;
    for (const std::string& file : {dir + "/" + base + "_arm32.so", dir + "/" + name}) {
        if (::access(file.c_str(), R_OK) == 0) return load_module_file(file, name);
    }
    return nullptr;
}

void run_constructors(Module& mod) {
    if (mod.constructors_ran) return;
    mod.constructors_ran = true;
    for (Module* dep : mod.deps) run_constructors(*dep);
    auto& t = GuestThread::current();
    if (mod.dt_init) t.call(mod.dt_init);
    for (uint32_t i = 0; i < mod.init_count; i++) {
        gaddr fn = mem().read<uint32_t>(mod.init_array + i * 4);
        if (fn == 0 || fn == 0xFFFFFFFF) continue;
        H32_TRACE("%s: constructor %u/%u at %s", mod.name.c_str(), i + 1, mod.init_count, symbolize(fn).c_str());
        t.call(fn);
    }
    H32_INFO("%s: ran %u constructors", mod.name.c_str(), mod.init_count);
}

Module* find_module(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lk(g_modules_mutex);
    for (auto& mod : g_modules)
        if (mod->name == name) return mod.get();
    return nullptr;
}

std::vector<Module*> all_modules() {
    std::lock_guard<std::recursive_mutex> lk(g_modules_mutex);
    std::vector<Module*> out;
    for (auto& mod : g_modules) out.push_back(mod.get());
    return out;
}

Module* module_containing(gaddr a) {
    std::lock_guard<std::recursive_mutex> lk(g_modules_mutex);
    for (auto& mod : g_modules)
        if (mod->contains(a)) return mod.get();
    return nullptr;
}

std::string symbolize(gaddr a) {
    Module* mod = module_containing(a);
    if (!mod) return "";
    char buf[512];
    gaddr pc = a & ~1u;
    auto& syms = mod->sorted_symbols;
    auto it = std::upper_bound(syms.begin(), syms.end(), std::make_pair(pc, std::string("\xff")));
    if (it != syms.begin()) {
        --it;
        snprintf(buf, sizeof buf, "%s+0x%x (%s+0x%x)", mod->name.c_str(), a - mod->base, it->second.c_str(), pc - it->first);
    } else {
        snprintf(buf, sizeof buf, "%s+0x%x", mod->name.c_str(), a - mod->base);
    }
    return buf;
}

}  // namespace h32
