// zlib. The guest z_stream (56 bytes, 32-bit pointers) is shadowed by a host
// z_stream; fields are copied in before and out after every call.
#include <zlib.h>

#include <mutex>
#include <unordered_map>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

namespace off {
constexpr gaddr next_in = 0, avail_in = 4, total_in = 8, next_out = 12, avail_out = 16, total_out = 20, msg = 24,
                state = 28, data_type = 44, adler = 48;
}

std::mutex g_lock;
std::unordered_map<gaddr, z_stream*> g_streams;
std::unordered_map<const char*, gaddr> g_msgs;  // zlib messages are static strings

z_stream* shadow(gaddr g, bool create) {
    std::lock_guard lk(g_lock);
    auto it = g_streams.find(g);
    if (it != g_streams.end()) return it->second;
    if (!create) return nullptr;
    auto* s = new z_stream{};
    g_streams[g] = s;
    return s;
}

void drop(gaddr g) {
    std::lock_guard lk(g_lock);
    auto it = g_streams.find(g);
    if (it != g_streams.end()) {
        delete it->second;
        g_streams.erase(it);
    }
}

void sync_in(gaddr g, z_stream* s) {
    auto& m = mem();
    s->next_in = m.ptr<Bytef>(m.read<uint32_t>(g + off::next_in));
    s->avail_in = m.read<uint32_t>(g + off::avail_in);
    s->total_in = m.read<uint32_t>(g + off::total_in);
    s->next_out = m.ptr<Bytef>(m.read<uint32_t>(g + off::next_out));
    s->avail_out = m.read<uint32_t>(g + off::avail_out);
    s->total_out = m.read<uint32_t>(g + off::total_out);
    s->data_type = m.read<int32_t>(g + off::data_type);
}

void sync_out(gaddr g, z_stream* s) {
    auto& m = mem();
    m.write<uint32_t>(g + off::next_in, m.addr(s->next_in));
    m.write<uint32_t>(g + off::avail_in, s->avail_in);
    m.write<uint32_t>(g + off::total_in, uint32_t(s->total_in));
    m.write<uint32_t>(g + off::next_out, m.addr(s->next_out));
    m.write<uint32_t>(g + off::avail_out, s->avail_out);
    m.write<uint32_t>(g + off::total_out, uint32_t(s->total_out));
    m.write<int32_t>(g + off::data_type, s->data_type);
    m.write<uint32_t>(g + off::adler, uint32_t(s->adler));
    gaddr msg = 0;
    if (s->msg) {
        std::lock_guard lk(g_lock);
        auto& slot = g_msgs[s->msg];
        if (!slot) slot = m.static_string(s->msg);
        msg = slot;
    }
    m.write<uint32_t>(g + off::msg, msg);
    // Callers sometimes test strm->state != NULL; point it at the stream.
    m.write<uint32_t>(g + off::state, g);
}

template <int (*Fn)(z_streamp, int)>
void t_stream_op(GuestThread& t) {
    gaddr g = t.regs()[0];
    z_stream* s = shadow(g, false);
    if (!s) return set_ret32(t, uint32_t(Z_STREAM_ERROR));
    sync_in(g, s);
    int r = Fn(s, int32_t(t.regs()[1]));
    sync_out(g, s);
    set_ret32(t, uint32_t(r));
}

template <int (*Fn)(z_streamp)>
void t_stream_end(GuestThread& t) {
    gaddr g = t.regs()[0];
    z_stream* s = shadow(g, false);
    if (!s) return set_ret32(t, uint32_t(Z_STREAM_ERROR));
    int r = Fn(s);
    drop(g);
    mem().write<uint32_t>(g + off::state, 0);
    set_ret32(t, uint32_t(r));
}

// int deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, stream_size)
void t_deflateInit2(GuestThread& t) {
    ArgCursor c{t};
    gaddr g = c.word();
    int level = int32_t(c.word()), method = int32_t(c.word()), bits = int32_t(c.word());
    int memlevel = int32_t(c.word()), strategy = int32_t(c.word());
    z_stream* s = shadow(g, true);
    *s = z_stream{};
    sync_in(g, s);
    int r = deflateInit2(s, level, method, bits, memlevel, strategy);
    if (r != Z_OK) drop(g);
    else sync_out(g, s);
    set_ret32(t, uint32_t(r));
}

// int inflateInit2_(strm, windowBits, version, stream_size)
void t_inflateInit2(GuestThread& t) {
    gaddr g = t.regs()[0];
    z_stream* s = shadow(g, true);
    *s = z_stream{};
    sync_in(g, s);
    int r = inflateInit2(s, int32_t(t.regs()[1]));
    if (r != Z_OK) drop(g);
    else sync_out(g, s);
    set_ret32(t, uint32_t(r));
}

void t_inflateInit(GuestThread& t) {
    gaddr g = t.regs()[0];
    z_stream* s = shadow(g, true);
    *s = z_stream{};
    sync_in(g, s);
    int r = inflateInit(s);
    if (r != Z_OK) drop(g);
    else sync_out(g, s);
    set_ret32(t, uint32_t(r));
}

// int compress(Bytef* dest, uLongf* destLen, const Bytef* src, uLong srcLen) — uLong is 32-bit on the guest.
template <int (*Fn)(Bytef*, uLongf*, const Bytef*, uLong)>
void t_buffer_op(GuestThread& t) {
    gaddr lenp = t.regs()[1];
    uLongf len = mem().read<uint32_t>(lenp);
    int r = Fn(mem().ptr<Bytef>(t.regs()[0]), &len, mem().ptr<const Bytef>(t.regs()[2]), t.regs()[3]);
    mem().write<uint32_t>(lenp, uint32_t(len));
    set_ret32(t, uint32_t(r));
}

uint32_t h_crc32(uint32_t crc, const Bytef* buf, uint32_t len) { return uint32_t(::crc32(crc, buf, len)); }
uint32_t h_adler32(uint32_t a, const Bytef* buf, uint32_t len) { return uint32_t(::adler32(a, buf, len)); }

}  // namespace

namespace thunks {

void register_zlib() {
    add("deflateInit2_", t_deflateInit2);
    add("deflate", t_stream_op<::deflate>);
    add("deflateEnd", t_stream_end<::deflateEnd>);
    add("inflateInit2_", t_inflateInit2);
    add("inflateInit_", t_inflateInit);
    add("inflate", t_stream_op<::inflate>);
    add("inflateEnd", t_stream_end<::inflateEnd>);
    add("compress", t_buffer_op<::compress>);
    add("uncompress", t_buffer_op<::uncompress>);
    add("crc32", H32_WRAP(h_crc32, uint32_t(uint32_t, const Bytef*, uint32_t)));
    add("adler32", H32_WRAP(h_adler32, uint32_t(uint32_t, const Bytef*, uint32_t)));
}

}  // namespace thunks
}  // namespace h32
