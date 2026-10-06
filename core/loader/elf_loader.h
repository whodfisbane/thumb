// Loads 32-bit ARM ELF shared objects (Android armeabi-v7a .so files) into
// guest memory: maps PT_LOAD segments, applies relocations, binds imports to
// thunks or to previously loaded modules, and runs constructors.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace h32 {

struct Module {
    std::string name;
    gaddr base = 0;   // load bias (guest address of vaddr 0)
    gaddr size = 0;   // span of all PT_LOAD segments
    gaddr exidx = 0;  // .ARM.exidx (for C++ exception unwinding)
    uint32_t exidx_count = 0;
    gaddr init_array = 0, fini_array = 0;
    uint32_t init_count = 0, fini_count = 0;
    gaddr dt_init = 0, dt_fini = 0;

    // Exported (defined) dynamic symbols: name -> guest address.
    std::unordered_map<std::string, gaddr> exports;
    // Sorted function symbols, for crash reports.
    std::vector<std::pair<gaddr, std::string>> sorted_symbols;

    std::optional<gaddr> find(std::string_view sym) const;
    bool contains(gaddr a) const { return a >= base && a - base < size; }
};

// Loads `bytes` as module `name`. Imports resolve against previously loaded
// modules first, then thunks. Returns nullptr on a malformed file.
Module* load_module(const std::vector<uint8_t>& bytes, const std::string& name);
// `name` defaults to the file name; THUMB passes the original name
// ("libfoo.so" for libfoo_arm32.so) so the guest sees itself as it expects.
Module* load_module_file(const std::string& path, const std::string& name = "");

// Runs DT_INIT and DT_INIT_ARRAY on the calling thread.
void run_constructors(Module& m);

Module* find_module(std::string_view name);
std::vector<Module*> all_modules();
Module* module_containing(gaddr a);
// "libfoo.so+0x1234 (symbol+0x10)" or "" if `a` is not in any module.
std::string symbolize(gaddr a);

}  // namespace h32
