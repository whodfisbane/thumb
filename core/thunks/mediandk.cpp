// <media/NdkMediaCodec.h> / <media/NdkMediaFormat.h>: hardware video/audio
// decoding (VLC's "mediacodec_ndk"). Codecs and formats are host pointers, so
// the guest gets handles; surfaces come from ANativeWindow_fromSurface.
//
// Codec buffers live outside the guest arena. Input buffers get a guest shadow
// that's copied into the real buffer on queueInputBuffer; output buffers (only
// used when not decoding straight to a surface) are copied out on get.
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "thunks/handles.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

namespace h32 {
namespace {

uint32_t arg(GuestThread& t, size_t i) { return t.arg_word(i); }

#ifdef __ANDROID__

HandleTable g_codecs(0x6C000000), g_formats(0x6D000000);
AMediaCodec* codec(uint32_t h) { return static_cast<AMediaCodec*>(g_codecs.to_host(h)); }
AMediaFormat* format(uint32_t h) { return static_cast<AMediaFormat*>(g_formats.to_host(h)); }

// Guest shadows of codec buffers, per (codec, input?, index); reused while big enough.
struct Shadow {
    gaddr addr = 0;
    size_t size = 0;
};
std::mutex g_lock;
std::map<std::tuple<AMediaCodec*, bool, size_t>, Shadow> g_shadows;

gaddr shadow_for(AMediaCodec* c, bool input, size_t idx, size_t size) {
    std::lock_guard lk(g_lock);
    Shadow& s = g_shadows[{c, input, idx}];
    if (s.size < size) {
        if (s.addr) mem().free(s.addr);
        s = {mem().malloc(size ? size : 1), size};
    }
    return s.addr;
}

gaddr existing_shadow(AMediaCodec* c, bool input, size_t idx) {
    std::lock_guard lk(g_lock);
    auto it = g_shadows.find({c, input, idx});
    return it == g_shadows.end() ? 0 : it->second.addr;
}

void drop_shadows(AMediaCodec* c) {
    std::lock_guard lk(g_lock);
    for (auto it = g_shadows.begin(); it != g_shadows.end();) {
        if (std::get<0>(it->first) == c) {
            mem().free(it->second.addr);
            it = g_shadows.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- AMediaCodec ----

void t_createCodecByName(GuestThread& t) {
    AMediaCodec* c = AMediaCodec_createCodecByName(mem().str(arg(t, 0)));
    H32_DEBUG("AMediaCodec_createCodecByName(%s) -> %p", mem().str(arg(t, 0)), static_cast<void*>(c));
    set_ret32(t, g_codecs.to_guest(c));
}

void t_createDecoderByType(GuestThread& t) { set_ret32(t, g_codecs.to_guest(AMediaCodec_createDecoderByType(mem().str(arg(t, 0))))); }

void t_codec_delete(GuestThread& t) {
    AMediaCodec* c = codec(arg(t, 0));
    if (!c) return set_ret32(t, uint32_t(AMEDIA_ERROR_INVALID_OBJECT));
    drop_shadows(c);
    set_ret32(t, uint32_t(AMediaCodec_delete(c)));
}

// media_status_t configure(codec, format, ANativeWindow* surface, AMediaCrypto* crypto, uint32_t flags)
void t_configure(GuestThread& t) {
    auto* surface = static_cast<ANativeWindow*>(window_handles().to_host(arg(t, 2)));
    if (arg(t, 3)) H32_WARN("AMediaCodec_configure: crypto not supported");
    set_ret32(t, uint32_t(AMediaCodec_configure(codec(arg(t, 0)), format(arg(t, 1)), surface, nullptr, arg(t, 4))));
}

template <media_status_t (*Fn)(AMediaCodec*)>
void t_codec_op(GuestThread& t) {
    AMediaCodec* c = codec(arg(t, 0));
    set_ret32(t, uint32_t(c ? Fn(c) : AMEDIA_ERROR_INVALID_OBJECT));
}

// ssize_t dequeueInputBuffer(codec, int64_t timeoutUs): codec in r0, timeout in r2:r3
void t_dequeueInputBuffer(GuestThread& t) {
    ArgCursor c{t};
    AMediaCodec* cd = codec(c.word());
    int64_t timeout = int64_t(c.dword());
    set_ret32(t, uint32_t(cd ? AMediaCodec_dequeueInputBuffer(cd, timeout) : -1));
}

// uint8_t* getInputBuffer(codec, size_t idx, size_t* out_size)
void t_getInputBuffer(GuestThread& t) {
    AMediaCodec* c = codec(arg(t, 0));
    size_t idx = arg(t, 1), size = 0;
    uint8_t* host = c ? AMediaCodec_getInputBuffer(c, idx, &size) : nullptr;
    if (gaddr out = arg(t, 2)) mem().write<uint32_t>(out, uint32_t(size));
    set_ret32(t, host ? shadow_for(c, true, idx, size) : 0);
}

// media_status_t queueInputBuffer(codec, size_t idx, off_t offset, size_t size, uint64_t pts, uint32_t flags)
void t_queueInputBuffer(GuestThread& t) {
    ArgCursor a{t};
    AMediaCodec* c = codec(a.word());
    size_t idx = a.word();
    uint32_t offset = a.word(), size = a.word();
    uint64_t pts = a.dword();
    uint32_t flags = a.word();
    if (!c) return set_ret32(t, uint32_t(AMEDIA_ERROR_INVALID_OBJECT));
    size_t cap = 0;
    uint8_t* host = AMediaCodec_getInputBuffer(c, idx, &cap);
    gaddr shadow = existing_shadow(c, true, idx);
    if (host && shadow && size && offset + size <= cap) std::memcpy(host + offset, mem().ptr<uint8_t>(shadow + offset), size);
    set_ret32(t, uint32_t(AMediaCodec_queueInputBuffer(c, idx, offset, size, pts, flags)));
}

// ssize_t dequeueOutputBuffer(codec, AMediaCodecBufferInfo* info, int64_t timeoutUs)
// The info struct (offset, size, int64 pts, flags) has the same 24-byte layout on both sides.
void t_dequeueOutputBuffer(GuestThread& t) {
    ArgCursor a{t};
    AMediaCodec* c = codec(a.word());
    gaddr info = a.word();
    int64_t timeout = int64_t(a.dword());
    if (!c) return set_ret32(t, uint32_t(-1));
    AMediaCodecBufferInfo bi{};
    ssize_t r = AMediaCodec_dequeueOutputBuffer(c, &bi, timeout);
    if (info) {
        mem().write<int32_t>(info, bi.offset);
        mem().write<int32_t>(info + 4, bi.size);
        mem().write<int64_t>(info + 8, bi.presentationTimeUs);
        mem().write<uint32_t>(info + 16, bi.flags);
    }
    set_ret32(t, uint32_t(r));
}

// uint8_t* getOutputBuffer(codec, size_t idx, size_t* out_size): a guest copy of the data.
void t_getOutputBuffer(GuestThread& t) {
    AMediaCodec* c = codec(arg(t, 0));
    size_t idx = arg(t, 1), size = 0;
    uint8_t* host = c ? AMediaCodec_getOutputBuffer(c, idx, &size) : nullptr;
    if (gaddr out = arg(t, 2)) mem().write<uint32_t>(out, uint32_t(size));
    if (!host) return set_ret32(t, 0);
    gaddr g = shadow_for(c, false, idx, size);
    if (g) std::memcpy(mem().ptr<void>(g), host, size);
    set_ret32(t, g);
}

void t_releaseOutputBuffer(GuestThread& t) {
    AMediaCodec* c = codec(arg(t, 0));
    set_ret32(t, uint32_t(c ? AMediaCodec_releaseOutputBuffer(c, arg(t, 1), arg(t, 2) != 0) : AMEDIA_ERROR_INVALID_OBJECT));
}

void t_getOutputFormat(GuestThread& t) {
    AMediaCodec* c = codec(arg(t, 0));
    set_ret32(t, c ? g_formats.to_guest(AMediaCodec_getOutputFormat(c)) : 0);
}

// ---- AMediaFormat ----

void t_format_new(GuestThread& t) { set_ret32(t, g_formats.to_guest(AMediaFormat_new())); }
void t_format_delete(GuestThread& t) {
    AMediaFormat* f = format(arg(t, 0));
    set_ret32(t, uint32_t(f ? AMediaFormat_delete(f) : AMEDIA_ERROR_INVALID_OBJECT));
}
void t_setInt32(GuestThread& t) {
    if (AMediaFormat* f = format(arg(t, 0))) AMediaFormat_setInt32(f, mem().str(arg(t, 1)), int32_t(arg(t, 2)));
}
void t_setInt64(GuestThread& t) {
    ArgCursor a{t};
    AMediaFormat* f = format(a.word());
    const char* name = mem().str(a.word());
    int64_t v = int64_t(a.dword());
    if (f) AMediaFormat_setInt64(f, name, v);
}
void t_setString(GuestThread& t) {
    if (AMediaFormat* f = format(arg(t, 0))) AMediaFormat_setString(f, mem().str(arg(t, 1)), mem().str(arg(t, 2)));
}
void t_getInt32(GuestThread& t) {
    AMediaFormat* f = format(arg(t, 0));
    int32_t v = 0;
    bool ok = f && AMediaFormat_getInt32(f, mem().str(arg(t, 1)), &v);
    if (ok && arg(t, 2)) mem().write<int32_t>(arg(t, 2), v);
    set_ret32(t, ok);
}
void t_getInt64(GuestThread& t) {
    AMediaFormat* f = format(arg(t, 0));
    int64_t v = 0;
    bool ok = f && AMediaFormat_getInt64(f, mem().str(arg(t, 1)), &v);
    if (ok && arg(t, 2)) mem().write<int64_t>(arg(t, 2), v);
    set_ret32(t, ok);
}

#else  // Linux harness: no codecs

void t_null(GuestThread& t) { set_ret32(t, 0); }
void t_fail(GuestThread& t) { set_ret32(t, uint32_t(-10000)); }  // AMEDIA_ERROR_UNKNOWN

#endif

}  // namespace

namespace thunks {

void register_mediandk() {
#ifdef __ANDROID__
    add("AMediaCodec_createCodecByName", t_createCodecByName);
    add("AMediaCodec_createDecoderByType", t_createDecoderByType);
    add("AMediaCodec_delete", t_codec_delete);
    add("AMediaCodec_configure", t_configure);
    add("AMediaCodec_start", t_codec_op<AMediaCodec_start>);
    add("AMediaCodec_stop", t_codec_op<AMediaCodec_stop>);
    add("AMediaCodec_flush", t_codec_op<AMediaCodec_flush>);
    add("AMediaCodec_dequeueInputBuffer", t_dequeueInputBuffer);
    add("AMediaCodec_getInputBuffer", t_getInputBuffer);
    add("AMediaCodec_queueInputBuffer", t_queueInputBuffer);
    add("AMediaCodec_dequeueOutputBuffer", t_dequeueOutputBuffer);
    add("AMediaCodec_getOutputBuffer", t_getOutputBuffer);
    add("AMediaCodec_releaseOutputBuffer", t_releaseOutputBuffer);
    add("AMediaCodec_getOutputFormat", t_getOutputFormat);
    add("AMediaFormat_new", t_format_new);
    add("AMediaFormat_delete", t_format_delete);
    add("AMediaFormat_setInt32", t_setInt32);
    add("AMediaFormat_setInt64", t_setInt64);
    add("AMediaFormat_setString", t_setString);
    add("AMediaFormat_getInt32", t_getInt32);
    add("AMediaFormat_getInt64", t_getInt64);
#else
    for (const char* n : {"AMediaCodec_createCodecByName", "AMediaCodec_createDecoderByType", "AMediaCodec_getInputBuffer",
                          "AMediaCodec_getOutputBuffer", "AMediaCodec_getOutputFormat", "AMediaFormat_new",
                          "AMediaFormat_getInt32", "AMediaFormat_getInt64"})
        add(n, t_null);
    for (const char* n : {"AMediaCodec_delete", "AMediaCodec_configure", "AMediaCodec_start", "AMediaCodec_stop",
                          "AMediaCodec_flush", "AMediaCodec_dequeueInputBuffer", "AMediaCodec_queueInputBuffer",
                          "AMediaCodec_dequeueOutputBuffer", "AMediaCodec_releaseOutputBuffer", "AMediaFormat_delete"})
        add(n, t_fail);
    for (const char* n : {"AMediaFormat_setInt32", "AMediaFormat_setInt64", "AMediaFormat_setString"}) add(n, t_null);
#endif
}

}  // namespace thunks
}  // namespace h32
