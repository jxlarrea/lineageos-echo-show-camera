/*
 * Compatibility shims for the stock cronos camera blobs.
 *
 * The MediaTek camera stack shipped by Amazon targets API 25. Every symbol it
 * imports still exists on Android 11, but four of them gained trailing
 * parameters along the way, so the old mangled names no longer resolve. Each
 * shim below re-exports the old name and forwards to the current one, passing
 * the value the old implementation used implicitly.
 *
 * android::CallStack is the exception: libutils no longer exports it at all, so
 * the members are stubbed. The blobs only use it for diagnostic backtraces, and
 * they allocate the storage themselves, so no-op bodies are safe as long as
 * nothing here writes through `this`.
 */

#include <android/log.h>
#include <cstdint>
#include <cstddef>

typedef int32_t status_t;

struct native_handle;
typedef struct native_handle native_handle_t;

// ---------------------------------------------------------------------------
// android::CallStack - removed from libutils' exported surface.
// ---------------------------------------------------------------------------

namespace android {

class CallStack {
public:
    CallStack();
    ~CallStack();
    void update(int32_t ignoreDepth, int32_t maxDepth);
    void log(const char* logtag, android_LogPriority priority,
             const char* prefix) const;
};

CallStack::CallStack() {}
CallStack::~CallStack() {}
void CallStack::update(int32_t /*ignoreDepth*/, int32_t /*maxDepth*/) {}
void CallStack::log(const char* /*logtag*/, android_LogPriority /*priority*/,
                    const char* /*prefix*/) const {}

}  // namespace android

// ---------------------------------------------------------------------------
// libui - GraphicBuffer and GraphicBufferMapper gained out-parameters for
// bytes-per-pixel / bytes-per-stride. Passing null asks the current
// implementation not to report them, which matches the old behaviour.
// ---------------------------------------------------------------------------

namespace android {

class Rect;

class GraphicBuffer {
public:
    // API 25: no layerCount parameter; buffers were implicitly single-layer.
    GraphicBuffer(uint32_t width, uint32_t height, int32_t format,
                  uint32_t usage, uint32_t stride, native_handle_t* handle,
                  bool keepOwnership);
    status_t lock(uint32_t usage, void** vaddr);
};

// _ZN7android13GraphicBufferC1EjjijjjP13native_handleb
extern "C" void _ZN7android13GraphicBufferC1EjjijjjP13native_handleb(
        void* thisptr, uint32_t width, uint32_t height, int32_t format,
        uint32_t layerCount, uint32_t usage, uint32_t stride,
        native_handle_t* handle, bool keepOwnership);

GraphicBuffer::GraphicBuffer(uint32_t width, uint32_t height, int32_t format,
                             uint32_t usage, uint32_t stride,
                             native_handle_t* handle, bool keepOwnership) {
    _ZN7android13GraphicBufferC1EjjijjjP13native_handleb(
            this, width, height, format, /*layerCount=*/1, usage, stride,
            handle, keepOwnership);
}

// _ZN7android13GraphicBuffer4lockEjPPvPiS3_
extern "C" status_t _ZN7android13GraphicBuffer4lockEjPPvPiS3_(
        void* thisptr, uint32_t usage, void** vaddr, int32_t* outBytesPerPixel,
        int32_t* outBytesPerStride);

status_t GraphicBuffer::lock(uint32_t usage, void** vaddr) {
    return _ZN7android13GraphicBuffer4lockEjPPvPiS3_(this, usage, vaddr,
                                                     nullptr, nullptr);
}

class GraphicBufferMapper {
public:
    status_t lock(const native_handle_t* handle, uint32_t usage,
                  const Rect& bounds, void** vaddr);
};

// _ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_
extern "C" status_t
_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_(
        void* thisptr, const native_handle_t* handle, uint32_t usage,
        const Rect& bounds, void** vaddr, int32_t* outBytesPerPixel,
        int32_t* outBytesPerStride);

status_t GraphicBufferMapper::lock(const native_handle_t* handle, uint32_t usage,
                                   const Rect& bounds, void** vaddr) {
    return _ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_(
            this, handle, usage, bounds, vaddr, nullptr, nullptr);
}

}  // namespace android

// ---------------------------------------------------------------------------
// libdpframework
//
// The DpIspStream/DpBlitStream forwarders that used to live here are gone. The
// camera blobs now link a private, matching-vintage copy of the library
// (libdpframework_cam.so, see scripts/install-private-dpframework.sh) which
// exports the API 25 signatures natively. Providing them here as well would
// shadow that library and forward back into the Android 9 one, which is exactly
// the mismatch the private copy exists to avoid.
// ---------------------------------------------------------------------------
