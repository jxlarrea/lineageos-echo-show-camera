/*
 * Adapter presenting the API 25 DpIspStream / DpBlitStream interface on top of
 * this ROM's Android 9 libdpframework.
 *
 * Why: libcam.campipe.so is API 25 and allocates a DpIspStream sized to the
 * Android 7 class definition. The Android 9 constructor initialises a larger
 * object and writes past that allocation:
 *   #00 DpIspStream::DpIspStream(ISPStreamType)+264   libdpframework.so
 *   #01 NSCamPipe::XdpPipe::init()+16                 libcam.campipe.so
 * A signature shim cannot help - the caller's allocation is already too small.
 *
 * Handing the camera a private copy of the *old* library avoids that, but the
 * old library then has to talk to this kernel's newer MDP/cmdq driver, and
 * dequeueDstBuffer() fails there. So instead: keep the Android 9 library, which
 * matches the kernel, and fix the size problem with indirection. The caller's
 * undersized object is used to store nothing but a pointer to a real, heap
 * allocated Android 9 object; every method loads that pointer and forwards.
 *
 * Of the 28 symbols the camera blobs import, 24 have identical signatures in
 * both versions and only need the `this` swap. Four gained trailing parameters
 * and are adapted here.
 *
 * Built with soname libdpframework_cam.so, which is what the camera blobs were
 * repointed at by scripts/install-private-dpframework.sh.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>

#define LOG_TAG "libdpframework_cam"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * We cannot know sizeof(Android 9 DpIspStream) from here, so over-allocate well
 * past any plausible size. The constructor only touches its own fields. 8 KB
 * was not enough: the VSS/pass2 stream type writes at least to offset 0x20ec
 * during construction (observed as a SEGV_ACCERR one page past an 8 KB
 * allocation), so give it a comfortable margin.
 */
static const size_t kRealObjectSize = 65536;

static void* real_library() {
    static void* handle = dlopen("libdpframework.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        LOGE("dlopen(libdpframework.so) failed: %s", dlerror());
    }
    return handle;
}

static void* real_symbol(const char* name) {
    void* handle = real_library();
    if (handle == nullptr) {
        return nullptr;
    }
    void* sym = dlsym(handle, name);
    if (sym == nullptr) {
        LOGE("dlsym(%s) failed", name);
    }
    return sym;
}

/* The caller's object holds exactly one thing: the real object's address. */
static inline void*& impl_of(void* self) {
    return *reinterpret_cast<void**>(self);
}

extern "C" void _ZN11DpIspStreamC1ENS_13ISPStreamTypeE(void* self, int32_t a0) {
    typedef void (*real_fn)(void*, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStreamC1ENS_13ISPStreamTypeE"));
    void* obj = calloc(1, kRealObjectSize);
    if (obj == nullptr || real == nullptr) {
        LOGE("_ZN11DpIspStreamC1ENS_13ISPStreamTypeE: cannot construct real object");
        impl_of(self) = nullptr;
        free(obj);
        return;
    }
    real(obj, a0);
    impl_of(self) = obj;
    LOGE("DpIspStream ctor: self=%p impl=%p type=%d", self, obj, a0);
}

extern "C" void _ZN12DpBlitStreamC1Ev(void* self) {
    typedef void (*real_fn)(void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStreamC1Ev"));
    void* obj = calloc(1, kRealObjectSize);
    if (obj == nullptr || real == nullptr) {
        LOGE("_ZN12DpBlitStreamC1Ev: cannot construct real object");
        impl_of(self) = nullptr;
        free(obj);
        return;
    }
    real(obj);
    impl_of(self) = obj;
}

extern "C" void _ZN11DpIspStreamD1Ev(void* self) {
    typedef void (*real_fn)(void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStreamD1Ev"));
    void* obj = impl_of(self);
    LOGE("DpIspStream dtor: self=%p impl=%p", self, obj);
    if (obj != nullptr) {
        if (real != nullptr) {
            real(obj);
        }
        free(obj);
        impl_of(self) = nullptr;
    }
}

extern "C" void _ZN12DpBlitStreamD1Ev(void* self) {
    typedef void (*real_fn)(void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStreamD1Ev"));
    void* obj = impl_of(self);
    if (obj != nullptr) {
        if (real != nullptr) {
            real(obj);
        }
        free(obj);
        impl_of(self) = nullptr;
    }
}

extern "C" int32_t _ZN11DpIspStream10setSrcCropEiiiiii(void* self, int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream10setSrcCropEiiiiii"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2, a3, a4, a5);
}

extern "C" int32_t _ZN11DpIspStream10stopStreamEv(void* self) {
    typedef int32_t (*real_fn)(void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream10stopStreamEv"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self));
}

extern "C" int32_t _ZN11DpIspStream11setRotationEii(void* self, int32_t a0, int32_t a1) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream11setRotationEii"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1);
}

extern "C" int32_t _ZN11DpIspStream11startStreamEv(void* self) {
    typedef int32_t (*real_fn)(void*, void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream11startStreamEP7timeval"));
    if (real == nullptr || impl_of(self) == nullptr) {
        LOGE("startStream: real=%p impl=%p -> -1", (void*)real, impl_of(self));
        return -1;
    }
    int32_t rc = real(impl_of(self), nullptr);
    if (rc != 0) {
        LOGE("startStream -> %d", rc);
    }
    return rc;
}

extern "C" int32_t _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb(void* self, int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, void* a8, bool a9) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*, bool, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, 0);
}

extern "C" int32_t _ZN11DpIspStream12setParameterER23ISP_TPIPE_CONFIG_STRUCTj(void* self, void* a0, uint32_t a1) {
    typedef int32_t (*real_fn)(void*, void*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream12setParameterER23ISP_TPIPE_CONFIG_STRUCTj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        LOGE("setParameter: real=%p impl=%p -> -1", (void*)real, impl_of(self));
        return -1;
    }
    int32_t rc = real(impl_of(self), a0, a1);
    if (rc != 0) {
        LOGE("setParameter: cfg=%p size=%u -> %d", a0, a1, rc);
    }
    return rc;
}

extern "C" int32_t _ZN11DpIspStream12setSrcConfigE13DP_COLOR_ENUMiiib(void* self, int32_t a0, int32_t a1, int32_t a2, int32_t a3, bool a4) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t, int32_t, int32_t, bool);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream12setSrcConfigE13DP_COLOR_ENUMiiib"));
    if (real == nullptr || impl_of(self) == nullptr) {
        LOGE("setSrcConfig(short): real=%p impl=%p -> -1", (void*)real, impl_of(self));
        return -1;
    }
    int32_t rc = real(impl_of(self), a0, a1, a2, a3, a4);
    if (rc != 0) {
        LOGE("setSrcConfig(short): color=0x%x w=%d h=%d yPitch=%d uvPitch=%d doFlush=%d -> %d",
             a0, a1, a2, a3, (int)a4, rc);
    }
    return rc;
}

extern "C" int32_t _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb(void* self, int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, void* a7, bool a8) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, void*, bool, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure"));
    if (real == nullptr || impl_of(self) == nullptr) {
        LOGE("setSrcConfig(long): real=%p impl=%p -> -1", (void*)real, impl_of(self));
        return -1;
    }
    int32_t rc = real(impl_of(self), a0, a1, a2, a3, a4, a5, a6, a7, a8, 0);
    if (rc != 0) {
        LOGE("setSrcConfig(long): w=%d h=%d yPitch=%d uvPitch=%d color=0x%x profile=%d interlace=%d roi=%p doFlush=%d -> %d",
             a0, a1, a2, a3, a4, a5, a6, a7, (int)a8, rc);
    }
    return rc;
}

extern "C" int32_t _ZN11DpIspStream13setFlipStatusEib(void* self, int32_t a0, bool a1) {
    typedef int32_t (*real_fn)(void*, int32_t, bool);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream13setFlipStatusEib"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1);
}

extern "C" int32_t _ZN11DpIspStream14queueDstBufferEiPPvPjS2_i(void* self, int32_t a0, void** a1, uint32_t* a2, void** a3, int32_t a4) {
    typedef int32_t (*real_fn)(void*, int32_t, void**, uint32_t*, void**, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream14queueDstBufferEiPPvPjS2_i"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2, a3, a4);
}

extern "C" int32_t _ZN11DpIspStream14queueSrcBufferEPPvPjS2_i(void* self, void** a0, uint32_t* a1, void** a2, int32_t a3) {
    typedef int32_t (*real_fn)(void*, void**, uint32_t*, void**, int32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream14queueSrcBufferEPPvPjS2_i"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2, a3);
}

extern "C" int32_t _ZN11DpIspStream14queueSrcBufferEPvjj(void* self, void* a0, uint32_t a1, uint32_t a2) {
    typedef int32_t (*real_fn)(void*, void*, uint32_t, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream14queueSrcBufferEPvjj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2);
}

extern "C" int32_t _ZN11DpIspStream15dequeueFrameEndEPj(void* self, uint32_t* a0) {
    typedef int32_t (*real_fn)(void*, uint32_t*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream15dequeueFrameEndEPj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0);
}

extern "C" int32_t _ZN11DpIspStream16dequeueDstBufferEiPPvb(void* self, int32_t a0, void** a1, bool a2) {
    typedef int32_t (*real_fn)(void*, int32_t, void**, bool);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream16dequeueDstBufferEiPPvb"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2);
}

extern "C" int32_t _ZN11DpIspStream16dequeueSrcBufferEv(void* self) {
    typedef int32_t (*real_fn)(void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN11DpIspStream16dequeueSrcBufferEv"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self));
}

extern "C" int32_t _ZN12DpBlitStream10invalidateEv(void* self) {
    typedef int32_t (*real_fn)(void*, void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream10invalidateEP7timeval"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), nullptr);
}

extern "C" int32_t _ZN12DpBlitStream12setDstBufferEiPjj(void* self, int32_t a0, uint32_t* a1, uint32_t a2) {
    typedef int32_t (*real_fn)(void*, int32_t, uint32_t*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setDstBufferEiPjj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2);
}

extern "C" int32_t _ZN12DpBlitStream12setDstBufferEPPvPjj(void* self, void** a0, uint32_t* a1, uint32_t a2) {
    typedef int32_t (*real_fn)(void*, void**, uint32_t*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setDstBufferEPPvPjj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2);
}

extern "C" int32_t _ZN12DpBlitStream12setDstBufferEPvj(void* self, void* a0, uint32_t a1) {
    typedef int32_t (*real_fn)(void*, void*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setDstBufferEPvj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1);
}

extern "C" int32_t _ZN12DpBlitStream12setDstConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect(void* self, int32_t a0, int32_t a1, int32_t a2, int32_t a3, void* a4) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t, int32_t, int32_t, void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setDstConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2, a3, a4);
}

extern "C" int32_t _ZN12DpBlitStream12setSrcBufferEiPjj(void* self, int32_t a0, uint32_t* a1, uint32_t a2) {
    typedef int32_t (*real_fn)(void*, int32_t, uint32_t*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setSrcBufferEiPjj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2);
}

extern "C" int32_t _ZN12DpBlitStream12setSrcBufferEPPvPjj(void* self, void** a0, uint32_t* a1, uint32_t a2) {
    typedef int32_t (*real_fn)(void*, void**, uint32_t*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setSrcBufferEPPvPjj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2);
}

extern "C" int32_t _ZN12DpBlitStream12setSrcBufferEPvj(void* self, void* a0, uint32_t a1) {
    typedef int32_t (*real_fn)(void*, void*, uint32_t);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setSrcBufferEPvj"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1);
}

extern "C" int32_t _ZN12DpBlitStream12setSrcConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect(void* self, int32_t a0, int32_t a1, int32_t a2, int32_t a3, void* a4) {
    typedef int32_t (*real_fn)(void*, int32_t, int32_t, int32_t, int32_t, void*);
    static real_fn real = reinterpret_cast<real_fn>(real_symbol("_ZN12DpBlitStream12setSrcConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect"));
    if (real == nullptr || impl_of(self) == nullptr) {
        return -1;
    }
    return real(impl_of(self), a0, a1, a2, a3, a4);
}
