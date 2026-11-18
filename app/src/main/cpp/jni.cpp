// JNI bridge between the Kotlin application and bt::Session.
//
// Structured data crosses the boundary as JSON strings; pixels cross as int
// arrays holding the raw memory of opaque ARGB_8888 bitmaps (little-endian:
// red in the low byte), encoded with the sRGB transfer function in the source
// image's own primaries.
#include <jni.h>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#include "bt/session_io.h"

using namespace bt;

namespace {

struct NativeSession {
    std::unique_ptr<Session> session;
    jobject loader = nullptr;   // global ref to the Kotlin RegionLoader
    jmethodID load_method = nullptr;
    JNIEnv* env = nullptr;      // valid only during a JNI call
};

struct JavaException : std::exception {};  // a Java exception is already pending

std::string to_string(JNIEnv* env, jstring s) {
    if (!s) throw std::invalid_argument("null string");
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

jstring to_jstring(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

void throw_java(JNIEnv* env, const char* cls, const char* message) {
    if (env->ExceptionCheck()) return;
    env->ThrowNew(env->FindClass(cls), message);
}

// Run `body`, translating C++ exceptions to Java exceptions.
template <typename T, typename F>
T guarded(JNIEnv* env, T fallback, F body) {
    try {
        return body();
    } catch (const JavaException&) {
    } catch (const std::invalid_argument& e) {
        throw_java(env, "java/lang/IllegalArgumentException", e.what());
    } catch (const std::bad_alloc&) {
        throw_java(env, "java/lang/OutOfMemoryError", "native image allocation failed");
    } catch (const std::exception& e) {
        throw_java(env, "java/lang/RuntimeException", e.what());
    }
    return fallback;
}

NativeSession& from_handle(jlong handle) {
    if (!handle) throw std::invalid_argument("session is closed");
    return *reinterpret_cast<NativeSession*>(handle);
}

const float* srgb_lut() {
    static const std::unique_ptr<float[]> lut = [] {
        std::unique_ptr<float[]> t(new float[256]);
        for (int i = 0; i < 256; ++i) t[i] = srgb_to_linear(i / 255.f);
        return t;
    }();
    return lut.get();
}

Image image_from_argb(JNIEnv* env, jintArray array, int width, int height) {
    if (!array) throw std::invalid_argument("region loader returned no pixels");
    if (env->GetArrayLength(array) != width * height)
        throw std::invalid_argument("region loader returned the wrong number of pixels");
    Image img(width, height, 3);
    jint* px = static_cast<jint*>(env->GetPrimitiveArrayCritical(array, nullptr));
    if (!px) throw std::bad_alloc();
    const float* lut = srgb_lut();
    for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
        uint32_t v = uint32_t(px[i]);
        img.data[i * 3] = lut[v & 0xff];
        img.data[i * 3 + 1] = lut[(v >> 8) & 0xff];
        img.data[i * 3 + 2] = lut[(v >> 16) & 0xff];
    }
    env->ReleasePrimitiveArrayCritical(array, px, JNI_ABORT);
    return img;
}

jintArray argb_from_image(JNIEnv* env, const Image& img) {
    jintArray array = env->NewIntArray(img.width * img.height);
    if (!array) throw JavaException();
    jint* px = static_cast<jint*>(env->GetPrimitiveArrayCritical(array, nullptr));
    if (!px) throw std::bad_alloc();
    for (size_t i = 0, n = size_t(img.width) * img.height; i < n; ++i) {
        uint32_t r = uint32_t(std::lround(linear_to_srgb(img.data[i * 3]) * 255.f));
        uint32_t g = uint32_t(std::lround(linear_to_srgb(img.data[i * 3 + 1]) * 255.f));
        uint32_t b = uint32_t(std::lround(linear_to_srgb(img.data[i * 3 + 2]) * 255.f));
        px[i] = jint(0xff000000u | (b << 16) | (g << 8) | r);
    }
    env->ReleasePrimitiveArrayCritical(array, px, 0);
    return array;
}

double scale_for(jint sample_size) {
    if (sample_size != 1 && sample_size != 2 && sample_size != 4 && sample_size != 8)
        throw std::invalid_argument("sample size must be 1, 2, 4, or 8");
    return 1.0 / sample_size;
}

// Scope that publishes the current JNIEnv to the region loader.
struct EnvScope {
    NativeSession& s;
    EnvScope(NativeSession& session, JNIEnv* env) : s(session) { s.env = env; }
    ~EnvScope() { s.env = nullptr; }
};

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeCreate(
    JNIEnv* env, jclass, jstring frames_json, jobject loader) {
    return guarded(env, jlong(0), [&]() -> jlong {
        if (!loader) throw std::invalid_argument("region loader is required");
        auto native = std::make_unique<NativeSession>();
        jclass cls = env->GetObjectClass(loader);
        native->load_method = env->GetMethodID(cls, "loadRegion", "(IIIIII)[I");
        if (!native->load_method) throw JavaException();
        NativeSession* raw = native.get();
        RegionLoader region_loader = [raw](int frame, const Rect& rect, double scale) {
            JNIEnv* e = raw->env;
            if (!e) throw std::runtime_error("region loader used outside a JNI call");
            jint sample = jint(std::lround(1.0 / scale));
            jintArray pixels = static_cast<jintArray>(
                e->CallObjectMethod(raw->loader, raw->load_method, frame, rect.x0, rect.y0, rect.x1, rect.y1, sample));
            if (e->ExceptionCheck()) throw JavaException();
            Image img = image_from_argb(e, pixels, rect.width(), rect.height());
            e->DeleteLocalRef(pixels);
            return img;
        };
        std::vector<FrameSource> frames = frames_from_json(json::parse(to_string(env, frames_json)));
        native->loader = env->NewGlobalRef(loader);
        try {
            EnvScope scope(*native, env);
            native->session = std::make_unique<Session>(std::move(frames), std::move(region_loader));
        } catch (...) {
            env->DeleteGlobalRef(native->loader);
            throw;
        }
        return reinterpret_cast<jlong>(native.release());
    });
}

JNIEXPORT void JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeDestroy(JNIEnv* env, jclass,
                                                                                              jlong handle) {
    if (!handle) return;
    NativeSession* native = reinterpret_cast<NativeSession*>(handle);
    env->DeleteGlobalRef(native->loader);
    delete native;
}

JNIEXPORT jstring JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeDescribe(JNIEnv* env, jclass,
                                                                                                  jlong handle) {
    return guarded(env, jstring(nullptr), [&] {
        NativeSession& s = from_handle(handle);
        const AssociationResult& a = s.session->association();
        json::Value o = json::Value::make_object();
        o.set("person_count", a.person_count);
        json::Value frames = json::Value::make_array();
        for (size_t f = 0; f < a.person.size(); ++f) {
            json::Value persons = json::Value::make_array();
            for (int p : a.person[f]) persons.array.emplace_back(p);
            frames.array.push_back(persons);
        }
        o.set("person_of", frames);
        return to_jstring(env, json::dump(o));
    });
}

JNIEXPORT void JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeSetConstraints(
    JNIEnv* env, jclass, jlong handle, jstring constraints_json) {
    guarded(env, 0, [&] {
        NativeSession& s = from_handle(handle);
        EnvScope scope(s, env);
        s.session->set_constraints(constraints_from_json(json::parse(to_string(env, constraints_json))));
        return 0;
    });
}

JNIEXPORT jstring JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativePropose(
    JNIEnv* env, jclass, jlong handle, jintArray lock_persons, jintArray lock_frames, jint locked_base) {
    return guarded(env, jstring(nullptr), [&] {
        NativeSession& s = from_handle(handle);
        EnvScope scope(s, env);
        std::map<int, int> locks;
        jsize n = lock_persons ? env->GetArrayLength(lock_persons) : 0;
        if (n != (lock_frames ? env->GetArrayLength(lock_frames) : 0))
            throw std::invalid_argument("lock arrays must have the same length");
        if (n > 0) {
            std::vector<jint> persons(n), frames(n);
            env->GetIntArrayRegion(lock_persons, 0, n, persons.data());
            env->GetIntArrayRegion(lock_frames, 0, n, frames.data());
            for (jsize i = 0; i < n; ++i) locks[persons[i]] = frames[i];
        }
        return to_jstring(env, json::dump(selection_to_json(s.session->propose(locks, locked_base))));
    });
}

JNIEXPORT jstring JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeCandidates(
    JNIEnv* env, jclass, jlong handle, jint person, jint base) {
    return guarded(env, jstring(nullptr), [&] {
        NativeSession& s = from_handle(handle);
        EnvScope scope(s, env);
        return to_jstring(env, json::dump(candidates_to_json(*s.session, person, s.session->candidates(person, base))));
    });
}

JNIEXPORT jstring JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeBuildRecipe(
    JNIEnv* env, jclass, jlong handle, jint base, jintArray sources) {
    return guarded(env, jstring(nullptr), [&] {
        NativeSession& s = from_handle(handle);
        EnvScope scope(s, env);
        if (!sources) throw std::invalid_argument("sources are required");
        std::vector<jint> src(env->GetArrayLength(sources));
        env->GetIntArrayRegion(sources, 0, jsize(src.size()), src.data());
        return to_jstring(env, recipe_to_json(s.session->build_recipe(base, std::vector<int>(src.begin(), src.end()))));
    });
}

// Renders the recipe at 1 / sample_size. Returns the pixels; the quality
// report (JSON) is stored in quality_out[0].
JNIEXPORT jintArray JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeRender(
    JNIEnv* env, jclass, jlong handle, jstring recipe_json, jint sample_size, jobjectArray quality_out) {
    return guarded(env, jintArray(nullptr), [&] {
        NativeSession& s = from_handle(handle);
        EnvScope scope(s, env);
        Recipe recipe = recipe_from_json(to_string(env, recipe_json));
        std::vector<QualityReport> reports;
        Image out = s.session->render(recipe, scale_for(sample_size), &reports);
        if (quality_out && env->GetArrayLength(quality_out) > 0)
            env->SetObjectArrayElement(quality_out, 0, to_jstring(env, json::dump(quality_to_json(recipe, reports))));
        return argb_from_image(env, out);
    });
}

// Renders the patch that placing `donor` for `person` into `base` would
// produce. rect_out receives [x0, y0, x1, y1] in scaled base pixels and
// ok_out[0] whether the patch passes validation.
JNIEXPORT jintArray JNICALL Java_io_github_alecjacobson_besttake_engine_NativeSession_nativeRenderCandidate(
    JNIEnv* env, jclass, jlong handle, jint person, jint donor, jint base, jint sample_size, jintArray rect_out,
    jbooleanArray ok_out) {
    return guarded(env, jintArray(nullptr), [&] {
        NativeSession& s = from_handle(handle);
        EnvScope scope(s, env);
        if (!rect_out || env->GetArrayLength(rect_out) != 4 || !ok_out || env->GetArrayLength(ok_out) != 1)
            throw std::invalid_argument("rect_out must hold 4 ints and ok_out 1 boolean");
        std::vector<int> sources(s.session->person_count(), -1);
        if (person < 0 || person >= int(sources.size())) throw std::invalid_argument("person out of range");
        sources[person] = donor;
        Recipe recipe = s.session->build_recipe(base, sources);
        if (recipe.replacements.size() != 1) throw std::invalid_argument("donor must differ from the base");
        const double scale = scale_for(sample_size);
        PatchResult patch = s.session->render_replacement_patch(recipe, 0, scale);
        jint rect[4] = {patch.rect.x0, patch.rect.y0, patch.rect.x1, patch.rect.y1};
        env->SetIntArrayRegion(rect_out, 0, 4, rect);
        jboolean ok = patch.excess_gradient <= kDefaultMaxExcessGradient ? JNI_TRUE : JNI_FALSE;
        env->SetBooleanArrayRegion(ok_out, 0, 1, &ok);
        return argb_from_image(env, patch.pixels);
    });
}

}  // extern "C"
