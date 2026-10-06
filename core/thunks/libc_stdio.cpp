// <stdio.h>, file system calls, directories.
#include <dirent.h>
#include <fcntl.h>
#ifdef __ANDROID__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {

// ---------------------------------------------------------------- paths ----

namespace {
std::mutex g_path_mutex;
std::vector<std::pair<std::string, std::string>> g_path_map;
}  // namespace

void add_path_mapping(const std::string& guest_prefix, const std::string& host_prefix) {
    std::lock_guard lk(g_path_mutex);
    g_path_map.emplace_back(guest_prefix, host_prefix);
    H32_INFO("path map: %s -> %s", guest_prefix.c_str(), host_prefix.c_str());
}

#ifdef __ANDROID__
namespace {

// Developer convenience: if a file under Android/obb/ is missing, fetch it
// once from http://127.0.0.1:47070/<name>. With `adb reverse tcp:47070
// tcp:47070` that port tunnels to a PC serving the OBB, which avoids having
// to copy it into another user's protected storage by hand.
constexpr uint16_t kObbFetchPort = 47070;

bool fetch_from_dev_host(const std::string& path) {
    std::string name = path.substr(path.find_last_of('/') + 1);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kObbFetchPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return false;  // nothing listening: the normal case
    }
    H32_INFO("obb: %s missing, fetching from dev host", path.c_str());
    std::string req = "GET /" + name + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (write(fd, req.data(), req.size()) != ssize_t(req.size())) {
        close(fd);
        return false;
    }
    std::string dir = path.substr(0, path.find_last_of('/'));
    ::mkdir(dir.c_str(), 0770);
    std::string part = path + ".part";
    FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        H32_ERROR("obb: cannot create %s: %s", part.c_str(), std::strerror(errno));
        close(fd);
        return false;
    }
    std::vector<char> buf(1 << 20);
    std::string header;
    bool in_body = false, ok = false;
    size_t total = 0;
    for (ssize_t n; (n = read(fd, buf.data(), buf.size())) > 0;) {
        const char* data = buf.data();
        size_t len = size_t(n);
        if (!in_body) {
            header.append(data, len);
            auto end = header.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            ok = header.compare(0, 12, "HTTP/1.0 200") == 0 || header.compare(0, 12, "HTTP/1.1 200") == 0;
            if (!ok) break;
            in_body = true;
            std::string body = header.substr(end + 4);
            std::fwrite(body.data(), 1, body.size(), out);
            total += body.size();
            continue;
        }
        std::fwrite(data, 1, len, out);
        total += len;
    }
    close(fd);
    std::fclose(out);
    if (!ok || total == 0) {
        H32_ERROR("obb: dev host did not serve %s", name.c_str());
        ::unlink(part.c_str());
        return false;
    }
    if (std::rename(part.c_str(), path.c_str()) != 0) {
        H32_ERROR("obb: rename failed: %s", std::strerror(errno));
        return false;
    }
    H32_INFO("obb: fetched %s (%zu MB)", name.c_str(), total >> 20);
    return true;
}

void maybe_fetch_obb(const std::string& path) {
    static std::mutex m;
    static std::vector<std::string> tried;
    if (path.find("/Android/obb/") == std::string::npos) return;
    std::lock_guard lk(m);
    if (std::find(tried.begin(), tried.end(), path) != tried.end()) return;
    tried.push_back(path);
    if (::access(path.c_str(), F_OK) == 0) return;
    fetch_from_dev_host(path);
}

}  // namespace
#endif

std::string map_path(const char* p) {
    if (!p) return {};
    std::string s = p;
    {
        std::lock_guard lk(g_path_mutex);
        for (auto& [from, to] : g_path_map)
            if (s.compare(0, from.size(), from) == 0) return to + s.substr(from.size());
    }
#ifdef __ANDROID__
    maybe_fetch_obb(s);
#endif
    return s;
}

// ------------------------------------------------------------- FILE* map ---

namespace {

std::mutex g_files_mutex;
std::unordered_map<gaddr, FILE*> g_files;
gaddr g_sF = 0;  // guest __sF[3]

#ifdef __ANDROID__
// Guest stdout/stderr go to logcat, one line per log entry.
int console_write(void* cookie, const char* buf, int n) {
    thread_local std::string line;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            H32_INFO("[guest %s] %s", cookie ? "stderr" : "stdout", line.c_str());
            line.clear();
        } else {
            line += buf[i];
        }
    }
    return n;
}
FILE* make_console(bool is_err) {
    FILE* f = funopen(is_err ? reinterpret_cast<void*>(1) : nullptr, nullptr, console_write, nullptr, nullptr);
    setvbuf(f, nullptr, _IONBF, 0);
    return f;
}
#else
FILE* make_console(bool) { return stderr; }
#endif

void init_std_files() {
    g_sF = mem().alloc_static(bionic::kFileSize * 3, 8);
    std::lock_guard lk(g_files_mutex);
    g_files[g_sF] = stdin;
    g_files[g_sF + bionic::kFileSize] = make_console(false);
    g_files[g_sF + 2 * bionic::kFileSize] = make_console(true);
}

}  // namespace

FILE* host_file(gaddr f) {
    if (!f) return nullptr;
    std::lock_guard lk(g_files_mutex);
    auto it = g_files.find(f);
    if (it == g_files.end()) {
        H32_ERROR("unknown guest FILE* 0x%08x", f);
        return nullptr;
    }
    return it->second;
}

gaddr guest_file_wrap(FILE* host) {
    if (!host) return 0;
    gaddr g = mem().calloc(1, bionic::kFileSize);
    std::lock_guard lk(g_files_mutex);
    g_files[g] = host;
    return g;
}

void guest_file_forget(gaddr f) {
    {
        std::lock_guard lk(g_files_mutex);
        g_files.erase(f);
    }
    if (f < g_sF || f >= g_sF + 3 * bionic::kFileSize) mem().free(f);
}

bool is_guest_console(gaddr f) { return f >= g_sF + bionic::kFileSize && f < g_sF + 3 * bionic::kFileSize; }

FILE* guest_proc_maps();
namespace {
FILE* guest_proc_maps_file() { return guest_proc_maps(); }

// ---------------------------------------------------------------- stdio ----

FILE* guest_proc_maps_file();

void t_fopen(GuestThread& t) {
    std::string path = map_path(mem().str(t.regs()[0]));
    const char* mode = mem().str(t.regs()[1]);
    if (path == "/proc/self/maps" || path == "/proc/" + std::to_string(getpid()) + "/maps") {
        H32_DEBUG("fopen(%s): synthesized guest view", path.c_str());
        return set_ret32(t, guest_file_wrap(guest_proc_maps_file()));
    }
    errno = 0;
    FILE* f = std::fopen(path.c_str(), mode);
    sync_guest_errno(t);
    H32_DEBUG("fopen(\"%s\", \"%s\") -> %s", path.c_str(), mode, f ? "ok" : std::strerror(errno));
    set_ret32(t, guest_file_wrap(f));
}

void t_fdopen(GuestThread& t) {
    FILE* f = ::fdopen(int32_t(t.regs()[0]), mem().str(t.regs()[1]));
    sync_guest_errno(t);
    set_ret32(t, guest_file_wrap(f));
}

void t_tmpfile(GuestThread& t) { set_ret32(t, guest_file_wrap(std::tmpfile())); }

void t_fclose(GuestThread& t) {
    gaddr g = t.regs()[0];
    FILE* f = host_file(g);
    if (!f) return set_ret32(t, uint32_t(EOF));
    int r = is_guest_console(g) ? 0 : std::fclose(f);
    if (!is_guest_console(g)) guest_file_forget(g);
    set_ret32(t, uint32_t(r));
}

// fpos_t is a 32-bit off_t on 32-bit bionic.
void t_fgetpos(GuestThread& t) {
    FILE* f = host_file(t.regs()[0]);
    long pos = f ? std::ftell(f) : -1;
    if (pos < 0) return set_ret32(t, uint32_t(-1));
    mem().write<int32_t>(t.regs()[1], int32_t(pos));
    set_ret32(t, 0);
}

int h_fseek(FILE* f, long off, int whence) { return std::fseek(f, off, whence); }
long h_ftell(FILE* f) { return std::ftell(f); }

// Variadic printf family.
void write_formatted(GuestThread& t, FILE* f, const std::string& s) {
    if (f) std::fwrite(s.data(), 1, s.size(), f);
    set_ret32(t, uint32_t(s.size()));
}

void t_printf(GuestThread& t) {
    ArgCursor c{t, 1};
    VarArgs va(c);
    write_formatted(t, host_file(g_sF + bionic::kFileSize), guest_format(mem().str(t.regs()[0]), va));
}
void t_vprintf(GuestThread& t) {
    VarArgs va(t, t.regs()[1]);
    write_formatted(t, host_file(g_sF + bionic::kFileSize), guest_format(mem().str(t.regs()[0]), va));
}
void t_fprintf(GuestThread& t) {
    ArgCursor c{t, 2};
    VarArgs va(c);
    write_formatted(t, host_file(t.regs()[0]), guest_format(mem().str(t.regs()[1]), va));
}
void t_vfprintf(GuestThread& t) {
    VarArgs va(t, t.regs()[2]);
    write_formatted(t, host_file(t.regs()[0]), guest_format(mem().str(t.regs()[1]), va));
}

// sprintf family: write into a guest buffer.
void store_formatted(GuestThread& t, gaddr buf, size_t size, bool bounded, const std::string& s) {
    if (buf && (!bounded || size > 0)) {
        size_t n = bounded ? std::min(s.size(), size - 1) : s.size();
        std::memcpy(mem().ptr<char>(buf), s.data(), n);
        mem().ptr<char>(buf)[n] = 0;
    }
    set_ret32(t, uint32_t(s.size()));
}
void t_sprintf(GuestThread& t) {
    ArgCursor c{t, 2};
    VarArgs va(c);
    store_formatted(t, t.regs()[0], 0, false, guest_format(mem().str(t.regs()[1]), va));
}
void t_vsprintf(GuestThread& t) {
    VarArgs va(t, t.regs()[2]);
    store_formatted(t, t.regs()[0], 0, false, guest_format(mem().str(t.regs()[1]), va));
}
void t_snprintf(GuestThread& t) {
    ArgCursor c{t, 3};
    VarArgs va(c);
    store_formatted(t, t.regs()[0], t.regs()[1], true, guest_format(mem().str(t.regs()[2]), va));
}
void t_vsnprintf(GuestThread& t) {
    VarArgs va(t, t.regs()[3]);
    store_formatted(t, t.regs()[0], t.regs()[1], true, guest_format(mem().str(t.regs()[2]), va));
}

void t_sscanf(GuestThread& t) {
    ArgCursor c{t, 2};
    VarArgs va(c);
    set_ret32(t, uint32_t(guest_sscanf(mem().str(t.regs()[0]), mem().str(t.regs()[1]), va)));
}
void t_fscanf(GuestThread& t) {
    ArgCursor c{t, 2};
    VarArgs va(c);
    set_ret32(t, uint32_t(guest_fscanf(host_file(t.regs()[0]), mem().str(t.regs()[1]), va)));
}

// --------------------------------------------------------- file system ----

// Converts a host struct stat to bionic's 32-bit ARM layout (kernel stat64).
void write_stat(gaddr g, const struct stat& s) {
    auto& m = mem();
    std::memset(m.ptr<void>(g), 0, bionic::kStatSize);
    m.write<uint64_t>(g + 0, s.st_dev);
    m.write<uint32_t>(g + 12, uint32_t(s.st_ino));
    m.write<uint32_t>(g + 16, s.st_mode);
    m.write<uint32_t>(g + 20, uint32_t(s.st_nlink));
    m.write<uint32_t>(g + 24, s.st_uid);
    m.write<uint32_t>(g + 28, s.st_gid);
    m.write<uint64_t>(g + 32, s.st_rdev);
    m.write<int64_t>(g + 48, s.st_size);
    m.write<uint32_t>(g + 56, uint32_t(s.st_blksize));
    m.write<uint64_t>(g + 64, uint64_t(s.st_blocks));
    m.write<uint32_t>(g + 72, uint32_t(s.st_atim.tv_sec));
    m.write<uint32_t>(g + 76, uint32_t(s.st_atim.tv_nsec));
    m.write<uint32_t>(g + 80, uint32_t(s.st_mtim.tv_sec));
    m.write<uint32_t>(g + 84, uint32_t(s.st_mtim.tv_nsec));
    m.write<uint32_t>(g + 88, uint32_t(s.st_ctim.tv_sec));
    m.write<uint32_t>(g + 92, uint32_t(s.st_ctim.tv_nsec));
    m.write<uint64_t>(g + 96, s.st_ino);
}

void t_stat(GuestThread& t) {
    std::string path = map_path(mem().str(t.regs()[0]));
    struct stat s;
    errno = 0;
    int r = ::stat(path.c_str(), &s);
    sync_guest_errno(t);
    if (r == 0) write_stat(t.regs()[1], s);
    H32_DEBUG("stat(\"%s\") -> %d", path.c_str(), r);
    set_ret32(t, uint32_t(r));
}

void t_fstat(GuestThread& t) {
    struct stat s;
    errno = 0;
    int r = ::fstat(int32_t(t.regs()[0]), &s);
    sync_guest_errno(t);
    if (r == 0) write_stat(t.regs()[1], s);
    set_ret32(t, uint32_t(r));
}

// Path-taking calls: map the first path argument.
template <int (*Fn)(const char*)>
void t_path1(GuestThread& t) {
    std::string p = map_path(mem().str(t.regs()[0]));
    errno = 0;
    set_ret32(t, uint32_t(Fn(p.c_str())));
    sync_guest_errno(t);
}
int h_remove(const char* p) { return std::remove(p); }
int h_unlink(const char* p) { return ::unlink(p); }
int h_rmdir(const char* p) { return ::rmdir(p); }

void t_mkdir(GuestThread& t) {
    std::string p = map_path(mem().str(t.regs()[0]));
    errno = 0;
    set_ret32(t, uint32_t(::mkdir(p.c_str(), t.regs()[1])));
    sync_guest_errno(t);
}
void t_chmod(GuestThread& t) {
    std::string p = map_path(mem().str(t.regs()[0]));
    errno = 0;
    set_ret32(t, uint32_t(::chmod(p.c_str(), t.regs()[1])));
    sync_guest_errno(t);
}
void t_rename(GuestThread& t) {
    std::string a = map_path(mem().str(t.regs()[0]));
    std::string b = map_path(mem().str(t.regs()[1]));
    errno = 0;
    set_ret32(t, uint32_t(std::rename(a.c_str(), b.c_str())));
    sync_guest_errno(t);
}
void t_mkstemp(GuestThread& t) {
    // The template is a guest path; rewrite it, then copy the result back.
    char* tmpl = mem().ptr<char>(t.regs()[0]);
    std::string mapped = map_path(tmpl);
    errno = 0;
    int fd = ::mkstemp(mapped.data());
    sync_guest_errno(t);
    if (fd >= 0) {
        size_t n = std::strlen(tmpl);
        std::memcpy(tmpl + n - 6, mapped.data() + mapped.size() - 6, 6);
    }
    set_ret32(t, uint32_t(fd));
}

// File descriptors: off_t is 32-bit on the guest.
int h_lseek(int fd, long off, int whence) { return int(::lseek(fd, off, whence)); }
int h_read(int fd, void* buf, size_t n) { return int(::read(fd, buf, n)); }
int h_write(int fd, const void* buf, size_t n) { return int(::write(fd, buf, n)); }

// int fcntl(int fd, int cmd, ...) — only integer-argument commands.
void t_fcntl(GuestThread& t) {
    int cmd = int32_t(t.regs()[1]);
    if (cmd != F_GETFL && cmd != F_SETFL && cmd != F_GETFD && cmd != F_SETFD) {
        H32_WARN("fcntl cmd %d not supported", cmd);
        return set_ret32(t, uint32_t(-1));
    }
    errno = 0;
    set_ret32(t, uint32_t(::fcntl(int32_t(t.regs()[0]), cmd, int32_t(t.regs()[2]))));
    sync_guest_errno(t);
}

// ---------------------------------------------------------- directories ----
// Guest DIR* is a guest buffer holding one bionic dirent (280 bytes, same
// layout as the 64-bit kernel dirent64) for readdir's return value.
constexpr size_t kDirentSize = 280;
std::mutex g_dirs_mutex;
std::unordered_map<gaddr, DIR*> g_dirs;

DIR* host_dir(gaddr g) {
    std::lock_guard lk(g_dirs_mutex);
    auto it = g_dirs.find(g);
    return it == g_dirs.end() ? nullptr : it->second;
}

void t_opendir(GuestThread& t) {
    std::string p = map_path(mem().str(t.regs()[0]));
    errno = 0;
    DIR* d = ::opendir(p.c_str());
    sync_guest_errno(t);
    if (!d) return set_ret32(t, 0);
    gaddr g = mem().calloc(1, kDirentSize);
    std::lock_guard lk(g_dirs_mutex);
    g_dirs[g] = d;
    set_ret32(t, g);
}

void t_readdir(GuestThread& t) {
    gaddr g = t.regs()[0];
    DIR* d = host_dir(g);
    if (!d) return set_ret32(t, 0);
    dirent* e = ::readdir(d);
    if (!e) return set_ret32(t, 0);
    auto& m = mem();
    m.write<uint64_t>(g + 0, e->d_ino);
    m.write<int64_t>(g + 8, e->d_off);
    m.write<uint16_t>(g + 16, 280);
    m.write<uint8_t>(g + 18, e->d_type);
    std::strncpy(m.ptr<char>(g + 19), e->d_name, 255);
    m.ptr<char>(g + 19)[255] = 0;
    set_ret32(t, g);
}

void t_rewinddir(GuestThread& t) {
    if (DIR* d = host_dir(t.regs()[0])) ::rewinddir(d);
}

void t_closedir(GuestThread& t) {
    gaddr g = t.regs()[0];
    DIR* d;
    {
        std::lock_guard lk(g_dirs_mutex);
        auto it = g_dirs.find(g);
        if (it == g_dirs.end()) return set_ret32(t, uint32_t(-1));
        d = it->second;
        g_dirs.erase(it);
    }
    ::closedir(d);
    mem().free(g);
    set_ret32(t, 0);
}

}  // namespace

namespace thunks {

void register_libc_stdio() {
    init_std_files();
    add_data("__sF", g_sF);

    add("fopen", t_fopen);
    add("fdopen", t_fdopen);
    add("tmpfile", t_tmpfile);
    add("fclose", t_fclose);
    H32_ADD(fread, size_t(void*, size_t, size_t, FILE*));
    H32_ADD(fwrite, size_t(const void*, size_t, size_t, FILE*));
    add("fseek", H32_WRAP(h_fseek, int(FILE*, long, int)));
    add("fseeko", H32_WRAP(h_fseek, int(FILE*, long, int)));
    add("ftell", H32_WRAP(h_ftell, long(FILE*)));
    add("ftello", H32_WRAP(h_ftell, long(FILE*)));
    add("fgetpos", t_fgetpos);
    H32_ADD(rewind, void(FILE*));
    H32_ADD(fflush, int(FILE*));
    H32_ADD(feof, int(FILE*));
    H32_ADD(ferror, int(FILE*));
    H32_ADD(clearerr, void(FILE*));
    H32_ADD(fileno, int(FILE*));
    H32_ADD(fgets, char*(char*, int, FILE*));
    H32_ADD(fputs, int(const char*, FILE*));
    H32_ADD(fputc, int(int, FILE*));
    H32_ADD(putc, int(int, FILE*));
    H32_ADD(getc, int(FILE*));
    H32_ADD(fgetc, int(FILE*));
    H32_ADD(ungetc, int(int, FILE*));
    H32_ADD(getwc, wint_t(FILE*));
    H32_ADD(putwc, wint_t(wchar_t, FILE*));
    H32_ADD(ungetwc, wint_t(wint_t, FILE*));
    H32_ADD(setvbuf, int(FILE*, char*, int, size_t));
    add("puts", +[](GuestThread& t) {
        FILE* out = host_file(g_sF + bionic::kFileSize);
        std::fputs(mem().str(t.regs()[0]), out);
        std::fputc('\n', out);
        set_ret32(t, 1);
    });
    add("putchar", +[](GuestThread& t) { set_ret32(t, uint32_t(std::fputc(int(t.regs()[0]), host_file(g_sF + bionic::kFileSize)))); });

    add("printf", t_printf);
    add("vprintf", t_vprintf);
    add("fprintf", t_fprintf);
    add("vfprintf", t_vfprintf);
    add("sprintf", t_sprintf);
    add("vsprintf", t_vsprintf);
    add("snprintf", t_snprintf);
    add("vsnprintf", t_vsnprintf);
    add("sscanf", t_sscanf);
    add("fscanf", t_fscanf);

    add("stat", t_stat);
    add("lstat", t_stat);
    add("fstat", t_fstat);
    add("remove", t_path1<h_remove>);
    add("unlink", t_path1<h_unlink>);
    add("rmdir", t_path1<h_rmdir>);
    add("mkdir", t_mkdir);
    add("chmod", t_chmod);
    add("rename", t_rename);
    add("mkstemp", t_mkstemp);
    H32_ADD(umask, mode_t(mode_t));
    H32_ADD(close, int(int));
    add("read", H32_WRAP(h_read, int(int, void*, size_t)));
    add("write", H32_WRAP(h_write, int(int, const void*, size_t)));
    add("lseek", H32_WRAP(h_lseek, int(int, long, int)));
    add("fcntl", t_fcntl);

    add("opendir", t_opendir);
    add("readdir", t_readdir);
    add("rewinddir", t_rewinddir);
    add("closedir", t_closedir);
}

}  // namespace thunks
}  // namespace h32
