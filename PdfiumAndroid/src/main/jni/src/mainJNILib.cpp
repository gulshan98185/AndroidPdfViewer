#include "util.hpp"
#include "fpdf_flatten.h"
#include "fpdf_formfill.h"

#define HAVE_PTHREADS true;
extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
}

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/bitmap.h>
#include <utils/Mutex.h>

using namespace android;

#include <fpdf_doc.h>
#include <fpdf_annot.h>
#include <fpdf_attachment.h>
#include <fpdf_edit.h>
#include <fpdfview.h>
#include <fpdf_doc.h>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <cfloat>
#include <climits>
#include <chrono>
#include <fpdf_text.h>

#define WM_LOGE(...) do {} while (0)

static Mutex sLibraryLock;
static Mutex sTextEditFontCacheLock;
static std::map<FPDF_DOCUMENT, std::map<std::string, FPDF_FONT>> sTextEditFontCache;
static void clearTextEditFontCache(FPDF_DOCUMENT document);

static int sLibraryReferenceCount = 0;

static void initLibraryIfNeed() {
    Mutex::Autolock lock(sLibraryLock);
    if (sLibraryReferenceCount == 0) {
        LOGD("Init FPDF library");
        FPDF_InitLibrary();
    }
    sLibraryReferenceCount++;
}

static void destroyLibraryIfNeed() {
    Mutex::Autolock lock(sLibraryLock);
    sLibraryReferenceCount--;
    if (sLibraryReferenceCount == 0) {
        LOGD("Destroy FPDF library");
        FPDF_DestroyLibrary();
    }
}

struct rgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

class DocumentFile {
private:
    int fileFd;

public:
    FPDF_DOCUMENT pdfDocument = NULL;
    FPDF_FORMHANDLE gForm = nullptr;
    size_t fileSize;

    DocumentFile() { initLibraryIfNeed(); }

    ~DocumentFile();
};

FPDF_BITMAP ConvertToFPDFBitmap(JNIEnv *pEnv, jobject pJobject);

DocumentFile::~DocumentFile() {
    if (pdfDocument != NULL) {
        clearTextEditFontCache(pdfDocument);
        FPDF_CloseDocument(pdfDocument);
    }

    destroyLibraryIfNeed();
}

template<class string_type>
inline typename string_type::value_type *WriteInto(string_type *str, size_t length_with_null) {
    str->reserve(length_with_null);
    str->resize(length_with_null - 1);
    return &((*str)[0]);
}

inline long getFileSize(int fd) {
    struct stat file_state;

    if (fstat(fd, &file_state) >= 0) {
        return (long) (file_state.st_size);
    } else {
        LOGE("Error getting file size");
        return 0;
    }
}

static char *getErrorDescription(const long error) {
    char *description = NULL;
    switch (error) {
        case FPDF_ERR_SUCCESS:
            asprintf(&description, "No error.");
            break;
        case FPDF_ERR_FILE:
            asprintf(&description, "File not found or could not be opened.");
            break;
        case FPDF_ERR_FORMAT:
            asprintf(&description, "File not in PDF format or corrupted.");
            break;
        case FPDF_ERR_PASSWORD:
            asprintf(&description, "Incorrect password.");
            break;
        case FPDF_ERR_SECURITY:
            asprintf(&description, "Unsupported security scheme.");
            break;
        case FPDF_ERR_PAGE:
            asprintf(&description, "Page not found or content error.");
            break;
        default:
            asprintf(&description, "Unknown error.");
    }

    return description;
}

int jniThrowException(JNIEnv *env, const char *className, const char *message) {
    jclass exClass = env->FindClass(className);
    if (exClass == NULL) {
        LOGE("Unable to find exception class %s", className);
        return -1;
    }

    if (env->ThrowNew(exClass, message) != JNI_OK) {
        LOGE("Failed throwing '%s' '%s'", className, message);
        return -1;
    }

    return 0;
}

int jniThrowExceptionFmt(JNIEnv *env, const char *className, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char msgBuf[512];
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    return jniThrowException(env, className, msgBuf);
    va_end(args);
}

jobject NewLong(JNIEnv *env, jlong value) {
    jclass cls = env->FindClass("java/lang/Long");
    jmethodID methodID = env->GetMethodID(cls, "<init>", "(J)V");
    return env->NewObject(cls, methodID, value);
}

jobject NewInteger(JNIEnv *env, jint value) {
    jclass cls = env->FindClass("java/lang/Integer");
    jmethodID methodID = env->GetMethodID(cls, "<init>", "(I)V");
    return env->NewObject(cls, methodID, value);
}

uint16_t rgbTo565(rgb *color) {
    return ((color->red >> 3) << 11) | ((color->green >> 2) << 5) | (color->blue >> 3);
}

void rgbBitmapTo565(void *source, int sourceStride, void *dest, AndroidBitmapInfo *info) {
    rgb *srcLine;
    uint16_t *dstLine;
    int y, x;
    for (y = 0; y < info->height; y++) {
        srcLine = (rgb *) source;
        dstLine = (uint16_t *) dest;
        for (x = 0; x < info->width; x++) {
            dstLine[x] = rgbTo565(&srcLine[x]);
        }
        source = (char *) source + sourceStride;
        dest = (char *) dest + info->stride;
    }
}

static int getBlock(void *param, unsigned long position, unsigned char *outBuffer,
                    unsigned long size);

static jobject DecodeBitmapFile(JNIEnv* env, const char* imagePath) {
    if (!env || !imagePath || strlen(imagePath) == 0) return nullptr;

    jclass bitmapFactoryClass = env->FindClass("android/graphics/BitmapFactory");
    if (!bitmapFactoryClass) return nullptr;

    jmethodID decodeFileMethod = env->GetStaticMethodID(
            bitmapFactoryClass,
            "decodeFile",
            "(Ljava/lang/String;)Landroid/graphics/Bitmap;"
    );
    if (!decodeFileMethod) {
        env->DeleteLocalRef(bitmapFactoryClass);
        return nullptr;
    }

    jstring jPath = env->NewStringUTF(imagePath);
    jobject bitmap = env->CallStaticObjectMethod(bitmapFactoryClass, decodeFileMethod, jPath);
    env->DeleteLocalRef(jPath);
    env->DeleteLocalRef(bitmapFactoryClass);

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (bitmap) env->DeleteLocalRef(bitmap);
        return nullptr;
    }
    return bitmap;
}

FPDF_BITMAP ConvertToFPDFBitmap(JNIEnv *env, jobject bitmap) {
    if (!env || !bitmap) return nullptr;

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        return nullptr;
    }
    if (info.width <= 0 || info.height <= 0) {
        return nullptr;
    }

    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS || !pixels) {
        return nullptr;
    }

    FPDF_BITMAP pdfBitmap = FPDFBitmap_Create(info.width, info.height, 1);
    if (!pdfBitmap) {
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }

    auto* dstBase = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(pdfBitmap));
    const int dstStride = FPDFBitmap_GetStride(pdfBitmap);
    if (!dstBase || dstStride <= 0) {
        FPDFBitmap_Destroy(pdfBitmap);
        AndroidBitmap_unlockPixels(env, bitmap);
        return nullptr;
    }

    switch (info.format) {
        case ANDROID_BITMAP_FORMAT_RGBA_8888: {
            for (uint32_t y = 0; y < info.height; ++y) {
                const auto* srcLine = static_cast<const uint8_t*>(pixels) + (y * info.stride);
                auto* dstLine = dstBase + (y * dstStride);
                for (uint32_t x = 0; x < info.width; ++x) {
                    const uint8_t* srcPixel = srcLine + (x * 4);
                    uint8_t* dstPixel = dstLine + (x * 4);
                    if (srcPixel[3] == 0) {
                        dstPixel[0] = 255;
                        dstPixel[1] = 255;
                        dstPixel[2] = 255;
                        dstPixel[3] = 0;
                    } else {
                        dstPixel[0] = srcPixel[2];
                        dstPixel[1] = srcPixel[1];
                        dstPixel[2] = srcPixel[0];
                        dstPixel[3] = srcPixel[3];
                    }
                }
            }
            break;
        }
        case ANDROID_BITMAP_FORMAT_RGB_565: {
            for (uint32_t y = 0; y < info.height; ++y) {
                const auto* srcLine = reinterpret_cast<const uint16_t*>(
                        static_cast<const uint8_t*>(pixels) + (y * info.stride)
                );
                auto* dstLine = dstBase + (y * dstStride);
                for (uint32_t x = 0; x < info.width; ++x) {
                    const uint16_t pixel = srcLine[x];
                    const uint8_t red = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
                    const uint8_t green = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
                    const uint8_t blue = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
                    uint8_t* dstPixel = dstLine + (x * 4);
                    dstPixel[0] = blue;
                    dstPixel[1] = green;
                    dstPixel[2] = red;
                    dstPixel[3] = 255;
                }
            }
            break;
        }
        default:
            FPDFBitmap_Destroy(pdfBitmap);
            AndroidBitmap_unlockPixels(env, bitmap);
            return nullptr;
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    return pdfBitmap;
}

static bool LoadJpegFileIntoImageObject(const char* imagePath, FPDF_PAGEOBJECT imageObj) {
    if (!imagePath || !imageObj) return false;

    const int imageFd = open(imagePath, O_RDONLY);
    if (imageFd < 0) {
        return false;
    }

    const size_t fileLength = static_cast<size_t>(getFileSize(imageFd));
    if (fileLength == 0) {
        close(imageFd);
        return false;
    }

    FPDF_FILEACCESS loader;
    loader.m_FileLen = fileLength;
    loader.m_Param = reinterpret_cast<void*>(intptr_t(imageFd));
    loader.m_GetBlock = &getBlock;

    const bool loaded = FPDFImageObj_LoadJpegFileInline(nullptr, 0, imageObj, &loader);
    close(imageFd);
    return loaded;
}

static bool LoadBitmapFileIntoImageObject(JNIEnv* env, const char* imagePath, FPDF_PAGEOBJECT imageObj) {
    if (!env || !imagePath || !imageObj) return false;

    jobject bitmap = DecodeBitmapFile(env, imagePath);
    if (!bitmap) return false;

    FPDF_BITMAP pdfBitmap = ConvertToFPDFBitmap(env, bitmap);
    env->DeleteLocalRef(bitmap);
    if (!pdfBitmap) {
        return false;
    }

    const bool loaded = FPDFImageObj_SetBitmap(nullptr, 0, imageObj, pdfBitmap);
    FPDFBitmap_Destroy(pdfBitmap);
    return loaded;
}

extern "C" { //For JNI support

static int getBlock(void *param, unsigned long position, unsigned char *outBuffer,
                    unsigned long size) {
    const int fd = reinterpret_cast<intptr_t>(param);
    const int readCount = pread(fd, outBuffer, size, position);
    if (readCount < 0) {
        LOGE("Cannot read from file descriptor. Error:%d", errno);
        return 0;
    }
    return 1;
}

static std::u16string ReadAnnotStringValueUtf16(FPDF_ANNOTATION annot, FPDF_BYTESTRING key) {
    const unsigned long valueLength = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
    if (valueLength <= sizeof(FPDF_WCHAR)) {
        return std::u16string();
    }

    std::vector<FPDF_WCHAR> valueBuffer(valueLength / sizeof(FPDF_WCHAR));
    FPDFAnnot_GetStringValue(annot, key, valueBuffer.data(), valueLength);
    const int valueCharCount = static_cast<int>(valueBuffer.size()) - 1;
    if (valueCharCount <= 0) {
        return std::u16string();
    }

    return std::u16string(
            reinterpret_cast<const char16_t*>(valueBuffer.data()),
            valueCharCount
    );
}

static jstring ReadAnnotStringValueJString(JNIEnv* env, FPDF_ANNOTATION annot, FPDF_BYTESTRING key) {
    const std::u16string value = ReadAnnotStringValueUtf16(annot, key);
    if (value.empty()) {
        return nullptr;
    }
    return env->NewString(reinterpret_cast<const jchar*>(value.data()), static_cast<jsize>(value.size()));
}

static std::string BuildLocalDestLinkTarget(FPDF_DOCUMENT doc, FPDF_DEST dest) {
    if (!doc || !dest) return std::string();
    const int pageIndex = FPDFDest_GetDestPageIndex(doc, dest);
    if (pageIndex < 0) return std::string();
    char buffer[16] = {0};
    buffer[0] = '@';
    snprintf(buffer + 1, sizeof(buffer) - 1, "%d", pageIndex);
    return std::string(buffer);
}

static jobject ConvertFPDFBitmapToAndroidBitmap(JNIEnv* env, FPDF_BITMAP pdfBitmap) {
    if (!env || !pdfBitmap) return nullptr;
    const int width = FPDFBitmap_GetWidth(pdfBitmap);
    const int height = FPDFBitmap_GetHeight(pdfBitmap);
    const int srcStride = FPDFBitmap_GetStride(pdfBitmap);
    const int srcFormat = FPDFBitmap_GetFormat(pdfBitmap);
    auto* srcBase = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(pdfBitmap));
    if (width <= 0 || height <= 0 || !srcBase || srcStride <= 0) return nullptr;
    if (static_cast<int64_t>(width) * static_cast<int64_t>(height) > 16000000LL) return nullptr;

    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    if (!bitmapClass || !configClass) return nullptr;
    jfieldID argbField = env->GetStaticFieldID(
            configClass,
            "ARGB_8888",
            "Landroid/graphics/Bitmap$Config;"
    );
    jmethodID createBitmap = env->GetStaticMethodID(
            bitmapClass,
            "createBitmap",
            "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"
    );
    jobject config = argbField ? env->GetStaticObjectField(configClass, argbField) : nullptr;
    jobject bitmap = (createBitmap && config)
                     ? env->CallStaticObjectMethod(bitmapClass, createBitmap, width, height, config)
                     : nullptr;
    if (config) env->DeleteLocalRef(config);
    env->DeleteLocalRef(configClass);
    env->DeleteLocalRef(bitmapClass);
    if (!bitmap || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (bitmap) env->DeleteLocalRef(bitmap);
        return nullptr;
    }

    AndroidBitmapInfo info{};
    void* dstPixels = nullptr;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_lockPixels(env, bitmap, &dstPixels) != ANDROID_BITMAP_RESULT_SUCCESS ||
        !dstPixels) {
        env->DeleteLocalRef(bitmap);
        return nullptr;
    }

    for (int y = 0; y < height; ++y) {
        const uint8_t* src = srcBase + (y * srcStride);
        uint8_t* dst = static_cast<uint8_t*>(dstPixels) + (y * info.stride);
        for (int x = 0; x < width; ++x) {
            uint8_t* dstPixel = dst + (x * 4);
            if (srcFormat == FPDFBitmap_BGRA || srcFormat == FPDFBitmap_BGRx) {
                const uint8_t* srcPixel = src + (x * 4);
                dstPixel[0] = srcPixel[2];
                dstPixel[1] = srcPixel[1];
                dstPixel[2] = srcPixel[0];
                dstPixel[3] = srcFormat == FPDFBitmap_BGRA ? srcPixel[3] : 255;
            } else if (srcFormat == FPDFBitmap_BGR) {
                const uint8_t* srcPixel = src + (x * 3);
                dstPixel[0] = srcPixel[2];
                dstPixel[1] = srcPixel[1];
                dstPixel[2] = srcPixel[0];
                dstPixel[3] = 255;
            } else if (srcFormat == FPDFBitmap_Gray) {
                const uint8_t value = src[x];
                dstPixel[0] = value;
                dstPixel[1] = value;
                dstPixel[2] = value;
                dstPixel[3] = 255;
            } else {
                AndroidBitmap_unlockPixels(env, bitmap);
                env->DeleteLocalRef(bitmap);
                return nullptr;
            }
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    return bitmap;
}

//static std::string ReadLinkAnnotationTarget(FPDF_DOCUMENT doc, FPDF_ANNOTATION annot) {
//    if (!doc || !annot || FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINK) return std::string();
//    FPDF_LINK link = FPDFAnnot_GetLink(annot);
//    if (!link) return std::string();
//
//    const std::string directDestTarget = BuildLocalDestLinkTarget(doc, FPDFLink_GetDest(doc, link));
//    if (!directDestTarget.empty()) return directDestTarget;
//
//    return std::string();
//}

 static std::string ReadLinkAnnotationTarget(FPDF_DOCUMENT doc,FPDF_ANNOTATION annot) {
    if (!doc || !annot) {return std::string();}
    if (FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_LINK) {
        return std::string();
    }
    FPDF_LINK link = FPDFAnnot_GetLink(annot);
    if (!link) {
        return std::string();
    }
    return std::string();
}

static void SetAnnotAsciiStringValue(FPDF_ANNOTATION annot, FPDF_BYTESTRING key, const char* value) {
    if (!annot || !value) return;
    const size_t valueLen = strlen(value);
    std::vector<unsigned short> utf16Value(valueLen + 1);
    for (size_t index = 0; index < valueLen; index++) {
        utf16Value[index] = static_cast<unsigned short>(value[index]);
    }
    utf16Value[valueLen] = 0;
    FPDFAnnot_SetStringValue(annot, key, (FPDF_WIDESTRING)utf16Value.data());
}

static std::string Utf16ToSimpleUtf8(const std::u16string& value) {
    std::string output;
    output.reserve(value.size());
    for (char16_t ch : value) {
        output.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
    }
    return output;
}

static std::string EscapeJsonString(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': break;
            case '\t': output += "\\t"; break;
            default: output += ch; break;
        }
    }
    return output;
}

static jstring AppendSignatureSubtypeToPropsJson(
        JNIEnv* env,
        jstring props,
        const std::u16string& signatureSubtype
) {
    if (!env || !props || signatureSubtype.empty()) {
        return props;
    }

    const char* propsChars = env->GetStringUTFChars(props, nullptr);
    if (!propsChars) {
        return props;
    }

    std::string propsJson(propsChars);
    env->ReleaseStringUTFChars(props, propsChars);

    if (propsJson.find("\"signatureSubType\"") != std::string::npos) {
        return props;
    }

    const size_t openBrace = propsJson.find('{');
    const size_t closeBrace = propsJson.find_last_of('}');
    if (openBrace == std::string::npos ||
        closeBrace == std::string::npos ||
        closeBrace <= openBrace) {
        return props;
    }

    bool hasExistingPairs = false;
    for (size_t index = openBrace + 1; index < closeBrace; index++) {
        if (!std::isspace(static_cast<unsigned char>(propsJson[index]))) {
            hasExistingPairs = true;
            break;
        }
    }

    std::string updatedJson = propsJson.substr(0, closeBrace);
    if (hasExistingPairs) {
        updatedJson += ",";
    }
    updatedJson += "\"signatureSubType\":\"";
    updatedJson += EscapeJsonString(Utf16ToSimpleUtf8(signatureSubtype));
    updatedJson += "\"";
    updatedJson += propsJson.substr(closeBrace);

    jstring updatedProps = env->NewStringUTF(updatedJson.c_str());
    if (!updatedProps) {
        return props;
    }

    env->DeleteLocalRef(props);
    return updatedProps;
}

static jstring AppendSignImageFlagToPropsJson(JNIEnv* env, jstring props) {
    if (!env || !props) {
        return props;
    }

    const char* propsChars = env->GetStringUTFChars(props, nullptr);
    if (!propsChars) {
        return props;
    }

    std::string propsJson(propsChars);
    env->ReleaseStringUTFChars(props, propsChars);

    if (propsJson.find("\"signImage\"") != std::string::npos) {
        return props;
    }

    const size_t openBrace = propsJson.find('{');
    const size_t closeBrace = propsJson.find_last_of('}');
    if (openBrace == std::string::npos ||
        closeBrace == std::string::npos ||
        closeBrace <= openBrace) {
        return props;
    }

    bool hasExistingPairs = false;
    for (size_t index = openBrace + 1; index < closeBrace; index++) {
        if (!std::isspace(static_cast<unsigned char>(propsJson[index]))) {
            hasExistingPairs = true;
            break;
        }
    }

    std::string updatedJson = propsJson.substr(0, closeBrace);
    if (hasExistingPairs) {
        updatedJson += ",";
    }
    updatedJson += "\"signImage\":true";
    updatedJson += propsJson.substr(closeBrace);

    jstring updatedProps = env->NewStringUTF(updatedJson.c_str());
    if (!updatedProps) {
        return props;
    }

    env->DeleteLocalRef(props);
    return updatedProps;
}

// once per document (store somewhere):
static void ensureForm(DocumentFile* docFile){
    if (!docFile->gForm) {
        static FPDF_FORMFILLINFO ffi{}; ffi.version = 2;
        docFile->gForm = FPDFDOC_InitFormFillEnvironment(docFile->pdfDocument, &ffi);
        FPDF_SetFormFieldHighlightAlpha(docFile->gForm, 0);
    }
}

JNI_FUNC(jlong, PdfiumCore, nativeOpenDocument)(JNI_ARGS, jint fd, jstring password) {

    size_t fileLength = (size_t) getFileSize(fd);
    if (fileLength <= 0) {
        jniThrowException(env, "java/io/IOException",
                          "File is empty");
        return -1;
    }

    DocumentFile *docFile = new DocumentFile();

    FPDF_FILEACCESS loader;
    loader.m_FileLen = fileLength;
    loader.m_Param = reinterpret_cast<void *>(intptr_t(fd));
    loader.m_GetBlock = &getBlock;

    const char *cpassword = NULL;
    if (password != NULL) {
        cpassword = env->GetStringUTFChars(password, NULL);
    }

    FPDF_DOCUMENT document = FPDF_LoadCustomDocument(&loader, cpassword);

    if (cpassword != NULL) {
        env->ReleaseStringUTFChars(password, cpassword);
    }

    if (!document) {
        delete docFile;

        const long errorNum = FPDF_GetLastError();
        if (errorNum == FPDF_ERR_PASSWORD) {
            jniThrowException(env, "com/shockwave/pdfium/PdfPasswordException",
                              "Password required or incorrect password.");
        } else {
            char *error = getErrorDescription(errorNum);
            jniThrowExceptionFmt(env, "java/io/IOException",
                                 "cannot create document: %s", error);

            free(error);
        }

        return -1;
    }

    docFile->pdfDocument = document;
    ensureForm(docFile);

    return reinterpret_cast<jlong>(docFile);
}

JNI_FUNC(jlong, PdfiumCore, nativeOpenMemDocument)(JNI_ARGS, jbyteArray data, jstring password) {
    DocumentFile *docFile = new DocumentFile();

    const char *cpassword = NULL;
    if (password != NULL) {
        cpassword = env->GetStringUTFChars(password, NULL);
    }

    jbyte *cData = env->GetByteArrayElements(data, NULL);
    int size = (int) env->GetArrayLength(data);
    jbyte *cDataCopy = new jbyte[size];
    memcpy(cDataCopy, cData, size);
    FPDF_DOCUMENT document = FPDF_LoadMemDocument(reinterpret_cast<const void *>(cDataCopy),
                                                  size, cpassword);
    env->ReleaseByteArrayElements(data, cData, JNI_ABORT);

    if (cpassword != NULL) {
        env->ReleaseStringUTFChars(password, cpassword);
    }

    if (!document) {
        delete docFile;

        const long errorNum = FPDF_GetLastError();
        if (errorNum == FPDF_ERR_PASSWORD) {
            jniThrowException(env, "com/shockwave/pdfium/PdfPasswordException",
                              "Password required or incorrect password.");
        } else {
            char *error = getErrorDescription(errorNum);
            jniThrowExceptionFmt(env, "java/io/IOException",
                                 "cannot create document: %s", error);

            free(error);
        }

        return -1;
    }

    docFile->pdfDocument = document;

    return reinterpret_cast<jlong>(docFile);
}

JNI_FUNC(jint, PdfiumCore, nativeGetPageCount)(JNI_ARGS, jlong documentPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(documentPtr);
    return (jint) FPDF_GetPageCount(doc->pdfDocument);
}

JNI_FUNC(void, PdfiumCore, nativeCloseDocument)(JNI_ARGS, jlong documentPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(documentPtr);
    delete doc;
}

static jlong loadPageInternal(JNIEnv *env, DocumentFile *doc, int pageIndex) {
    try {
        if (doc == NULL) throw "Get page document null";

        FPDF_DOCUMENT pdfDoc = doc->pdfDocument;
        if (pdfDoc != NULL) {
            FPDF_PAGE page = FPDF_LoadPage(pdfDoc, pageIndex);
            if (page == NULL) {
                throw "Loaded page is null";
            }
            return reinterpret_cast<jlong>(page);
        } else {
            throw "Get page pdf document null";
        }

    } catch (const char *msg) {
        LOGE("%s", msg);

        jniThrowException(env, "java/lang/IllegalStateException",
                          "cannot load page");

        return -1;
    }
}

static void closePageInternal(jlong pagePtr) {
    FPDF_ClosePage(reinterpret_cast<FPDF_PAGE>(pagePtr));
}

JNI_FUNC(jlong, PdfiumCore, nativeLoadPage)(JNI_ARGS, jlong docPtr, jint pageIndex) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    return loadPageInternal(env, doc, (int) pageIndex);
}
JNI_FUNC(jlongArray, PdfiumCore, nativeLoadPages)(JNI_ARGS, jlong docPtr, jint fromIndex,
                                                  jint toIndex) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);

    if (toIndex < fromIndex) return NULL;
    jlong pages[toIndex - fromIndex + 1];

    int i;
    for (i = 0; i <= (toIndex - fromIndex); i++) {
        pages[i] = loadPageInternal(env, doc, (int) (i + fromIndex));
    }

    jlongArray javaPages = env->NewLongArray((jsize) (toIndex - fromIndex + 1));
    env->SetLongArrayRegion(javaPages, 0, (jsize) (toIndex - fromIndex + 1), (const jlong *) pages);

    return javaPages;
}

JNI_FUNC(void, PdfiumCore, nativeClosePage)(JNI_ARGS, jlong pagePtr) { closePageInternal(pagePtr); }
JNI_FUNC(void, PdfiumCore, nativeClosePages)(JNI_ARGS, jlongArray pagesPtr) {
    int length = (int) (env->GetArrayLength(pagesPtr));
    jlong *pages = env->GetLongArrayElements(pagesPtr, NULL);

    int i;
    for (i = 0; i < length; i++) { closePageInternal(pages[i]); }
}

JNI_FUNC(jint, PdfiumCore, nativeGetPageWidthPixel)(JNI_ARGS, jlong pagePtr, jint dpi) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    return (jint) (FPDF_GetPageWidth(page) * dpi / 72);
}
JNI_FUNC(jint, PdfiumCore, nativeGetPageHeightPixel)(JNI_ARGS, jlong pagePtr, jint dpi) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    return (jint) (FPDF_GetPageHeight(page) * dpi / 72);
}
JNI_FUNC(jint, PdfiumCore, nativeGetPageWidthPoint)(JNI_ARGS, jlong pagePtr) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    return (jint) FPDF_GetPageWidth(page);
}
JNI_FUNC(jint, PdfiumCore, nativeGetPageHeightPoint)(JNI_ARGS, jlong pagePtr) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    return (jint) FPDF_GetPageHeight(page);
}
JNI_FUNC(jobject, PdfiumCore, nativeGetPageSizeByIndex)(JNI_ARGS, jlong docPtr, jint pageIndex,
                                                        jint dpi) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    if (doc == NULL) {
        LOGE("Document is null");

        jniThrowException(env, "java/lang/IllegalStateException",
                          "Document is null");
        return NULL;
    }

    double width, height;
    int result = FPDF_GetPageSizeByIndex(doc->pdfDocument, pageIndex, &width, &height);

    if (result == 0) {
        width = 0;
        height = 0;
    }

    jint widthInt = (jint) (width * dpi / 72);
    jint heightInt = (jint) (height * dpi / 72);

    jclass clazz = env->FindClass("com/shockwave/pdfium/util/Size");
    jmethodID constructorID = env->GetMethodID(clazz, "<init>", "(II)V");
    return env->NewObject(clazz, constructorID, widthInt, heightInt);
}

static void renderPageInternal(FPDF_PAGE page,
                               ANativeWindow_Buffer *windowBuffer,
                               int startX, int startY,
                               int canvasHorSize, int canvasVerSize,
                               int drawSizeHor, int drawSizeVer,
                               bool renderAnnot) {

    FPDF_BITMAP pdfBitmap = FPDFBitmap_CreateEx(canvasHorSize, canvasVerSize,
                                                FPDFBitmap_BGRA,
                                                windowBuffer->bits,
                                                (int) (windowBuffer->stride) * 4);

    /*LOGD("Start X: %d", startX);
    LOGD("Start Y: %d", startY);
    LOGD("Canvas Hor: %d", canvasHorSize);
    LOGD("Canvas Ver: %d", canvasVerSize);
    LOGD("Draw Hor: %d", drawSizeHor);
    LOGD("Draw Ver: %d", drawSizeVer);*/

    if (drawSizeHor < canvasHorSize || drawSizeVer < canvasVerSize) {
        FPDFBitmap_FillRect(pdfBitmap, 0, 0, canvasHorSize, canvasVerSize,
                            0x848484FF); //Gray
    }

    int baseHorSize = (canvasHorSize < drawSizeHor) ? canvasHorSize : drawSizeHor;
    int baseVerSize = (canvasVerSize < drawSizeVer) ? canvasVerSize : drawSizeVer;
    int baseX = (startX < 0) ? 0 : startX;
    int baseY = (startY < 0) ? 0 : startY;
    int flags = FPDF_REVERSE_BYTE_ORDER;

    if (renderAnnot) {
        flags |= FPDF_ANNOT;
    }

    FPDFBitmap_FillRect(pdfBitmap, baseX, baseY, baseHorSize, baseVerSize,
                        0xFFFFFFFF); //White

    FPDF_RenderPageBitmap(pdfBitmap, page,
                          startX, startY,
                          drawSizeHor, drawSizeVer,
                          0, flags);
}

JNI_FUNC(void, PdfiumCore, nativeRenderPage)(JNI_ARGS, jlong pagePtr, jobject objSurface,
                                             jint dpi, jint startX, jint startY,
                                             jint drawSizeHor, jint drawSizeVer,
                                             jboolean renderAnnot) {
    ANativeWindow *nativeWindow = ANativeWindow_fromSurface(env, objSurface);
    if (nativeWindow == NULL) {
        LOGE("native window pointer null");
        return;
    }
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);

    if (page == NULL || nativeWindow == NULL) {
        LOGE("Render page pointers invalid");
        return;
    }

    if (ANativeWindow_getFormat(nativeWindow) != WINDOW_FORMAT_RGBA_8888) {
        LOGD("Set format to RGBA_8888");
        ANativeWindow_setBuffersGeometry(nativeWindow,
                                         ANativeWindow_getWidth(nativeWindow),
                                         ANativeWindow_getHeight(nativeWindow),
                                         WINDOW_FORMAT_RGBA_8888);
    }

    ANativeWindow_Buffer buffer;
    int ret;
    if ((ret = ANativeWindow_lock(nativeWindow, &buffer, NULL)) != 0) {
        LOGE("Locking native window failed: %s", strerror(ret * -1));
        return;
    }

    renderPageInternal(page, &buffer,
                       (int) startX, (int) startY,
                       buffer.width, buffer.height,
                       (int) drawSizeHor, (int) drawSizeVer,
                       (bool) renderAnnot);

    ANativeWindow_unlockAndPost(nativeWindow);
    ANativeWindow_release(nativeWindow);
}

JNI_FUNC(jstring, PdfiumCore, nativeGetLinkTarget)(JNI_ARGS, jlong docPtr, jlong linkPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_LINK link = reinterpret_cast<FPDF_LINK>(linkPtr);
    FPDF_DEST dest = FPDFLink_GetDest(doc->pdfDocument, link);
    if (dest != NULL) {
        long pageIdx = FPDFDest_GetDestPageIndex(doc->pdfDocument, dest);
        char buffer[16] = {0};
        buffer[0] = '@';
        sprintf(buffer + 1, "%d", (int) pageIdx);
        return env->NewStringUTF(buffer);
    }
    FPDF_ACTION action = FPDFLink_GetAction(link);
    if (action == NULL) {
        return NULL;
    }
    size_t bufferLen = FPDFAction_GetURIPath(doc->pdfDocument, action, NULL, 0);
    if (bufferLen <= 0) {
        return NULL;
    }
    std::string uri;
    FPDFAction_GetURIPath(doc->pdfDocument, action, WriteInto(&uri, bufferLen), bufferLen);
    return env->NewStringUTF(uri.c_str());
}
JNI_FUNC(jlong, PdfiumCore, nativeGetLinkAtCoord)(JNI_ARGS, jlong pagePtr, jdouble width,
                                                  jdouble height, jdouble posX, jdouble posY) {
    double px, py;
    FPDF_DeviceToPage((FPDF_PAGE) pagePtr, 0, 0, width, height, 0, posX, posY, &px, &py);
    return (jlong) FPDFLink_GetLinkAtPoint((FPDF_PAGE) pagePtr, px, py);
}
JNI_FUNC(jint, PdfiumCore, nativeGetCharIndexAtCoord)(JNI_ARGS, jlong pagePtr, jdouble width,
                                                      jdouble height, jlong textPtr, jdouble posX,
                                                      jdouble posY, jdouble tolX, jdouble tolY) {
    double px, py;
    FPDF_DeviceToPage((FPDF_PAGE) pagePtr, 0, 0, width, height, 0, posX, posY, &px, &py);
    return FPDFText_GetCharIndexAtPos((FPDF_TEXTPAGE) textPtr, px, py, tolX, tolY);
}
JNI_FUNC(jstring, PdfiumCore, nativeGetText)(JNI_ARGS, jlong textPtr) {
    int len = FPDFText_CountChars((FPDF_TEXTPAGE) textPtr);
    //unsigned short* buffer = malloc(len*sizeof(unsigned short));
    unsigned short *buffer = new unsigned short[len + 1];
    FPDFText_GetText((FPDF_TEXTPAGE) textPtr, 0, len, buffer);
    jstring ret = env->NewString(buffer, len);
    delete[]buffer;
    return ret;
}

JNI_FUNC(void, PdfiumCore, nativeRenderPageBitmap)(JNI_ARGS, jlong docPtr, jlong pagePtr, jobject bitmap,
                                                   jint dpi, jint startX, jint startY,
                                                   jint drawSizeHor, jint drawSizeVer,
                                                   jboolean renderAnnot) {

    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);

    if (page == NULL || bitmap == NULL) {
        LOGE("Render page pointers invalid");
        return;
    }

    AndroidBitmapInfo info;
    int ret;
    if ((ret = AndroidBitmap_getInfo(env, bitmap, &info)) < 0) {
        LOGE("Fetching bitmap info failed: %s", strerror(ret * -1));
        return;
    }

    int canvasHorSize = info.width;
    int canvasVerSize = info.height;

    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 &&
        info.format != ANDROID_BITMAP_FORMAT_RGB_565) {
        LOGE("Bitmap format must be RGBA_8888 or RGB_565");
        return;
    }

    void *addr;
    if ((ret = AndroidBitmap_lockPixels(env, bitmap, &addr)) != 0) {
        LOGE("Locking bitmap failed: %s", strerror(ret * -1));
        return;
    }

    void *tmp;
    int format;
    int sourceStride;
    if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        tmp = malloc(canvasVerSize * canvasHorSize * sizeof(rgb));
        sourceStride = canvasHorSize * sizeof(rgb);
        format = FPDFBitmap_BGR;
    } else {
        tmp = addr;
        sourceStride = info.stride;
        format = FPDFBitmap_BGRA;
    }

    FPDF_BITMAP pdfBitmap = FPDFBitmap_CreateEx(canvasHorSize, canvasVerSize,
                                                format, tmp, sourceStride);

    /*LOGD("Start X: %d", startX);
    LOGD("Start Y: %d", startY);
    LOGD("Canvas Hor: %d", canvasHorSize);
    LOGD("Canvas Ver: %d", canvasVerSize);
    LOGD("Draw Hor: %d", drawSizeHor);
    LOGD("Draw Ver: %d", drawSizeVer);*/

    if (drawSizeHor < canvasHorSize || drawSizeVer < canvasVerSize) {
        FPDFBitmap_FillRect(pdfBitmap, 0, 0, canvasHorSize, canvasVerSize,
                            0x848484FF); //Gray
    }

    int baseHorSize = (canvasHorSize < drawSizeHor) ? canvasHorSize : (int) drawSizeHor;
    int baseVerSize = (canvasVerSize < drawSizeVer) ? canvasVerSize : (int) drawSizeVer;
    int baseX = (startX < 0) ? 0 : (int) startX;
    int baseY = (startY < 0) ? 0 : (int) startY;

    FPDFBitmap_FillRect(pdfBitmap, baseX, baseY, baseHorSize, baseVerSize,
                        0xFFFFFFFF); //White

    // 1) Page content (text, images, vectors, optional annotations)
    int flagsBase = FPDF_REVERSE_BYTE_ORDER | FPDF_LCD_TEXT;
    if (renderAnnot) {
        flagsBase |= FPDF_ANNOT;
    }
    FPDF_RenderPageBitmap(pdfBitmap, page,
                          startX, startY,
                          (int)drawSizeHor, (int)drawSizeVer,
                          0, flagsBase);

    // 2) Form widgets (AcroForm fields, signature appearance)
    int flagsFFL = FPDF_REVERSE_BYTE_ORDER | FPDF_LCD_TEXT;
    FPDF_FFLDraw(doc->gForm, pdfBitmap, page,
                 startX, startY,
                 (int)drawSizeHor, (int)drawSizeVer,
                 0, flagsFFL);

    if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        rgbBitmapTo565(tmp, sourceStride, addr, &info);
        free(tmp);
    }

    AndroidBitmap_unlockPixels(env, bitmap);
}

JNI_FUNC(jstring, PdfiumCore, nativeGetDocumentMetaText)(JNI_ARGS, jlong docPtr, jstring tag) {
    const char *ctag = env->GetStringUTFChars(tag, NULL);
    if (ctag == NULL) {
        return env->NewStringUTF("");
    }
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);

    size_t bufferLen = FPDF_GetMetaText(doc->pdfDocument, ctag, NULL, 0);
    if (bufferLen <= 2) {
        return env->NewStringUTF("");
    }
    std::wstring text;
    FPDF_GetMetaText(doc->pdfDocument, ctag, WriteInto(&text, bufferLen + 1), bufferLen);
    env->ReleaseStringUTFChars(tag, ctag);
    return env->NewString((jchar *) text.c_str(), bufferLen / 2 - 1);
}

JNI_FUNC(jobject, PdfiumCore, nativeGetFirstChildBookmark)(JNI_ARGS, jlong docPtr,
                                                           jobject bookmarkPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_BOOKMARK parent;
    if (bookmarkPtr == NULL) {
        parent = NULL;
    } else {
        jclass longClass = env->GetObjectClass(bookmarkPtr);
        jmethodID longValueMethod = env->GetMethodID(longClass, "longValue", "()J");

        jlong ptr = env->CallLongMethod(bookmarkPtr, longValueMethod);
        parent = reinterpret_cast<FPDF_BOOKMARK>(ptr);
    }
    FPDF_BOOKMARK bookmark = FPDFBookmark_GetFirstChild(doc->pdfDocument, parent);
    if (bookmark == NULL) {
        return NULL;
    }
    return NewLong(env, reinterpret_cast<jlong>(bookmark));
}

JNI_FUNC(jobject, PdfiumCore, nativeGetSiblingBookmark)(JNI_ARGS, jlong docPtr, jlong bookmarkPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_BOOKMARK parent = reinterpret_cast<FPDF_BOOKMARK>(bookmarkPtr);
    FPDF_BOOKMARK bookmark = FPDFBookmark_GetNextSibling(doc->pdfDocument, parent);
    if (bookmark == NULL) {
        return NULL;
    }
    return NewLong(env, reinterpret_cast<jlong>(bookmark));
}

JNI_FUNC(jstring, PdfiumCore, nativeGetBookmarkTitle)(JNI_ARGS, jlong bookmarkPtr) {
    FPDF_BOOKMARK bookmark = reinterpret_cast<FPDF_BOOKMARK>(bookmarkPtr);
    size_t bufferLen = FPDFBookmark_GetTitle(bookmark, NULL, 0);
    if (bufferLen <= 2) {
        return env->NewStringUTF("");
    }
    std::wstring title;
    FPDFBookmark_GetTitle(bookmark, WriteInto(&title, bufferLen + 1), bufferLen);
    return env->NewString((jchar *) title.c_str(), bufferLen / 2 - 1);
}

JNI_FUNC(jlong, PdfiumCore, nativeGetBookmarkDestIndex)(JNI_ARGS, jlong docPtr, jlong bookmarkPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_BOOKMARK bookmark = reinterpret_cast<FPDF_BOOKMARK>(bookmarkPtr);

    FPDF_DEST dest = FPDFBookmark_GetDest(doc->pdfDocument, bookmark);
    if (dest == NULL) {
        return -1;
    }
    return (jlong) FPDFDest_GetDestPageIndex(doc->pdfDocument, dest);
}

JNI_FUNC(jlongArray, PdfiumCore, nativeGetPageLinks)(JNI_ARGS, jlong pagePtr) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    int pos = 0;
    std::vector<jlong> links;
    FPDF_LINK link;
    while (FPDFLink_Enumerate(page, &pos, &link)) {
        links.push_back(reinterpret_cast<jlong>(link));
    }

    jlongArray result = env->NewLongArray(links.size());
    env->SetLongArrayRegion(result, 0, links.size(), &links[0]);
    return result;
}

JNI_FUNC(jobject, PdfiumCore, nativeGetDestPageIndex)(JNI_ARGS, jlong docPtr, jlong linkPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_LINK link = reinterpret_cast<FPDF_LINK>(linkPtr);
    FPDF_DEST dest = FPDFLink_GetDest(doc->pdfDocument, link);
    if (dest == NULL) {
        return NULL;
    }
    unsigned long index = FPDFDest_GetDestPageIndex(doc->pdfDocument, dest);
    return NewInteger(env, (jint) index);
}

JNI_FUNC(jstring, PdfiumCore, nativeGetLinkURI)(JNI_ARGS, jlong docPtr, jlong linkPtr) {
    DocumentFile *doc = reinterpret_cast<DocumentFile *>(docPtr);
    FPDF_LINK link = reinterpret_cast<FPDF_LINK>(linkPtr);
    FPDF_ACTION action = FPDFLink_GetAction(link);
    if (action == NULL) {
        return NULL;
    }
    size_t bufferLen = FPDFAction_GetURIPath(doc->pdfDocument, action, NULL, 0);
    if (bufferLen <= 0) {
        return env->NewStringUTF("");
    }
    std::string uri;
    FPDFAction_GetURIPath(doc->pdfDocument, action, WriteInto(&uri, bufferLen), bufferLen);
    return env->NewStringUTF(uri.c_str());
}
JNI_FUNC(void, PdfiumCore, nativeGetCharPos)(JNI_ARGS, jlong pagePtr, jint offsetY, jint offsetX,
                                             jint width, jint height, jobject pt, jlong textPtr,
                                             jint idx, jboolean loose) {
    //jclass point = env->FindClass("android/graphics/PointF");
    //jmethodID point_set = env->GetMethodID(point,"set","(FF)V");
    jclass rectF = env->FindClass("android/graphics/RectF");
    jmethodID rectF_ = env->GetMethodID(rectF, "<init>", "(FFFF)V");
    jmethodID rectF_set = env->GetMethodID(rectF, "set", "(FFFF)V");
    double left, top, right, bottom;
    if (loose) {
        FS_RECTF res = {0};
        if (!FPDFText_GetLooseCharBox((FPDF_TEXTPAGE) textPtr, idx, &res)) {
            return;
        }
        left = res.left;
        top = res.top;
        right = res.right;
        bottom = res.bottom;
    } else {
        if (!FPDFText_GetCharBox((FPDF_TEXTPAGE) textPtr, idx, &left, &right, &bottom, &top)) {
            return;
        }
    }
    int deviceX, deviceY;
    FPDF_PageToDevice((FPDF_PAGE) pagePtr, 0, 0, width, height, 0, left, top, &deviceX, &deviceY);
    width = right - left;
    height = top - bottom;
    left = deviceX + offsetX;
    top = deviceY + offsetY;
    right = left + width;
    bottom = top + height;
    //env->CallVoidMethod(pt, point_set, left, top);
    env->CallVoidMethod(pt, rectF_set, (float) left, (float) top, (float) right, (float) bottom);
}


static bool init_classes = true;
static jclass arrList;
static jmethodID arrList_add;
static jmethodID arrList_get;
static jmethodID arrList_size;
static jmethodID arrList_enssurecap;

static jclass integer;
static jmethodID integer_;

static jclass rectF;
static jmethodID rectF_;
static jmethodID rectF_set;
static jfieldID rectF_left;
static jfieldID rectF_top;
static jfieldID rectF_right;
static jfieldID rectF_bottom;
void initClasses(JNIEnv *env) {
    LOGE("fatal initClasses");
    jclass arrListTmp = env->FindClass("java/util/ArrayList");
    arrList = (jclass) env->NewGlobalRef(arrListTmp);
    env->DeleteLocalRef(arrListTmp);
    arrList_add = env->GetMethodID(arrList, "add", "(Ljava/lang/Object;)Z");
    arrList_get = env->GetMethodID(arrList, "get", "(I)Ljava/lang/Object;");
    arrList_size = env->GetMethodID(arrList, "size", "()I");
    arrList_enssurecap = env->GetMethodID(arrList, "ensureCapacity", "(I)V");

    jclass integerTmp = env->FindClass("java/lang/Integer");
    integer = (jclass) env->NewGlobalRef(integerTmp);
    env->DeleteLocalRef(integerTmp);
    integer_ = env->GetMethodID(integer, "<init>", "(I)V");

    jclass rectFTmp = env->FindClass("android/graphics/RectF");
    rectF = (jclass) env->NewGlobalRef(rectFTmp);
    env->DeleteLocalRef(rectFTmp);
    rectF_ = env->GetMethodID(rectF, "<init>", "(FFFF)V");
    rectF_set = env->GetMethodID(rectF, "set", "(FFFF)V");
    rectF_left = env->GetFieldID(rectF, "left", "F");
    rectF_top = env->GetFieldID(rectF, "top", "F");
    rectF_right = env->GetFieldID(rectF, "right", "F");
    rectF_bottom = env->GetFieldID(rectF, "bottom", "F");


    init_classes = false;
}
JNI_FUNC(jboolean, PdfiumCore, nativeGetMixedLooseCharPos)(JNI_ARGS, jlong pagePtr, jint offsetY,
                                                           jint offsetX, jint width, jint height,
                                                           jobject pt, jlong textPtr, jint idx,
                                                           jboolean loose) {
    jclass rectF = env->FindClass("android/graphics/RectF");
    jmethodID rectF_ = env->GetMethodID(rectF, "<init>", "(FFFF)V");
    jmethodID rectF_set = env->GetMethodID(rectF, "set", "(FFFF)V");
    double left, top, right, bottom;
    if (!FPDFText_GetCharBox((FPDF_TEXTPAGE) textPtr, idx, &left, &right, &bottom, &top)) {
        return false;
    }
    FS_RECTF res = {0};
    if (!FPDFText_GetLooseCharBox((FPDF_TEXTPAGE) textPtr, idx, &res)) {
        return false;
    }
    top = fmax(res.top, top);
    bottom = fmin(res.bottom, bottom);
    left = fmin(res.left, left);
    right = fmax(res.right, right);//width=1080,height=1527,left=365,top=621,right=686,bottom=440,deviceX=663,deviceY=400,ptr=543663849984
    int deviceX, deviceY;
    FPDF_PageToDevice((FPDF_PAGE) pagePtr, 0, 0, width, height, 0, left, top, &deviceX, &deviceY);

   /* width = right - left;
    height = top - bottom;*/
    height = top - bottom;
    top = deviceY + offsetY;
    left = deviceX + offsetX;

    FPDF_PageToDevice((FPDF_PAGE) pagePtr, 0, 0, width, height, 0, right, bottom, &deviceX, &deviceY);

    width = deviceX - left;


    right = left + width;
    bottom = top + height;
    env->CallVoidMethod(pt, rectF_set, (float) left, (float) top, (float) right, (float) bottom);
    return true;
}

JNI_FUNC(jint, PdfiumCore, nativeCountAndGetRects)(JNI_ARGS, jlong pagePtr, jint offsetY,
                                                   jint offsetX, jint width, jint height,
                                                   jobject arr, jlong textPtr, jint st, jint ed,
                                                   jboolean mergeAdjacent) {
    if (init_classes) initClasses(env);
    //jclass arrList = env->FindClass("java/util/ArrayList");
    //jmethodID arrList_add = env->GetMethodID(arrList,"add","(Ljava/lang/Object;)Z");
    //jmethodID arrList_get = env->GetMethodID(arrList,"get","(I)Ljava/lang/Object;");
    //jmethodID arrList_size = env->GetMethodID(arrList,"size","()I");
    //jmethodID arrList_enssurecap = env->GetMethodID(arrList,"ensureCapacity","(I)V");
//
    //jclass rectF = env->FindClass("android/graphics/RectF");
    //jmethodID rectF_ = env->GetMethodID(rectF, "<init>", "(FFFF)V");
    //jmethodID rectF_set = env->GetMethodID(rectF, "set", "(FFFF)V");

    int rectCount = FPDFText_CountRects((FPDF_TEXTPAGE) textPtr, (int) st, (int) ed);
    struct DeviceTextRect {
        float left;
        float top;
        float right;
        float bottom;
    };
    std::vector<DeviceTextRect> deviceRects;
    deviceRects.reserve(rectCount);
    double left, top, right, bottom;//width=1080,height=1527,left=365,top=621,right=686,bottom=440,deviceX=663,deviceY=400,ptr=543663849984
    int deviceX, deviceY;
    int deviceRight, deviceBottom;
    for (int i = 0; i < rectCount; i++) {//"RectF(373.0, 405.0, 556.0, 434.0)"
        if (FPDFText_GetRect((FPDF_TEXTPAGE) textPtr, i, &left, &top, &right, &bottom)) {
            FPDF_PageToDevice((FPDF_PAGE) pagePtr, 0, 0, (int)width, (int)height, 0, left, top, &deviceX,
                              &deviceY);

            FPDF_PageToDevice((FPDF_PAGE) pagePtr, 0, 0, (int)width, (int)height, 0, right, bottom, &deviceRight,
                              &deviceBottom);
            /*int new_width = right - left;
            int new_height = top - bottom;*/
            deviceRects.push_back({
                (float) (deviceX + offsetX),
                (float) (deviceY + offsetY),
                (float) (deviceRight + offsetX),
                (float) (deviceBottom + offsetY)
            });
        }
    }

    if (mergeAdjacent && deviceRects.size() > 1) {
        std::vector<DeviceTextRect> mergedRects;
        mergedRects.reserve(deviceRects.size());

        const auto canMergeOnSameLine = [](const DeviceTextRect &current,
                                           const DeviceTextRect &next) {
            const float currentHeight = current.bottom - current.top;
            const float nextHeight = next.bottom - next.top;
            const float minHeight = std::min(currentHeight, nextHeight);
            const float maxHeight = std::max(currentHeight, nextHeight);
            const float verticalOverlap =
                    std::min(current.bottom, next.bottom) - std::max(current.top, next.top);
            const float centerDistance =
                    std::fabs((current.top + current.bottom) * 0.5f -
                              (next.top + next.bottom) * 0.5f);
            const float bottomDistance = std::fabs(current.bottom - next.bottom);
            // Complex-script runs (for example Devanagari base glyphs and matras)
            // can have very different bounds while still sharing the same line.
            const bool sameLine =
                    verticalOverlap >= minHeight * 0.2f ||
                    bottomDistance <= std::max(2.0f, maxHeight * 0.5f) ||
                    centerDistance <= maxHeight * 0.75f;
            const float horizontalGap = std::max(
                    0.0f,
                    std::max(next.left - current.right, current.left - next.right)
            );
            const float maxGap = std::max(3.0f, maxHeight * 2.0f);
            return sameLine && horizontalGap <= maxGap;
        };

        const auto mergeRect = [](DeviceTextRect &current, const DeviceTextRect &next) {
            current.left = std::min(current.left, next.left);
            current.top = std::min(current.top, next.top);
            current.right = std::max(current.right, next.right);
            current.bottom = std::max(current.bottom, next.bottom);
        };

        for (DeviceTextRect next : deviceRects) {
            if (next.left > next.right) std::swap(next.left, next.right);
            if (next.top > next.bottom) std::swap(next.top, next.bottom);

            int matchingLine = -1;
            for (int i = (int)mergedRects.size() - 1; i >= 0; --i) {
                if (canMergeOnSameLine(mergedRects[i], next)) {
                    matchingLine = i;
                    break;
                }
            }

            if (matchingLine < 0) {
                mergedRects.push_back(next);
            } else {
                mergeRect(mergedRects[matchingLine], next);
            }
        }

        deviceRects.swap(mergedRects);
        for (DeviceTextRect &rect : deviceRects) {
            const float verticalPadding =
                    std::max(1.0f, (rect.bottom - rect.top) * 0.15f);
            rect.top -= verticalPadding;
            rect.bottom += verticalPadding;
        }
    }

    const int resultCount = (int) deviceRects.size();
    env->CallVoidMethod(arr, arrList_enssurecap, resultCount);
    const int arraySize = env->CallIntMethod(arr, arrList_size);
    for (int i = 0; i < resultCount; i++) {
        const DeviceTextRect &rect = deviceRects[i];
        if (i >= arraySize) {
            jobject newRect = env->NewObject(
                    rectF, rectF_, rect.left, rect.top, rect.right, rect.bottom);
            env->CallBooleanMethod(arr, arrList_add, newRect);
            env->DeleteLocalRef(newRect);
        } else {
            jobject existingRect = env->CallObjectMethod(arr, arrList_get, i);
            env->CallVoidMethod(
                    existingRect, rectF_set, rect.left, rect.top, rect.right, rect.bottom);
            env->DeleteLocalRef(existingRect);
        }
    }
    return resultCount;
}


JNI_FUNC(jobject, PdfiumCore, nativeGetLinkRect)(JNI_ARGS, jlong linkPtr) {
    FPDF_LINK link = reinterpret_cast<FPDF_LINK>(linkPtr);
    FS_RECTF fsRectF;
    FPDF_BOOL result = FPDFLink_GetAnnotRect(link, &fsRectF);

    if (!result) {
        return NULL;
    }

    jclass clazz = env->FindClass("android/graphics/RectF");
    jmethodID constructorID = env->GetMethodID(clazz, "<init>", "(FFFF)V");
    return env->NewObject(clazz, constructorID, fsRectF.left, fsRectF.top, fsRectF.right,
                          fsRectF.bottom);
}
JNI_FUNC(jint, PdfiumCore, nativeGetFindIdx)(JNI_ARGS, jlong searchPtr) {
    return FPDFText_GetSchResultIndex((FPDF_SCHHANDLE)searchPtr);
}
JNI_FUNC(jint, PdfiumCore, nativeCountRects)(JNI_ARGS, jlong textPtr, jint st, jint ed) {
    return FPDFText_CountRects((FPDF_TEXTPAGE)textPtr, st, ed);
}

JNI_FUNC(jboolean, PdfiumCore, nativeGetRect)(JNI_ARGS, jlong pagePtr, jint offsetY, jint offsetX, jint width, jint height, jlong textPtr, jobject rect, jint idx) {
    if(init_classes) initClasses(env);
    double left, top, right, bottom;


    bool ret = FPDFText_GetRect((FPDF_TEXTPAGE)textPtr, idx, &left, &top, &right, &bottom);
    if(ret) {
        int deviceX, deviceY;
        int deviceRight, deviceBottom;
        FPDF_PageToDevice((FPDF_PAGE)pagePtr, 0, 0, width, height, 0, left, top, &deviceX, &deviceY);
        FPDF_PageToDevice((FPDF_PAGE) pagePtr, 0, 0, (int)width, (int)height, 0, right, bottom, &deviceRight,
                          &deviceBottom);


       // int width = right-left;
       // int height = top-bottom;
        left=deviceX+offsetX;
        top=deviceY+offsetY;
        int  new_width =deviceRight - left;
        int   new_height =deviceBottom - top;

        right = left + new_width;
        bottom = top + new_height;
       /* right=left+width;
        bottom=top+height;*/
        env->CallVoidMethod(rect, rectF_set, (float)left, (float)top, (float)right, (float)bottom);
    }
    return ret;
}

JNI_FUNC(void, PdfiumCore, nativeFindTextPageEnd)(JNI_ARGS, jlong searchPtr) {
    FPDFText_FindClose((FPDF_SCHHANDLE)searchPtr);
}
JNI_FUNC(jint, PdfiumCore, nativeGetFindLength)(JNI_ARGS, jlong searchPtr) {
    return FPDFText_GetSchCount((FPDF_SCHHANDLE)searchPtr);
}

JNI_FUNC(jlong, PdfiumCore, nativeGetStringChars)(JNI_ARGS, jstring key) {
    //LOGE("fatal nativeGetStringChars %ld", (long)key);
    return (long)env->GetStringChars(key, 0);
}
JNI_FUNC(jint, PdfiumCore, nativeFindTextPage)(JNI_ARGS, jlong textPtr, jstring key, jint flag) {
    const unsigned short * keyStr = env->GetStringChars(key, 0);
    FPDF_TEXTPAGE text = (FPDF_TEXTPAGE)textPtr;
    int foundIdx=-1;
    if(text) {
        FPDF_SCHHANDLE findHandle = FPDFText_FindStart(text, keyStr, flag, 0);
        bool ret = FPDFText_FindNext(findHandle);
        if(ret) {
            foundIdx = FPDFText_GetSchResultIndex(findHandle);
        }
        FPDFText_FindClose(findHandle);
    }
    env->ReleaseStringChars(key, keyStr);
    return foundIdx;
}
JNI_FUNC(jboolean, PdfiumCore, nativeFindTextPageNext)(JNI_ARGS, jlong searchPtr) {
    return FPDFText_FindNext((FPDF_SCHHANDLE)searchPtr);
}
JNI_FUNC(jlong, PdfiumCore, nativeFindTextPageStart)(JNI_ARGS, jlong textPtr, jlong keyStr, jint flag, jint startIdx) {
    //const unsigned short * keyStr = env->GetStringChars(key, 0);
    FPDF_SCHHANDLE findHandle = FPDFText_FindStart((FPDF_TEXTPAGE)textPtr, (const jchar *)keyStr, flag, startIdx);
    return (jlong)findHandle;
}
JNI_FUNC(jobject, PdfiumCore, nativePageCoordsToDevice)(JNI_ARGS, jlong pagePtr, jint startX,
                                                        jint startY, jint sizeX,
                                                        jint sizeY, jint rotate, jdouble pageX,
                                                        jdouble pageY) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    int deviceX, deviceY;

    FPDF_PageToDevice(page, startX, startY, sizeX, sizeY, rotate, pageX, pageY, &deviceX, &deviceY);

    jclass clazz = env->FindClass("android/graphics/Point");
    jmethodID constructorID = env->GetMethodID(clazz, "<init>", "(II)V");
    return env->NewObject(clazz, constructorID, deviceX, deviceY);
}
static jlong loadTextPageInternal(JNIEnv *env, FPDF_PAGE page) {
    try {
        FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
        if (page == NULL) {
            throw "Loaded page is null";
        }
        return reinterpret_cast<jlong>(text);
    } catch (const char *msg) {
        LOGE("%s", msg);

        jniThrowException(env, "java/lang/IllegalStateException",
                          "cannot load text");

        return -1;
    }
}
JNI_FUNC(jlong, PdfiumCore, nativeLoadTextPage)(JNI_ARGS, jlong pagePtr) {
    return loadTextPageInternal(env, (FPDF_PAGE) pagePtr);
}

JNIEXPORT void JNICALL
Java_com_shockwave_pdfium_PdfiumCore_nativeCloseTextPage(JNIEnv *env, jobject thiz,
                                                         jlong text_ptr) {
    if (text_ptr != 0) {
        FPDFText_ClosePage(reinterpret_cast<FPDF_TEXTPAGE>(text_ptr));
    }
}

JNIEXPORT jint JNICALL
Java_com_shockwave_pdfium_PdfiumCore_nativeGetTextCount(
        JNIEnv* env,
        jobject thiz,
        jlong text_ptr) {

    if(text_ptr == 0) return 0;

    return FPDFText_CountChars(
            reinterpret_cast<FPDF_TEXTPAGE>(text_ptr)
    );
}

// save new
#include <android/log.h>
#include <stdio.h>
#include <string.h>
#include "fpdf_save.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"
#include "fpdf_transformpage.h"
#include "fpdfview.h"
#include <jni.h>
#include <vector>
#include "fpdf_text.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "PDF_SAVE"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct {
    FPDF_FILEWRITE base;
    FILE* file;
} PdfFileWriter;

// Writer callback
int WriteBlock(FPDF_FILEWRITE* pThis,
               const void* data,
               unsigned long size) {

    PdfFileWriter* writer = (PdfFileWriter*)pThis;
    if (!writer->file) return 0;

    return fwrite(data, 1, size, writer->file) == size;
}

// to convert the canvas coordinates top-left (pixels) into pdf coordinates bottom-left (points)
JNIEXPORT jfloatArray JNICALL
Java_com_shockwave_pdfium_PdfiumCore_nativeDeviceRectToPageRect(
        JNIEnv *env,
        jobject thiz,
        jlong pagePtr,
        jint viewWidth,
        jint viewHeight,
        jfloat left,
        jfloat top,
        jfloat right,
        jfloat bottom) {

    if (pagePtr == 0) return nullptr;

    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);

    double pageLeft, pageTop, pageRight, pageBottom;

    // Convert top-left
    FPDF_DeviceToPage(
            page,
            0,                 // start_x
            0,                 // start_y
            viewWidth,         // size_x
            viewHeight,        // size_y
            0,                 // rotation
            (int) left,
            (int) top,
            &pageLeft,
            &pageTop
    );

    // Convert bottom-right
    FPDF_DeviceToPage(
            page,
            0,
            0,
            viewWidth,
            viewHeight,
            0,
            (int) right,
            (int) bottom,
            &pageRight,
            &pageBottom
    );

    jfloatArray result = env->NewFloatArray(4);
    float values[4] = {
            (float) pageLeft,
            (float) pageTop,
            (float) pageRight,
            (float) pageBottom
    };

    env->SetFloatArrayRegion(result, 0, 4, values);
    return result;
}

// --- Helper Function Signatures --- Main Code for saving
static void processLink(JNIEnv* env, jobject obj, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID urlField);
static jstring GetBridgeDataPropertyJString(JNIEnv* env, jobject obj, jfieldID dataPropsField, jclass jsonClass, jmethodID jsonInit, const char* key);
static jstring BuildBridgeDataPropertiesJString(JNIEnv* env, jclass jsonClass, jmethodID jsonInit, jmethodID jsonPut, jmethodID jsonToString, jstring textProps, jstring fhProps, jstring imageProps, jstring shapeProps, jstring simplePdfStampProps = nullptr, jstring attachmentProps = nullptr);
static void processStickyNoteComment(JNIEnv* env, jobject obj, FPDF_ANNOTATION annot, jfieldID commentPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static void processTextStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static void processFreeText(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static bool processStickerStamp(JNIEnv* env, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jobject json, jstring jJsonStr, jmethodID optS, jmethodID optD, jmethodID optI, jmethodID optB, int r, int g, int b, int alpha, bool saveAsPageContent = false);
static bool processSvgPathStamp(JNIEnv* env, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jobject json, jstring jJsonStr, jmethodID optS, jmethodID optD, jmethodID optI, jmethodID optB, int r, int g, int b, int alpha, bool saveAsPageContent = false);
static bool processImageOrPresetStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID imagePropsField, jclass jsonClass, jmethodID jsonInit, bool saveAsPageContent = false);
static bool isSimplePdfStampBridgeAnnotation(JNIEnv* env, jobject obj, jfieldID dataPropsField, jclass jsonClass, jmethodID jsonInit);
static bool appendSimplePdfStampTextObject(JNIEnv* env, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jobject textJson, jclass jsonClass, int r, int g, int b, int alpha, bool saveAsPageContent);
static bool processSimplePdfStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, int typeInt, jfieldID dataPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit, bool saveAsPageContent = false);
static bool processPageLevelTextWatermark(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, jfieldID dataPropsField, jclass jsonClass, jmethodID jsonInit);
struct RawPdfWatermarkSpec;
static bool CollectPageLevelTextWatermarkPatternSpec(JNIEnv* env, jobject obj, jfieldID dataPropsField, jclass jsonClass, jmethodID jsonInit, RawPdfWatermarkSpec* outSpec);
static bool PatchRawPdfWatermarkPatterns(JNIEnv* env, const char* outputPath, const std::vector<RawPdfWatermarkSpec>& specs);
static bool AppendPdfEditContentMarker(const char* outputPath);
static bool PdfFileHasEditContentMarker(const char* inputPath);
static bool AppendPageLevelTextWatermarkGlyphPaths(FPDF_PAGE page, const char* fontPath, const jchar* textContent, jsize textLength, int textR, int textG, int textB, int textA, bool isBold, double scale, double letterSpacing, double textHeight, double cosA, double sinA, double skewX, float minX, float maxX, float minY, float maxY, float centerX, float centerY, float tileWidth, float tileHeight, int maxStampObjects);

static std::string ResolvePageLevelWatermarkFontPath(const char* requestedPath, bool isBold, bool isItalic) {
    if (requestedPath && strlen(requestedPath) > 0 && access(requestedPath, R_OK) == 0) {
        return std::string(requestedPath);
    }

    std::vector<const char*> candidates;
    if (isBold && isItalic) {
        candidates.push_back("/system/fonts/Roboto-BoldItalic.ttf");
    }
    if (isBold) {
        candidates.push_back("/system/fonts/Roboto-Bold.ttf");
    }
    if (isItalic) {
        candidates.push_back("/system/fonts/Roboto-Italic.ttf");
    }
    candidates.push_back("/system/fonts/Roboto-Regular.ttf");
    candidates.push_back("/system/fonts/NotoSans-Regular.ttf");
    candidates.push_back("/system/fonts/DroidSans.ttf");

    for (const char* candidate : candidates) {
        if (candidate && access(candidate, R_OK) == 0) {
            return std::string(candidate);
        }
    }
    return std::string();
}

static bool processPdfShape(JNIEnv* env, jobject obj, FPDF_ANNOTATION annot, FS_RECTF rect, int typeInt, jfieldID shapePropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static bool processPdfShapeContent(JNIEnv* env, jobject obj, FPDF_PAGE page, FS_RECTF rect, int typeInt, jfieldID shapePropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static void processFreeHand(JNIEnv* env, jobject obj, FPDF_PAGE page, jfieldID fhDrawingProperties, int r, int g, int b, jclass jsonClass, jmethodID jsonInit, bool saveAsPageContent = false);

// helper for sticky note (to tell the native the name of the icon we want to set on the icon on note instead of default)
static void SetAnnotWideStringValueFromJString(
        JNIEnv* env,
        FPDF_ANNOTATION annot,
        FPDF_BYTESTRING key,
        jstring value
) {
    if (!env || !annot || !value) return;
    const jchar* rawValue = env->GetStringChars(value, nullptr);
    if (!rawValue) return;
    FPDFAnnot_SetStringValue(annot, key, reinterpret_cast<FPDF_WIDESTRING>(rawValue));
    env->ReleaseStringChars(value, rawValue);
}

static void AppendEscapedJsonUtf16(std::u16string* target, const std::u16string& value) {
    if (!target) return;
    for (char16_t ch : value) {
        switch (ch) {
            case u'\\':
                target->append(u"\\\\");
                break;
            case u'"':
                target->append(u"\\\"");
                break;
            case u'\n':
                target->append(u"\\n");
                break;
            case u'\r':
                break;
            case u'\t':
                target->append(u"\\t");
                break;
            default:
                if (ch < 0x20) {
                    static const char16_t hexDigits[] = u"0123456789ABCDEF";
                    target->append(u"\\u00");
                    target->push_back(hexDigits[(ch >> 4) & 0xF]);
                    target->push_back(hexDigits[ch & 0xF]);
                } else {
                    target->push_back(ch);
                }
                break;
        }
    }
}

static std::u16string AsciiToUtf16(const char* value) {
    std::u16string result;
    if (!value) return result;
    while (*value != '\0') {
        result.push_back(static_cast<char16_t>(*value));
        value++;
    }
    return result;
}

static std::u16string AsciiToUtf16(const std::string& value) {
    std::u16string result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<char16_t>(ch));
    }
    return result;
}

static std::u16string JStringToUtf16(JNIEnv* env, jstring value) {
    std::u16string result;
    if (!env || !value) return result;

    const jchar* rawValue = env->GetStringChars(value, nullptr);
    if (!rawValue) return result;

    const jsize valueLength = env->GetStringLength(value);
    result.assign(
            reinterpret_cast<const char16_t*>(rawValue),
            reinterpret_cast<const char16_t*>(rawValue) + valueLength
    );
    env->ReleaseStringChars(value, rawValue);
    return result;
}

static const char* MapStickyNoteIconKeyToPdfCommentName(const std::string& iconKey) {
    if (iconKey == "check") return "Check";
    if (iconKey == "circle") return "Circle";
    if (iconKey == "comment") return "Comment";
    if (iconKey == "cross") return "Cross";
    if (iconKey == "help") return "Help";
    if (iconKey == "flag") return "Key";
    if (iconKey == "arrow_right") return "RightArrow";
    if (iconKey == "right_pointer") return "RightPointer";
    if (iconKey == "star") return "Star";
    if (iconKey == "insert") return "Insert";
    return "Note";
}

static const char* MapPdfCommentNameToStickyNoteIconKey(const std::u16string& pdfName) {
    if (pdfName == u"Check") return "check";
    if (pdfName == u"Circle") return "circle";
    if (pdfName == u"Comment") return "comment";
    if (pdfName == u"Cross") return "cross";
    if (pdfName == u"Help") return "help";
    if (pdfName == u"Key") return "flag";
    if (pdfName == u"Insert") return "arrow_right";
    if (pdfName == u"RightArrow") return "arrow_right";
    if (pdfName == u"RightPointer") return "right_pointer";
    if (pdfName == u"Star") return "star";
    if (pdfName == u"Note" || pdfName.empty()) return "comment";
    return "comment";
}

static void clearTextEditFontCache(FPDF_DOCUMENT document) {
    if (!document) return;
    Mutex::Autolock lock(sTextEditFontCacheLock);
    sTextEditFontCache.erase(document);
}

static void AppendPdfCirclePath(std::ostringstream& stream, float cx, float cy, float radius) {
    const float kappa = 0.5522847498f;
    const float control = radius * kappa;
    stream << (cx + radius) << ' ' << cy << " m ";
    stream << (cx + radius) << ' ' << (cy + control) << ' '
           << (cx + control) << ' ' << (cy + radius) << ' '
           << cx << ' ' << (cy + radius) << " c ";
    stream << (cx - control) << ' ' << (cy + radius) << ' '
           << (cx - radius) << ' ' << (cy + control) << ' '
           << (cx - radius) << ' ' << cy << " c ";
    stream << (cx - radius) << ' ' << (cy - control) << ' '
           << (cx - control) << ' ' << (cy - radius) << ' '
           << cx << ' ' << (cy - radius) << " c ";
    stream << (cx + control) << ' ' << (cy - radius) << ' '
           << (cx + radius) << ' ' << (cy - control) << ' '
           << (cx + radius) << ' ' << cy << " c ";
}

static void AppendPdfRoundedRectPath(
        std::ostringstream& stream,
        float left,
        float bottom,
        float right,
        float top,
        float radius
) {
    const float clampedRadius = std::max(0.0f, std::min(radius, std::min((right - left) * 0.5f, (top - bottom) * 0.5f)));
    const float kappa = 0.5522847498f;
    const float control = clampedRadius * kappa;
    stream << (left + clampedRadius) << ' ' << top << " m ";
    stream << (right - clampedRadius) << ' ' << top << " l ";
    stream << (right - clampedRadius + control) << ' ' << top << ' '
           << right << ' ' << (top - clampedRadius + control) << ' '
           << right << ' ' << (top - clampedRadius) << " c ";
    stream << right << ' ' << (bottom + clampedRadius) << " l ";
    stream << right << ' ' << (bottom + clampedRadius - control) << ' '
           << (right - clampedRadius + control) << ' ' << bottom << ' '
           << (right - clampedRadius) << ' ' << bottom << " c ";
    stream << (left + clampedRadius) << ' ' << bottom << " l ";
    stream << (left + clampedRadius - control) << ' ' << bottom << ' '
           << left << ' ' << (bottom + clampedRadius - control) << ' '
           << left << ' ' << (bottom + clampedRadius) << " c ";
    stream << left << ' ' << (top - clampedRadius) << " l ";
    stream << left << ' ' << (top - clampedRadius + control) << ' '
           << (left + clampedRadius - control) << ' ' << top << ' '
           << (left + clampedRadius) << ' ' << top << " c ";
}

static void AppendPdfEllipsePath(
        std::ostringstream& stream,
        float cx,
        float cy,
        float rx,
        float ry
) {
    const float kappa = 0.5522847498f;
    const float controlX = rx * kappa;
    const float controlY = ry * kappa;
    stream << (cx + rx) << ' ' << cy << " m ";
    stream << (cx + rx) << ' ' << (cy + controlY) << ' '
           << (cx + controlX) << ' ' << (cy + ry) << ' '
           << cx << ' ' << (cy + ry) << " c ";
    stream << (cx - controlX) << ' ' << (cy + ry) << ' '
           << (cx - rx) << ' ' << (cy + controlY) << ' '
           << (cx - rx) << ' ' << cy << " c ";
    stream << (cx - rx) << ' ' << (cy - controlY) << ' '
           << (cx - controlX) << ' ' << (cy - ry) << ' '
           << cx << ' ' << (cy - ry) << " c ";
    stream << (cx + controlX) << ' ' << (cy - ry) << ' '
           << (cx + rx) << ' ' << (cy - controlY) << ' '
           << (cx + rx) << ' ' << cy << " c ";
}

static bool IsPdfShapeNativeType(int typeInt) {
    return typeInt >= 12 && typeInt <= 18;
}

static int GetPdfShapeAnnotationSubtype(int typeInt) {
    switch (typeInt) {
        case 14:
            return FPDF_ANNOT_CIRCLE;
        case 15:
            return FPDF_ANNOT_POLYGON;
        case 16:
            return FPDF_ANNOT_POLYLINE;
        case 17:
        case 18:
            return FPDF_ANNOT_LINE;
        case 12:
        case 13:
        default:
            return FPDF_ANNOT_SQUARE;
    }
}

static bool NeedsSavedPdfShapeDictionaryPatch(int typeInt) {
    return typeInt == 15 || typeInt == 16 || typeInt == 17 || typeInt == 18;
}

static bool IsDirectNativePdfBoxShape(int typeInt) {
    return typeInt == 12 || typeInt == 13 || typeInt == 14;
}

static int GetPdfShapeCreationSubtype(int typeInt) {
    // This PDFium build exposes line/polygon/polyline constants and readers,
    // but its public CreateAnnot API only creates a smaller subtype set. Create
    // these as a supported temporary annotation and patch the saved dictionary
    // to the real native subtype after PDFium writes the file.
    if (typeInt == 17 || typeInt == 18) {
        return FPDF_ANNOT_SQUARE;
    }
    return NeedsSavedPdfShapeDictionaryPatch(typeInt)
           ? FPDF_ANNOT_HIGHLIGHT
           : GetPdfShapeAnnotationSubtype(typeInt);
}

static FPDF_ANNOTATION CreatePdfShapeAnnotation(FPDF_PAGE page, int typeInt) {
    if (!page || !IsPdfShapeNativeType(typeInt)) return nullptr;

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, GetPdfShapeAnnotationSubtype(typeInt));
    if (annot) return annot;

    if (NeedsSavedPdfShapeDictionaryPatch(typeInt)) {
        return FPDFPage_CreateAnnot(page, GetPdfShapeCreationSubtype(typeInt));
    }
    return nullptr;
}

static const char* GetPdfShapePatchMarkerKey(int typeInt) {
    switch (typeInt) {
        case 15: return "LufickPdfShapePatchPolygon";
        case 16: return "LufickPdfShapePatchPolyLine";
        case 17: return "LufickPdfShapePatchLine";
        case 18: return "LufickPdfShapePatchArrowLine";
        default: return "LufickPdfShapePatchUnknown";
    }
}

static const char* GetPdfBoxShapePatchMarkerKey(int typeInt) {
    switch (typeInt) {
        case 12: return "LufickPdfBoxShapePatchRectangle";
        case 13: return "LufickPdfBoxShapePatchSquare";
        case 14: return "LufickPdfBoxShapePatchCircle";
        default: return "LufickPdfBoxShapePatchUnknown";
    }
}

static const char* GetPdfShapeName(int typeInt) {
    switch (typeInt) {
        case 12: return "rectangle";
        case 13: return "square";
        case 14: return "circle";
        case 15: return "polygon";
        case 16: return "polyline";
        case 17: return "line";
        case 18: return "arrow_line";
        default: return "rectangle";
    }
}

static int GetPdfShapeTypeFromMeta(const std::string& meta, int fallbackType) {
    if (meta.find("\"shapeType\":\"arrow_line\"") != std::string::npos ||
        meta.find("\"shapeType\": \"arrow_line\"") != std::string::npos) {
        return 18;
    }
    if (meta.find("\"shapeType\":\"line\"") != std::string::npos ||
        meta.find("\"shapeType\": \"line\"") != std::string::npos) {
        return 17;
    }
    if (meta.find("\"shapeType\":\"polyline\"") != std::string::npos ||
        meta.find("\"shapeType\": \"polyline\"") != std::string::npos) {
        return 16;
    }
    if (meta.find("\"shapeType\":\"polygon\"") != std::string::npos ||
        meta.find("\"shapeType\": \"polygon\"") != std::string::npos) {
        return 15;
    }
    if (meta.find("\"shapeType\":\"circle\"") != std::string::npos ||
        meta.find("\"shapeType\": \"circle\"") != std::string::npos) {
        return 14;
    }
    if (meta.find("\"shapeType\":\"square\"") != std::string::npos ||
        meta.find("\"shapeType\": \"square\"") != std::string::npos) {
        return 13;
    }
    if (meta.find("\"shapeType\":\"rectangle\"") != std::string::npos ||
        meta.find("\"shapeType\": \"rectangle\"") != std::string::npos) {
        return 12;
    }
    return fallbackType;
}

struct PdfShapePoint {
    float x;
    float y;
};

static PdfShapePoint RotatePdfShapePoint(PdfShapePoint point, float centerX, float centerY, float rotationDegrees) {
    if (fabs(rotationDegrees) < 0.001f) return point;
    const double radians = rotationDegrees * M_PI / 180.0;
    const double cosA = cos(radians);
    const double sinA = sin(radians);
    const float dx = point.x - centerX;
    const float dy = point.y - centerY;
    return {
            centerX + static_cast<float>((dx * cosA) - (dy * sinA)),
            centerY + static_cast<float>((dx * sinA) + (dy * cosA))
    };
}

static PdfShapePoint PdfShapePointFromFraction(
        const FS_RECTF& baseRect,
        float fractionX,
        float fractionFromTop,
        float rotationDegrees
) {
    const float left = fmin(baseRect.left, baseRect.right);
    const float right = fmax(baseRect.left, baseRect.right);
    const float bottom = fmin(baseRect.bottom, baseRect.top);
    const float top = fmax(baseRect.bottom, baseRect.top);
    const float width = right - left;
    const float height = top - bottom;
    const float centerX = (left + right) * 0.5f;
    const float centerY = (bottom + top) * 0.5f;
    PdfShapePoint point{
            left + (width * fractionX),
            top - (height * fractionFromTop)
    };
    return RotatePdfShapePoint(point, centerX, centerY, rotationDegrees);
}

static std::vector<PdfShapePoint> ReadPdfShapePointsFromJson(
        JNIEnv* env,
        jobject json,
        jclass jsonClass
) {
    std::vector<PdfShapePoint> points;
    if (!env || !json || !jsonClass) return points;

    jmethodID optArrayMethod = env->GetMethodID(jsonClass, "optJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");
    if (!optArrayMethod) return points;

    jstring pointsKey = env->NewStringUTF("points");
    jobject pointsArray = env->CallObjectMethod(json, optArrayMethod, pointsKey);
    env->DeleteLocalRef(pointsKey);
    if (!pointsArray) return points;

    jclass arrayClass = env->FindClass("org/json/JSONArray");
    jmethodID lengthMethod = arrayClass ? env->GetMethodID(arrayClass, "length", "()I") : nullptr;
    jmethodID optObjectMethod = arrayClass ? env->GetMethodID(arrayClass, "optJSONObject", "(I)Lorg/json/JSONObject;") : nullptr;
    jmethodID optDoubleMethod = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    if (!lengthMethod || !optObjectMethod || !optDoubleMethod) {
        env->DeleteLocalRef(pointsArray);
        if (arrayClass) env->DeleteLocalRef(arrayClass);
        return points;
    }

    jstring xKey = env->NewStringUTF("x");
    jstring yKey = env->NewStringUTF("y");
    const jint count = env->CallIntMethod(pointsArray, lengthMethod);
    const jint safeCount = std::min(count, static_cast<jint>(64));
    for (jint index = 0; index < safeCount; index++) {
        jobject pointObject = env->CallObjectMethod(pointsArray, optObjectMethod, index);
        if (!pointObject) continue;
        const double x = env->CallDoubleMethod(pointObject, optDoubleMethod, xKey, 0.0);
        const double y = env->CallDoubleMethod(pointObject, optDoubleMethod, yKey, 0.0);
        points.push_back({static_cast<float>(x), static_cast<float>(y)});
        env->DeleteLocalRef(pointObject);
    }
    env->DeleteLocalRef(xKey);
    env->DeleteLocalRef(yKey);
    env->DeleteLocalRef(pointsArray);
    if (arrayClass) env->DeleteLocalRef(arrayClass);
    return points;
}

static PdfShapePoint OffsetPdfShapePointFromLineEnd(
        PdfShapePoint start,
        PdfShapePoint end,
        float backDistance,
        float perpendicularDistance
) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = fmax(hypotf(dx, dy), 0.001f);
    const float ux = dx / length;
    const float uy = dy / length;
    return {
            end.x - (ux * backDistance) - (uy * perpendicularDistance),
            end.y - (uy * backDistance) + (ux * perpendicularDistance)
    };
}

static float ResolvePdfArrowHeadLength(float strokeWidth, float lineLength) {
    return fmax(10.0f, fmin(fmax(lineLength * 0.12f, strokeWidth * 5.0f), 18.0f));
}

static const char* PdfShapeLineEndName(bool enabled) {
    return enabled ? "/OpenArrow" : "/None";
}

static void AppendPdfShapePaintOperator(
        std::ostringstream& stream,
        bool allowFill,
        int fillAlpha,
        int strokeAlpha,
        float strokeWidth
) {
    const bool shouldFill = allowFill && fillAlpha > 0;
    const bool shouldStroke = strokeAlpha > 0 && strokeWidth > 0.0f;
    if (shouldFill && shouldStroke) {
        stream << "B ";
    } else if (shouldFill) {
        stream << "f ";
    } else if (shouldStroke) {
        stream << "S ";
    }
}

static bool ShouldUseCustomPdfShapeAppearanceStream(
        int typeInt,
        int strokeAlpha,
        int fillAlpha,
        float rotationDegrees
) {
    if (NeedsSavedPdfShapeDictionaryPatch(typeInt)) return true;
    if (fabs(rotationDegrees) >= 0.001f) return true;
    return strokeAlpha >= 255 && (fillAlpha == 0 || fillAlpha >= 255);
}

static bool AppendPdfShapeAppearanceObject(
        FPDF_ANNOTATION annot,
        int typeInt,
        const FS_RECTF& baseRect,
        float rotationDegrees,
        int strokeR,
        int strokeG,
        int strokeB,
        int strokeA,
        int fillR,
        int fillG,
        int fillB,
        int fillA,
        float strokeWidth,
        float cornerRadius = 0.0f,
        float dashWidth = 0.0f,
        float dashGap = 0.0f,
        float startAngle = 0.0f,
        float sweepAngle = 360.0f,
        bool isSpike = false,
        int spikeCount = 12,
        FPDF_PAGE contentPage = nullptr
) {
    if (!annot && !contentPage) return false;

    const float effectiveStrokeWidth = fmax(strokeWidth, 0.0f);
    const bool shouldStroke = strokeA > 0 && effectiveStrokeWidth > 0.0f;
    const bool shouldFill = fillA > 0 && typeInt != 16 && typeInt != 17 && typeInt != 18;
    if (!shouldStroke && !shouldFill) return false;

    auto appendStyledPath = [&](FPDF_PAGEOBJECT styledPath, bool fill, bool stroke, int fillMode = 1) {
        if (!styledPath) return false;
        FPDFPageObj_SetStrokeWidth(styledPath, effectiveStrokeWidth);
        FPDFPageObj_SetLineJoin(styledPath, FPDF_LINEJOIN_ROUND);
        FPDFPageObj_SetLineCap(styledPath, FPDF_LINECAP_ROUND);
        FPDFPageObj_SetStrokeColor(styledPath, strokeR, strokeG, strokeB, strokeA);
        FPDFPageObj_SetFillColor(styledPath, fillR, fillG, fillB, fillA);
        if (stroke && dashWidth > 0.0f && dashGap > 0.0f) {
            const float dashArray[] = {dashWidth, dashGap};
            FPDFPageObj_SetDashArray(styledPath, dashArray, 2, 0.0f);
        }
        FPDFPath_SetDrawMode(styledPath, fill ? fillMode : 0, stroke ? 1 : 0);
        if (contentPage) {
            FPDFPage_InsertObject(contentPage, styledPath);
        } else {
            if (!FPDFAnnot_AppendObject(annot, styledPath)) {
                FPDFPageObj_Destroy(styledPath);
                return false;
            }
            FPDFAnnot_UpdateObject(annot, styledPath);
        }
        return true;
    };

    const bool isPartialArc = typeInt == 14 && fabs(sweepAngle) < 359.999f;
    if (isPartialArc) {
        const float clampedSweep = fmax(-360.0f, fmin(sweepAngle, 360.0f));
        if (fabs(clampedSweep) <= 0.001f) return false;
        const int segmentCount = std::max(4, static_cast<int>(ceil(fabs(clampedSweep) / 4.0f)));
        auto arcPoint = [&](int index) {
            const double angleDegrees = startAngle + (clampedSweep * index / segmentCount);
            const double angle = angleDegrees * M_PI / 180.0;
            return PdfShapePointFromFraction(
                    baseRect,
                    0.5f + static_cast<float>(cos(angle) * 0.5),
                    0.5f + static_cast<float>(sin(angle) * 0.5),
                    rotationDegrees);
        };

        bool appended = false;
        if (shouldFill) {
            const PdfShapePoint center = PdfShapePointFromFraction(baseRect, 0.5f, 0.5f, rotationDegrees);
            FPDF_PAGEOBJECT fillPath = FPDFPageObj_CreateNewPath(center.x, center.y);
            if (fillPath) {
                const PdfShapePoint first = arcPoint(0);
                FPDFPath_LineTo(fillPath, first.x, first.y);
                for (int index = 1; index <= segmentCount; index++) {
                    const PdfShapePoint point = arcPoint(index);
                    FPDFPath_LineTo(fillPath, point.x, point.y);
                }
                FPDFPath_Close(fillPath);
                appended = appendStyledPath(fillPath, true, false) || appended;
            }
        }
        if (shouldStroke) {
            const PdfShapePoint first = arcPoint(0);
            FPDF_PAGEOBJECT strokePath = FPDFPageObj_CreateNewPath(first.x, first.y);
            if (strokePath) {
                for (int index = 1; index <= segmentCount; index++) {
                    const PdfShapePoint point = arcPoint(index);
                    FPDFPath_LineTo(strokePath, point.x, point.y);
                }
                appended = appendStyledPath(strokePath, false, true) || appended;
            }
        }
        return appended;
    }

    FPDF_PAGEOBJECT path = nullptr;
    auto appendPoint = [&](PdfShapePoint point, bool first) {
        if (first) {
            FPDFPath_MoveTo(path, point.x, point.y);
        } else {
            FPDFPath_LineTo(path, point.x, point.y);
        }
    };

    if (isSpike) {
        const float left = fmin(baseRect.left, baseRect.right);
        const float right = fmax(baseRect.left, baseRect.right);
        const float bottom = fmin(baseRect.bottom, baseRect.top);
        const float top = fmax(baseRect.bottom, baseRect.top);
        const float centerX = (left + right) * 0.5f;
        const float centerY = (bottom + top) * 0.5f;
        const float outerRadius = fmax(0.1f, fmin(right - left, top - bottom) * 0.5f - effectiveStrokeWidth * 0.5f);
        const float innerRadius = outerRadius * 0.85f;
        const int resolvedSpikeCount = std::max(2, std::min(spikeCount, 180));
        auto rotatedRadialPoint = [&](float radius, double angle) {
            return RotatePdfShapePoint(
                    PdfShapePoint{
                            centerX + static_cast<float>(cos(angle) * radius),
                            centerY - static_cast<float>(sin(angle) * radius)},
                    centerX,
                    centerY,
                    rotationDegrees);
        };
        for (int index = 0; index < resolvedSpikeCount * 2; index++) {
            const double angle = M_PI * index / resolvedSpikeCount;
            const PdfShapePoint point = rotatedRadialPoint(
                    index % 2 == 0 ? outerRadius : innerRadius,
                    angle);
            if (index == 0) {
                path = FPDFPageObj_CreateNewPath(point.x, point.y);
            } else {
                FPDFPath_LineTo(path, point.x, point.y);
            }
        }
        if (!path) return false;
        FPDFPath_Close(path);
        const int innerSegmentCount = 48;
        for (int index = 0; index < innerSegmentCount; index++) {
            const double angle = 2.0 * M_PI * index / innerSegmentCount;
            const PdfShapePoint point = rotatedRadialPoint(innerRadius, angle);
            if (index == 0) {
                FPDFPath_MoveTo(path, point.x, point.y);
            } else {
                FPDFPath_LineTo(path, point.x, point.y);
            }
        }
        FPDFPath_Close(path);
    } else if (typeInt == 14) {
        const int segmentCount = 32;
        const PdfShapePoint start = PdfShapePointFromFraction(baseRect, 1.0f, 0.5f, rotationDegrees);
        path = FPDFPageObj_CreateNewPath(start.x, start.y);
        if (!path) return false;
        for (int index = 1; index < segmentCount; index++) {
            const double angle = (2.0 * M_PI * index) / segmentCount;
            const float fx = 0.5f + static_cast<float>(cos(angle) * 0.5);
            const float fy = 0.5f - static_cast<float>(sin(angle) * 0.5);
            appendPoint(PdfShapePointFromFraction(baseRect, fx, fy, rotationDegrees), false);
        }
        FPDFPath_Close(path);
    } else if (typeInt == 12 && cornerRadius > 0.0f) {
        const float left = fmin(baseRect.left, baseRect.right);
        const float right = fmax(baseRect.left, baseRect.right);
        const float bottom = fmin(baseRect.bottom, baseRect.top);
        const float top = fmax(baseRect.bottom, baseRect.top);
        const float radius = fmax(0.0f, fmin(cornerRadius, fmin((right - left) * 0.5f, (top - bottom) * 0.5f)));
        const float control = radius * 0.5522847498f;
        const float centerX = (left + right) * 0.5f;
        const float centerY = (bottom + top) * 0.5f;
        auto rotatePoint = [&](float x, float y) -> PdfShapePoint {
            return RotatePdfShapePoint(PdfShapePoint{x, y}, centerX, centerY, rotationDegrees);
        };
        const PdfShapePoint start = rotatePoint(left + radius, top);
        path = FPDFPageObj_CreateNewPath(start.x, start.y);
        if (!path) return false;
        auto lineTo = [&](float x, float y) {
            const PdfShapePoint p = rotatePoint(x, y);
            FPDFPath_LineTo(path, p.x, p.y);
        };
        auto bezierTo = [&](float c1x, float c1y, float c2x, float c2y, float ex, float ey) {
            const PdfShapePoint c1 = rotatePoint(c1x, c1y);
            const PdfShapePoint c2 = rotatePoint(c2x, c2y);
            const PdfShapePoint end = rotatePoint(ex, ey);
            FPDFPath_BezierTo(path, c1.x, c1.y, c2.x, c2.y, end.x, end.y);
        };
        lineTo(right - radius, top);
        bezierTo(right - radius + control, top, right, top - radius + control, right, top - radius);
        lineTo(right, bottom + radius);
        bezierTo(right, bottom + radius - control, right - radius + control, bottom, right - radius, bottom);
        lineTo(left + radius, bottom);
        bezierTo(left + radius - control, bottom, left, bottom + radius - control, left, bottom + radius);
        lineTo(left, top - radius);
        bezierTo(left, top - radius + control, left + radius - control, top, left + radius, top);
        FPDFPath_Close(path);
    } else {
        const PdfShapePoint topLeft = PdfShapePointFromFraction(baseRect, 0.0f, 0.0f, rotationDegrees);
        path = FPDFPageObj_CreateNewPath(topLeft.x, topLeft.y);
        if (!path) return false;
        appendPoint(PdfShapePointFromFraction(baseRect, 1.0f, 0.0f, rotationDegrees), false);
        appendPoint(PdfShapePointFromFraction(baseRect, 1.0f, 1.0f, rotationDegrees), false);
        appendPoint(PdfShapePointFromFraction(baseRect, 0.0f, 1.0f, rotationDegrees), false);
        FPDFPath_Close(path);
    }

    return appendStyledPath(path, shouldFill, shouldStroke, FPDF_FILLMODE_ALTERNATE);
}

static bool InsertPdfShapeContentPath(
        FPDF_PAGE page,
        FPDF_PAGEOBJECT path,
        bool allowFill,
        int strokeR,
        int strokeG,
        int strokeB,
        int strokeA,
        int fillR,
        int fillG,
        int fillB,
        int fillA,
        float strokeWidth
) {
    if (!page || !path) {
        if (path) FPDFPageObj_Destroy(path);
        return false;
    }

    const float effectiveStrokeWidth = fmax(strokeWidth, 0.0f);
    const bool shouldStroke = strokeA > 0 && effectiveStrokeWidth > 0.0f;
    const bool shouldFill = allowFill && fillA > 0;
    if (!shouldStroke && !shouldFill) {
        FPDFPageObj_Destroy(path);
        return false;
    }

    FPDFPageObj_SetStrokeWidth(path, effectiveStrokeWidth);
    FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND);
    FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND);
    FPDFPageObj_SetStrokeColor(path, strokeR, strokeG, strokeB, strokeA);
    FPDFPageObj_SetFillColor(path, fillR, fillG, fillB, fillA);
    FPDFPath_SetDrawMode(path, shouldFill ? 1 : 0, shouldStroke ? 1 : 0);
    FPDFPage_InsertObject(page, path);
    return true;
}

static FPDF_PAGEOBJECT CreatePdfShapeContentPath(
        int typeInt,
        const FS_RECTF& baseRect,
        float rotationDegrees,
        float cornerRadius,
        const std::vector<PdfShapePoint>& customPoints,
        std::vector<PdfShapePoint>* linePointsOut
) {
    const float left = fmin(baseRect.left, baseRect.right);
    const float right = fmax(baseRect.left, baseRect.right);
    const float bottom = fmin(baseRect.bottom, baseRect.top);
    const float top = fmax(baseRect.bottom, baseRect.top);
    const float width = right - left;
    const float height = top - bottom;
    if (width <= 0.1f || height <= 0.1f) return nullptr;

    FPDF_PAGEOBJECT path = nullptr;
    auto appendPoint = [&](PdfShapePoint point, bool first) {
        if (first) {
            path = FPDFPageObj_CreateNewPath(point.x, point.y);
        } else if (path) {
            FPDFPath_LineTo(path, point.x, point.y);
        }
    };

    const bool hasCustomVertexPoints =
            customPoints.size() >= 2 && (typeInt == 15 || typeInt == 16);
    const bool hasCustomLinePoints =
            customPoints.size() >= 2 && (typeInt == 17 || typeInt == 18);

    if (hasCustomVertexPoints) {
        for (size_t index = 0; index < customPoints.size(); index++) {
            appendPoint(customPoints[index], index == 0);
        }
        if (typeInt == 15 && path) FPDFPath_Close(path);
        if (linePointsOut) *linePointsOut = customPoints;
        return path;
    }

    if (typeInt == 17 || typeInt == 18) {
        const PdfShapePoint start = hasCustomLinePoints
                ? customPoints[0]
                : PdfShapePointFromFraction(baseRect, 0.08f, 0.50f, rotationDegrees);
        const PdfShapePoint end = hasCustomLinePoints
                ? customPoints[1]
                : PdfShapePointFromFraction(baseRect, 0.92f, 0.50f, rotationDegrees);
        appendPoint(start, true);
        appendPoint(end, false);
        if (linePointsOut) {
            linePointsOut->clear();
            linePointsOut->push_back(start);
            linePointsOut->push_back(end);
        }
        return path;
    }

    if (typeInt == 16) {
        std::vector<PdfShapePoint> points{
                PdfShapePointFromFraction(baseRect, 0.05f, 0.80f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 0.35f, 0.20f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 0.65f, 0.65f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 0.95f, 0.10f, rotationDegrees)
        };
        for (size_t index = 0; index < points.size(); index++) {
            appendPoint(points[index], index == 0);
        }
        if (linePointsOut) *linePointsOut = points;
        return path;
    }

    if (typeInt == 15) {
        std::vector<PdfShapePoint> points{
                PdfShapePointFromFraction(baseRect, 0.50f, 0.00f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 1.00f, 0.38f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 0.82f, 1.00f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 0.18f, 1.00f, rotationDegrees),
                PdfShapePointFromFraction(baseRect, 0.00f, 0.38f, rotationDegrees)
        };
        for (size_t index = 0; index < points.size(); index++) {
            appendPoint(points[index], index == 0);
        }
        if (path) FPDFPath_Close(path);
        return path;
    }

    if (typeInt == 14) {
        const int segmentCount = 32;
        for (int index = 0; index < segmentCount; index++) {
            const double angle = (2.0 * M_PI * index) / segmentCount;
            const float fx = 0.5f + static_cast<float>(cos(angle) * 0.5);
            const float fy = 0.5f - static_cast<float>(sin(angle) * 0.5);
            appendPoint(PdfShapePointFromFraction(baseRect, fx, fy, rotationDegrees), index == 0);
        }
        if (path) FPDFPath_Close(path);
        return path;
    }

    if (typeInt == 12 && cornerRadius > 0.0f) {
        const float radius = fmax(0.0f, fmin(cornerRadius, fmin(width * 0.5f, height * 0.5f)));
        const float control = radius * 0.5522847498f;
        const float centerX = (left + right) * 0.5f;
        const float centerY = (bottom + top) * 0.5f;
        auto rotatePoint = [&](float x, float y) -> PdfShapePoint {
            return RotatePdfShapePoint(PdfShapePoint{x, y}, centerX, centerY, rotationDegrees);
        };
        const PdfShapePoint start = rotatePoint(left + radius, top);
        path = FPDFPageObj_CreateNewPath(start.x, start.y);
        if (!path) return nullptr;
        auto lineTo = [&](float x, float y) {
            const PdfShapePoint p = rotatePoint(x, y);
            FPDFPath_LineTo(path, p.x, p.y);
        };
        auto bezierTo = [&](float c1x, float c1y, float c2x, float c2y, float ex, float ey) {
            const PdfShapePoint c1 = rotatePoint(c1x, c1y);
            const PdfShapePoint c2 = rotatePoint(c2x, c2y);
            const PdfShapePoint end = rotatePoint(ex, ey);
            FPDFPath_BezierTo(path, c1.x, c1.y, c2.x, c2.y, end.x, end.y);
        };
        lineTo(right - radius, top);
        bezierTo(right - radius + control, top, right, top - radius + control, right, top - radius);
        lineTo(right, bottom + radius);
        bezierTo(right, bottom + radius - control, right - radius + control, bottom, right - radius, bottom);
        lineTo(left + radius, bottom);
        bezierTo(left + radius - control, bottom, left, bottom + radius - control, left, bottom + radius);
        lineTo(left, top - radius);
        bezierTo(left, top - radius + control, left + radius - control, top, left + radius, top);
        FPDFPath_Close(path);
        return path;
    }

    appendPoint(PdfShapePointFromFraction(baseRect, 0.00f, 0.00f, rotationDegrees), true);
    appendPoint(PdfShapePointFromFraction(baseRect, 1.00f, 0.00f, rotationDegrees), false);
    appendPoint(PdfShapePointFromFraction(baseRect, 1.00f, 1.00f, rotationDegrees), false);
    appendPoint(PdfShapePointFromFraction(baseRect, 0.00f, 1.00f, rotationDegrees), false);
    if (path) FPDFPath_Close(path);
    return path;
}

static bool InsertPdfShapeArrowHeadContent(
        FPDF_PAGE page,
        PdfShapePoint from,
        PdfShapePoint to,
        int strokeR,
        int strokeG,
        int strokeB,
        int strokeA,
        float strokeWidth
) {
    const float effectiveStrokeWidth = fmax(strokeWidth, 0.0f);
    if (!page || strokeA <= 0 || effectiveStrokeWidth <= 0.0f) return false;

    const float lineLength = hypotf(to.x - from.x, to.y - from.y);
    const float headLength = ResolvePdfArrowHeadLength(effectiveStrokeWidth, lineLength);
    const float headHalfHeight = fmax(headLength * 0.36f, effectiveStrokeWidth * 1.2f);
    const PdfShapePoint first = OffsetPdfShapePointFromLineEnd(from, to, headLength, headHalfHeight);
    const PdfShapePoint second = OffsetPdfShapePointFromLineEnd(from, to, headLength, -headHalfHeight);
    FPDF_PAGEOBJECT arrowPath = FPDFPageObj_CreateNewPath(to.x, to.y);
    if (!arrowPath) return false;
    FPDFPath_LineTo(arrowPath, first.x, first.y);
    FPDFPath_LineTo(arrowPath, second.x, second.y);
    FPDFPath_Close(arrowPath);
    return InsertPdfShapeContentPath(
            page,
            arrowPath,
            true,
            strokeR,
            strokeG,
            strokeB,
            strokeA,
            strokeR,
            strokeG,
            strokeB,
            strokeA,
            effectiveStrokeWidth
    );
}

static std::string BuildPdfShapeAppearanceStreamAscii(
        int typeInt,
        const FS_RECTF& baseRect,
        float rotationDegrees,
        int strokeR,
        int strokeG,
        int strokeB,
        int strokeA,
        int fillR,
        int fillG,
        int fillB,
        int fillA,
        float strokeWidth,
        bool arrowStart,
        bool arrowEnd,
        const char* graphicsStateName = nullptr,
        const std::vector<PdfShapePoint>* customPoints = nullptr
) {
    const float left = fmin(baseRect.left, baseRect.right);
    const float right = fmax(baseRect.left, baseRect.right);
    const float bottom = fmin(baseRect.bottom, baseRect.top);
    const float top = fmax(baseRect.bottom, baseRect.top);
    const float width = right - left;
    const float height = top - bottom;
    if (width <= 0.1f || height <= 0.1f) return std::string();

    const float effectiveStrokeWidth = fmax(strokeWidth, 0.0f);
    const float strokeRed = std::max(0.0f, std::min(strokeR / 255.0f, 1.0f));
    const float strokeGreen = std::max(0.0f, std::min(strokeG / 255.0f, 1.0f));
    const float strokeBlue = std::max(0.0f, std::min(strokeB / 255.0f, 1.0f));
    const float fillRed = std::max(0.0f, std::min(fillR / 255.0f, 1.0f));
    const float fillGreen = std::max(0.0f, std::min(fillG / 255.0f, 1.0f));
    const float fillBlue = std::max(0.0f, std::min(fillB / 255.0f, 1.0f));

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(3);
    stream << "q ";
    if (graphicsStateName && graphicsStateName[0] != '\0') {
        stream << "/" << graphicsStateName << " gs ";
    }
    stream << "1 J 1 j " << effectiveStrokeWidth << " w "
           << strokeRed << ' ' << strokeGreen << ' ' << strokeBlue << " RG "
           << fillRed << ' ' << fillGreen << ' ' << fillBlue << " rg ";

    const auto emitMove = [&](PdfShapePoint point) {
        stream << point.x << ' ' << point.y << " m ";
    };
    const auto emitLine = [&](PdfShapePoint point) {
        stream << point.x << ' ' << point.y << " l ";
    };

    const bool hasCustomVertexPoints =
            customPoints && customPoints->size() >= 2 && (typeInt == 15 || typeInt == 16);
    const bool hasCustomLinePoints =
            customPoints && customPoints->size() >= 2 && (typeInt == 17 || typeInt == 18);
    const auto emitArrowHead = [&](PdfShapePoint from, PdfShapePoint to) {
        const float lineLength = hypotf(to.x - from.x, to.y - from.y);
        const float headLength = ResolvePdfArrowHeadLength(effectiveStrokeWidth, lineLength);
        const float headHalfHeight = fmax(headLength * 0.36f, effectiveStrokeWidth * 1.2f);
        const PdfShapePoint first = OffsetPdfShapePointFromLineEnd(from, to, headLength, headHalfHeight);
        const PdfShapePoint second = OffsetPdfShapePointFromLineEnd(from, to, headLength, -headHalfHeight);
        stream << strokeRed << ' ' << strokeGreen << ' ' << strokeBlue << " rg ";
        emitMove(to);
        emitLine(first);
        emitLine(second);
        stream << "h B ";
        stream << fillRed << ' ' << fillGreen << ' ' << fillBlue << " rg ";
    };
    if (hasCustomVertexPoints) {
        emitMove((*customPoints)[0]);
        for (size_t index = 1; index < customPoints->size(); index++) {
            emitLine((*customPoints)[index]);
        }
        if (typeInt == 15) {
            stream << "h ";
            AppendPdfShapePaintOperator(stream, true, fillA, strokeA, effectiveStrokeWidth);
        } else {
            stream << "S ";
            if (arrowStart) {
                emitArrowHead((*customPoints)[1], (*customPoints)[0]);
            }
            if (arrowEnd) {
                emitArrowHead((*customPoints)[customPoints->size() - 2], customPoints->back());
            }
        }
    } else if (typeInt == 17 || typeInt == 18) {
        const PdfShapePoint start = hasCustomLinePoints
                ? (*customPoints)[0]
                : PdfShapePointFromFraction(baseRect, 0.08f, 0.50f, rotationDegrees);
        const PdfShapePoint end = hasCustomLinePoints
                ? (*customPoints)[1]
                : PdfShapePointFromFraction(baseRect, 0.92f, 0.50f, rotationDegrees);
        emitMove(start);
        emitLine(end);
        stream << "S ";
        if (arrowStart) {
            emitArrowHead(end, start);
        }
        if (arrowEnd) {
            emitArrowHead(start, end);
        }
    } else if (typeInt == 16) {
        emitMove(PdfShapePointFromFraction(baseRect, 0.05f, 0.80f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.35f, 0.20f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.65f, 0.65f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.95f, 0.10f, rotationDegrees));
        stream << "S ";
    } else if (typeInt == 15) {
        emitMove(PdfShapePointFromFraction(baseRect, 0.50f, 0.00f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 1.00f, 0.38f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.82f, 1.00f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.18f, 1.00f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.00f, 0.38f, rotationDegrees));
        stream << "h ";
        AppendPdfShapePaintOperator(stream, true, fillA, strokeA, effectiveStrokeWidth);
    } else if (typeInt == 14) {
        if (fabs(rotationDegrees) < 0.001f) {
            AppendPdfEllipsePath(
                    stream,
                    (left + right) * 0.5f,
                    (bottom + top) * 0.5f,
                    width * 0.5f,
                    height * 0.5f
            );
            AppendPdfShapePaintOperator(stream, true, fillA, strokeA, effectiveStrokeWidth);
        } else {
            const int segmentCount = 32;
            for (int index = 0; index < segmentCount; index++) {
                const double angle = (2.0 * M_PI * index) / segmentCount;
                const float fx = 0.5f + static_cast<float>(cos(angle) * 0.5);
                const float fy = 0.5f - static_cast<float>(sin(angle) * 0.5);
                if (index == 0) {
                    emitMove(PdfShapePointFromFraction(baseRect, fx, fy, rotationDegrees));
                } else {
                    emitLine(PdfShapePointFromFraction(baseRect, fx, fy, rotationDegrees));
                }
            }
            stream << "h ";
            AppendPdfShapePaintOperator(stream, true, fillA, strokeA, effectiveStrokeWidth);
        }
    } else {
        emitMove(PdfShapePointFromFraction(baseRect, 0.00f, 0.00f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 1.00f, 0.00f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 1.00f, 1.00f, rotationDegrees));
        emitLine(PdfShapePointFromFraction(baseRect, 0.00f, 1.00f, rotationDegrees));
        stream << "h ";
        AppendPdfShapePaintOperator(stream, true, fillA, strokeA, effectiveStrokeWidth);
    }

    stream << "Q";
    return stream.str();
}

static std::u16string BuildPdfShapeAppearanceStream(
        int typeInt,
        const FS_RECTF& baseRect,
        float rotationDegrees,
        int strokeR,
        int strokeG,
        int strokeB,
        int strokeA,
        int fillR,
        int fillG,
        int fillB,
        int fillA,
        float strokeWidth,
        bool arrowStart,
        bool arrowEnd,
        const std::vector<PdfShapePoint>* customPoints = nullptr
) {
    const std::string appearanceStream = BuildPdfShapeAppearanceStreamAscii(
            typeInt,
            baseRect,
            rotationDegrees,
            strokeR,
            strokeG,
            strokeB,
            strokeA,
            fillR,
            fillG,
            fillB,
            fillA,
            strokeWidth,
            arrowStart,
            arrowEnd,
            nullptr,
            customPoints
    );
    return AsciiToUtf16(appearanceStream.c_str());
}

static bool IsPdfNameDelimiter(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) ||
           ch == '/' ||
           ch == '<' ||
           ch == '>' ||
           ch == '[' ||
           ch == ']' ||
           ch == '(' ||
           ch == ')';
}

static bool ParsePdfRectFromObject(
        const std::string& objectText,
        FS_RECTF* outRect
) {
    if (!outRect) return false;
    const size_t rectKey = objectText.find("/Rect");
    if (rectKey == std::string::npos) return false;
    const size_t arrayStart = objectText.find('[', rectKey);
    const size_t arrayEnd = objectText.find(']', arrayStart);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart) {
        return false;
    }

    const std::string numbers = objectText.substr(arrayStart + 1, arrayEnd - arrayStart - 1);
    const char* cursor = numbers.c_str();
    char* end = nullptr;
    float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int index = 0; index < 4; index++) {
        values[index] = std::strtof(cursor, &end);
        if (end == cursor) return false;
        cursor = end;
    }

    outRect->left = values[0];
    outRect->bottom = values[1];
    outRect->right = values[2];
    outRect->top = values[3];
    return true;
}

static std::string FormatPdfFloat(float value) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(3) << value;
    std::string output = stream.str();
    while (output.size() > 1 && output.back() == '0') {
        output.pop_back();
    }
    if (!output.empty() && output.back() == '.') {
        output.pop_back();
    }
    return output.empty() ? "0" : output;
}

static void AppendPdfPoint(std::ostringstream& stream, PdfShapePoint point) {
    stream << FormatPdfFloat(point.x) << ' ' << FormatPdfFloat(point.y);
}

static float ParsePdfShapeBorderWidthFromObject(const std::string& objectText) {
    const size_t bsKey = objectText.find("/BS");
    if (bsKey != std::string::npos) {
        const size_t bsEnd = objectText.find(">>", bsKey);
        const size_t widthKey = objectText.find("/W", bsKey);
        if (widthKey != std::string::npos && (bsEnd == std::string::npos || widthKey < bsEnd)) {
            const char* cursor = objectText.c_str() + widthKey + 2;
            char* end = nullptr;
            const float width = std::strtof(cursor, &end);
            if (end != cursor && width > 0.0f) {
                return width;
            }
        }
    }

    const size_t borderKey = objectText.find("/Border");
    if (borderKey != std::string::npos) {
        const size_t arrayStart = objectText.find('[', borderKey);
        const size_t arrayEnd = objectText.find(']', arrayStart);
        if (arrayStart != std::string::npos && arrayEnd != std::string::npos && arrayEnd > arrayStart) {
            const std::string numbers = objectText.substr(arrayStart + 1, arrayEnd - arrayStart - 1);
            const char* cursor = numbers.c_str();
            char* end = nullptr;
            float values[3] = {0.0f, 0.0f, 1.0f};
            for (int index = 0; index < 3; index++) {
                values[index] = std::strtof(cursor, &end);
                if (end == cursor) return 1.0f;
                cursor = end;
            }
            if (values[2] > 0.0f) {
                return values[2];
            }
        }
    }

    return 1.0f;
}

static float ParsePdfShapeOpacityFromMarker(
        const std::string& objectText,
        const std::string& marker
) {
    const size_t markerPos = objectText.find(marker);
    if (markerPos == std::string::npos) return -1.0f;

    size_t cursor = markerPos + marker.size();
    int alpha = 0;
    bool hasDigit = false;
    while (cursor < objectText.size() &&
           std::isdigit(static_cast<unsigned char>(objectText[cursor]))) {
        hasDigit = true;
        alpha = (alpha * 10) + (objectText[cursor] - '0');
        cursor++;
    }
    if (!hasDigit) return -1.0f;
    alpha = std::max(0, std::min(alpha, 255));
    return alpha / 255.0f;
}

static float ParsePdfShapeOpacityFromObject(const std::string& objectText) {
    return ParsePdfShapeOpacityFromMarker(objectText, "/LufickPdfShapeAlpha");
}

static float ParsePdfBoxShapeOpacityFromObject(const std::string& objectText) {
    return ParsePdfShapeOpacityFromMarker(objectText, "/LufickPdfBoxShapeAlpha");
}

static void SetPdfShapeFloatMarker(FPDF_ANNOTATION annot, const char* prefix, float value) {
    if (!annot || !prefix) return;
    const long long scaled = static_cast<long long>(std::llround(value * 1000.0f));
    const long long magnitude = scaled < 0 ? -scaled : scaled;
    std::ostringstream marker;
    marker << prefix << (scaled < 0 ? 'N' : 'P') << magnitude;
    SetAnnotAsciiStringValue(annot, marker.str().c_str(), "1");
}

static bool ParsePdfShapeFloatMarker(
        const std::string& objectText,
        const std::string& marker,
        float* outValue
) {
    if (!outValue) return false;
    const size_t markerPos = objectText.find(marker);
    if (markerPos == std::string::npos) return false;

    size_t cursor = markerPos + marker.size();
    if (cursor >= objectText.size()) return false;
    const bool isNegative = objectText[cursor] == 'N';
    if (!isNegative && objectText[cursor] != 'P') return false;
    cursor++;

    long long scaled = 0;
    bool hasDigit = false;
    while (cursor < objectText.size() &&
           std::isdigit(static_cast<unsigned char>(objectText[cursor]))) {
        hasDigit = true;
        scaled = (scaled * 10) + (objectText[cursor] - '0');
        cursor++;
    }
    if (!hasDigit) return false;
    *outValue = (isNegative ? -1.0f : 1.0f) * (static_cast<float>(scaled) / 1000.0f);
    return true;
}

static std::vector<PdfShapePoint> ReadPdfShapePointsFromMarkers(const std::string& objectText) {
    std::vector<PdfShapePoint> points;
    for (int index = 0; index < 64; index++) {
        std::ostringstream xMarker;
        xMarker << "/LufickPdfShapePoint" << index << "X";
        std::ostringstream yMarker;
        yMarker << "/LufickPdfShapePoint" << index << "Y";

        float x = 0.0f;
        float y = 0.0f;
        if (!ParsePdfShapeFloatMarker(objectText, xMarker.str(), &x) ||
            !ParsePdfShapeFloatMarker(objectText, yMarker.str(), &y)) {
            break;
        }
        points.push_back({x, y});
    }
    return points;
}

static std::vector<PdfShapePoint> BuildNativePdfShapePatchPoints(
        int typeInt,
        const FS_RECTF& rect,
        float rotationDegrees
) {
    if (typeInt == 15) {
        return {
                PdfShapePointFromFraction(rect, 0.50f, 0.00f, rotationDegrees),
                PdfShapePointFromFraction(rect, 1.00f, 0.38f, rotationDegrees),
                PdfShapePointFromFraction(rect, 0.82f, 1.00f, rotationDegrees),
                PdfShapePointFromFraction(rect, 0.18f, 1.00f, rotationDegrees),
                PdfShapePointFromFraction(rect, 0.00f, 0.38f, rotationDegrees)
        };
    }
    if (typeInt == 16) {
        return {
                PdfShapePointFromFraction(rect, 0.05f, 0.80f, rotationDegrees),
                PdfShapePointFromFraction(rect, 0.35f, 0.20f, rotationDegrees),
                PdfShapePointFromFraction(rect, 0.65f, 0.65f, rotationDegrees),
                PdfShapePointFromFraction(rect, 0.95f, 0.10f, rotationDegrees)
        };
    }
    return {
            PdfShapePointFromFraction(rect, 0.08f, 0.50f, rotationDegrees),
            PdfShapePointFromFraction(rect, 0.92f, 0.50f, rotationDegrees)
    };
}

static std::string BuildNativePdfShapeDictionaryPatch(
        int typeInt,
        const FS_RECTF& rect,
        const std::string& objectText
) {
    float rotation = 0.0f;
    ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeRotation", &rotation);

    FS_RECTF pointRect = rect;
    if (fabs(rotation) >= 0.001f) {
        float left = rect.left;
        float top = rect.top;
        float right = rect.right;
        float bottom = rect.bottom;
        const bool hasBaseRect =
                ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeBaseLeft", &left) &&
                ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeBaseTop", &top) &&
                ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeBaseRight", &right) &&
                ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeBaseBottom", &bottom);
        if (hasBaseRect) {
            pointRect = {left, top, right, bottom};
        }
    }

    std::vector<PdfShapePoint> points = ReadPdfShapePointsFromMarkers(objectText);
    if (points.empty()) {
        points = BuildNativePdfShapePatchPoints(
                typeInt,
                pointRect,
                rotation
        );
    }
    if (points.empty()) return std::string();

    std::ostringstream patch;
    patch.setf(std::ios::fixed);
    patch << std::setprecision(3);
    if (objectText.find("/BS") == std::string::npos) {
        patch << "/BS << /W " << FormatPdfFloat(ParsePdfShapeBorderWidthFromObject(objectText)) << " /S /S >> ";
    }
    const float opacity = ParsePdfShapeOpacityFromObject(objectText);
    if (opacity >= 0.0f) {
        patch << "/CA " << FormatPdfFloat(opacity) << " ";
    }

    if (typeInt == 17) {
        if (points.size() < 2) return std::string();
        patch << "/L [";
        AppendPdfPoint(patch, points[0]);
        patch << ' ';
        AppendPdfPoint(patch, points[1]);
        patch << "] ";
        return patch.str();
    }

    if (typeInt == 18) {
        if (points.size() < 2) return std::string();
        float arrowStartValue = 0.0f;
        float arrowEndValue = 1.0f;
        ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeArrowStart", &arrowStartValue);
        ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeArrowEnd", &arrowEndValue);
        patch << "/L [";
        AppendPdfPoint(patch, points[0]);
        patch << ' ';
        AppendPdfPoint(patch, points[1]);
        patch << "] ";
        patch << "/LE [" << PdfShapeLineEndName(arrowStartValue > 0.5f) << ' '
              << PdfShapeLineEndName(arrowEndValue > 0.5f) << "] ";
        return patch.str();
    }

    patch << "/Vertices [";
    for (size_t index = 0; index < points.size(); index++) {
        if (index > 0) patch << ' ';
        AppendPdfPoint(patch, points[index]);
    }
    patch << "] ";
    if (typeInt == 16) {
        float arrowStartValue = 0.0f;
        float arrowEndValue = 0.0f;
        ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeArrowStart", &arrowStartValue);
        ParsePdfShapeFloatMarker(objectText, "/LufickPdfShapeArrowEnd", &arrowEndValue);
        patch << "/LE [" << PdfShapeLineEndName(arrowStartValue > 0.5f) << ' '
              << PdfShapeLineEndName(arrowEndValue > 0.5f) << "] ";
    }
    return patch.str();
}

static bool ReplacePdfNameInRange(
        std::string* data,
        size_t objectStart,
        size_t objectEnd,
        const std::string& key,
        const std::string& newName
) {
    if (!data || objectStart >= objectEnd || objectEnd > data->size()) return false;
    const std::string keyToken = "/" + key;
    const size_t keyPos = data->find(keyToken, objectStart);
    if (keyPos == std::string::npos || keyPos >= objectEnd) return false;

    size_t valueStart = keyPos + keyToken.size();
    while (valueStart < objectEnd && std::isspace(static_cast<unsigned char>((*data)[valueStart]))) {
        valueStart++;
    }
    if (valueStart >= objectEnd || (*data)[valueStart] != '/') return false;

    size_t valueEnd = valueStart + 1;
    while (valueEnd < objectEnd && !IsPdfNameDelimiter((*data)[valueEnd])) {
        valueEnd++;
    }

    const size_t oldLength = valueEnd - valueStart;
    if (newName.size() > oldLength) return false;
    std::string replacement = newName;
    replacement.append(oldLength - replacement.size(), ' ');
    data->replace(valueStart, oldLength, replacement);
    return true;
}

static bool FindPdfDictionaryValueSegment(
        const std::string& data,
        size_t objectStart,
        size_t objectEnd,
        const std::string& key,
        size_t* outStart,
        size_t* outEnd
) {
    const std::string keyToken = "/" + key;
    const size_t keyPos = data.find(keyToken, objectStart);
    if (keyPos == std::string::npos || keyPos >= objectEnd) return false;

    size_t valueStart = keyPos + keyToken.size();
    while (valueStart < objectEnd && std::isspace(static_cast<unsigned char>(data[valueStart]))) {
        valueStart++;
    }
    if (valueStart >= objectEnd) return false;

    size_t valueEnd = valueStart;
    if (data[valueStart] == '<' && valueStart + 1 < objectEnd && data[valueStart + 1] != '<') {
        valueEnd = data.find('>', valueStart + 1);
        if (valueEnd == std::string::npos || valueEnd >= objectEnd) return false;
        valueEnd++;
    } else if (data[valueStart] == '(') {
        int depth = 1;
        bool escaped = false;
        valueEnd = valueStart + 1;
        while (valueEnd < objectEnd && depth > 0) {
            const char ch = data[valueEnd++];
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '(') {
                depth++;
            } else if (ch == ')') {
                depth--;
            }
        }
        if (depth != 0) return false;
    } else {
        valueEnd = valueStart;
        while (valueEnd < objectEnd && !std::isspace(static_cast<unsigned char>(data[valueEnd]))) {
            valueEnd++;
        }
    }

    *outStart = keyPos;
    *outEnd = valueEnd;
    return *outEnd > *outStart;
}

static void BlankPdfDictionaryValueSegment(
        std::string* data,
        size_t objectStart,
        size_t objectEnd,
        const std::string& key
) {
    if (!data) return;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindPdfDictionaryValueSegment(*data, objectStart, objectEnd, key, &valueStart, &valueEnd)) {
        return;
    }
    data->replace(valueStart, valueEnd - valueStart, valueEnd - valueStart, ' ');
}

static int GetPdfShapePatchTypeFromObject(const std::string& objectText) {
    if (objectText.find("/LufickPdfShapePatchArrowLine") != std::string::npos) return 18;
    if (objectText.find("/LufickPdfShapePatchLine") != std::string::npos) return 17;
    if (objectText.find("/LufickPdfShapePatchPolyLine") != std::string::npos) return 16;
    if (objectText.find("/LufickPdfShapePatchPolygon") != std::string::npos) return 15;
    return -1;
}

static const char* GetPdfShapePatchedSubtypeName(int typeInt) {
    switch (typeInt) {
        case 15: return "/Polygon";
        case 16: return "/PolyLine";
        case 17:
        case 18:
            return "/Line";
        default:
            return "/Square";
    }
}

static bool FindEnclosingPdfDictionaryRange(
        const std::string& data,
        size_t objectStart,
        size_t objectEnd,
        size_t position,
        size_t* outStart,
        size_t* outEnd
) {
    if (!outStart || !outEnd || position < objectStart || position >= objectEnd || objectEnd > data.size()) {
        return false;
    }

    std::vector<size_t> dictionaryStack;
    size_t bestStart = std::string::npos;
    size_t bestEnd = std::string::npos;

    for (size_t index = objectStart; index + 1 < objectEnd; index++) {
        const char current = data[index];
        const char next = data[index + 1];
        if (current == '<' && next == '<') {
            dictionaryStack.push_back(index);
            index++;
        } else if (current == '>' && next == '>' && !dictionaryStack.empty()) {
            const size_t start = dictionaryStack.back();
            dictionaryStack.pop_back();
            const size_t end = index + 2;
            if (start <= position && position < end &&
                (bestStart == std::string::npos || start > bestStart)) {
                bestStart = start;
                bestEnd = end;
            }
            index++;
        }
    }

    if (bestStart == std::string::npos || bestEnd == std::string::npos || bestEnd <= bestStart) {
        return false;
    }

    *outStart = bestStart;
    *outEnd = bestEnd;
    return true;
}

struct PdfObjectReplacement {
    int objectNumber;
    int generation;
    std::string body;
};

struct RawPdfWatermarkSpec {
    int pageIndex = -1;
    std::string text;
    std::string objectId;
    std::string fontName;
    std::string fontPath;
    std::string textPathData;
    float pageWidth = 0.0f;
    float pageHeight = 0.0f;
    float pageLeft = 0.0f;
    float pageBottom = 0.0f;
    float fontSize = 0.0f;
    float patternWidth = 0.0f;
    float patternHeight = 0.0f;
    float repeatStepWidth = 0.0f;
    float repeatStepHeight = 0.0f;
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    float textPathWidth = 0.0f;
    float textPathHeight = 0.0f;
    float baselineX = 0.0f;
    float baselineY = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float rotation = 0.0f;
    float opacity = 1.0f;
    float characterSpacing = 0.0f;
    bool isRepeated = true;
    int textR = 0;
    int textG = 0;
    int textB = 0;
    bool isBold = false;
    bool isItalic = false;
    bool isUnderline = false;
    bool isStrikeout = false;
    bool isIconImage = false;
    bool isRasterImage = false;
    bool requiresTextShaping = false;
    std::string imagePath;
    int imagePixelWidth = 0;
    int imagePixelHeight = 0;
};

struct PdfObjectInfo {
    int objectNumber = 0;
    int generation = 0;
    size_t start = 0;
    size_t end = 0;
    std::string body;
};

static int GetPdfBoxShapePatchTypeFromObject(const std::string& objectText) {
    if (objectText.find("/LufickPdfBoxShapePatchRectangle") != std::string::npos) return 12;
    if (objectText.find("/LufickPdfBoxShapePatchSquare") != std::string::npos) return 13;
    if (objectText.find("/LufickPdfBoxShapePatchCircle") != std::string::npos) return 14;
    return -1;
}

static size_t FindPdfKeyTokenInRange(
        const std::string& data,
        size_t start,
        size_t end,
        const std::string& key
) {
    if (start >= end || end > data.size()) return std::string::npos;
    const std::string keyToken = "/" + key;
    size_t pos = data.find(keyToken, start);
    while (pos != std::string::npos && pos < end) {
        const size_t afterKey = pos + keyToken.size();
        if (afterKey >= end || IsPdfNameDelimiter(data[afterKey])) {
            return pos;
        }
        pos = data.find(keyToken, afterKey);
    }
    return std::string::npos;
}

static size_t FindTopLevelPdfKeyTokenInDictionaryRange(
        const std::string& data,
        size_t start,
        size_t end,
        const std::string& key
) {
    if (start >= end || end > data.size()) return std::string::npos;
    const std::string keyToken = "/" + key;
    int dictionaryDepth = 0;
    int arrayDepth = 0;
    bool inLiteralString = false;
    bool escaped = false;
    for (size_t index = start; index < end; index++) {
        const char ch = data[index];
        if (inLiteralString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == ')') {
                inLiteralString = false;
            }
            continue;
        }
        if (ch == '(') {
            inLiteralString = true;
            escaped = false;
            continue;
        }
        if (index + 1 < end && ch == '<' && data[index + 1] == '<') {
            dictionaryDepth++;
            index++;
            continue;
        }
        if (index + 1 < end && ch == '>' && data[index + 1] == '>') {
            dictionaryDepth = std::max(0, dictionaryDepth - 1);
            index++;
            continue;
        }
        if (ch == '[') {
            arrayDepth++;
            continue;
        }
        if (ch == ']') {
            arrayDepth = std::max(0, arrayDepth - 1);
            continue;
        }
        if (dictionaryDepth == 1 && arrayDepth == 0 &&
            index + keyToken.size() <= end &&
            data.compare(index, keyToken.size(), keyToken) == 0) {
            const size_t afterKey = index + keyToken.size();
            if (afterKey >= end || IsPdfNameDelimiter(data[afterKey])) {
                return index;
            }
        }
    }
    return std::string::npos;
}

static bool ExtractPdfArrayValueFromObject(
        const std::string& objectText,
        const std::string& key,
        std::string* outArray
) {
    if (!outArray) return false;
    const size_t keyPos = FindPdfKeyTokenInRange(objectText, 0, objectText.size(), key);
    if (keyPos == std::string::npos) return false;
    const size_t arrayStart = objectText.find('[', keyPos);
    const size_t arrayEnd = objectText.find(']', arrayStart);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart) {
        return false;
    }
    *outArray = objectText.substr(arrayStart, arrayEnd - arrayStart + 1);
    return true;
}

static bool ParsePdfFloatValueFromObject(
        const std::string& objectText,
        const std::string& key,
        float* outValue
) {
    if (!outValue) return false;
    const size_t keyPos = FindPdfKeyTokenInRange(objectText, 0, objectText.size(), key);
    if (keyPos == std::string::npos) return false;

    size_t valueStart = keyPos + key.size() + 1;
    while (valueStart < objectText.size() &&
           std::isspace(static_cast<unsigned char>(objectText[valueStart]))) {
        valueStart++;
    }
    if (valueStart >= objectText.size()) return false;

    char* end = nullptr;
    const float value = std::strtof(objectText.c_str() + valueStart, &end);
    if (end == objectText.c_str() + valueStart) return false;
    *outValue = value;
    return true;
}

static int PdfColorComponentToByte(float value) {
    const float scaled = value <= 1.0f ? value * 255.0f : value;
    return std::max(0, std::min(static_cast<int>(std::lround(scaled)), 255));
}

static bool ParsePdfColorArrayFromObject(
        const std::string& objectText,
        const std::string& key,
        int* outR,
        int* outG,
        int* outB
) {
    if (!outR || !outG || !outB) return false;
    std::string arrayValue;
    if (!ExtractPdfArrayValueFromObject(objectText, key, &arrayValue)) return false;

    const char* cursor = arrayValue.c_str() + 1;
    char* end = nullptr;
    float values[3] = {0.0f, 0.0f, 0.0f};
    for (int index = 0; index < 3; index++) {
        values[index] = std::strtof(cursor, &end);
        if (end == cursor) return false;
        cursor = end;
    }

    *outR = PdfColorComponentToByte(values[0]);
    *outG = PdfColorComponentToByte(values[1]);
    *outB = PdfColorComponentToByte(values[2]);
    return true;
}

static bool ParsePdfNormalAppearanceReference(
        const std::string& objectText,
        int* outObjectNumber,
        int* outGeneration
) {
    if (!outObjectNumber || !outGeneration) return false;
    const size_t apPos = FindPdfKeyTokenInRange(objectText, 0, objectText.size(), "AP");
    if (apPos == std::string::npos) return false;
    const size_t normalPos = FindPdfKeyTokenInRange(objectText, apPos, objectText.size(), "N");
    if (normalPos == std::string::npos) return false;

    const char* cursor = objectText.c_str() + normalPos + 2;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    char* end = nullptr;
    const long objectNumber = std::strtol(cursor, &end, 10);
    if (end == cursor) return false;
    cursor = end;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    const long generation = std::strtol(cursor, &end, 10);
    if (end == cursor) return false;
    cursor = end;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    if (*cursor != 'R') return false;

    *outObjectNumber = static_cast<int>(objectNumber);
    *outGeneration = static_cast<int>(generation);
    return *outObjectNumber > 0 && *outGeneration >= 0;
}

static bool FindPdfObjectRange(
        const std::string& data,
        int objectNumber,
        int generation,
        size_t* outStart,
        size_t* outEnd
) {
    if (!outStart || !outEnd || objectNumber <= 0 || generation < 0) return false;
    const std::string objectHeader =
            std::to_string(objectNumber) + " " + std::to_string(generation) + " obj";
    size_t pos = data.find(objectHeader);
    while (pos != std::string::npos) {
        const bool validStart = pos == 0 || std::isspace(static_cast<unsigned char>(data[pos - 1]));
        const size_t afterHeader = pos + objectHeader.size();
        const bool validEnd = afterHeader >= data.size() ||
                              std::isspace(static_cast<unsigned char>(data[afterHeader]));
        if (validStart && validEnd) {
            const size_t endObj = data.find("endobj", afterHeader);
            if (endObj == std::string::npos) return false;
            *outStart = pos;
            *outEnd = endObj + strlen("endobj");
            return true;
        }
        pos = data.find(objectHeader, afterHeader);
    }
    return false;
}

static bool ExtractPdfStreamDictionaryText(
        const std::string& data,
        size_t objectStart,
        size_t objectEnd,
        std::string* outDictionary
) {
    if (!outDictionary || objectStart >= objectEnd || objectEnd > data.size()) return false;
    const size_t streamPos = data.find("stream", objectStart);
    if (streamPos == std::string::npos || streamPos >= objectEnd) return false;
    const size_t dictStart = data.find("<<", objectStart);
    const size_t dictEnd = data.rfind(">>", streamPos);
    if (dictStart == std::string::npos || dictEnd == std::string::npos ||
        dictEnd <= dictStart || dictEnd >= streamPos) {
        return false;
    }
    *outDictionary = data.substr(dictStart, dictEnd - dictStart + 2);
    return true;
}

static std::string FormatPdfRectArray(const FS_RECTF& rect) {
    std::ostringstream stream;
    stream << '['
           << FormatPdfFloat(rect.left) << ' '
           << FormatPdfFloat(rect.bottom) << ' '
           << FormatPdfFloat(rect.right) << ' '
           << FormatPdfFloat(rect.top) << ']';
    return stream.str();
}

static float ResolvePdfBoxShapeOpacity(const std::string& objectText) {
    float opacity = -1.0f;
    if (ParsePdfFloatValueFromObject(objectText, "CA", &opacity)) {
        return std::max(0.0f, std::min(opacity, 1.0f));
    }
    opacity = ParsePdfBoxShapeOpacityFromObject(objectText);
    return opacity >= 0.0f ? std::max(0.0f, std::min(opacity, 1.0f)) : -1.0f;
}

static bool BuildPdfBoxShapeAppearanceReplacementObject(
        const std::string& data,
        int appearanceObjectNumber,
        int appearanceGeneration,
        const std::string& annotationText,
        int typeInt,
        float opacity,
        std::string* outObjectBody
) {
    if (!outObjectBody || !IsDirectNativePdfBoxShape(typeInt) || opacity < 0.0f) {
        return false;
    }

    size_t appearanceStart = 0;
    size_t appearanceEnd = 0;
    if (!FindPdfObjectRange(data, appearanceObjectNumber, appearanceGeneration, &appearanceStart, &appearanceEnd)) {
        return false;
    }

    std::string appearanceDictionary;
    if (!ExtractPdfStreamDictionaryText(data, appearanceStart, appearanceEnd, &appearanceDictionary)) {
        return false;
    }

    FS_RECTF rect;
    if (!ParsePdfRectFromObject(annotationText, &rect)) return false;

    FS_RECTF baseRect = rect;
    float rotation = 0.0f;
    ParsePdfShapeFloatMarker(annotationText, "/LufickPdfBoxShapeRotation", &rotation);
    float baseLeft = rect.left;
    float baseTop = rect.top;
    float baseRight = rect.right;
    float baseBottom = rect.bottom;
    const bool hasBaseRect =
            ParsePdfShapeFloatMarker(annotationText, "/LufickPdfBoxShapeBaseLeft", &baseLeft) &&
            ParsePdfShapeFloatMarker(annotationText, "/LufickPdfBoxShapeBaseTop", &baseTop) &&
            ParsePdfShapeFloatMarker(annotationText, "/LufickPdfBoxShapeBaseRight", &baseRight) &&
            ParsePdfShapeFloatMarker(annotationText, "/LufickPdfBoxShapeBaseBottom", &baseBottom);
    if (hasBaseRect) {
        baseRect = {baseLeft, baseTop, baseRight, baseBottom};
    }

    int strokeR = 0;
    int strokeG = 0;
    int strokeB = 0;
    ParsePdfColorArrayFromObject(annotationText, "C", &strokeR, &strokeG, &strokeB);

    int fillR = 0;
    int fillG = 0;
    int fillB = 0;
    const bool hasFill = ParsePdfColorArrayFromObject(annotationText, "IC", &fillR, &fillG, &fillB);

    const int alpha = std::max(0, std::min(static_cast<int>(std::lround(opacity * 255.0f)), 255));
    const float strokeWidth = ParsePdfShapeBorderWidthFromObject(annotationText);
    const char* graphicsStateName = "LufickPdfBoxShapeGS";
    const std::string appearanceStream = BuildPdfShapeAppearanceStreamAscii(
            typeInt,
            baseRect,
            rotation,
            strokeR,
            strokeG,
            strokeB,
            alpha,
            fillR,
            fillG,
            fillB,
            hasFill ? alpha : 0,
            strokeWidth,
            false,
            false,
            graphicsStateName
    );
    if (appearanceStream.empty()) return false;

    std::string bbox;
    if (!ExtractPdfArrayValueFromObject(appearanceDictionary, "BBox", &bbox)) {
        bbox = FormatPdfRectArray(rect);
    }
    std::string matrix;
    const bool hasMatrix = ExtractPdfArrayValueFromObject(appearanceDictionary, "Matrix", &matrix);

    std::ostringstream replacement;
    replacement << "<< /Type /XObject /Subtype /Form /BBox " << bbox << ' ';
    if (hasMatrix) {
        replacement << "/Matrix " << matrix << ' ';
    }
    replacement << "/Resources << /ExtGState << /" << graphicsStateName
                << " << /Type /ExtGState /CA " << FormatPdfFloat(opacity)
                << " /ca " << FormatPdfFloat(opacity)
                << " >> >> >> /Length " << appearanceStream.size() << " >>\n"
                << "stream\n"
                << appearanceStream
                << "\nendstream";
    *outObjectBody = replacement.str();
    return true;
}

static bool ParseLastStartXref(const std::string& data, long long* outStartXref) {
    if (!outStartXref) return false;
    const size_t startXrefPos = data.rfind("startxref");
    if (startXrefPos == std::string::npos) return false;
    const char* cursor = data.c_str() + startXrefPos + strlen("startxref");
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    char* end = nullptr;
    const long long value = std::strtoll(cursor, &end, 10);
    if (end == cursor || value < 0) return false;
    *outStartXref = value;
    return true;
}

static bool FindBalancedPdfDictionary(
        const std::string& data,
        size_t dictStart,
        size_t limit,
        size_t* outStart,
        size_t* outEnd
);

static bool ExtractLastTrailerDictionary(const std::string& data, std::string* outTrailer) {
    if (!outTrailer) return false;
    const size_t trailerPos = data.rfind("trailer");
    size_t trailerStart = std::string::npos;
    size_t searchLimit = data.size();
    if (trailerPos != std::string::npos) {
        trailerStart = data.find("<<", trailerPos);
    } else {
        long long startXref = 0;
        if (!ParseLastStartXref(data, &startXref) ||
            startXref < 0 ||
            static_cast<size_t>(startXref) >= data.size()) {
            return false;
        }
        const size_t streamPos = data.find("stream", static_cast<size_t>(startXref));
        const size_t endObjPos = data.find("endobj", static_cast<size_t>(startXref));
        if (streamPos == std::string::npos ||
            endObjPos == std::string::npos ||
            streamPos > endObjPos) {
            return false;
        }
        trailerStart = data.find("<<", static_cast<size_t>(startXref));
        searchLimit = streamPos;
    }
    if (trailerStart == std::string::npos || trailerStart >= searchLimit) return false;

    std::vector<size_t> stack;
    for (size_t index = trailerStart; index + 1 < searchLimit; index++) {
        if (data[index] == '<' && data[index + 1] == '<') {
            stack.push_back(index);
            index++;
        } else if (data[index] == '>' && data[index + 1] == '>' && !stack.empty()) {
            stack.pop_back();
            if (stack.empty()) {
                *outTrailer = data.substr(trailerStart, index + 2 - trailerStart);
                return true;
            }
            index++;
        }
    }
    return false;
}

static bool FindPdfDictionaryRawValueSegment(
        const std::string& data,
        size_t rangeStart,
        size_t rangeEnd,
        const std::string& key,
        size_t* outStart,
        size_t* outEnd
) {
    if (!outStart || !outEnd || rangeStart >= rangeEnd || rangeEnd > data.size()) return false;
    const size_t keyPos = FindTopLevelPdfKeyTokenInDictionaryRange(data, rangeStart, rangeEnd, key);
    if (keyPos == std::string::npos) return false;

    size_t valueStart = keyPos + key.size() + 1;
    while (valueStart < rangeEnd && std::isspace(static_cast<unsigned char>(data[valueStart]))) {
        valueStart++;
    }
    if (valueStart >= rangeEnd) return false;

    size_t valueEnd = valueStart;
    if (data[valueStart] == '[') {
        int depth = 1;
        valueEnd = valueStart + 1;
        while (valueEnd < rangeEnd && depth > 0) {
            if (data[valueEnd] == '[') depth++;
            else if (data[valueEnd] == ']') depth--;
            valueEnd++;
        }
        if (depth != 0) return false;
    } else if (valueStart + 1 < rangeEnd && data[valueStart] == '<' && data[valueStart + 1] == '<') {
        size_t dictStart = 0;
        if (!FindBalancedPdfDictionary(data, valueStart, rangeEnd, &dictStart, &valueEnd)) {
            return false;
        }
    } else if (data[valueStart] == '<') {
        valueEnd = data.find('>', valueStart + 1);
        if (valueEnd == std::string::npos || valueEnd >= rangeEnd) return false;
        valueEnd++;
    } else if (data[valueStart] == '(') {
        int depth = 1;
        bool escaped = false;
        valueEnd = valueStart + 1;
        while (valueEnd < rangeEnd && depth > 0) {
            const char ch = data[valueEnd++];
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '(') {
                depth++;
            } else if (ch == ')') {
                depth--;
            }
        }
        if (depth != 0) return false;
    } else {
        const char* cursor = data.c_str() + valueStart;
        char* end = nullptr;
        const long firstNumber = std::strtol(cursor, &end, 10);
        if (end != cursor) {
            cursor = end;
            while (cursor < data.c_str() + rangeEnd && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
            char* secondEnd = nullptr;
            std::strtol(cursor, &secondEnd, 10);
            if (secondEnd != cursor) {
                const char* afterSecond = secondEnd;
                while (afterSecond < data.c_str() + rangeEnd &&
                       std::isspace(static_cast<unsigned char>(*afterSecond))) {
                    afterSecond++;
                }
                if (afterSecond < data.c_str() + rangeEnd && *afterSecond == 'R') {
                    valueEnd = static_cast<size_t>((afterSecond + 1) - data.c_str());
                } else {
                    valueEnd = static_cast<size_t>(end - data.c_str());
                }
            } else {
                valueEnd = static_cast<size_t>(end - data.c_str());
            }
            (void)firstNumber;
        } else {
            valueEnd = valueStart;
            while (valueEnd < rangeEnd && !std::isspace(static_cast<unsigned char>(data[valueEnd])) &&
                   data[valueEnd] != '/' && data[valueEnd] != '>' && data[valueEnd] != ']') {
                valueEnd++;
            }
        }
    }

    *outStart = valueStart;
    *outEnd = valueEnd;
    return valueEnd > valueStart;
}

static bool ExtractPdfDictionaryRawValue(
        const std::string& dictionary,
        const std::string& key,
        std::string* outValue
) {
    if (!outValue) return false;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindPdfDictionaryRawValueSegment(dictionary, 0, dictionary.size(), key, &valueStart, &valueEnd)) {
        return false;
    }
    *outValue = dictionary.substr(valueStart, valueEnd - valueStart);
    return !outValue->empty();
}

static bool ParsePdfIndirectReferenceString(
        const std::string& value,
        int* outObjectNumber,
        int* outGeneration
) {
    if (!outObjectNumber || !outGeneration) return false;
    const char* cursor = value.c_str();
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    char* end = nullptr;
    const long objectNumber = std::strtol(cursor, &end, 10);
    if (end == cursor || objectNumber <= 0) return false;
    cursor = end;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    const long generation = std::strtol(cursor, &end, 10);
    if (end == cursor || generation < 0) return false;
    cursor = end;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    if (*cursor != 'R') return false;
    *outObjectNumber = static_cast<int>(objectNumber);
    *outGeneration = static_cast<int>(generation);
    return true;
}

static bool BuildClassicIncrementalTrailer(
        const std::string& previousTrailer,
        int maxObjectNumber,
        long long previousStartXref,
        std::string* outTrailer,
        const std::string* overrideInfoRef = nullptr
) {
    if (!outTrailer || maxObjectNumber <= 0 || previousStartXref < 0) return false;

    std::string sizeValue;
    if (!ExtractPdfDictionaryRawValue(previousTrailer, "Size", &sizeValue)) {
        LOGE("Incremental PDF append failed: previous trailer has no Size");
        return false;
    }
    const long oldSize = std::strtol(sizeValue.c_str(), nullptr, 10);
    const int newSize = std::max(static_cast<int>(oldSize), maxObjectNumber + 1);

    std::string rootValue;
    if (!ExtractPdfDictionaryRawValue(previousTrailer, "Root", &rootValue)) {
        LOGE("Incremental PDF append failed: previous trailer has no Root");
        return false;
    }

    std::ostringstream trailer;
    trailer << "<< /Size " << newSize
            << " /Root " << rootValue;

    std::string value;
    if (overrideInfoRef && !overrideInfoRef->empty()) {
        trailer << " /Info " << *overrideInfoRef;
    } else if (ExtractPdfDictionaryRawValue(previousTrailer, "Info", &value)) {
        trailer << " /Info " << value;
    }
    if (ExtractPdfDictionaryRawValue(previousTrailer, "ID", &value)) {
        trailer << " /ID " << value;
    }
    if (ExtractPdfDictionaryRawValue(previousTrailer, "Encrypt", &value)) {
        trailer << " /Encrypt " << value;
    }
    trailer << " /Prev " << previousStartXref << " >>";
    *outTrailer = trailer.str();
    return true;
}

static bool AppendIncrementalPdfObjectUpdates(
        std::string* data,
        std::vector<PdfObjectReplacement>* replacements,
        const std::string* overrideInfoRef = nullptr
) {
    if (!data || !replacements || replacements->empty()) return true;

    long long previousStartXref = 0;
    if (!ParseLastStartXref(*data, &previousStartXref)) {
        LOGE("Incremental PDF append failed: unable to parse startxref");
        return false;
    }

    int maxObjectNumber = 0;
    for (const PdfObjectReplacement& replacement : *replacements) {
        maxObjectNumber = std::max(maxObjectNumber, replacement.objectNumber);
    }

    std::string previousTrailer;
    if (!ExtractLastTrailerDictionary(*data, &previousTrailer)) {
        LOGE("Incremental PDF append failed: unable to extract previous trailer");
        return false;
    }

    std::string trailer;
    if (!BuildClassicIncrementalTrailer(previousTrailer, maxObjectNumber, previousStartXref, &trailer, overrideInfoRef)) {
        return false;
    }

    std::sort(replacements->begin(), replacements->end(), [](const PdfObjectReplacement& first, const PdfObjectReplacement& second) {
        if (first.objectNumber != second.objectNumber) return first.objectNumber < second.objectNumber;
        return first.generation < second.generation;
    });
    replacements->erase(std::unique(replacements->begin(), replacements->end(), [](const PdfObjectReplacement& first, const PdfObjectReplacement& second) {
        return first.objectNumber == second.objectNumber && first.generation == second.generation;
    }), replacements->end());

    if (!data->empty() && data->back() != '\n') {
        data->push_back('\n');
    }

    std::vector<long long> offsets;
    offsets.reserve(replacements->size());
    for (const PdfObjectReplacement& replacement : *replacements) {
        offsets.push_back(static_cast<long long>(data->size()));
        data->append(std::to_string(replacement.objectNumber));
        data->push_back(' ');
        data->append(std::to_string(replacement.generation));
        data->append(" obj\n");
        data->append(replacement.body);
        data->append("\nendobj\n");
    }

    const long long xrefStart = static_cast<long long>(data->size());
    data->append("xref\n");
    for (size_t index = 0; index < replacements->size(); index++) {
        const PdfObjectReplacement& replacement = (*replacements)[index];
        data->append(std::to_string(replacement.objectNumber));
        data->append(" 1\n");
        char offsetBuffer[32];
        snprintf(offsetBuffer, sizeof(offsetBuffer), "%010lld %05d n \n", offsets[index], replacement.generation);
        data->append(offsetBuffer);
    }
    data->append("trailer\n");
    data->append(trailer);
    data->append("\nstartxref\n");
    data->append(std::to_string(xrefStart));
    data->append("\n%%EOF\n");
    return true;
}

static bool ReadFileToString(const char* path, std::string* outData) {
    if (!path || !outData) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    *outData = std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return !outData->empty();
}

static bool WriteStringToFile(const char* path, const std::string& data) {
    if (!path) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    return true;
}

static bool CopyFileBinary(const char* inputPath, const char* outputPath) {
    if (!inputPath || !outputPath) return false;
    std::ifstream input(inputPath, std::ios::binary);
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!input || !output) return false;
    output << input.rdbuf();
    return output.good();
}

static long long GetFileSizeForLog(const char* path) {
    if (!path) return -1;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return -1;
    return static_cast<long long>(input.tellg());
}

static bool ParsePdfObjectHeaderLine(const std::string& line, int* objectNumber, int* generation) {
    if (!objectNumber || !generation) return false;
    const char* cursor = line.c_str();
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    char* end = nullptr;
    const long obj = std::strtol(cursor, &end, 10);
    if (end == cursor || obj <= 0) return false;
    cursor = end;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    const long gen = std::strtol(cursor, &end, 10);
    if (end == cursor || gen < 0) return false;
    cursor = end;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    if (strncmp(cursor, "obj", 3) != 0) return false;
    *objectNumber = static_cast<int>(obj);
    *generation = static_cast<int>(gen);
    return true;
}

static size_t FindPdfObjectEndToken(const std::string& data, size_t searchPos);

static std::vector<PdfObjectInfo> ScanPdfObjects(const std::string& data) {
    std::vector<PdfObjectInfo> objects;
    size_t searchPos = 0;
    while (true) {
        const size_t objPos = data.find(" obj", searchPos);
        if (objPos == std::string::npos) break;
        size_t lineStart = data.rfind('\n', objPos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        const size_t lineEnd = data.find('\n', objPos);
        if (lineEnd == std::string::npos) break;
        const std::string header = data.substr(lineStart, lineEnd - lineStart);
        int objectNumber = 0;
        int generation = 0;
        if (!ParsePdfObjectHeaderLine(header, &objectNumber, &generation)) {
            searchPos = objPos + 4;
            continue;
        }
        const size_t endObj = FindPdfObjectEndToken(data, lineEnd);
        if (endObj == std::string::npos) break;
        const size_t bodyStart = lineEnd + 1;
        PdfObjectInfo info;
        info.objectNumber = objectNumber;
        info.generation = generation;
        info.start = lineStart;
        info.end = endObj + strlen("endobj");
        info.body = data.substr(bodyStart, endObj - bodyStart);
        objects.push_back(info);
        searchPos = info.end;
    }
    return objects;
}

static std::string PdfObjectRefKey(int objectNumber, int generation) {
    return std::to_string(objectNumber) + ":" + std::to_string(generation);
}

static bool IsPdfKeywordBoundary(const std::string& data, size_t pos, size_t length) {
    const size_t after = pos + length;
    const bool beforeOk = pos == 0 ||
                          std::isspace(static_cast<unsigned char>(data[pos - 1])) ||
                          IsPdfNameDelimiter(data[pos - 1]);
    const bool afterOk = after >= data.size() ||
                         std::isspace(static_cast<unsigned char>(data[after])) ||
                         IsPdfNameDelimiter(data[after]);
    return beforeOk && afterOk;
}

static size_t FindPdfObjectEndToken(const std::string& data, size_t searchPos) {
    size_t endObj = data.find("endobj", searchPos);
    while (endObj != std::string::npos) {
        if (IsPdfKeywordBoundary(data, endObj, strlen("endobj"))) {
            return endObj;
        }
        endObj = data.find("endobj", endObj + strlen("endobj"));
    }
    return std::string::npos;
}

static bool IsPdfPageObject(const PdfObjectInfo& object);
static bool ContainsPdfNameValue(const std::string& objectBody, const std::string& key, const std::string& value);
static bool FindTopLevelPdfDictionary(
        const std::string& objectBody,
        size_t* outStart,
        size_t* outEnd
);
static const PdfObjectInfo* FindPdfObjectInfoByRef(
        const std::vector<PdfObjectInfo>& objects,
        int objectNumber,
        int generation
);

static std::vector<PdfObjectInfo> BuildLatestPdfObjectsByRef(const std::vector<PdfObjectInfo>& objects) {
    std::vector<PdfObjectInfo> latestObjects;
    std::map<std::string, size_t> indexByRef;
    for (const PdfObjectInfo& object : objects) {
        const std::string key = PdfObjectRefKey(object.objectNumber, object.generation);
        const auto existing = indexByRef.find(key);
        if (existing == indexByRef.end()) {
            indexByRef[key] = latestObjects.size();
            latestObjects.push_back(object);
        } else {
            latestObjects[existing->second] = object;
        }
    }
    return latestObjects;
}

static std::vector<PdfObjectInfo> BuildLatestPdfPageObjects(
        const std::vector<PdfObjectInfo>& scannedObjects,
        const std::vector<PdfObjectInfo>& latestObjects
) {
    std::vector<PdfObjectInfo> pages;
    std::map<std::string, bool> seenPageRefs;
    for (const PdfObjectInfo& object : scannedObjects) {
        if (!IsPdfPageObject(object)) continue;
        const std::string key = PdfObjectRefKey(object.objectNumber, object.generation);
        if (seenPageRefs[key]) continue;
        seenPageRefs[key] = true;
        const PdfObjectInfo* latestObject = FindPdfObjectInfoByRef(
                latestObjects,
                object.objectNumber,
                object.generation
        );
        pages.push_back(latestObject ? *latestObject : object);
    }
    return pages;
}

static std::vector<std::pair<int, int>> ParsePdfIndirectReferencesFromArray(const std::string& arrayValue) {
    std::vector<std::pair<int, int>> refs;
    const char* cursor = arrayValue.c_str();
    const char* endOfString = cursor + arrayValue.size();
    while (cursor < endOfString) {
        while (cursor < endOfString &&
               !std::isdigit(static_cast<unsigned char>(*cursor)) &&
               *cursor != '+' &&
               *cursor != '-') {
            cursor++;
        }
        if (cursor >= endOfString) break;
        char* firstEnd = nullptr;
        const long objectNumber = std::strtol(cursor, &firstEnd, 10);
        if (firstEnd == cursor || objectNumber <= 0) {
            cursor++;
            continue;
        }
        cursor = firstEnd;
        while (cursor < endOfString && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
        char* secondEnd = nullptr;
        const long generation = std::strtol(cursor, &secondEnd, 10);
        if (secondEnd == cursor || generation < 0) {
            cursor = firstEnd;
            continue;
        }
        cursor = secondEnd;
        while (cursor < endOfString && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
        if (cursor < endOfString && *cursor == 'R') {
            refs.push_back({static_cast<int>(objectNumber), static_cast<int>(generation)});
            cursor++;
        }
    }
    return refs;
}

static bool AppendPdfPageTreePages(
        const std::vector<PdfObjectInfo>& latestObjects,
        int objectNumber,
        int generation,
        std::vector<PdfObjectInfo>* pages,
        std::map<std::string, bool>* visited,
        bool allowLeafFallback = false
) {
    if (!pages || !visited || objectNumber <= 0 || generation < 0) return false;
    const std::string key = PdfObjectRefKey(objectNumber, generation);
    if ((*visited)[key]) return true;
    (*visited)[key] = true;

    const PdfObjectInfo* object = FindPdfObjectInfoByRef(latestObjects, objectNumber, generation);
    if (!object) return false;
    if (IsPdfPageObject(*object)) {
        pages->push_back(*object);
        return true;
    }
    if (!ContainsPdfNameValue(object->body, "Type", "Pages")) {
        if (allowLeafFallback) {
            size_t dictStart = 0;
            size_t dictEnd = 0;
            if (FindTopLevelPdfDictionary(object->body, &dictStart, &dictEnd) &&
                FindPdfKeyTokenInRange(object->body, dictStart, dictEnd, "Kids") == std::string::npos) {
                WM_LOGE(
                        "Native watermark page tree recovered leaf without /Type /Page obj=%d gen=%d bodyLen=%zu annots=%d resources=%d contents=%d",
                        object->objectNumber,
                        object->generation,
                        object->body.size(),
                        FindPdfKeyTokenInRange(object->body, dictStart, dictEnd, "Annots") != std::string::npos ? 1 : 0,
                        FindPdfKeyTokenInRange(object->body, dictStart, dictEnd, "Resources") != std::string::npos ? 1 : 0,
                        FindPdfKeyTokenInRange(object->body, dictStart, dictEnd, "Contents") != std::string::npos ? 1 : 0
                );
                pages->push_back(*object);
                return true;
            }
        }
        return false;
    }

    std::string kidsArray;
    if (!ExtractPdfDictionaryRawValue(object->body, "Kids", &kidsArray)) {
        return false;
    }
    const std::vector<std::pair<int, int>> kids = ParsePdfIndirectReferencesFromArray(kidsArray);
    if (kids.empty()) return false;
    bool appendedAny = false;
    for (const auto& kid : kids) {
        const size_t before = pages->size();
        if (AppendPdfPageTreePages(latestObjects, kid.first, kid.second, pages, visited, true) &&
            pages->size() > before) {
            appendedAny = true;
        }
    }
    return appendedAny;
}

static std::vector<PdfObjectInfo> BuildPdfPageObjectsFromCatalog(
        const std::string& data,
        const std::vector<PdfObjectInfo>& latestObjects
) {
    std::vector<PdfObjectInfo> pages;
    std::string trailer;
    if (!ExtractLastTrailerDictionary(data, &trailer)) return pages;

    std::string rootValue;
    if (!ExtractPdfDictionaryRawValue(trailer, "Root", &rootValue)) return pages;
    int rootObjectNumber = 0;
    int rootGeneration = 0;
    if (!ParsePdfIndirectReferenceString(rootValue, &rootObjectNumber, &rootGeneration)) return pages;

    const PdfObjectInfo* rootObject = FindPdfObjectInfoByRef(latestObjects, rootObjectNumber, rootGeneration);
    if (!rootObject) return pages;

    std::string pagesValue;
    if (!ExtractPdfDictionaryRawValue(rootObject->body, "Pages", &pagesValue)) return pages;
    int pagesObjectNumber = 0;
    int pagesGeneration = 0;
    if (!ParsePdfIndirectReferenceString(pagesValue, &pagesObjectNumber, &pagesGeneration)) return pages;

    std::map<std::string, bool> visited;
    AppendPdfPageTreePages(latestObjects, pagesObjectNumber, pagesGeneration, &pages, &visited);
    return pages;
}

static int GetMaxPdfObjectNumber(const std::vector<PdfObjectInfo>& objects) {
    int maxObjectNumber = 0;
    for (const PdfObjectInfo& object : objects) {
        maxObjectNumber = std::max(maxObjectNumber, object.objectNumber);
    }
    return maxObjectNumber;
}

static bool FindBalancedPdfDictionary(
        const std::string& data,
        size_t dictStart,
        size_t limit,
        size_t* outStart,
        size_t* outEnd
) {
    if (!outStart || !outEnd || dictStart + 1 >= limit || limit > data.size() ||
        data[dictStart] != '<' || data[dictStart + 1] != '<') {
        return false;
    }
    int depth = 0;
    for (size_t index = dictStart; index + 1 < limit; index++) {
        if (data[index] == '<' && data[index + 1] == '<') {
            depth++;
            index++;
        } else if (data[index] == '>' && data[index + 1] == '>') {
            depth--;
            index++;
            if (depth == 0) {
                *outStart = dictStart;
                *outEnd = index + 1;
                return true;
            }
        }
    }
    return false;
}

static bool FindTopLevelPdfDictionary(
        const std::string& objectBody,
        size_t* outStart,
        size_t* outEnd
) {
    const size_t dictStart = objectBody.find("<<");
    const size_t streamPos = objectBody.find("stream");
    const size_t limit = streamPos == std::string::npos ? objectBody.size() : streamPos;
    if (dictStart == std::string::npos || dictStart >= limit) return false;
    return FindBalancedPdfDictionary(objectBody, dictStart, limit, outStart, outEnd);
}

static bool FindDirectDictionaryValue(
        const std::string& data,
        size_t rangeStart,
        size_t rangeEnd,
        const std::string& key,
        size_t* outStart,
        size_t* outEnd
) {
    const size_t keyPos = FindTopLevelPdfKeyTokenInDictionaryRange(data, rangeStart, rangeEnd, key);
    if (keyPos == std::string::npos) return false;
    size_t valueStart = keyPos + key.size() + 1;
    while (valueStart < rangeEnd && std::isspace(static_cast<unsigned char>(data[valueStart]))) {
        valueStart++;
    }
    if (valueStart + 1 >= rangeEnd || data[valueStart] != '<' || data[valueStart + 1] != '<') {
        return false;
    }
    return FindBalancedPdfDictionary(data, valueStart, rangeEnd, outStart, outEnd);
}

static bool ParseIndirectReferenceAt(
        const std::string& data,
        size_t valueStart,
        size_t rangeEnd,
        int* outObjectNumber,
        int* outGeneration,
        size_t* outValueEnd
) {
    if (!outObjectNumber || !outGeneration || !outValueEnd || valueStart >= rangeEnd) return false;
    const char* cursor = data.c_str() + valueStart;
    char* end = nullptr;
    const long objectNumber = std::strtol(cursor, &end, 10);
    if (end == cursor || objectNumber <= 0) return false;
    cursor = end;
    while (cursor < data.c_str() + rangeEnd && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    const long generation = std::strtol(cursor, &end, 10);
    if (end == cursor || generation < 0) return false;
    cursor = end;
    while (cursor < data.c_str() + rangeEnd && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
    if (cursor >= data.c_str() + rangeEnd || *cursor != 'R') return false;
    cursor++;
    *outObjectNumber = static_cast<int>(objectNumber);
    *outGeneration = static_cast<int>(generation);
    *outValueEnd = static_cast<size_t>(cursor - data.c_str());
    return true;
}

static bool FindIndirectReferenceValue(
        const std::string& data,
        size_t rangeStart,
        size_t rangeEnd,
        const std::string& key,
        int* outObjectNumber,
        int* outGeneration,
        size_t* outValueStart,
        size_t* outValueEnd
) {
    const size_t keyPos = FindTopLevelPdfKeyTokenInDictionaryRange(data, rangeStart, rangeEnd, key);
    if (keyPos == std::string::npos) return false;
    size_t valueStart = keyPos + key.size() + 1;
    while (valueStart < rangeEnd && std::isspace(static_cast<unsigned char>(data[valueStart]))) {
        valueStart++;
    }
    if (outValueStart) *outValueStart = valueStart;
    return ParseIndirectReferenceAt(data, valueStart, rangeEnd, outObjectNumber, outGeneration, outValueEnd);
}

static bool ContainsPdfNameValue(const std::string& objectBody, const std::string& key, const std::string& value) {
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!FindTopLevelPdfDictionary(objectBody, &dictStart, &dictEnd)) return false;
    const size_t keyPos = FindTopLevelPdfKeyTokenInDictionaryRange(objectBody, dictStart, dictEnd, key);
    if (keyPos == std::string::npos) return false;
    size_t valueStart = keyPos + key.size() + 1;
    while (valueStart < dictEnd && std::isspace(static_cast<unsigned char>(objectBody[valueStart]))) {
        valueStart++;
    }
    if (valueStart >= dictEnd || objectBody[valueStart] != '/') return false;
    const size_t nameStart = valueStart + 1;
    size_t nameEnd = nameStart;
    while (nameEnd < dictEnd && !IsPdfNameDelimiter(objectBody[nameEnd])) {
        nameEnd++;
    }
    return objectBody.compare(nameStart, nameEnd - nameStart, value) == 0;
}

static bool IsPdfPageObject(const PdfObjectInfo& object) {
    return ContainsPdfNameValue(object.body, "Type", "Page");
}

static bool IsPdfNameCharSafe(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
}

static std::string MakePdfResourceName(const std::string& prefix, int index) {
    std::ostringstream stream;
    stream << prefix << index;
    return stream.str();
}

static std::string EscapePdfLiteralString(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '(': escaped += "\\("; break;
            case ')': escaped += "\\)"; break;
            case '\r': escaped += "\\r"; break;
            case '\n': escaped += "\\n"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 32) {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\%03o", static_cast<unsigned char>(ch));
                    escaped += buffer;
                } else {
                    escaped.push_back(ch);
                }
                break;
        }
    }
    return escaped;
}

static const char* kPdfEditContentMarker = "CVPdfEditContent";

static bool PdfFileHasEditContentMarker(const char* inputPath) {
    if (!inputPath) return false;
    std::string data;
    return ReadFileToString(inputPath, &data) &&
           data.find(kPdfEditContentMarker) != std::string::npos;
}

static bool PdfFileHasToolkitTextStampOrCommentMarker(const char* inputPath) {
    if (!inputPath) return false;
    std::string data;
    return ReadFileToString(inputPath, &data) &&
           (data.find("/LufickTextStampMeta") != std::string::npos ||
            data.find("/LufickCommentMeta") != std::string::npos);
}

static bool AppendPdfEditContentMarker(const char* outputPath) {
    if (!outputPath) return false;
    std::string data;
    if (!ReadFileToString(outputPath, &data)) return false;
    if (data.find(kPdfEditContentMarker) != std::string::npos) return true;
    data.append("\n%");
    data.append(kPdfEditContentMarker);
    data.append("\n");
    return WriteStringToFile(outputPath, data);
}

static std::string BuildPdfWatermarkPatternStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
) {
    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(spec.textR / 255.0f) << ' '
           << FormatPdfFloat(spec.textG / 255.0f) << ' '
           << FormatPdfFloat(spec.textB / 255.0f) << " rg\n"
           << "BT\n";
    if (fabs(spec.characterSpacing) > 0.0001f) {
        stream << FormatPdfFloat(spec.characterSpacing) << " Tc\n";
    }
    stream << "/Fwm " << FormatPdfFloat(spec.fontSize) << " Tf\n"
           << FormatPdfFloat(spec.baselineX) << ' ' << FormatPdfFloat(spec.baselineY) << " Td\n"
           << "(" << EscapePdfLiteralString(spec.text) << ") Tj\n"
           << "ET\n";
    if (spec.isUnderline || spec.isStrikeout) {
        const float decorationWidth = fmax(
                spec.contentWidth > 0.0f ? spec.contentWidth : spec.repeatStepWidth,
                spec.fontSize
        );
        const float decorationThickness = fmax(spec.fontSize * 0.06f, 0.5f);
        const float maxDecorationY = fmax(spec.patternHeight - decorationThickness, 0.0f);
        auto appendDecorationRect = [&](float y) {
            const float clampedY = fmax(0.0f, fmin(y, maxDecorationY));
            stream << FormatPdfFloat(spec.baselineX) << ' '
                   << FormatPdfFloat(clampedY) << ' '
                   << FormatPdfFloat(decorationWidth) << ' '
                   << FormatPdfFloat(decorationThickness) << " re f\n";
        };
        if (spec.isUnderline) {
            appendDecorationRect(spec.baselineY - (spec.fontSize * 0.12f));
        }
        if (spec.isStrikeout) {
            appendDecorationRect(spec.baselineY + (spec.fontSize * 0.32f));
        }
    }
    stream << "Q";
    return stream.str();
}

static std::string BuildPdfWatermarkFontOutlinePatternStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
);

static std::string BuildPdfSingleWatermarkFontOutlineContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
);

static std::string BuildPdfSingleWatermarkContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& fontName,
        const std::string& graphicsStateName
);

static const char* GetWatermarkBaseFontName(const RawPdfWatermarkSpec& spec);

static bool IsJpegImagePath(const std::string& imagePath) {
    std::string normalized = imagePath;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return (normalized.size() >= 4 && normalized.compare(normalized.size() - 4, 4, ".jpg") == 0) ||
           (normalized.size() >= 5 && normalized.compare(normalized.size() - 5, 5, ".jpeg") == 0);
}

static std::string BuildPdfImageXObjectBody(
        const std::string& imageStream,
        int width,
        int height,
        const char* colorSpace,
        const char* filter,
        int smaskObjectNumber
) {
    std::ostringstream body;
    body << "<< /Type /XObject /Subtype /Image /Width " << width
         << " /Height " << height
         << " /ColorSpace " << colorSpace
         << " /BitsPerComponent 8 ";
    if (filter && strlen(filter) > 0) {
        body << "/Filter " << filter << ' ';
    }
    if (smaskObjectNumber > 0) {
        body << "/SMask " << smaskObjectNumber << " 0 R ";
    }
    body << "/Length " << imageStream.size() << " >>\nstream\n";
    std::string result = body.str();
    result.append(imageStream);
    result.append("\nendstream");
    return result;
}

static bool BuildRasterWatermarkImageReplacements(
        JNIEnv* env,
        const RawPdfWatermarkSpec& spec,
        int imageObjectNumber,
        int smaskObjectNumber,
        std::vector<PdfObjectReplacement>* replacements
) {
    if (!replacements || spec.imagePath.empty() || imageObjectNumber <= 0) return false;

    if (IsJpegImagePath(spec.imagePath)) {
        std::string jpegData;
        if (!ReadFileToString(spec.imagePath.c_str(), &jpegData) || jpegData.empty()) return false;
        const int width = std::max(spec.imagePixelWidth, 1);
        const int height = std::max(spec.imagePixelHeight, 1);
        replacements->push_back({
                imageObjectNumber,
                0,
                BuildPdfImageXObjectBody(jpegData, width, height, "/DeviceRGB", "/DCTDecode", 0)
        });
        return true;
    }

    WM_LOGE("Native raster watermark requires prepared JPEG asset: path=%s", spec.imagePath.c_str());
    return false;
}

static std::string BuildPdfWatermarkRasterImagePatternStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName,
        const std::string& imageName
) {
    const float drawWidth = fmax(spec.contentWidth, 1.0f);
    const float drawHeight = fmax(spec.contentHeight, 1.0f);
    const float drawX = (spec.patternWidth - drawWidth) * 0.5f;
    const float drawY = (spec.patternHeight - drawHeight) * 0.5f;
    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(drawWidth) << " 0 0 " << FormatPdfFloat(drawHeight) << ' '
           << FormatPdfFloat(drawX) << ' ' << FormatPdfFloat(drawY) << " cm\n"
           << "/" << imageName << " Do\n"
           << "Q";
    return stream.str();
}

static std::string BuildPdfSingleRasterWatermarkContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& imageName,
        const std::string& graphicsStateName
) {
    const double angleRad = spec.rotation * M_PI / 180.0;
    const float cosA = static_cast<float>(cos(angleRad));
    const float sinA = static_cast<float>(sin(angleRad));
    const float drawWidth = fmax(spec.contentWidth, 1.0f);
    const float drawHeight = fmax(spec.contentHeight, 1.0f);
    const float a = cosA * drawWidth;
    const float b = sinA * drawWidth;
    const float c = -sinA * drawHeight;
    const float d = cosA * drawHeight;
    const float centerX = spec.centerX > 0.0f ? spec.centerX : (spec.pageWidth * 0.5f);
    const float centerY = spec.centerY > 0.0f ? spec.centerY : (spec.pageHeight * 0.5f);
    const float e = centerX - ((a + c) * 0.5f);
    const float f = centerY - ((b + d) * 0.5f);
    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(a) << ' ' << FormatPdfFloat(b) << ' '
           << FormatPdfFloat(c) << ' ' << FormatPdfFloat(d) << ' '
           << FormatPdfFloat(e) << ' ' << FormatPdfFloat(f) << " cm\n"
           << "/" << imageName << " Do\n"
           << "Q";
    return stream.str();
}


// Method creates one PDF /Pattern object, pattern contains the watermark text only once and reuse them later
// This is the important compact part. Instead of writing “Text” hundreds of times on every page, the PDF has one reusable pattern.
static std::string BuildPdfWatermarkPatternObjectBody(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName,
        const std::string& imageName = "",
        int imageObjectNumber = 0
) {
    std::string stream;
    bool usedFontOutline = false;
    if (spec.isRasterImage) {
        stream = BuildPdfWatermarkRasterImagePatternStream(spec, graphicsStateName, imageName);
    } else {
        stream = BuildPdfWatermarkFontOutlinePatternStream(spec, graphicsStateName);
        usedFontOutline = !stream.empty();
    }
    if (!usedFontOutline && !spec.isRasterImage) {
        stream = BuildPdfWatermarkPatternStream(spec, graphicsStateName);
    }
    const double angleRad = spec.rotation * M_PI / 180.0;
    const float cosA = static_cast<float>(cos(angleRad));
    const float sinA = static_cast<float>(sin(angleRad));
    const char* baseFontName = spec.isBold && spec.isItalic
                               ? "Helvetica-BoldOblique"
                               : (spec.isBold ? "Helvetica-Bold" : (spec.isItalic ? "Helvetica-Oblique" : "Helvetica"));
    std::ostringstream body;
    body << "<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 "
         << "/BBox [0 0 " << FormatPdfFloat(spec.patternWidth) << ' ' << FormatPdfFloat(spec.patternHeight) << "] "
         << "/XStep " << FormatPdfFloat(spec.repeatStepWidth) << ' '
         << "/YStep " << FormatPdfFloat(spec.repeatStepHeight) << ' '
         << "/Matrix [" << FormatPdfFloat(cosA) << ' ' << FormatPdfFloat(sinA) << ' '
         << FormatPdfFloat(-sinA) << ' ' << FormatPdfFloat(cosA) << " 0 0] "
         << "/Resources << ";
    if (!usedFontOutline && !spec.isRasterImage) {
        body << "/Font << /Fwm << /Type /Font /Subtype /Type1 /BaseFont /" << baseFontName << " >> >> ";
    }
    if (spec.isRasterImage && !imageName.empty() && imageObjectNumber > 0) {
        body << "/XObject << /" << imageName << ' ' << imageObjectNumber << " 0 R >> ";
    }
    body << "/ExtGState << /" << graphicsStateName << " << /Type /ExtGState /CA "
         << FormatPdfFloat(spec.opacity) << " /ca " << FormatPdfFloat(spec.opacity) << " >> >> >> "
         << "/Length " << stream.size() << " >>\n"
         << "stream\n"
         << stream
         << "\nendstream";
    return body.str();
}

static std::string BuildPdfSingleWatermarkPatternObjectBody(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName,
        const std::string& imageName = "",
        int imageObjectNumber = 0
) {
    const float patternWidth = fmax(spec.pageWidth, 1.0f);
    const float patternHeight = fmax(spec.pageHeight, 1.0f);
    const char* fontName = "Fwm";
    const std::string stream = spec.isRasterImage
                               ? BuildPdfSingleRasterWatermarkContentStream(spec, imageName, graphicsStateName)
                               : BuildPdfSingleWatermarkContentStream(spec, fontName, graphicsStateName);
    const char* baseFontName = GetWatermarkBaseFontName(spec);

    std::ostringstream body;
    body << "<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 "
         << "/BBox [0 0 " << FormatPdfFloat(patternWidth) << ' ' << FormatPdfFloat(patternHeight) << "] "
         << "/XStep " << FormatPdfFloat(patternWidth) << ' '
         << "/YStep " << FormatPdfFloat(patternHeight) << ' '
         << "/Resources << ";
    if (spec.isRasterImage && !imageName.empty() && imageObjectNumber > 0) {
        body << "/XObject << /" << imageName << ' ' << imageObjectNumber << " 0 R >> ";
    } else if (!spec.isRasterImage) {
        body << "/Font << /" << fontName << " << /Type /Font /Subtype /Type1 /BaseFont /"
             << baseFontName << " >> >> ";
    }
    body << "/ExtGState << /" << graphicsStateName << " << /Type /ExtGState /CA "
         << FormatPdfFloat(spec.opacity) << " /ca " << FormatPdfFloat(spec.opacity) << " >> >> >> "
         << "/Length " << stream.size() << " >>\n"
         << "stream\n"
         << stream
         << "\nendstream";
    return body.str();
}

static std::string BuildPdfWatermarkContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& patternName
) {
    std::ostringstream stream;
    const float pageLeft = spec.pageLeft;
    const float pageBottom = spec.pageBottom;
    stream << "q\n"
           << "/Pattern cs\n"
           << "/" << patternName << " scn\n"
           << FormatPdfFloat(pageLeft) << ' ' << FormatPdfFloat(pageBottom) << ' '
           << FormatPdfFloat(spec.pageWidth) << ' ' << FormatPdfFloat(spec.pageHeight)
           << " re f\n"
           << "Q";
    return stream.str();
}

static const char* GetWatermarkBaseFontName(const RawPdfWatermarkSpec& spec) {
    const bool wantsSerif = spec.fontName.find("Serif") != std::string::npos ||
                            spec.fontName.find("serif") != std::string::npos ||
                            spec.fontName.find("Times") != std::string::npos;
    const bool wantsMono = spec.fontName.find("Mono") != std::string::npos ||
                           spec.fontName.find("mono") != std::string::npos ||
                           spec.fontName.find("Courier") != std::string::npos;
    if (wantsSerif) {
        return spec.isBold && spec.isItalic
               ? "Times-BoldItalic"
               : (spec.isBold ? "Times-Bold" : (spec.isItalic ? "Times-Italic" : "Times-Roman"));
    }
    if (wantsMono) {
        return spec.isBold && spec.isItalic
               ? "Courier-BoldOblique"
               : (spec.isBold ? "Courier-Bold" : (spec.isItalic ? "Courier-Oblique" : "Courier"));
    }
    return spec.isBold && spec.isItalic
           ? "Helvetica-BoldOblique"
           : (spec.isBold ? "Helvetica-Bold" : (spec.isItalic ? "Helvetica-Oblique" : "Helvetica"));
}

static std::string BuildPdfSingleWatermarkContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& fontName,
        const std::string& graphicsStateName
) {
    std::string outlineStream = BuildPdfSingleWatermarkFontOutlineContentStream(spec, graphicsStateName);
    if (!outlineStream.empty()) {
        return outlineStream;
    }

    const int textLength = std::max(1, static_cast<int>(spec.text.size()));
    const float approximateTextWidth =
            fmax((textLength * spec.fontSize * 0.55f) +
                 (std::max(0, textLength - 1) * spec.characterSpacing), spec.fontSize);
    const float localX = -approximateTextWidth * 0.5f;
    const float localY = -spec.fontSize * 0.25f;
    const double angleRad = spec.rotation * M_PI / 180.0;
    const float cosA = static_cast<float>(cos(angleRad));
    const float sinA = static_cast<float>(sin(angleRad));
    const float centerX = spec.centerX > 0.0f ? spec.centerX : (spec.pageWidth * 0.5f);
    const float centerY = spec.centerY > 0.0f ? spec.centerY : (spec.pageHeight * 0.5f);
    const float textX = centerX + (localX * cosA) - (localY * sinA);
    const float textY = centerY + (localX * sinA) + (localY * cosA);

    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(spec.textR / 255.0f) << ' '
           << FormatPdfFloat(spec.textG / 255.0f) << ' '
           << FormatPdfFloat(spec.textB / 255.0f) << " rg\n"
           << "BT\n";
    if (fabs(spec.characterSpacing) > 0.0001f) {
        stream << FormatPdfFloat(spec.characterSpacing) << " Tc\n";
    }
    stream << "/" << fontName << ' ' << FormatPdfFloat(spec.fontSize) << " Tf\n"
           << FormatPdfFloat(cosA) << ' ' << FormatPdfFloat(sinA) << ' '
           << FormatPdfFloat(-sinA) << ' ' << FormatPdfFloat(cosA) << ' '
           << FormatPdfFloat(textX) << ' ' << FormatPdfFloat(textY) << " Tm\n"
           << "(" << EscapePdfLiteralString(spec.text) << ") Tj\n"
           << "ET\n";
    if (spec.isUnderline || spec.isStrikeout) {
        const float decorationThickness = fmax(spec.fontSize * 0.06f, 0.5f);
        auto appendDecorationRect = [&](float y) {
            stream << "q\n"
                   << FormatPdfFloat(cosA) << ' ' << FormatPdfFloat(sinA) << ' '
                   << FormatPdfFloat(-sinA) << ' ' << FormatPdfFloat(cosA) << ' '
                   << FormatPdfFloat(textX) << ' ' << FormatPdfFloat(textY) << " cm\n"
                   << "0 " << FormatPdfFloat(y) << ' '
                   << FormatPdfFloat(approximateTextWidth) << ' '
                   << FormatPdfFloat(decorationThickness) << " re f\n"
                   << "Q\n";
        };
        if (spec.isUnderline) {
            appendDecorationRect(-spec.fontSize * 0.12f);
        }
        if (spec.isStrikeout) {
            appendDecorationRect(spec.fontSize * 0.32f);
        }
    }
    stream << "Q";
    return stream.str();
}

static std::string BuildPdfWatermarkFontObjectBody(const RawPdfWatermarkSpec& spec) {
    std::ostringstream body;
    body << "<< /Type /Font /Subtype /Type1 /BaseFont /"
         << GetWatermarkBaseFontName(spec)
         << " >>";
    return body.str();
}

static std::string BuildPdfWatermarkGraphicsStateObjectBody(const RawPdfWatermarkSpec& spec) {
    std::ostringstream body;
    body << "<< /Type /ExtGState /CA "
         << FormatPdfFloat(spec.opacity)
         << " /ca "
         << FormatPdfFloat(spec.opacity)
         << " >>";
    return body.str();
}

static std::string GetCurrentPdfObjectBody(
        const PdfObjectInfo& object,
        std::vector<PdfObjectReplacement>* replacements
);
static bool UpsertPdfObjectReplacement(
        std::vector<PdfObjectReplacement>* replacements,
        int objectNumber,
        int generation,
        const std::string& body
);

static bool AddNameReferenceToTopLevelDictionary(
        const std::string& body,
        const std::string& name,
        int objectNumber,
        std::string* outBody
) {
    if (!outBody || objectNumber <= 0) return false;
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!FindTopLevelPdfDictionary(body, &dictStart, &dictEnd)) return false;
    if (FindPdfKeyTokenInRange(body, dictStart, dictEnd, name) != std::string::npos) {
        *outBody = body;
        return true;
    }
    std::string updated = body;
    updated.insert(dictEnd - 2, "/" + name + " " + std::to_string(objectNumber) + " 0 R ");
    *outBody = updated;
    return true;
}

static std::string DescribePdfDictionaryValueForLog(
        const std::string& body,
        size_t dictStart,
        size_t dictEnd,
        const std::string& key
) {
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindPdfDictionaryRawValueSegment(body, dictStart, dictEnd, key, &valueStart, &valueEnd)) {
        return "missing";
    }
    if (valueStart >= valueEnd || valueEnd > body.size()) {
        return "invalid";
    }
    if (body[valueStart] == '[') {
        return "array";
    }
    if (valueStart + 1 < valueEnd && body[valueStart] == '<' && body[valueStart + 1] == '<') {
        return "direct-dict";
    }
    int objectNumber = 0;
    int generation = 0;
    size_t indirectEnd = 0;
    if (ParseIndirectReferenceAt(body, valueStart, dictEnd, &objectNumber, &generation, &indirectEnd)) {
        return "ref " + std::to_string(objectNumber) + " " + std::to_string(generation);
    }
    const size_t length = std::min<size_t>(valueEnd - valueStart, 24);
    return "raw " + body.substr(valueStart, length);
}

static bool AddResourceReferenceToDictionaryRange(
        std::string* body,
        size_t resourcesStart,
        size_t resourcesEnd,
        const std::vector<PdfObjectInfo>& objects,
        const std::string& categoryName,
        const std::string& resourceName,
        int resourceObjectNumber,
        std::vector<PdfObjectReplacement>* replacements
) {
    if (!body || !replacements || resourcesStart >= resourcesEnd || resourcesEnd > body->size() ||
        categoryName.empty() || resourceName.empty() || resourceObjectNumber <= 0) {
        return false;
    }

    size_t categoryDictStart = 0;
    size_t categoryDictEnd = 0;
    if (FindDirectDictionaryValue(*body, resourcesStart, resourcesEnd, categoryName, &categoryDictStart, &categoryDictEnd)) {
        if (FindPdfKeyTokenInRange(*body, categoryDictStart, categoryDictEnd, resourceName) != std::string::npos) {
            return true;
        }
        body->insert(categoryDictEnd - 2, "/" + resourceName + " " + std::to_string(resourceObjectNumber) + " 0 R ");
        return true;
    }

    int categoryObjectNumber = 0;
    int categoryGeneration = 0;
    size_t categoryValueStart = 0;
    size_t categoryValueEnd = 0;
    if (FindIndirectReferenceValue(
            *body,
            resourcesStart,
            resourcesEnd,
            categoryName,
            &categoryObjectNumber,
            &categoryGeneration,
            &categoryValueStart,
            &categoryValueEnd
    )) {
        const PdfObjectInfo* categoryObject = FindPdfObjectInfoByRef(
                objects,
                categoryObjectNumber,
                categoryGeneration
        );
        if (!categoryObject) {
            WM_LOGE(
                    "Native watermark resource patch failed: indirect %s missing obj=%d gen=%d",
                    categoryName.c_str(),
                    categoryObjectNumber,
                    categoryGeneration
            );
            return false;
        }
        std::string categoryBody = GetCurrentPdfObjectBody(*categoryObject, replacements);
        if (!AddNameReferenceToTopLevelDictionary(
                categoryBody,
                resourceName,
                resourceObjectNumber,
                &categoryBody
        )) {
            return false;
        }
        return UpsertPdfObjectReplacement(
                replacements,
                categoryObjectNumber,
                categoryGeneration,
                categoryBody
        );
    }

    body->insert(
            resourcesEnd - 2,
            "/" + categoryName + " << /" + resourceName + " " + std::to_string(resourceObjectNumber) + " 0 R >> "
    );
    return true;
}

static bool AddPatternToResourceDictionaryRange(
        std::string* body,
        size_t resourcesStart,
        size_t resourcesEnd,
        const std::vector<PdfObjectInfo>& objects,
        const std::string& patternName,
        int patternObjectNumber,
        std::vector<PdfObjectReplacement>* replacements
) {
    if (!body || !replacements || resourcesStart >= resourcesEnd || resourcesEnd > body->size()) return false;

    size_t patternDictStart = 0;
    size_t patternDictEnd = 0;
    if (FindDirectDictionaryValue(*body, resourcesStart, resourcesEnd, "Pattern", &patternDictStart, &patternDictEnd)) {
        if (FindPdfKeyTokenInRange(*body, patternDictStart, patternDictEnd, patternName) != std::string::npos) {
            WM_LOGE(
                    "Native watermark resource patch: pattern already exists name=%s range=%zu-%zu",
                    patternName.c_str(),
                    patternDictStart,
                    patternDictEnd
            );
            return true;
        }
        WM_LOGE(
                "Native watermark resource patch: append to direct pattern name=%s patternObj=%d range=%zu-%zu",
                patternName.c_str(),
                patternObjectNumber,
                patternDictStart,
                patternDictEnd
        );
        body->insert(patternDictEnd - 2, "/" + patternName + " " + std::to_string(patternObjectNumber) + " 0 R ");
        return true;
    }

    int existingPatternObjectNumber = 0;
    int existingPatternGeneration = 0;
    size_t patternValueStart = 0;
    size_t patternValueEnd = 0;
    if (FindIndirectReferenceValue(
            *body,
            resourcesStart,
            resourcesEnd,
            "Pattern",
            &existingPatternObjectNumber,
            &existingPatternGeneration,
            &patternValueStart,
            &patternValueEnd
    )) {
        const PdfObjectInfo* patternObject = FindPdfObjectInfoByRef(
                objects,
                existingPatternObjectNumber,
                existingPatternGeneration
        );
        if (!patternObject) {
            WM_LOGE(
                    "Native watermark resource patch failed: indirect pattern missing obj=%d gen=%d",
                    existingPatternObjectNumber,
                    existingPatternGeneration
            );
            return false;
        }
        WM_LOGE(
                "Native watermark resource patch: append to indirect pattern obj=%d gen=%d name=%s patternObj=%d",
                existingPatternObjectNumber,
                existingPatternGeneration,
                patternName.c_str(),
                patternObjectNumber
        );
        std::string patternBody = GetCurrentPdfObjectBody(*patternObject, replacements);
        if (!AddNameReferenceToTopLevelDictionary(
                patternBody,
                patternName,
                patternObjectNumber,
                &patternBody
        )) {
            return false;
        }
        return UpsertPdfObjectReplacement(
                replacements,
                existingPatternObjectNumber,
                existingPatternGeneration,
                patternBody
        );
    }

    WM_LOGE(
            "Native watermark resource patch: create direct pattern name=%s patternObj=%d resourcesRange=%zu-%zu",
            patternName.c_str(),
            patternObjectNumber,
            resourcesStart,
            resourcesEnd
    );
    body->insert(resourcesEnd - 2, "/Pattern << /" + patternName + " " + std::to_string(patternObjectNumber) + " 0 R >> ");
    return true;
}

static const PdfObjectInfo* FindPdfObjectInfoByRef(
        const std::vector<PdfObjectInfo>& objects,
        int objectNumber,
        int generation
) {
    for (const PdfObjectInfo& object : objects) {
        if (object.objectNumber == objectNumber && object.generation == generation) {
            return &object;
        }
    }
    return nullptr;
}

static PdfObjectReplacement* FindPdfObjectReplacement(
        std::vector<PdfObjectReplacement>* replacements,
        int objectNumber,
        int generation
) {
    if (!replacements) return nullptr;
    for (PdfObjectReplacement& replacement : *replacements) {
        if (replacement.objectNumber == objectNumber && replacement.generation == generation) {
            return &replacement;
        }
    }
    return nullptr;
}

static std::string GetCurrentPdfObjectBody(
        const PdfObjectInfo& object,
        std::vector<PdfObjectReplacement>* replacements
) {
    PdfObjectReplacement* replacement = FindPdfObjectReplacement(replacements, object.objectNumber, object.generation);
    return replacement ? replacement->body : object.body;
}

static bool UpsertPdfObjectReplacement(
        std::vector<PdfObjectReplacement>* replacements,
        int objectNumber,
        int generation,
        const std::string& body
) {
    if (!replacements || objectNumber <= 0 || generation < 0) return false;
    PdfObjectReplacement* existing = FindPdfObjectReplacement(replacements, objectNumber, generation);
    if (existing) {
        existing->body = body;
    } else {
        replacements->push_back({objectNumber, generation, body});
    }
    return true;
}

// Method that adds the pattern into the page resource dictionary using this
static bool AddPatternToPageResources(
        const std::vector<PdfObjectInfo>& objects,
        const std::string& patternName,
        int patternObjectNumber,
        std::string* pageBody,
        std::vector<PdfObjectReplacement>* replacements,
        int depth = 0
) {
    if (!pageBody || !replacements || depth > 8) return false;
    size_t pageDictStart = 0;
    size_t pageDictEnd = 0;
    if (!FindTopLevelPdfDictionary(*pageBody, &pageDictStart, &pageDictEnd)) return false;

    size_t resourcesStart = 0;
    size_t resourcesEnd = 0;
    if (FindDirectDictionaryValue(*pageBody, pageDictStart, pageDictEnd, "Resources", &resourcesStart, &resourcesEnd)) {
        WM_LOGE(
                "Native watermark resource patch: direct page resources depth=%d range=%zu-%zu",
                depth,
                resourcesStart,
                resourcesEnd
        );
        return AddPatternToResourceDictionaryRange(
                pageBody,
                resourcesStart,
                resourcesEnd,
                objects,
                patternName,
                patternObjectNumber,
                replacements
        );
    }

    int resourceObjectNumber = 0;
    int resourceGeneration = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindIndirectReferenceValue(
            *pageBody,
            pageDictStart,
            pageDictEnd,
            "Resources",
            &resourceObjectNumber,
            &resourceGeneration,
             &valueStart,
             &valueEnd
     )) {
        int parentObjectNumber = 0;
        int parentGeneration = 0;
        size_t parentValueStart = 0;
        size_t parentValueEnd = 0;
        if (!FindIndirectReferenceValue(
                *pageBody,
                pageDictStart,
                pageDictEnd,
                "Parent",
                &parentObjectNumber,
                &parentGeneration,
                &parentValueStart,
                &parentValueEnd
        )) {
            WM_LOGE(
                    "Native watermark resource patch: create page resources depth=%d",
                    depth
            );
            pageBody->insert(
                    pageDictEnd - 2,
                    "/Resources << /Pattern << /" + patternName + " " +
                    std::to_string(patternObjectNumber) + " 0 R >> >> "
            );
            return true;
        }
        const PdfObjectInfo* parentObject = FindPdfObjectInfoByRef(objects, parentObjectNumber, parentGeneration);
        if (!parentObject) {
            WM_LOGE(
                    "Native watermark resource patch failed: parent missing obj=%d gen=%d depth=%d",
                    parentObjectNumber,
                    parentGeneration,
                    depth
            );
            return false;
        }
        WM_LOGE(
                "Native watermark resource patch: inherited parent obj=%d gen=%d depth=%d",
                parentObjectNumber,
                parentGeneration,
                depth
        );
        std::string parentBody = GetCurrentPdfObjectBody(*parentObject, replacements);
        if (!AddPatternToPageResources(
                objects,
                patternName,
                patternObjectNumber,
                &parentBody,
                replacements,
                depth + 1
        )) {
            return false;
        }
        return UpsertPdfObjectReplacement(replacements, parentObjectNumber, parentGeneration, parentBody);
    }

    const PdfObjectInfo* resourceObject = FindPdfObjectInfoByRef(objects, resourceObjectNumber, resourceGeneration);
    if (!resourceObject) {
        WM_LOGE(
                "Native watermark resource patch failed: resources object missing obj=%d gen=%d depth=%d",
                resourceObjectNumber,
                resourceGeneration,
                depth
        );
        return false;
    }
    WM_LOGE(
            "Native watermark resource patch: indirect resources obj=%d gen=%d depth=%d",
            resourceObjectNumber,
            resourceGeneration,
            depth
    );
    std::string resourceBody = GetCurrentPdfObjectBody(*resourceObject, replacements);
    size_t resourceDictStart = 0;
    size_t resourceDictEnd = 0;
    if (!FindTopLevelPdfDictionary(resourceBody, &resourceDictStart, &resourceDictEnd) ||
        !AddPatternToResourceDictionaryRange(
                &resourceBody,
                resourceDictStart,
                resourceDictEnd,
                objects,
                patternName,
                patternObjectNumber,
                replacements
        )) {
        return false;
    }
    return UpsertPdfObjectReplacement(replacements, resourceObjectNumber, resourceGeneration, resourceBody);
}

static bool AddSingleWatermarkToResourceDictionaryRange(
        std::string* body,
        size_t resourcesStart,
        size_t resourcesEnd,
        const std::vector<PdfObjectInfo>& objects,
        const std::string& fontName,
        int fontObjectNumber,
        const std::string& graphicsStateName,
        int graphicsStateObjectNumber,
        std::vector<PdfObjectReplacement>* replacements
) {
    if (!AddResourceReferenceToDictionaryRange(
            body,
            resourcesStart,
            resourcesEnd,
            objects,
            "Font",
            fontName,
            fontObjectNumber,
            replacements
    )) {
        return false;
    }

    size_t updatedResourcesStart = 0;
    size_t updatedResourcesEnd = 0;
    if (!FindBalancedPdfDictionary(*body, resourcesStart, body->size(), &updatedResourcesStart, &updatedResourcesEnd)) {
        updatedResourcesStart = resourcesStart;
        updatedResourcesEnd = std::min(resourcesEnd, body->size());
    }
    return AddResourceReferenceToDictionaryRange(
            body,
            updatedResourcesStart,
            updatedResourcesEnd,
            objects,
            "ExtGState",
            graphicsStateName,
            graphicsStateObjectNumber,
            replacements
    );
}

static bool AddSingleWatermarkToPageResources(
        const std::vector<PdfObjectInfo>& objects,
        const std::string& fontName,
        int fontObjectNumber,
        const std::string& graphicsStateName,
        int graphicsStateObjectNumber,
        std::string* pageBody,
        std::vector<PdfObjectReplacement>* replacements,
        int depth = 0
) {
    if (!pageBody || !replacements || depth > 8) return false;
    size_t pageDictStart = 0;
    size_t pageDictEnd = 0;
    if (!FindTopLevelPdfDictionary(*pageBody, &pageDictStart, &pageDictEnd)) return false;

    size_t resourcesStart = 0;
    size_t resourcesEnd = 0;
    if (FindDirectDictionaryValue(*pageBody, pageDictStart, pageDictEnd, "Resources", &resourcesStart, &resourcesEnd)) {
        WM_LOGE("Native watermark single resource patch: direct page resources depth=%d", depth);
        return AddSingleWatermarkToResourceDictionaryRange(
                pageBody,
                resourcesStart,
                resourcesEnd,
                objects,
                fontName,
                fontObjectNumber,
                graphicsStateName,
                graphicsStateObjectNumber,
                replacements
        );
    }

    int resourceObjectNumber = 0;
    int resourceGeneration = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindIndirectReferenceValue(
            *pageBody,
            pageDictStart,
            pageDictEnd,
            "Resources",
            &resourceObjectNumber,
            &resourceGeneration,
            &valueStart,
            &valueEnd
    )) {
        int parentObjectNumber = 0;
        int parentGeneration = 0;
        size_t parentValueStart = 0;
        size_t parentValueEnd = 0;
        if (!FindIndirectReferenceValue(
                *pageBody,
                pageDictStart,
                pageDictEnd,
                "Parent",
                &parentObjectNumber,
                &parentGeneration,
                &parentValueStart,
                &parentValueEnd
        )) {
            pageBody->insert(
                    pageDictEnd - 2,
                    "/Resources << /Font << /" + fontName + " " + std::to_string(fontObjectNumber) +
                    " 0 R >> /ExtGState << /" + graphicsStateName + " " +
                    std::to_string(graphicsStateObjectNumber) + " 0 R >> >> "
            );
            return true;
        }
        const PdfObjectInfo* parentObject = FindPdfObjectInfoByRef(objects, parentObjectNumber, parentGeneration);
        if (!parentObject) return false;
        std::string parentBody = GetCurrentPdfObjectBody(*parentObject, replacements);
        if (!AddSingleWatermarkToPageResources(
                objects,
                fontName,
                fontObjectNumber,
                graphicsStateName,
                graphicsStateObjectNumber,
                &parentBody,
                replacements,
                depth + 1
        )) {
            return false;
        }
        return UpsertPdfObjectReplacement(replacements, parentObjectNumber, parentGeneration, parentBody);
    }

    const PdfObjectInfo* resourceObject = FindPdfObjectInfoByRef(objects, resourceObjectNumber, resourceGeneration);
    if (!resourceObject) return false;
    std::string resourceBody = GetCurrentPdfObjectBody(*resourceObject, replacements);
    size_t resourceDictStart = 0;
    size_t resourceDictEnd = 0;
    if (!FindTopLevelPdfDictionary(resourceBody, &resourceDictStart, &resourceDictEnd) ||
        !AddSingleWatermarkToResourceDictionaryRange(
                &resourceBody,
                resourceDictStart,
                resourceDictEnd,
                objects,
                fontName,
                fontObjectNumber,
                graphicsStateName,
                graphicsStateObjectNumber,
                replacements
        )) {
        return false;
    }
    return UpsertPdfObjectReplacement(replacements, resourceObjectNumber, resourceGeneration, resourceBody);
}

static bool AddRasterWatermarkToResourceDictionaryRange(
        std::string* body,
        size_t resourcesStart,
        size_t resourcesEnd,
        const std::vector<PdfObjectInfo>& objects,
        const std::string& imageName,
        int imageObjectNumber,
        const std::string& graphicsStateName,
        int graphicsStateObjectNumber,
        std::vector<PdfObjectReplacement>* replacements
) {
    if (!AddResourceReferenceToDictionaryRange(
            body,
            resourcesStart,
            resourcesEnd,
            objects,
            "XObject",
            imageName,
            imageObjectNumber,
            replacements
    )) {
        return false;
    }

    size_t updatedResourcesStart = 0;
    size_t updatedResourcesEnd = 0;
    if (!FindBalancedPdfDictionary(*body, resourcesStart, body->size(), &updatedResourcesStart, &updatedResourcesEnd)) {
        updatedResourcesStart = resourcesStart;
        updatedResourcesEnd = std::min(resourcesEnd, body->size());
    }
    return AddResourceReferenceToDictionaryRange(
            body,
            updatedResourcesStart,
            updatedResourcesEnd,
            objects,
            "ExtGState",
            graphicsStateName,
            graphicsStateObjectNumber,
            replacements
    );
}

static bool AddRasterWatermarkToPageResources(
        const std::vector<PdfObjectInfo>& objects,
        const std::string& imageName,
        int imageObjectNumber,
        const std::string& graphicsStateName,
        int graphicsStateObjectNumber,
        std::string* pageBody,
        std::vector<PdfObjectReplacement>* replacements,
        int depth = 0
) {
    if (!pageBody || !replacements || depth > 8) return false;
    size_t pageDictStart = 0;
    size_t pageDictEnd = 0;
    if (!FindTopLevelPdfDictionary(*pageBody, &pageDictStart, &pageDictEnd)) return false;

    size_t resourcesStart = 0;
    size_t resourcesEnd = 0;
    if (FindDirectDictionaryValue(*pageBody, pageDictStart, pageDictEnd, "Resources", &resourcesStart, &resourcesEnd)) {
        return AddRasterWatermarkToResourceDictionaryRange(
                pageBody,
                resourcesStart,
                resourcesEnd,
                objects,
                imageName,
                imageObjectNumber,
                graphicsStateName,
                graphicsStateObjectNumber,
                replacements
        );
    }

    int resourceObjectNumber = 0;
    int resourceGeneration = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindIndirectReferenceValue(
            *pageBody,
            pageDictStart,
            pageDictEnd,
            "Resources",
            &resourceObjectNumber,
            &resourceGeneration,
            &valueStart,
            &valueEnd
    )) {
        int parentObjectNumber = 0;
        int parentGeneration = 0;
        size_t parentValueStart = 0;
        size_t parentValueEnd = 0;
        if (!FindIndirectReferenceValue(
                *pageBody,
                pageDictStart,
                pageDictEnd,
                "Parent",
                &parentObjectNumber,
                &parentGeneration,
                &parentValueStart,
                &parentValueEnd
        )) {
            pageBody->insert(
                    pageDictEnd - 2,
                    "/Resources << /XObject << /" + imageName + " " + std::to_string(imageObjectNumber) +
                    " 0 R >> /ExtGState << /" + graphicsStateName + " " +
                    std::to_string(graphicsStateObjectNumber) + " 0 R >> >> "
            );
            return true;
        }
        const PdfObjectInfo* parentObject = FindPdfObjectInfoByRef(objects, parentObjectNumber, parentGeneration);
        if (!parentObject) return false;
        std::string parentBody = GetCurrentPdfObjectBody(*parentObject, replacements);
        if (!AddRasterWatermarkToPageResources(
                objects,
                imageName,
                imageObjectNumber,
                graphicsStateName,
                graphicsStateObjectNumber,
                &parentBody,
                replacements,
                depth + 1
        )) {
            return false;
        }
        return UpsertPdfObjectReplacement(replacements, parentObjectNumber, parentGeneration, parentBody);
    }

    const PdfObjectInfo* resourceObject = FindPdfObjectInfoByRef(objects, resourceObjectNumber, resourceGeneration);
    if (!resourceObject) return false;
    std::string resourceBody = GetCurrentPdfObjectBody(*resourceObject, replacements);
    size_t resourceDictStart = 0;
    size_t resourceDictEnd = 0;
    if (!FindTopLevelPdfDictionary(resourceBody, &resourceDictStart, &resourceDictEnd) ||
        !AddRasterWatermarkToResourceDictionaryRange(
                &resourceBody,
                resourceDictStart,
                resourceDictEnd,
                objects,
                imageName,
                imageObjectNumber,
                graphicsStateName,
                graphicsStateObjectNumber,
                replacements
        )) {
        return false;
    }
    return UpsertPdfObjectReplacement(replacements, resourceObjectNumber, resourceGeneration, resourceBody);
}


// Method that fill the whole page rectangle using the repeating watermark pattern.
static bool IsToolkitWatermarkContentObject(const PdfObjectInfo& object) {
    return (object.body.find("/Pattern cs") != std::string::npos &&
            object.body.find("/LufickWmP") != std::string::npos) ||
           object.body.find("/LufickWmWrap") != std::string::npos;
}

static std::set<int> CollectToolkitWatermarkContentObjectNumbers(const std::vector<PdfObjectInfo>& objects) {
    std::set<int> objectNumbers;
    for (const PdfObjectInfo& object : objects) {
        if (IsToolkitWatermarkContentObject(object)) {
            objectNumbers.insert(object.objectNumber);
        }
    }
    return objectNumbers;
}

static bool IsToolkitWatermarkContentRef(int objectNumber, const std::set<int>& toolkitContentObjects) {
    return objectNumber > 0 && toolkitContentObjects.find(objectNumber) != toolkitContentObjects.end();
}

static bool RebuildContentsArrayWithoutToolkitWatermarks(
        const std::string& body,
        size_t arrayStart,
        size_t arrayEnd,
        const std::set<int>& toolkitContentObjects,
        int prefixObjectNumber,
        int suffixObjectNumber,
        int contentObjectNumber,
        std::string* outArray
) {
    if (!outArray || arrayStart >= arrayEnd || arrayEnd > body.size()) return false;
    std::vector<std::pair<int, int>> refs;
    size_t cursor = arrayStart + 1;
    while (cursor < arrayEnd) {
        while (cursor < arrayEnd && std::isspace(static_cast<unsigned char>(body[cursor]))) cursor++;
        if (cursor >= arrayEnd) break;

        int objectNumber = 0;
        int generation = 0;
        size_t refEnd = cursor;
        if (!ParseIndirectReferenceAt(body, cursor, arrayEnd, &objectNumber, &generation, &refEnd)) {
            return false;
        }
        if (!IsToolkitWatermarkContentRef(objectNumber, toolkitContentObjects)) {
            refs.push_back({objectNumber, generation});
        }
        cursor = refEnd;
    }

    std::ostringstream rebuilt;
    rebuilt << "[";
    if (!refs.empty() && prefixObjectNumber > 0) {
        rebuilt << " " << prefixObjectNumber << " 0 R";
    }
    for (const auto& ref : refs) {
        rebuilt << " " << ref.first << " " << ref.second << " R";
    }
    if (!refs.empty() && suffixObjectNumber > 0) {
        rebuilt << " " << suffixObjectNumber << " 0 R";
    }
    if (contentObjectNumber > 0) {
        rebuilt << " " << contentObjectNumber << " 0 R";
    }
    rebuilt << " ]";
    *outArray = rebuilt.str();
    return true;
}

static bool RemoveToolkitWatermarkContentFromPageBody(
        const std::string& originalBody,
        std::string* outBody,
        const std::set<int>& toolkitContentObjects
) {
    if (!outBody || toolkitContentObjects.empty()) return false;
    std::string body = originalBody;
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!FindTopLevelPdfDictionary(body, &dictStart, &dictEnd)) return false;

    const size_t contentsKey = FindPdfKeyTokenInRange(body, dictStart, dictEnd, "Contents");
    if (contentsKey == std::string::npos) return false;

    size_t valueStart = contentsKey + strlen("/Contents");
    while (valueStart < dictEnd && std::isspace(static_cast<unsigned char>(body[valueStart]))) {
        valueStart++;
    }
    if (valueStart >= dictEnd) return false;

    if (body[valueStart] == '[') {
        const size_t arrayEnd = body.find(']', valueStart + 1);
        if (arrayEnd == std::string::npos || arrayEnd >= dictEnd) return false;
        std::string rebuiltArray;
        if (!RebuildContentsArrayWithoutToolkitWatermarks(
                body,
                valueStart,
                arrayEnd,
                toolkitContentObjects,
                0,
                0,
                0,
                &rebuiltArray
        )) {
            return false;
        }
        if (rebuiltArray == body.substr(valueStart, arrayEnd - valueStart + 1)) return false;
        body.replace(valueStart, arrayEnd - valueStart + 1, rebuiltArray);
        *outBody = body;
        return true;
    }

    int existingObjectNumber = 0;
    int existingGeneration = 0;
    size_t valueEnd = 0;
    if (ParseIndirectReferenceAt(
            body,
            valueStart,
            dictEnd,
            &existingObjectNumber,
            &existingGeneration,
            &valueEnd
    ) && IsToolkitWatermarkContentRef(existingObjectNumber, toolkitContentObjects)) {
        body.replace(valueStart, valueEnd - valueStart, "[]");
        *outBody = body;
        return true;
    }
    return false;
}

static bool AddWatermarkContentToPageBody(
        const std::string& originalBody,
        const std::vector<PdfObjectInfo>& objects,
        std::vector<PdfObjectReplacement>* replacements,
        int prefixObjectNumber,
        int suffixObjectNumber,
        int contentObjectNumber,
        std::string* outBody,
        const std::set<int>& toolkitContentObjects
) {
    if (!outBody || !replacements) return false;
    std::string body = originalBody;
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!FindTopLevelPdfDictionary(body, &dictStart, &dictEnd)) return false;

    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindPdfDictionaryRawValueSegment(body, dictStart, dictEnd, "Contents", &valueStart, &valueEnd)) {
        WM_LOGE("Native watermark content patch: no contents, inserting contentObj=%d", contentObjectNumber);
        body.insert(dictEnd - 2, "/Contents [ " + std::to_string(contentObjectNumber) + " 0 R ] ");
        *outBody = body;
        return true;
    }

    if (body[valueStart] == '[') {
        WM_LOGE("Native watermark content patch: append to contents array contentObj=%d", contentObjectNumber);
        std::string rebuiltArray;
        if (RebuildContentsArrayWithoutToolkitWatermarks(
                    body,
                    valueStart,
                    valueEnd - 1,
                    toolkitContentObjects,
                    prefixObjectNumber,
                    suffixObjectNumber,
                    contentObjectNumber,
                    &rebuiltArray
        )) {
            body.replace(valueStart, valueEnd - valueStart, rebuiltArray);
        } else {
            std::string existingArray = body.substr(valueStart, valueEnd - valueStart);
            const size_t closeBracket = existingArray.rfind(']');
            if (closeBracket == std::string::npos) return false;
            std::string appendRefs;
            if (suffixObjectNumber > 0) {
                appendRefs += " " + std::to_string(suffixObjectNumber) + " 0 R";
            }
            appendRefs += " " + std::to_string(contentObjectNumber) + " 0 R";
            existingArray.insert(closeBracket, appendRefs);
            if (prefixObjectNumber > 0) {
                existingArray.insert(1, " " + std::to_string(prefixObjectNumber) + " 0 R");
            }
            body.replace(valueStart, valueEnd - valueStart, existingArray);
        }
        *outBody = body;
        return true;
    }

    int existingObjectNumber = 0;
    int existingGeneration = 0;
    size_t refEnd = 0;
    if (ParseIndirectReferenceAt(
            body,
            valueStart,
            dictEnd,
            &existingObjectNumber,
            &existingGeneration,
            &refEnd
    )) {
        const PdfObjectInfo* existingContentsObject = FindPdfObjectInfoByRef(
                objects,
                existingObjectNumber,
                existingGeneration
        );
        if (existingContentsObject) {
            std::string contentsBody = GetCurrentPdfObjectBody(*existingContentsObject, replacements);
            size_t contentsArrayStart = 0;
            while (contentsArrayStart < contentsBody.size() &&
                   std::isspace(static_cast<unsigned char>(contentsBody[contentsArrayStart]))) {
                contentsArrayStart++;
            }
            if (contentsArrayStart < contentsBody.size() && contentsBody[contentsArrayStart] == '[') {
                size_t contentsArrayEnd = contentsBody.find(']', contentsArrayStart + 1);
                if (contentsArrayEnd != std::string::npos) {
                    WM_LOGE(
                            "Native watermark content patch: append to indirect contents array obj=%d gen=%d contentObj=%d",
                            existingObjectNumber,
                            existingGeneration,
                            contentObjectNumber
                    );
                    std::string rebuiltArray;
                    if (RebuildContentsArrayWithoutToolkitWatermarks(
                            contentsBody,
                            contentsArrayStart,
                            contentsArrayEnd,
                            toolkitContentObjects,
                            prefixObjectNumber,
                            suffixObjectNumber,
                            contentObjectNumber,
                            &rebuiltArray
                    )) {
                        contentsBody.replace(contentsArrayStart, contentsArrayEnd - contentsArrayStart + 1, rebuiltArray);
                    } else {
                        std::string appendRefs;
                        if (suffixObjectNumber > 0) {
                            appendRefs += " " + std::to_string(suffixObjectNumber) + " 0 R";
                        }
                        appendRefs += " " + std::to_string(contentObjectNumber) + " 0 R";
                        contentsBody.insert(contentsArrayEnd, appendRefs);
                        if (prefixObjectNumber > 0) {
                            contentsBody.insert(contentsArrayStart + 1, " " + std::to_string(prefixObjectNumber) + " 0 R");
                        }
                    }
                    if (!UpsertPdfObjectReplacement(
                            replacements,
                            existingObjectNumber,
                            existingGeneration,
                            contentsBody
                    )) {
                        return false;
                    }
                    *outBody = body;
                    return true;
                }
            }
        }
        WM_LOGE(
                "Native watermark content patch: convert contents ref %d %d to array with contentObj=%d",
                existingObjectNumber,
                existingGeneration,
                contentObjectNumber
        );
        const bool existingIsToolkitWatermark = IsToolkitWatermarkContentRef(existingObjectNumber, toolkitContentObjects);
        const std::string replacement = existingIsToolkitWatermark
                                        ? std::to_string(contentObjectNumber) + " 0 R"
                                        : "[ " +
                                          (prefixObjectNumber > 0 ? std::to_string(prefixObjectNumber) + " 0 R " : "") +
                                          std::to_string(existingObjectNumber) + " " + std::to_string(existingGeneration) +
                                          " R " +
                                          (suffixObjectNumber > 0 ? std::to_string(suffixObjectNumber) + " 0 R " : "") +
                                          std::to_string(contentObjectNumber) + " 0 R ]";
        body.replace(valueStart, refEnd - valueStart, replacement);
        *outBody = body;
        return true;
    }

    std::string existingValue = body.substr(valueStart, valueEnd - valueStart);
    if (existingValue.empty()) return false;
    const std::string replacement = "[ " +
                                    (prefixObjectNumber > 0 ? std::to_string(prefixObjectNumber) + " 0 R " : "") +
                                    existingValue + " " +
                                    (suffixObjectNumber > 0 ? std::to_string(suffixObjectNumber) + " 0 R " : "") +
                                    std::to_string(contentObjectNumber) + " 0 R ]";
    body.replace(valueStart, valueEnd - valueStart, replacement);
    *outBody = body;
    return true;
}

struct RawPdfWatermarkPatternBinding {
    int objectNumber = 0;
    int imageObjectNumber = 0;
    int imageSmaskObjectNumber = 0;
    std::string patternName;
    std::string graphicsStateName;
    std::string imageName;
};

static std::string BuildRawPdfWatermarkPatternKey(const RawPdfWatermarkSpec& spec) {
    std::ostringstream key;
    key << (spec.isRepeated ? "REPEATED" : "SINGLE") << '|'
        << FormatPdfFloat(spec.pageWidth) << 'x' << FormatPdfFloat(spec.pageHeight) << '|'
        << spec.text << '|'
        << spec.fontName << '|'
        << spec.fontPath << '|'
        << std::hash<std::string>{}(spec.textPathData) << '|'
        << spec.textR << ',' << spec.textG << ',' << spec.textB << '|'
        << FormatPdfFloat(spec.fontSize) << '|'
        << FormatPdfFloat(spec.patternWidth) << 'x' << FormatPdfFloat(spec.patternHeight) << '|'
        << FormatPdfFloat(spec.contentWidth) << 'x' << FormatPdfFloat(spec.contentHeight) << '|'
        << FormatPdfFloat(spec.repeatStepWidth) << 'x' << FormatPdfFloat(spec.repeatStepHeight) << '|'
        << FormatPdfFloat(spec.baselineX) << ',' << FormatPdfFloat(spec.baselineY) << '|'
        << FormatPdfFloat(spec.rotation) << '|'
        << FormatPdfFloat(spec.opacity) << '|'
        << FormatPdfFloat(spec.characterSpacing) << '|'
        << (spec.isBold ? "b" : "r") << (spec.isItalic ? "i" : "n")
        << (spec.isUnderline ? "u" : "n") << (spec.isStrikeout ? "s" : "n")
        << (spec.isRasterImage ? "raster" : (spec.isIconImage ? "icon" : "text")) << '|'
        << spec.imagePath << '|'
        << spec.imagePixelWidth << 'x' << spec.imagePixelHeight;
    return key.str();
}

// Method that reads the saved PDF as bytes/text and scans PDF objects
static bool PatchRawPdfWatermarkPatterns(JNIEnv* env, const char* outputPath, const std::vector<RawPdfWatermarkSpec>& specs) {
    if (!outputPath) return false;

    std::string data;
    if (!ReadFileToString(outputPath, &data)) {
        WM_LOGE("Native watermark compact patch failed: unable to read output PDF");
        return false;
    }

    std::vector<PdfObjectInfo> objects = ScanPdfObjects(data);
    if (objects.empty()) {
        WM_LOGE("Native watermark compact patch failed: no plain PDF objects found, bytes=%zu", data.size());
        return false;
    }

    const std::vector<PdfObjectInfo> latestObjects = BuildLatestPdfObjectsByRef(objects);
    const std::set<int> toolkitWatermarkContentObjects = CollectToolkitWatermarkContentObjectNumbers(latestObjects);
    WM_LOGE(
            "Native watermark compact patch found %zu previous toolkit watermark content stream(s)",
            toolkitWatermarkContentObjects.size()
    );
    int maxSpecPageIndex = -1;
    for (const RawPdfWatermarkSpec& spec : specs) {
        maxSpecPageIndex = std::max(maxSpecPageIndex, spec.pageIndex);
    }
    std::vector<PdfObjectInfo> pages = BuildPdfPageObjectsFromCatalog(data, latestObjects);
    bool usedCatalogPageTree = !pages.empty();
    if (pages.empty()) {
        pages = BuildLatestPdfPageObjects(objects, latestObjects);
        usedCatalogPageTree = false;
    } else if (maxSpecPageIndex >= 0 && static_cast<size_t>(maxSpecPageIndex) >= pages.size()) {
        const std::vector<PdfObjectInfo> scannedPages = BuildLatestPdfPageObjects(objects, latestObjects);
        WM_LOGE(
                "Native watermark page discovery incomplete: catalogPages=%zu scannedPages=%zu maxSpecPage=%d",
                pages.size(),
                scannedPages.size(),
                maxSpecPageIndex
        );
        if (scannedPages.size() > pages.size() && static_cast<size_t>(maxSpecPageIndex) < scannedPages.size()) {
            pages = scannedPages;
            usedCatalogPageTree = false;
            WM_LOGE("Native watermark page discovery switched to scanned page objects");
        }
    }
    if (pages.empty()) {
        WM_LOGE("Native watermark compact patch failed: no plain page dictionaries found, objects=%zu, bytes=%zu", objects.size(), data.size());
        return false;
    }
    WM_LOGE(
            "Native watermark compact patch start: specs=%zu objects=%zu latestObjects=%zu pages=%zu pageTree=%d nextObj=%d",
            specs.size(),
            objects.size(),
            latestObjects.size(),
            pages.size(),
            usedCatalogPageTree ? 1 : 0,
            GetMaxPdfObjectNumber(objects) + 1
    );

    std::vector<PdfObjectReplacement> replacements;
    int nextObjectNumber = GetMaxPdfObjectNumber(objects) + 1;
    std::map<std::string, RawPdfWatermarkPatternBinding> patternBindings;
    int patchedCount = 0;

    if (specs.empty()) {
        for (const PdfObjectInfo& page : pages) {
            std::string pageBody = GetCurrentPdfObjectBody(page, &replacements);
            if (RemoveToolkitWatermarkContentFromPageBody(
                    pageBody,
                    &pageBody,
                    toolkitWatermarkContentObjects
            )) {
                if (!UpsertPdfObjectReplacement(&replacements, page.objectNumber, page.generation, pageBody)) {
                    return false;
                }
                patchedCount++;
            }
        }
    }
    for (size_t specIndex = 0; specIndex < specs.size(); specIndex++) {
        const RawPdfWatermarkSpec& spec = specs[specIndex];
        if (spec.pageIndex < 0 || static_cast<size_t>(spec.pageIndex) >= pages.size() ||
            (!spec.isRasterImage && spec.text.empty()) || spec.patternWidth <= 0.0f || spec.patternHeight <= 0.0f ||
            (spec.isRasterImage && spec.imagePath.empty())) {
            WM_LOGE(
                    "Native watermark compact patch skip spec=%zu page=%d pages=%zu textLen=%zu raster=%d imagePathLen=%zu pattern=%fx%f",
                    specIndex,
                    spec.pageIndex,
                    pages.size(),
                    spec.text.size(),
                    spec.isRasterImage ? 1 : 0,
                    spec.imagePath.size(),
                    spec.patternWidth,
                    spec.patternHeight
            );
            continue;
        }

        const int prefixContentObjectNumber = nextObjectNumber++;
        const int suffixContentObjectNumber = nextObjectNumber++;
        const int contentObjectNumber = nextObjectNumber++;
        RawPdfWatermarkPatternBinding binding;
        std::string contentStream;
        int fontObjectNumber = 0;
        int graphicsStateObjectNumber = 0;
        int imageObjectNumber = 0;
        int imageSmaskObjectNumber = 0;
        std::string fontName;
        std::string imageName;
        const std::string patternKey = BuildRawPdfWatermarkPatternKey(spec);
        const auto existingPattern = patternBindings.find(patternKey);
        if (existingPattern != patternBindings.end()) {
            binding = existingPattern->second;
        } else {
            const int patternObjectNumber = nextObjectNumber++;
            const int patternIndex = static_cast<int>(patternBindings.size()) + 1;
            binding.objectNumber = patternObjectNumber;
            const int resourceNameIndex = nextObjectNumber + patternIndex;
            binding.patternName = MakePdfResourceName("LufickWmP", resourceNameIndex);
            binding.graphicsStateName = MakePdfResourceName("LufickWmGS", resourceNameIndex);
            if (spec.isRasterImage) {
                binding.imageObjectNumber = nextObjectNumber++;
                binding.imageSmaskObjectNumber = nextObjectNumber++;
                binding.imageName = MakePdfResourceName("LufickWmIm", resourceNameIndex);
            }
            patternBindings[patternKey] = binding;
        }
        contentStream = BuildPdfWatermarkContentStream(spec, binding.patternName);
        imageObjectNumber = binding.imageObjectNumber;
        imageSmaskObjectNumber = binding.imageSmaskObjectNumber;
        imageName = binding.imageName;

        const PdfObjectInfo& page = pages[spec.pageIndex];
        WM_LOGE(
                "Native watermark compact patch page spec=%zu mode=%s page=%d obj=%d gen=%d patternObj=%d fontObj=%d imageObj=%d gsObj=%d contentObj=%d textLen=%zu raster=%d",
                specIndex,
                spec.isRepeated ? "REPEATED" : "SINGLE",
                spec.pageIndex,
                page.objectNumber,
                page.generation,
                binding.objectNumber,
                fontObjectNumber,
                imageObjectNumber,
                graphicsStateObjectNumber,
                contentObjectNumber,
                spec.text.size(),
                spec.isRasterImage ? 1 : 0
        );
        std::string pageBody = page.body;
        int existingPageReplacementIndex = -1;
        for (size_t replacementIndex = 0; replacementIndex < replacements.size(); replacementIndex++) {
            if (replacements[replacementIndex].objectNumber == page.objectNumber &&
                replacements[replacementIndex].generation == page.generation) {
                pageBody = replacements[replacementIndex].body;
                existingPageReplacementIndex = static_cast<int>(replacementIndex);
                break;
            }
        }
        size_t pageDictStart = 0;
        size_t pageDictEnd = 0;
        if (FindTopLevelPdfDictionary(pageBody, &pageDictStart, &pageDictEnd)) {
            const bool hasAnnots = FindPdfKeyTokenInRange(pageBody, pageDictStart, pageDictEnd, "Annots") != std::string::npos;
            WM_LOGE(
                    "Native watermark page diagnostics: page=%d obj=%d gen=%d bodyLen=%zu existingReplacement=%d annots=%d resources=%s contents=%s",
                    spec.pageIndex,
                    page.objectNumber,
                    page.generation,
                    pageBody.size(),
                    existingPageReplacementIndex >= 0 ? 1 : 0,
                    hasAnnots ? 1 : 0,
                    DescribePdfDictionaryValueForLog(pageBody, pageDictStart, pageDictEnd, "Resources").c_str(),
                    DescribePdfDictionaryValueForLog(pageBody, pageDictStart, pageDictEnd, "Contents").c_str()
            );
        } else {
            WM_LOGE(
                    "Native watermark page diagnostics failed: no page dictionary page=%d obj=%d gen=%d bodyLen=%zu",
                    spec.pageIndex,
                    page.objectNumber,
                    page.generation,
                    pageBody.size()
            );
        }
        const bool resourcesAdded = AddPatternToPageResources(
                latestObjects,
                binding.patternName,
                binding.objectNumber,
                &pageBody,
                &replacements
        );
        if (!resourcesAdded) {
            WM_LOGE(
                    "Unable to add native watermark resources for page %d mode=%s obj=%d gen=%d bodyLen=%zu",
                    spec.pageIndex,
                    spec.isRepeated ? "REPEATED" : "SINGLE",
                    page.objectNumber,
                    page.generation,
                    pageBody.size()
            );
            continue;
        }
        if (!AddWatermarkContentToPageBody(
                pageBody,
                latestObjects,
                &replacements,
                prefixContentObjectNumber,
                suffixContentObjectNumber,
                contentObjectNumber,
                &pageBody,
                {}
        )) {
            WM_LOGE(
                    "Unable to add native watermark content stream for page %d obj=%d gen=%d bodyLen=%zu",
                    spec.pageIndex,
                    page.objectNumber,
                    page.generation,
                    pageBody.size()
            );
            continue;
        }
        if (!FindPdfObjectReplacement(&replacements, binding.objectNumber, 0)) {
            if (spec.isRasterImage && !FindPdfObjectReplacement(&replacements, binding.imageObjectNumber, 0)) {
                if (!BuildRasterWatermarkImageReplacements(
                        env,
                        spec,
                        binding.imageObjectNumber,
                        binding.imageSmaskObjectNumber,
                        &replacements
                )) {
                    WM_LOGE("Native watermark raster image object build failed: page=%d path=%s", spec.pageIndex, spec.imagePath.c_str());
                    continue;
                }
            }
            replacements.push_back({
                    binding.objectNumber,
                    0,
                    spec.isRepeated
                    ? BuildPdfWatermarkPatternObjectBody(
                            spec,
                            binding.graphicsStateName,
                            binding.imageName,
                            binding.imageObjectNumber
                    )
                    : BuildPdfSingleWatermarkPatternObjectBody(
                            spec,
                            binding.graphicsStateName,
                            binding.imageName,
                            binding.imageObjectNumber
                    )
            });
        }
        replacements.push_back({
                prefixContentObjectNumber,
                0,
                "<< /Length 1 /LufickWmWrap true >>\nstream\nq\nendstream"
        });
        replacements.push_back({
                suffixContentObjectNumber,
                0,
                "<< /Length 1 /LufickWmWrap true >>\nstream\nQ\nendstream"
        });
        replacements.push_back({
                contentObjectNumber,
                0,
                "<< /Length " + std::to_string(contentStream.size()) + " >>\nstream\n" +
                contentStream + "\nendstream"
        });
        if (existingPageReplacementIndex >= 0) {
            replacements[existingPageReplacementIndex].body = pageBody;
        } else if (!UpsertPdfObjectReplacement(&replacements, page.objectNumber, page.generation, pageBody)) {
            return false;
        }
        WM_LOGE(
                "Native watermark patched page result: page=%d obj=%d gen=%d replacements=%zu pageBodyLen=%zu",
                spec.pageIndex,
                page.objectNumber,
                page.generation,
                replacements.size(),
                pageBody.size()
        );
        patchedCount++;
    }

    if (patchedCount <= 0) {
        WM_LOGE("Native watermark compact patch failed: no page was patched, specs=%zu pages=%zu objects=%zu", specs.size(), pages.size(), objects.size());
        return false;
    }
    const size_t replacementLogCount = std::min<size_t>(replacements.size(), 24);
    for (size_t index = 0; index < replacementLogCount; index++) {
        WM_LOGE(
                "Native watermark replacement diagnostics: index=%zu obj=%d gen=%d bodyLen=%zu",
                index,
                replacements[index].objectNumber,
                replacements[index].generation,
                replacements[index].body.size()
        );
    }
    if (replacements.size() > replacementLogCount) {
        WM_LOGE(
                "Native watermark replacement diagnostics: truncated total=%zu logged=%zu",
                replacements.size(),
                replacementLogCount
        );
    }
    if (!AppendIncrementalPdfObjectUpdates(&data, &replacements)) {
        WM_LOGE("Native watermark compact patch failed: unable to append incremental PDF updates, replacements=%zu patched=%d", replacements.size(), patchedCount);
        return false;
    }
    if (!WriteStringToFile(outputPath, data)) {
        WM_LOGE("Native watermark compact patch failed: unable to write patched PDF");
        return false;
    }
    WM_LOGE("Patched %d compact native PDF watermark pattern(s)", patchedCount);
    return true;
}

static bool PatchSavedPdfShapeNativeDictionaries(const char* outputPath) {
    if (!outputPath) return false;

    std::ifstream input(outputPath, std::ios::binary);
    if (!input) return false;
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (data.empty()) return false;

    std::vector<PdfObjectReplacement> objectReplacements;
    const std::string padKey = "/LufickPdfShapePatchPad";
    size_t searchPos = 0;
    int patchedCount = 0;
    while (true) {
        const size_t padPos = data.find(padKey, searchPos);
        if (padPos == std::string::npos) break;

        const size_t objKeyword = data.rfind(" obj", padPos);
        const size_t endObj = data.find("endobj", padPos);
        if (objKeyword == std::string::npos || endObj == std::string::npos) {
            searchPos = padPos + padKey.size();
            continue;
        }

        size_t objectStart = data.rfind('\n', objKeyword);
        objectStart = (objectStart == std::string::npos) ? 0 : objectStart + 1;
        const size_t objectEnd = endObj + strlen("endobj");
        if (objectEnd <= objectStart || objectEnd > data.size()) {
            searchPos = padPos + padKey.size();
            continue;
        }

        size_t patchStart = objectStart;
        size_t patchEnd = objectEnd;
        FindEnclosingPdfDictionaryRange(data, objectStart, objectEnd, padPos, &patchStart, &patchEnd);

        const std::string objectText = data.substr(patchStart, patchEnd - patchStart);
        const int typeInt = GetPdfShapePatchTypeFromObject(objectText);
        if (!NeedsSavedPdfShapeDictionaryPatch(typeInt)) {
            searchPos = padPos + padKey.size();
            continue;
        }

        FS_RECTF rect;
        if (!ParsePdfRectFromObject(objectText, &rect)) {
            searchPos = padPos + padKey.size();
            continue;
        }

        if (!ReplacePdfNameInRange(
                &data,
                patchStart,
                patchEnd,
                "Subtype",
                GetPdfShapePatchedSubtypeName(typeInt)
        )) {
            searchPos = padPos + padKey.size();
            continue;
        }

        BlankPdfDictionaryValueSegment(&data, patchStart, patchEnd, "QuadPoints");
        if (ParsePdfShapeOpacityFromObject(objectText) >= 0.0f) {
            BlankPdfDictionaryValueSegment(&data, patchStart, patchEnd, "CA");
        }

        size_t padStart = 0;
        size_t padEnd = 0;
        if (!FindPdfDictionaryValueSegment(
                data,
                patchStart,
                patchEnd,
                "LufickPdfShapePatchPad",
                &padStart,
                &padEnd
        )) {
            searchPos = padPos + padKey.size();
            continue;
        }

        std::string nativePatch = BuildNativePdfShapeDictionaryPatch(typeInt, rect, objectText);
        const size_t padLength = padEnd - padStart;
        if (nativePatch.empty() || nativePatch.size() > padLength) {
            searchPos = padPos + padKey.size();
            continue;
        }
        nativePatch.append(padLength - nativePatch.size(), ' ');
        data.replace(padStart, padLength, nativePatch);
        patchedCount++;
        searchPos = patchEnd;
    }

    const std::string boxShapePatchKey = "/LufickPdfBoxShapePatch";
    searchPos = 0;
    while (true) {
        const size_t markerPos = data.find(boxShapePatchKey, searchPos);
        if (markerPos == std::string::npos) break;

        const size_t objKeyword = data.rfind(" obj", markerPos);
        const size_t endObj = data.find("endobj", markerPos);
        if (objKeyword == std::string::npos || endObj == std::string::npos) {
            searchPos = markerPos + boxShapePatchKey.size();
            continue;
        }

        size_t objectStart = data.rfind('\n', objKeyword);
        objectStart = (objectStart == std::string::npos) ? 0 : objectStart + 1;
        const size_t objectEnd = endObj + strlen("endobj");
        if (objectEnd <= objectStart || objectEnd > data.size()) {
            searchPos = markerPos + boxShapePatchKey.size();
            continue;
        }

        size_t patchStart = objectStart;
        size_t patchEnd = objectEnd;
        FindEnclosingPdfDictionaryRange(data, objectStart, objectEnd, markerPos, &patchStart, &patchEnd);

        const std::string objectText = data.substr(patchStart, patchEnd - patchStart);
        const int typeInt = GetPdfBoxShapePatchTypeFromObject(objectText);
        if (!IsDirectNativePdfBoxShape(typeInt)) {
            searchPos = markerPos + boxShapePatchKey.size();
            continue;
        }

        const float opacity = ResolvePdfBoxShapeOpacity(objectText);
        if (opacity < 0.0f) {
            searchPos = markerPos + boxShapePatchKey.size();
            continue;
        }

        size_t padStart = 0;
        size_t padEnd = 0;
        if (FindPdfDictionaryValueSegment(
                data,
                patchStart,
                patchEnd,
                "LufickPdfBoxShapeOpacityPad",
                &padStart,
                &padEnd
        )) {
            BlankPdfDictionaryValueSegment(&data, patchStart, patchEnd, "CA");

            std::string opacityPatch = "/CA " + FormatPdfFloat(opacity) + " ";
            const size_t padLength = padEnd - padStart;
            if (opacityPatch.size() <= padLength) {
                opacityPatch.append(padLength - opacityPatch.size(), ' ');
                data.replace(padStart, padLength, opacityPatch);
                patchedCount++;
            }
        }

        int appearanceObjectNumber = -1;
        int appearanceGeneration = 0;
        std::string appearanceObjectBody;
        if (ParsePdfNormalAppearanceReference(objectText, &appearanceObjectNumber, &appearanceGeneration) &&
            BuildPdfBoxShapeAppearanceReplacementObject(
                    data,
                    appearanceObjectNumber,
                    appearanceGeneration,
                    objectText,
                    typeInt,
                    opacity,
                    &appearanceObjectBody
            )) {
            objectReplacements.push_back({
                    appearanceObjectNumber,
                    appearanceGeneration,
                    appearanceObjectBody
            });
        }
        searchPos = patchEnd;
    }

    if (!objectReplacements.empty() && !AppendIncrementalPdfObjectUpdates(&data, &objectReplacements)) {
        return false;
    }

    if (patchedCount <= 0 && objectReplacements.empty()) return true;

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    LOGE("Patched %d PDF shape annotation dictionaries and %zu appearance streams", patchedCount, objectReplacements.size());
    return true;
}

static std::u16string BuildStickyNoteAppearanceStream(
        const FS_RECTF& rect,
        const std::string& iconKey,
        int r,
        int g,
        int b
) {
    const float width = rect.right - rect.left;
    const float height = rect.top - rect.bottom;
    if (width <= 0.1f || height <= 0.1f) return std::u16string();

    const std::string resolvedIconKey = iconKey.empty() ? "comment" : iconKey;
    const float strokeWidth = std::max(std::min(width, height) * 0.12f, 1.5f);
    const float red = std::max(0.0f, std::min(r / 255.0f, 1.0f));
    const float green = std::max(0.0f, std::min(g / 255.0f, 1.0f));
    const float blue = std::max(0.0f, std::min(b / 255.0f, 1.0f));
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(3);
    stream << "q 1 J 1 j " << strokeWidth << " w "
           << red << ' ' << green << ' ' << blue << " RG "
           << red << ' ' << green << ' ' << blue << " rg ";

    const auto x = [&](float fraction) { return rect.left + (width * fraction); };
    const auto y = [&](float fractionFromTop) { return rect.top - (height * fractionFromTop); };

    if (resolvedIconKey == "right_pointer") {
        stream << x(0.14f) << ' ' << y(0.12f) << " m "
               << x(0.84f) << ' ' << y(0.50f) << " l "
               << x(0.14f) << ' ' << y(0.88f) << " l "
               << x(0.38f) << ' ' << y(0.50f) << " l h f ";
    } else if (resolvedIconKey == "arrow_right") {
        stream << x(0.12f) << ' ' << y(0.50f) << " m "
               << x(0.84f) << ' ' << y(0.50f) << " l "
               << x(0.62f) << ' ' << y(0.22f) << " m "
               << x(0.84f) << ' ' << y(0.50f) << " l "
               << x(0.62f) << ' ' << y(0.78f) << " m "
               << x(0.84f) << ' ' << y(0.50f) << " l S ";
    } else if (resolvedIconKey == "check") {
        stream << x(0.16f) << ' ' << y(0.58f) << " m "
               << x(0.38f) << ' ' << y(0.82f) << " l "
               << x(0.86f) << ' ' << y(0.18f) << " l S ";
    } else if (resolvedIconKey == "circle") {
        AppendPdfCirclePath(stream, x(0.50f), y(0.50f), std::min(width, height) * 0.28f);
        stream << "S ";
    } else if (resolvedIconKey == "cross") {
        stream << x(0.18f) << ' ' << y(0.18f) << " m "
               << x(0.82f) << ' ' << y(0.82f) << " l "
               << x(0.82f) << ' ' << y(0.18f) << " m "
               << x(0.18f) << ' ' << y(0.82f) << " l S ";
    } else if (resolvedIconKey == "flag") {
        stream << x(0.22f) << ' ' << y(0.14f) << " m "
               << x(0.22f) << ' ' << y(0.84f) << " l S "
               << x(0.22f) << ' ' << y(0.14f) << " m "
               << x(0.84f) << ' ' << y(0.24f) << " l "
               << x(0.22f) << ' ' << y(0.52f) << " l h f ";
    } else if (resolvedIconKey == "comment") {
        const float bubbleLeft = x(0.10f);
        const float bubbleRight = x(0.88f);
        const float bubbleTop = y(0.14f);
        const float bubbleBottom = y(0.74f);
        const float bubbleRadius = std::min(width, height) * 0.12f;
        AppendPdfRoundedRectPath(stream, bubbleLeft, bubbleBottom, bubbleRight, bubbleTop, bubbleRadius);
        stream << "S "
               << x(0.28f) << ' ' << bubbleBottom << " m "
               << x(0.22f) << ' ' << y(0.91f) << " l "
               << x(0.46f) << ' ' << bubbleBottom << " l S "
               << (strokeWidth * 0.72f) << " w "
               << x(0.25f) << ' ' << y(0.34f) << " m "
               << x(0.73f) << ' ' << y(0.34f) << " l "
               << x(0.25f) << ' ' << y(0.52f) << " m "
               << x(0.65f) << ' ' << y(0.52f) << " l S ";
    } else if (resolvedIconKey == "help") {
        stream << x(0.32f) << ' ' << y(0.28f) << " m "
               << x(0.32f) << ' ' << y(0.18f) << ' '
               << x(0.42f) << ' ' << y(0.12f) << ' '
               << x(0.53f) << ' ' << y(0.12f) << " c "
               << x(0.67f) << ' ' << y(0.12f) << ' '
               << x(0.77f) << ' ' << y(0.22f) << ' '
               << x(0.77f) << ' ' << y(0.33f) << " c "
               << x(0.77f) << ' ' << y(0.44f) << ' '
               << x(0.70f) << ' ' << y(0.50f) << ' '
               << x(0.61f) << ' ' << y(0.56f) << " c "
               << x(0.54f) << ' ' << y(0.60f) << ' '
               << x(0.50f) << ' ' << y(0.64f) << ' '
               << x(0.50f) << ' ' << y(0.71f) << " c "
               << x(0.50f) << ' ' << y(0.76f) << " l S ";
        AppendPdfCirclePath(stream, x(0.50f), y(0.86f), std::min(width, height) * 0.06f);
        stream << "f ";
    } else if (resolvedIconKey == "star") {
        const float cx = x(0.50f);
        const float cy = y(0.50f);
        const float outer = std::min(width, height) * 0.34f;
        const float inner = outer * 0.45f;
        const double pi = 3.14159265358979323846;
        for (int index = 0; index < 10; index++) {
            const double angle = (pi / 2.0) + ((pi / 5.0) * index);
            const float radius = (index % 2 == 0) ? outer : inner;
            const float pointX = cx + static_cast<float>(std::cos(angle) * radius);
            const float pointY = cy + static_cast<float>(std::sin(angle) * radius);
            stream << pointX << ' ' << pointY << (index == 0 ? " m " : " l ");
        }
        stream << "h f ";
    } else {
        const float bubbleLeft = x(0.16f);
        const float bubbleRight = x(0.84f);
        const float bubbleTop = y(0.16f);
        const float bubbleBottom = y(0.80f);
        const float bubbleRadius = (bubbleTop - bubbleBottom) * 0.22f;
        AppendPdfRoundedRectPath(stream, bubbleLeft, bubbleBottom, bubbleRight, bubbleTop, bubbleRadius);
        stream << "f ";
    }

    stream << "Q";
    return AsciiToUtf16(stream.str());
}

static std::u16string BuildFileAttachmentAppearanceStream(
        const FS_RECTF& rect,
        const std::string& iconName,
        int red,
        int green,
        int blue
) {
    const float width = rect.right - rect.left;
    const float height = rect.top - rect.bottom;
    if (width <= 0.1f || height <= 0.1f) return std::u16string();

    const auto x = [&](float fraction) { return rect.left + (width * fraction); };
    const auto y = [&](float fractionFromTop) { return rect.top - (height * fractionFromTop); };
    const float strokeWidth = std::max(std::min(width, height) * 0.075f, 1.2f);

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(3);
    stream << "q "
           << (std::max(0, std::min(red, 255)) / 255.0f) << ' '
           << (std::max(0, std::min(green, 255)) / 255.0f) << ' '
           << (std::max(0, std::min(blue, 255)) / 255.0f) << " RG "
           << (std::max(0, std::min(red, 255)) / 255.0f) << ' '
           << (std::max(0, std::min(green, 255)) / 255.0f) << ' '
           << (std::max(0, std::min(blue, 255)) / 255.0f) << " rg "
           << "1 J 1 j " << strokeWidth << " w ";
    if (iconName == "Graph") {
        stream << x(0.22f) << ' ' << y(0.78f) << ' ' << width * 0.14f << ' ' << height * 0.21f << " re f "
               << x(0.43f) << ' ' << y(0.78f) << ' ' << width * 0.14f << ' ' << height * 0.39f << " re f "
               << x(0.64f) << ' ' << y(0.78f) << ' ' << width * 0.14f << ' ' << height * 0.55f << " re f ";
    } else if (iconName == "PushPin") {
        stream << x(0.34f) << ' ' << y(0.25f) << " m "
               << x(0.66f) << ' ' << y(0.25f) << " l "
               << x(0.61f) << ' ' << y(0.49f) << " l "
               << x(0.73f) << ' ' << y(0.63f) << " l "
               << x(0.54f) << ' ' << y(0.63f) << " l "
               << x(0.50f) << ' ' << y(0.84f) << " l "
               << x(0.46f) << ' ' << y(0.63f) << " l "
               << x(0.27f) << ' ' << y(0.63f) << " l "
               << x(0.39f) << ' ' << y(0.49f) << " l h S ";
    } else if (iconName == "Tag") {
        stream << x(0.22f) << ' ' << y(0.31f) << " m "
               << x(0.55f) << ' ' << y(0.20f) << " l "
               << x(0.80f) << ' ' << y(0.45f) << " l "
               << x(0.47f) << ' ' << y(0.79f) << " l "
               << x(0.20f) << ' ' << y(0.52f) << " l h S ";
        const float holeRadius = std::min(width, height) * 0.055f;
        AppendPdfCirclePath(stream, x(0.39f), y(0.39f), holeRadius);
        stream << "S ";
    } else {
        stream << x(0.62f) << ' ' << y(0.18f) << " m "
               << x(0.34f) << ' ' << y(0.61f) << " l "
               << x(0.25f) << ' ' << y(0.76f) << ' '
               << x(0.30f) << ' ' << y(0.88f) << ' '
               << x(0.43f) << ' ' << y(0.88f) << " c "
               << x(0.52f) << ' ' << y(0.88f) << ' '
               << x(0.58f) << ' ' << y(0.81f) << ' '
               << x(0.64f) << ' ' << y(0.71f) << " c "
               << x(0.80f) << ' ' << y(0.45f) << " l "
               << x(0.86f) << ' ' << y(0.34f) << ' '
               << x(0.82f) << ' ' << y(0.23f) << ' '
               << x(0.71f) << ' ' << y(0.20f) << " c "
               << x(0.62f) << ' ' << y(0.18f) << ' '
               << x(0.56f) << ' ' << y(0.24f) << ' '
               << x(0.50f) << ' ' << y(0.34f) << " c "
               << x(0.37f) << ' ' << y(0.55f) << " l S ";
    }
    stream << "Q";
    return AsciiToUtf16(stream.str());
}

static void AppendPdfEscapedLiteralText(std::ostringstream& stream, const std::u16string& value) {
    for (char16_t ch : value) {
        if (ch == u'(' || ch == u')' || ch == u'\\') {
            stream << '\\' << static_cast<char>(ch);
        } else if (ch >= 0x20 && ch <= 0x7E) {
            stream << static_cast<char>(ch);
        } else {
            stream << '?';
        }
    }
}

struct WrappedFreeTextLine {
    std::u16string text;
    bool alignLeft;
};

static bool IsFreeTextWrapSpace(char16_t ch) {
    return ch == u' ' || ch == u'\t';
}

static size_t TrimFreeTextLineEnd(const std::u16string& line, size_t start, size_t end) {
    while (end > start && IsFreeTextWrapSpace(line[end - 1])) {
        end--;
    }
    return end;
}

static void AppendWrappedFreeTextParagraph(
        const std::u16string& line,
        size_t maxCharactersPerLine,
        std::vector<WrappedFreeTextLine>* outLines
) {
    if (!outLines) return;
    if (line.empty()) {
        outLines->push_back({std::u16string(), false});
        return;
    }

    const bool needsWrap = line.size() > maxCharactersPerLine;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && IsFreeTextWrapSpace(line[start])) {
            start++;
        }
        if (start >= line.size()) break;

        const size_t endLimit = std::min(line.size(), start + maxCharactersPerLine);
        if (endLimit >= line.size()) {
            const size_t trimmedEnd = TrimFreeTextLineEnd(line, start, line.size());
            if (trimmedEnd > start) {
                outLines->push_back({line.substr(start, trimmedEnd - start), needsWrap});
            }
            break;
        }

        size_t breakAt = endLimit;
        for (size_t index = endLimit; index > start; index--) {
            if (IsFreeTextWrapSpace(line[index - 1])) {
                breakAt = index - 1;
                break;
            }
        }
        if (breakAt <= start) {
            breakAt = endLimit;
        }

        const size_t trimmedEnd = TrimFreeTextLineEnd(line, start, breakAt);
        if (trimmedEnd > start) {
            outLines->push_back({line.substr(start, trimmedEnd - start), true});
        }
        start = breakAt;
    }

    if (outLines->empty()) {
        outLines->push_back({std::u16string(), false});
    }
}

static std::vector<WrappedFreeTextLine> WrapFreeTextAppearanceText(
        const std::u16string& text,
        float maxTextWidth,
        float fontSize
) {
    std::vector<WrappedFreeTextLine> wrappedLines;
    const float estimatedCharacterWidth = std::max(1.0f, fontSize * 0.50f);
    const size_t maxCharactersPerLine = std::max<size_t>(
            1,
            static_cast<size_t>(std::floor(std::max(1.0f, maxTextWidth) / estimatedCharacterWidth))
    );

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(u'\n', start);
        if (end == std::u16string::npos) {
            end = text.size();
        }
        AppendWrappedFreeTextParagraph(
                text.substr(start, end - start),
                maxCharactersPerLine,
                &wrappedLines
        );

        if (end >= text.size()) break;
        start = end + 1;
    }

    if (wrappedLines.empty()) {
        wrappedLines.push_back({std::u16string(), false});
    }
    return wrappedLines;
}

static float EstimateFreeTextLineWidth(const std::u16string& line, float fontSize) {
    return static_cast<float>(line.size()) * fontSize * 0.50f;
}

static const char* MapFreeTextFontToAppearanceResource(const std::string& fontName) {
    if (fontName.find("Courier") != std::string::npos ||
        fontName.find("Mono") != std::string::npos ||
        fontName.find("mono") != std::string::npos) {
        return "Cour";
    }
    if (fontName.find("Times") != std::string::npos ||
        fontName.find("Serif") != std::string::npos ||
        fontName.find("serif") != std::string::npos) {
        return "TiRo";
    }
    return "Helv";
}

static float ClampFreeTextFontSize(float fontSize) {
    if (!std::isfinite(fontSize) || fontSize <= 0.0f) return 12.0f;
    return std::max(4.0f, std::min(fontSize, 160.0f));
}

static bool ParsePositiveFloatToken(const std::string& token, float* outValue) {
    if (!outValue || token.empty()) return false;
    std::istringstream stream(token);
    float parsed = 0.0f;
    stream >> parsed;
    if (stream.fail() || !std::isfinite(parsed) || parsed <= 0.0f) return false;
    *outValue = parsed;
    return true;
}

static std::vector<std::string> SplitPdfAppearanceTokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::istringstream stream(value);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

static float ParseFreeTextFontSizeFromAppearanceString(const std::string& defaultAppearance) {
    const std::vector<std::string> tokens = SplitPdfAppearanceTokens(defaultAppearance);
    for (size_t index = 0; index < tokens.size(); index++) {
        if (tokens[index] != "Tf" || index < 1) continue;
        float fontSize = 0.0f;
        if (ParsePositiveFloatToken(tokens[index - 1], &fontSize)) {
            return fontSize;
        }
    }
    return 0.0f;
}

static std::string NormalizeFreeTextFontName(const std::string& fontToken) {
    if (fontToken.empty()) return std::string();
    std::string fontName = fontToken[0] == '/' ? fontToken.substr(1) : fontToken;
    if (fontName.empty()) return std::string();
    if (fontName == "Helv") return "Helvetica";
    if (fontName == "Cour") return "Courier";
    if (fontName == "TiRo") return "Times-Roman";
    return fontName;
}

static std::string ParseFreeTextFontNameFromAppearanceString(const std::string& defaultAppearance) {
    const std::vector<std::string> tokens = SplitPdfAppearanceTokens(defaultAppearance);
    for (size_t index = 0; index < tokens.size(); index++) {
        if (tokens[index] != "Tf" || index < 2) continue;
        float ignoredFontSize = 0.0f;
        if (!ParsePositiveFloatToken(tokens[index - 1], &ignoredFontSize)) continue;
        const std::string fontName = NormalizeFreeTextFontName(tokens[index - 2]);
        if (!fontName.empty()) return fontName;
    }
    return std::string();
}

static float ParseFreeTextFontSizeFromStyleString(const std::string& defaultStyle) {
    size_t ptPosition = defaultStyle.find("pt");
    while (ptPosition != std::string::npos) {
        size_t numberEnd = ptPosition;
        while (numberEnd > 0) {
            const char ch = defaultStyle[numberEnd - 1];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') break;
            numberEnd--;
        }

        size_t numberStart = numberEnd;
        while (numberStart > 0) {
            const char ch = defaultStyle[numberStart - 1];
            if ((ch < '0' || ch > '9') && ch != '.') break;
            numberStart--;
        }

        if (numberStart < numberEnd) {
            float fontSize = 0.0f;
            if (ParsePositiveFloatToken(defaultStyle.substr(numberStart, numberEnd - numberStart), &fontSize)) {
                return fontSize;
            }
        }
        ptPosition = defaultStyle.find("pt", ptPosition + 2);
    }
    return 0.0f;
}

static float ResolveFreeTextFontSize(FPDF_ANNOTATION annot) {
    const std::string defaultAppearance = Utf16ToSimpleUtf8(ReadAnnotStringValueUtf16(annot, "DA"));
    float fontSize = ParseFreeTextFontSizeFromAppearanceString(defaultAppearance);
    if (fontSize > 0.0f) return ClampFreeTextFontSize(fontSize);

    const std::string defaultStyle = Utf16ToSimpleUtf8(ReadAnnotStringValueUtf16(annot, "DS"));
    fontSize = ParseFreeTextFontSizeFromStyleString(defaultStyle);
    if (fontSize > 0.0f) return ClampFreeTextFontSize(fontSize);

    const int objectCount = FPDFAnnot_GetObjectCount(annot);
    for (int objectIndex = 0; objectIndex < objectCount; objectIndex++) {
        FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
        if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_TEXT) continue;

        fontSize = 0.0f;
        FPDFTextObj_GetFontSize(pageObject, &fontSize);
        if (fontSize > 0.0f) return ClampFreeTextFontSize(fontSize);
    }

    return 12.0f;
}

static std::string ResolveFreeTextFontName(FPDF_ANNOTATION annot) {
    const std::string defaultAppearance = Utf16ToSimpleUtf8(ReadAnnotStringValueUtf16(annot, "DA"));
    const std::string fontName = ParseFreeTextFontNameFromAppearanceString(defaultAppearance);
    return fontName.empty() ? "Helvetica" : fontName;
}

static std::u16string BuildFreeTextAppearanceStream(
        const FS_RECTF& rect,
        const std::u16string& text,
        const char* fontResourceName,
        float fontSize,
        int r,
        int g,
        int b,
        int bgR,
        int bgG,
        int bgB,
        int bgA
) {
    const float width = rect.right - rect.left;
    const float height = rect.top - rect.bottom;
    if (width <= 0.1f || height <= 0.1f || text.empty()) return std::u16string();

    const float resolvedFontSize = std::max(4.0f, fontSize > 0.0f ? fontSize : 12.0f);
    const float padding = std::max(2.0f, std::min(width, height) * 0.08f);
    const float lineHeight = resolvedFontSize * 1.20f;
    const float maxTextWidth = std::max(1.0f, width - (padding * 2.0f));
    const std::vector<WrappedFreeTextLine> wrappedLines = WrapFreeTextAppearanceText(
            text,
            maxTextWidth,
            resolvedFontSize
    );

    const int lineCount = static_cast<int>(wrappedLines.size());
    const float ascent = resolvedFontSize * 0.72f;
    const float descent = resolvedFontSize * 0.21f;
    const float textBlockHeight = ascent + descent + ((lineCount - 1) * lineHeight);
    float baselineY = rect.bottom +
                      (std::max(0.0f, height - textBlockHeight) * 0.5f) +
                      ((lineCount - 1) * lineHeight) +
                      descent;
    baselineY = std::min(baselineY, rect.top - padding - ascent);
    const float minY = rect.bottom + padding;

    const float red = std::max(0.0f, std::min(r / 255.0f, 1.0f));
    const float green = std::max(0.0f, std::min(g / 255.0f, 1.0f));
    const float blue = std::max(0.0f, std::min(b / 255.0f, 1.0f));

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(3);

    if (bgA > 0) {
        const float bgRed = std::max(0.0f, std::min(bgR / 255.0f, 1.0f));
        const float bgGreen = std::max(0.0f, std::min(bgG / 255.0f, 1.0f));
        const float bgBlue = std::max(0.0f, std::min(bgB / 255.0f, 1.0f));
        stream << "q " << bgRed << ' ' << bgGreen << ' ' << bgBlue << " rg "
               << rect.left << ' ' << rect.bottom << ' ' << width << ' ' << height << " re f Q ";
    }

    stream << "q BT /" << (fontResourceName ? fontResourceName : "Helv") << ' ' << resolvedFontSize << " Tf "
           << red << ' ' << green << ' ' << blue << " rg ";

    for (const WrappedFreeTextLine& line : wrappedLines) {
        if (baselineY < minY) break;
        const float estimatedLineWidth = EstimateFreeTextLineWidth(line.text, resolvedFontSize);
        const float textX = !line.alignLeft && estimatedLineWidth < maxTextWidth
                            ? rect.left + ((width - estimatedLineWidth) * 0.5f)
                            : rect.left + padding;
        stream << "1 0 0 1 " << textX << ' ' << baselineY << " Tm (";
        AppendPdfEscapedLiteralText(stream, line.text);
        stream << ") Tj ";

        baselineY -= lineHeight;
    }

    stream << "ET Q";
    return AsciiToUtf16(stream.str());
}

static jstring BuildStickyNoteCommentMetaJString(JNIEnv* env, FPDF_ANNOTATION annot) {
    if (!env || !annot) return nullptr;

    jstring storedMeta = ReadAnnotStringValueJString(env, annot, "LufickCommentMeta");
    if (storedMeta) {
        return storedMeta;
    }

    const std::u16string title = ReadAnnotStringValueUtf16(annot, "T");
    const std::u16string text = ReadAnnotStringValueUtf16(annot, "Contents");
    const std::u16string pdfName = ReadAnnotStringValueUtf16(annot, "Name");
    std::u16string createdAtRaw = ReadAnnotStringValueUtf16(annot, "CreationDate");
    if (createdAtRaw.empty()) {
        createdAtRaw = ReadAnnotStringValueUtf16(annot, "M");
    }
    if (title.empty() && text.empty() && pdfName.empty() && createdAtRaw.empty()) {
        return nullptr;
    }

    std::u16string json = u"{\"title\":\"";
    AppendEscapedJsonUtf16(&json, title);
    json += u"\",\"text\":\"";
    AppendEscapedJsonUtf16(&json, text);
    json += u"\",\"iconKey\":\"";
    json += AsciiToUtf16(MapPdfCommentNameToStickyNoteIconKey(pdfName));
    json += u"\"";
    if (!createdAtRaw.empty()) {
        json += u",\"createdAtRaw\":\"";
        AppendEscapedJsonUtf16(&json, createdAtRaw);
        json += u"\"";
    }
    json += u"}";
    return env->NewString(reinterpret_cast<const jchar*>(json.data()), static_cast<jsize>(json.size()));
}
//----------------------------------------------------------------------------------------
static jstring GetBridgeDataPropertyJString(
        JNIEnv* env,
        jobject obj,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit,
        const char* key
) {
    if (!obj || !dataPropsField || !jsonClass || !jsonInit || !key) return nullptr;

    jstring jDataProps = (jstring)env->GetObjectField(obj, dataPropsField);
    if (!jDataProps) return nullptr;

    jobject dataJson = env->NewObject(jsonClass, jsonInit, jDataProps);
    if (!dataJson) {
        env->DeleteLocalRef(jDataProps);
        return nullptr;
    }

    jmethodID optString = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring jKey = env->NewStringUTF(key);
    jstring jValue = (jstring)env->CallObjectMethod(dataJson, optString, jKey);
    env->DeleteLocalRef(jKey);
    env->DeleteLocalRef(dataJson);
    env->DeleteLocalRef(jDataProps);

    if (!jValue || env->GetStringLength(jValue) == 0) {
        if (jValue) env->DeleteLocalRef(jValue);
        return nullptr;
    }
    return jValue;
}

// Method use for Compact instruction set for the watermark
static bool CollectPageLevelTextWatermarkPatternSpec(
        JNIEnv* env,
        jobject obj,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit,
        RawPdfWatermarkSpec* outSpec
) {
    if (!outSpec) return false;
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, dataPropsField, jsonClass, jsonInit, "watermarkProperties");
    if (!jJsonStr) {
        WM_LOGE("Native watermark spec collect failed: missing watermarkProperties");
        return false;
    }

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(jJsonStr);
        WM_LOGE("Native watermark spec collect failed: invalid watermarkProperties json");
        return false;
    }

    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    auto optStringValue = [&](const char* key) -> jstring {
        jstring jKey = env->NewStringUTF(key);
        jstring value = (jstring)env->CallObjectMethod(json, optS, jKey);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optBoolValue = [&](const char* key, bool fallback) -> bool {
        jstring jKey = env->NewStringUTF(key);
        bool value = env->CallBooleanMethod(json, optB, jKey, fallback ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
        env->DeleteLocalRef(jKey);
        return value;
    };

    jstring jType = optStringValue("watermarkType");
    jstring jMode = optStringValue("watermarkMode");
    jstring jText = optStringValue("text");
    jstring jObjectId = optStringValue("objectId");
    jstring jFont = optStringValue("font");
    jstring jFontPath = optStringValue("fontPath");
    jstring jTextPathData = optStringValue("textPathData");
    jstring jImagePath = optStringValue("imagePath");

    const char* typeStr = jType ? env->GetStringUTFChars(jType, nullptr) : nullptr;
    const char* modeStr = jMode ? env->GetStringUTFChars(jMode, nullptr) : nullptr;
    const bool isTextWatermark = !typeStr || strlen(typeStr) == 0 || strcmp(typeStr, "TEXT") == 0;
    const bool isImageWatermark = typeStr && strcmp(typeStr, "IMAGE") == 0;
    const bool isIconImageWatermark = isImageWatermark && optBoolValue("isIconImage", false);
    const bool isRasterImageWatermark = isImageWatermark && !isIconImageWatermark;
    const bool isRepeatedMode = !modeStr || strlen(modeStr) == 0 || strcmp(modeStr, "REPEATED") == 0;
    if (typeStr) env->ReleaseStringUTFChars(jType, typeStr);
    if (modeStr) env->ReleaseStringUTFChars(jMode, modeStr);

    if ((!isTextWatermark && !isIconImageWatermark && !isRasterImageWatermark) ||
        (!isRasterImageWatermark && (!jText || env->GetStringLength(jText) == 0)) ||
        (isRasterImageWatermark && (!jImagePath || env->GetStringLength(jImagePath) == 0))) {
        WM_LOGE(
                "Native watermark spec collect skipped: isText=%d isIconImage=%d isRaster=%d isRepeated=%d hasText=%d hasImagePath=%d",
                isTextWatermark ? 1 : 0,
                isIconImageWatermark ? 1 : 0,
                isRasterImageWatermark ? 1 : 0,
                isRepeatedMode ? 1 : 0,
                (jText && env->GetStringLength(jText) > 0) ? 1 : 0,
                (jImagePath && env->GetStringLength(jImagePath) > 0) ? 1 : 0
        );
        if (jType) env->DeleteLocalRef(jType);
        if (jMode) env->DeleteLocalRef(jMode);
        if (jText) env->DeleteLocalRef(jText);
        if (jObjectId) env->DeleteLocalRef(jObjectId);
        if (jFont) env->DeleteLocalRef(jFont);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        if (jTextPathData) env->DeleteLocalRef(jTextPathData);
        if (jImagePath) env->DeleteLocalRef(jImagePath);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    const char* textChars = jText ? env->GetStringUTFChars(jText, nullptr) : nullptr;
    const char* objectIdChars = jObjectId ? env->GetStringUTFChars(jObjectId, nullptr) : nullptr;
    const char* fontChars = jFont ? env->GetStringUTFChars(jFont, nullptr) : nullptr;
    const char* fontPathChars = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;
    const char* textPathDataChars = jTextPathData ? env->GetStringUTFChars(jTextPathData, nullptr) : nullptr;
    const char* imagePathChars = jImagePath ? env->GetStringUTFChars(jImagePath, nullptr) : nullptr;
    RawPdfWatermarkSpec spec;
    spec.text = textChars ? textChars : "";
    spec.objectId = objectIdChars ? objectIdChars : "";
    spec.fontName = fontChars ? fontChars : "";
    spec.fontPath = fontPathChars ? fontPathChars : "";
    spec.textPathData = textPathDataChars ? textPathDataChars : "";
    spec.imagePath = imagePathChars ? imagePathChars : "";
    spec.pageIndex = optIntValue("pdfPageIndex", optIntValue("sourcePageIndex", -1));
    spec.pageWidth = fmax((float)optDoubleValue("pageWidth", 0.0), 0.0f);
    spec.pageHeight = fmax((float)optDoubleValue("pageHeight", 0.0), 0.0f);
    spec.pageLeft = (float)optDoubleValue("pageLeft", 0.0);
    spec.pageBottom = (float)optDoubleValue("pageBottom", 0.0);
    const float centerNormX = fmax(0.0f, fmin((float)optDoubleValue("centerNormX", 0.5), 1.0f));
    const float centerNormY = fmax(0.0f, fmin((float)optDoubleValue("centerNormY", 0.5), 1.0f));
    spec.centerX = spec.pageLeft + (spec.pageWidth * centerNormX);
    spec.centerY = spec.pageBottom + (spec.pageHeight * (1.0f - centerNormY));
    spec.isRepeated = isRepeatedMode;
    spec.isRasterImage = isRasterImageWatermark;
    spec.imagePixelWidth = std::max(0, optIntValue("imagePixelWidth", 0));
    spec.imagePixelHeight = std::max(0, optIntValue("imagePixelHeight", 0));

    const float canvasWidth = fmax((float)optDoubleValue("canvasWidth", spec.pageWidth), 0.0001f);
    const float canvasHeight = fmax((float)optDoubleValue("canvasHeight", spec.pageHeight), 0.0001f);
    const float pageScale = fmin(spec.pageWidth / canvasWidth, spec.pageHeight / canvasHeight);
    const float inverseBgScale = fmax((float)optDoubleValue("inverseBgScale", 1.0), 0.0001f);
    const float fixedWatermarkPdfScale = (float)optDoubleValue("watermarkPdfScale", -1.0);
    const float referencePageWidth = fmax((float)optDoubleValue("watermarkReferencePageWidth", spec.pageWidth), 0.0001f);
    const float visualWidthScale = fmax(spec.pageWidth / referencePageWidth, 0.0001f);
    const float watermarkPdfScale = fixedWatermarkPdfScale > 0.0f
                                    ? fixedWatermarkPdfScale * visualWidthScale
                                    : ((isIconImageWatermark || isRasterImageWatermark) ? 1.0f : inverseBgScale) * fmax(pageScale, 0.0001f);
    const float modelTextSize = fmax((float)optDoubleValue("fontSize", 12.0), 0.1f);
    spec.fontSize = fmax(modelTextSize * watermarkPdfScale, 1.0f);
    spec.rotation = (float)optDoubleValue("rotation", 0.0);
    spec.textR = std::max(0, std::min(optIntValue("textColorR", 0), 255));
    spec.textG = std::max(0, std::min(optIntValue("textColorG", 0), 255));
    spec.textB = std::max(0, std::min(optIntValue("textColorB", 0), 255));
    const float rawOpacity = fmax(0.0f, fmin((float)optDoubleValue("opacity", 1.0), 1.0f));
    spec.opacity = rawOpacity;

    spec.isBold = !isIconImageWatermark && !isRasterImageWatermark && optBoolValue("bold", false);
    spec.isItalic = !isIconImageWatermark && !isRasterImageWatermark && optBoolValue("italic", false);
    spec.isUnderline = !isIconImageWatermark && !isRasterImageWatermark && optBoolValue("underline", false);
    spec.isStrikeout = !isIconImageWatermark && !isRasterImageWatermark && optBoolValue("strikeout", false);
    spec.isIconImage = isIconImageWatermark;
    spec.requiresTextShaping = !isIconImageWatermark && !isRasterImageWatermark && optBoolValue("requiresTextShaping", false);
    const int textLength = std::max(1, (int)spec.text.size());
    spec.characterSpacing = fmax((float)optDoubleValue("letterSpacing", 0.0) * spec.fontSize, 0.0f);
    const float measuredTextWidth = (float)optDoubleValue("measuredTextWidth", 0.0);
    const float measuredTextHeight = (float)optDoubleValue("measuredTextHeight", 0.0);
    spec.textPathWidth = fmax((float)optDoubleValue("textPathWidth", 0.0), 0.0f);
    spec.textPathHeight = fmax((float)optDoubleValue("textPathHeight", 0.0), 0.0f);
    const float measuredFontAscent = (float)optDoubleValue("measuredFontAscent", -modelTextSize * 0.8f);
    const float measuredFontDescent = (float)optDoubleValue("measuredFontDescent", modelTextSize * 0.2f);
    const float measuredSpacingOffset = (float)optDoubleValue("measuredSpacingOffset", 0.0);
    const float measuredPdfTextWidth = (measuredTextWidth + measuredSpacingOffset) * watermarkPdfScale;
    const float measuredPdfTextHeight = measuredTextHeight * watermarkPdfScale;
    const float measuredPdfAscent = measuredFontAscent * watermarkPdfScale;
    const float measuredPdfDescent = measuredFontDescent * watermarkPdfScale;
    const float approximateTextWidth = isRasterImageWatermark
            ? fmax(measuredPdfTextWidth, 1.0f)
            : fmax(
                    measuredPdfTextWidth > 0.0f
                    ? measuredPdfTextWidth
                    : ((textLength * spec.fontSize * 0.55f) + (std::max(0, textLength - 1) * spec.characterSpacing)),
                    spec.fontSize
            );
    const float textHeight = isRasterImageWatermark
            ? fmax(measuredPdfTextHeight, 1.0f)
            : fmax(measuredPdfTextHeight > 0.0f ? measuredPdfTextHeight : spec.fontSize * 1.15f, spec.fontSize);
    const float glyphPadding = fmax(spec.fontSize * 0.18f, 1.0f);
    const float horizontalSpacing = fmax((float)optDoubleValue("horizontalSpacing", 0.0) * watermarkPdfScale, 0.0f);
    const float verticalSpacing = fmax((float)optDoubleValue("verticalSpacing", 0.0) * watermarkPdfScale, 0.0f);
    spec.contentWidth = approximateTextWidth;
    spec.contentHeight = textHeight;
    spec.repeatStepWidth = isRasterImageWatermark
            ? fmax(approximateTextWidth + horizontalSpacing, approximateTextWidth)
            : fmax(
                    approximateTextWidth + horizontalSpacing,
                    spec.fontSize
            );
    spec.repeatStepHeight = isRasterImageWatermark
            ? fmax(textHeight + verticalSpacing, textHeight)
            : fmax(
                    textHeight + verticalSpacing,
                    spec.fontSize
            );
    spec.patternWidth = spec.repeatStepWidth + (glyphPadding * 3.0f);
    spec.patternHeight = spec.repeatStepHeight + (glyphPadding * 3.0f);
    spec.baselineX = glyphPadding + (spec.isItalic ? spec.fontSize * 0.25f : 0.0f);
    spec.baselineY = glyphPadding - measuredPdfAscent;
    if (spec.baselineY + measuredPdfDescent > spec.patternHeight - glyphPadding) {
        spec.patternHeight = spec.baselineY + measuredPdfDescent + glyphPadding;
    }

    if (textChars) env->ReleaseStringUTFChars(jText, textChars);
    if (objectIdChars) env->ReleaseStringUTFChars(jObjectId, objectIdChars);
    if (fontChars) env->ReleaseStringUTFChars(jFont, fontChars);
    if (fontPathChars) env->ReleaseStringUTFChars(jFontPath, fontPathChars);
    if (textPathDataChars) env->ReleaseStringUTFChars(jTextPathData, textPathDataChars);
    if (imagePathChars) env->ReleaseStringUTFChars(jImagePath, imagePathChars);
    if (jType) env->DeleteLocalRef(jType);
    if (jMode) env->DeleteLocalRef(jMode);
    if (jText) env->DeleteLocalRef(jText);
    if (jObjectId) env->DeleteLocalRef(jObjectId);
    if (jFont) env->DeleteLocalRef(jFont);
    if (jFontPath) env->DeleteLocalRef(jFontPath);
    if (jTextPathData) env->DeleteLocalRef(jTextPathData);
    if (jImagePath) env->DeleteLocalRef(jImagePath);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);

    if (spec.pageIndex < 0 || (!spec.isRasterImage && spec.text.empty()) ||
        (spec.isRasterImage && spec.imagePath.empty()) || spec.pageWidth <= 0.0f || spec.pageHeight <= 0.0f) {
        WM_LOGE(
                "Native watermark spec collect failed: page=%d textLen=%zu raster=%d imagePathLen=%zu pageSize=%fx%f",
                spec.pageIndex,
                spec.text.size(),
                spec.isRasterImage ? 1 : 0,
                spec.imagePath.size(),
                spec.pageWidth,
                spec.pageHeight
        );
        return false;
    }
    WM_LOGE(
            "Native watermark spec collected: page=%d mode=%s fontName=%s textLen=%zu raster=%d image=%dx%d font=%f pattern=%fx%f step=%fx%f baseline=%f rotation=%f opacity=%f",
            spec.pageIndex,
            spec.isRepeated ? "REPEATED" : "SINGLE",
            spec.fontName.c_str(),
            spec.text.size(),
            spec.isRasterImage ? 1 : 0,
            spec.imagePixelWidth,
            spec.imagePixelHeight,
            spec.fontSize,
            spec.patternWidth,
            spec.patternHeight,
            spec.repeatStepWidth,
            spec.repeatStepHeight,
            spec.baselineY,
            spec.rotation,
            spec.opacity
    );
    *outSpec = spec;
    return true;
}

static jstring BuildBridgeDataPropertiesJString(
        JNIEnv* env,
        jclass jsonClass,
        jmethodID jsonInit,
        jmethodID jsonPut,
        jmethodID jsonToString,
        jstring textProps,
        jstring fhProps,
        jstring imageProps,
        jstring shapeProps,
        jstring simplePdfStampProps,
        jstring attachmentProps
) {
    if (!textProps && !fhProps && !imageProps && !shapeProps && !simplePdfStampProps && !attachmentProps) return nullptr;
    jstring jEmptyJson = env->NewStringUTF("{}");
    jobject json = env->NewObject(jsonClass, jsonInit, jEmptyJson);
    env->DeleteLocalRef(jEmptyJson);
    if (!json) return nullptr;

    auto putValue = [&](const char* key, jstring value) {
        if (!value) return;
        jstring jKey = env->NewStringUTF(key);
        env->CallObjectMethod(json, jsonPut, jKey, value);
        env->DeleteLocalRef(jKey);
    };
    putValue("textProperties", textProps);
    putValue("fhDrawingProperties", fhProps);
    putValue("imageProperties", imageProps);
    putValue("shapeProperties", shapeProps);
    putValue("simplePdfStampProperties", simplePdfStampProps);
    putValue("attachmentProperties", attachmentProps);

    jstring result = (jstring)env->CallObjectMethod(json, jsonToString);
    env->DeleteLocalRef(json);
    return result;
}

struct LufickContentObjectMetadata {
    std::string kind;
    std::string subtype;
    std::string groupId;

    bool isValid() const { return !kind.empty(); }
};

static void AddLufickContentObjectMetadata(
        FPDF_DOCUMENT doc,
        FPDF_PAGEOBJECT pageObject,
        const std::string& kind,
        const std::string& subtype,
        const std::string& groupId
) {
    if (!doc || !pageObject || kind.empty()) return;
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(pageObject, "LufickContentMeta");
    if (!mark) return;
    FPDFPageObjMark_SetStringParam(doc, pageObject, mark, "kind", kind.c_str());
    if (!subtype.empty()) {
        FPDFPageObjMark_SetStringParam(doc, pageObject, mark, "subtype", subtype.c_str());
    }
    if (!groupId.empty()) {
        FPDFPageObjMark_SetStringParam(doc, pageObject, mark, "groupId", groupId.c_str());
    }
    FPDFPageObjMark_SetIntParam(doc, pageObject, mark, "version", 1);
}

static std::string ReadPageObjectMarkUtf8(
        FPDF_PAGEOBJECTMARK mark,
        const char* key
) {
    if (!mark || !key) return std::string();
    unsigned long byteLength = 0;
    if (!FPDFPageObjMark_GetParamStringValue(mark, key, nullptr, 0, &byteLength) ||
        byteLength < sizeof(FPDF_WCHAR)) {
        return std::string();
    }
    std::vector<FPDF_WCHAR> buffer((byteLength / sizeof(FPDF_WCHAR)) + 1, 0);
    if (!FPDFPageObjMark_GetParamStringValue(
            mark,
            key,
            buffer.data(),
            byteLength,
            &byteLength
    )) {
        return std::string();
    }
    return Utf16ToSimpleUtf8(std::u16string(
            reinterpret_cast<const char16_t*>(buffer.data())
    ));
}

static bool IsLufickContentMetadataMark(FPDF_PAGEOBJECTMARK mark) {
    if (!mark) return false;
    unsigned long byteLength = 0;
    if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &byteLength) ||
        byteLength < sizeof(FPDF_WCHAR)) {
        return false;
    }
    std::vector<FPDF_WCHAR> buffer((byteLength / sizeof(FPDF_WCHAR)) + 1, 0);
    if (!FPDFPageObjMark_GetName(mark, buffer.data(), byteLength, &byteLength)) return false;
    return Utf16ToSimpleUtf8(std::u16string(
            reinterpret_cast<const char16_t*>(buffer.data())
    )) == "LufickContentMeta";
}

static std::u16string ReadAttachmentNameUtf16(FPDF_ATTACHMENT attachment) {
    if (!attachment) return {};
    const unsigned long byteLength = FPDFAttachment_GetName(attachment, nullptr, 0);
    if (byteLength <= sizeof(FPDF_WCHAR)) return {};
    std::vector<FPDF_WCHAR> buffer(byteLength / sizeof(FPDF_WCHAR));
    if (!FPDFAttachment_GetName(attachment, buffer.data(), byteLength)) return {};
    return std::u16string(
            reinterpret_cast<const char16_t*>(buffer.data()),
            buffer.size() - 1);
}

static std::u16string ReadAttachmentSubtypeUtf16(FPDF_ATTACHMENT attachment) {
    if (!attachment) return {};
    const unsigned long byteLength = FPDFAttachment_GetSubtype(attachment, nullptr, 0);
    if (byteLength <= sizeof(FPDF_WCHAR)) return {};
    std::vector<FPDF_WCHAR> buffer(byteLength / sizeof(FPDF_WCHAR));
    if (!FPDFAttachment_GetSubtype(attachment, buffer.data(), byteLength)) return {};
    return std::u16string(
            reinterpret_cast<const char16_t*>(buffer.data()),
            buffer.size() - 1);
}

static bool ProcessFileAttachment(
        JNIEnv* env,
        jobject bridgeObject,
        FPDF_DOCUMENT document,
        FPDF_ANNOTATION annot,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit,
        int red,
        int green,
        int blue,
        int alpha
) {
    jstring attachmentProps = GetBridgeDataPropertyJString(
            env, bridgeObject, dataPropsField, jsonClass, jsonInit, "attachmentProperties");
    if (!attachmentProps) return false;

    jobject json = env->NewObject(jsonClass, jsonInit, attachmentProps);
    if (!json || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (json) env->DeleteLocalRef(json);
        env->DeleteLocalRef(attachmentProps);
        return false;
    }
    jmethodID optString = env->GetMethodID(
            jsonClass, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jstring empty = env->NewStringUTF("");
    auto readString = [&](const char* key) -> jstring {
        jstring jsonKey = env->NewStringUTF(key);
        jstring value = (jstring) env->CallObjectMethod(json, optString, jsonKey, empty);
        env->DeleteLocalRef(jsonKey);
        return value;
    };

    jstring filePathValue = readString("filePath");
    jstring fileNameValue = readString("fileName");
    jstring mimeTypeValue = readString("mimeType");
    jstring iconNameValue = readString("iconName");
    const char* filePath = filePathValue
            ? env->GetStringUTFChars(filePathValue, nullptr)
            : nullptr;
    bool success = false;

    if (filePath && fileNameValue && env->GetStringLength(fileNameValue) > 0) {
        std::ifstream input(filePath, std::ios::binary);
        std::vector<unsigned char> bytes(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
        if (input.good() || input.eof()) {
            const std::u16string fileName = JStringToUtf16(env, fileNameValue);
            FPDF_ATTACHMENT attachment = FPDFAnnot_AddFileAttachment(
                    annot, reinterpret_cast<FPDF_WIDESTRING>(fileName.c_str()));
            if (attachment && FPDFAttachment_SetFile(
                    attachment,
                    document,
                    bytes.empty() ? nullptr : bytes.data(),
                    static_cast<unsigned long>(bytes.size()))) {
                if (mimeTypeValue && env->GetStringLength(mimeTypeValue) > 0) {
                    const std::u16string mimeType = JStringToUtf16(env, mimeTypeValue);
                    FPDFAttachment_SetStringValue(
                            attachment,
                            "Subtype",
                            reinterpret_cast<FPDF_WIDESTRING>(mimeType.c_str()));
                    FPDFAnnot_SetStringValue(
                            annot,
                            "LufickAttachmentMime",
                            reinterpret_cast<FPDF_WIDESTRING>(mimeType.c_str()));
                }
                FPDFAnnot_SetStringValue(
                        annot,
                        "Contents",
                        reinterpret_cast<FPDF_WIDESTRING>(fileName.c_str()));
                const char* requestedIconName = iconNameValue
                        ? env->GetStringUTFChars(iconNameValue, nullptr)
                        : nullptr;
                const std::string iconName = requestedIconName && requestedIconName[0] != '\0'
                        ? requestedIconName
                        : "Paperclip";
                SetAnnotAsciiStringValue(annot, "Name", iconName.c_str());
                FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, red, green, blue, alpha);
                FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, red, green, blue, alpha);
                FS_RECTF attachmentRect{};
                if (FPDFAnnot_GetRect(annot, &attachmentRect)) {
                    const std::u16string appearance =
                            BuildFileAttachmentAppearanceStream(
                                    attachmentRect, iconName, red, green, blue);
                    if (!appearance.empty()) {
                        FPDFAnnot_SetAP(
                                annot,
                                FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                reinterpret_cast<FPDF_WIDESTRING>(appearance.c_str()));
                    }
                }
                if (requestedIconName) {
                    env->ReleaseStringUTFChars(iconNameValue, requestedIconName);
                }
                success = true;
            }
        }
    }

    if (filePath) env->ReleaseStringUTFChars(filePathValue, filePath);
    if (filePathValue) env->DeleteLocalRef(filePathValue);
    if (fileNameValue) env->DeleteLocalRef(fileNameValue);
    if (mimeTypeValue) env->DeleteLocalRef(mimeTypeValue);
    if (iconNameValue) env->DeleteLocalRef(iconNameValue);
    env->DeleteLocalRef(empty);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(attachmentProps);
    return success;
}

static jstring BuildAttachmentPropertiesJString(
        JNIEnv* env,
        jclass jsonClass,
        jmethodID jsonInit,
        jmethodID jsonPut,
        jmethodID jsonToString,
        const std::u16string& fileName,
        const std::u16string& mimeType,
        unsigned long fileSize,
        const std::u16string& iconName
) {
    jstring emptyJson = env->NewStringUTF("{}");
    jobject json = env->NewObject(jsonClass, jsonInit, emptyJson);
    env->DeleteLocalRef(emptyJson);
    if (!json) return nullptr;

    auto putUtf16 = [&](const char* key, const std::u16string& value) {
        jstring jsonKey = env->NewStringUTF(key);
        jstring jsonValue = env->NewString(
                reinterpret_cast<const jchar*>(value.data()),
                static_cast<jsize>(value.size()));
        env->CallObjectMethod(json, jsonPut, jsonKey, jsonValue);
        env->DeleteLocalRef(jsonValue);
        env->DeleteLocalRef(jsonKey);
    };
    putUtf16("fileName", fileName);
    putUtf16("mimeType", mimeType);
    putUtf16("iconName", iconName);
    if (iconName == u"Graph") {
        putUtf16("iconStyle", u"graph");
    } else if (iconName == u"PushPin") {
        putUtf16("iconStyle", u"push_pin");
    } else if (iconName == u"Tag") {
        putUtf16("iconStyle", u"tag");
    } else {
        putUtf16("iconStyle", u"paperclip");
    }

    jclass longClass = env->FindClass("java/lang/Long");
    jmethodID longValueOf = env->GetStaticMethodID(longClass, "valueOf", "(J)Ljava/lang/Long;");
    jobject boxedSize = env->CallStaticObjectMethod(longClass, longValueOf, static_cast<jlong>(fileSize));
    jstring sizeKey = env->NewStringUTF("fileSize");
    env->CallObjectMethod(json, jsonPut, sizeKey, boxedSize);
    env->DeleteLocalRef(sizeKey);
    env->DeleteLocalRef(boxedSize);
    env->DeleteLocalRef(longClass);

    jstring result = (jstring) env->CallObjectMethod(json, jsonToString);
    env->DeleteLocalRef(json);
    return result;
}

static LufickContentObjectMetadata ReadLufickContentObjectMetadata(FPDF_PAGEOBJECT pageObject) {
    LufickContentObjectMetadata metadata;
    if (!pageObject) return metadata;
    const int markCount = FPDFPageObj_CountMarks(pageObject);
    for (int index = 0; index < markCount; ++index) {
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(pageObject, index);
        if (!IsLufickContentMetadataMark(mark)) continue;
        metadata.kind = ReadPageObjectMarkUtf8(mark, "kind");
        metadata.subtype = ReadPageObjectMarkUtf8(mark, "subtype");
        metadata.groupId = ReadPageObjectMarkUtf8(mark, "groupId");
        break;
    }
    return metadata;
}

static bool IsEditableShadingProxy(FPDF_PAGEOBJECT pageObject) {
    return pageObject &&
           FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_IMAGE &&
           ReadLufickContentObjectMetadata(pageObject).kind == "shading_proxy";
}

static bool IsEditableShadingObject(FPDF_PAGEOBJECT pageObject) {
    return pageObject &&
           (FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_SHADING ||
            IsEditableShadingProxy(pageObject));
}

static FPDF_PAGEOBJECT CreateEditableShadingProxy(
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_PAGEOBJECT shadingObject
) {
    float rawLeft = 0.0f;
    float rawBottom = 0.0f;
    float rawRight = 0.0f;
    float rawTop = 0.0f;
    if (!doc || !page || !shadingObject ||
        !FPDFPageObj_GetBounds(
                shadingObject,
                &rawLeft,
                &rawBottom,
                &rawRight,
                &rawTop
        )) {
        return nullptr;
    }

    const float left = std::min(rawLeft, rawRight);
    const float right = std::max(rawLeft, rawRight);
    const float bottom = std::min(rawBottom, rawTop);
    const float top = std::max(rawBottom, rawTop);
    const float width = right - left;
    const float height = top - bottom;
    if (width < 0.0001f || height < 0.0001f) return nullptr;

    constexpr float kPreferredPixelsPerPoint = 2.0f;
    constexpr float kMaximumBitmapSide = 2048.0f;
    const float renderScale = std::max(
            0.1f,
            std::min(
                    kPreferredPixelsPerPoint,
                    std::min(kMaximumBitmapSide / width, kMaximumBitmapSide / height)
            )
    );
    const int bitmapWidth = std::max(1, static_cast<int>(ceilf(width * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(ceilf(height * renderScale)));
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
            bitmapWidth,
            bitmapHeight,
            FPDFBitmap_BGRA,
            nullptr,
            0
    );
    if (!bitmap) return nullptr;

    FPDFBitmap_FillRect(bitmap, 0, 0, bitmapWidth, bitmapHeight, 0x00000000);
    const float pageWidth = FPDF_GetPageWidthF(page);
    const float pageHeight = FPDF_GetPageHeightF(page);
    const int renderedPageWidth =
            std::max(1, static_cast<int>(ceilf(pageWidth * renderScale)));
    const int renderedPageHeight =
            std::max(1, static_cast<int>(ceilf(pageHeight * renderScale)));
    const int startX = static_cast<int>(roundf(-left * renderScale));
    const int startY = static_cast<int>(roundf(-(pageHeight - top) * renderScale));
    FPDF_RenderPageBitmap(
            bitmap,
            page,
            startX,
            startY,
            renderedPageWidth,
            renderedPageHeight,
            0,
            0
    );

    FPDF_PAGEOBJECT proxy = FPDFPageObj_NewImageObj(doc);
    if (!proxy || !FPDFImageObj_SetBitmap(nullptr, 0, proxy, bitmap)) {
        if (proxy) FPDFPageObj_Destroy(proxy);
        FPDFBitmap_Destroy(bitmap);
        return nullptr;
    }
    FPDFBitmap_Destroy(bitmap);

    FPDFImageObj_SetMatrix(proxy, width, 0.0, 0.0, height, left, bottom);
    AddLufickContentObjectMetadata(doc, proxy, "shading_proxy", "", "");
    return proxy;
}

static bool ConvertNativeShadingsToEditableProxies(
        FPDF_DOCUMENT doc,
        FPDF_PAGE page
) {
    const int objectCount = page ? FPDFPage_CountObjects(page) : 0;
    if (!doc || objectCount <= 0) return true;

    std::vector<FPDF_PAGEOBJECT> originalObjects;
    originalObjects.reserve(objectCount);
    bool hasNativeShading = false;
    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, index);
        if (!pageObject) return false;
        originalObjects.push_back(pageObject);
        hasNativeShading =
                hasNativeShading ||
                FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_SHADING;
    }
    if (!hasNativeShading) return true;

    std::vector<FPDF_PAGEOBJECT> replacements(objectCount, nullptr);
    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT original = originalObjects[index];
        if (FPDFPageObj_GetType(original) != FPDF_PAGEOBJ_SHADING) continue;
        replacements[index] = CreateEditableShadingProxy(doc, page, original);
        if (replacements[index]) continue;

        for (FPDF_PAGEOBJECT replacement : replacements) {
            if (replacement) FPDFPageObj_Destroy(replacement);
        }
        return false;
    }

    for (int index = objectCount - 1; index >= 0; --index) {
        if (FPDFPageObj_GetType(originalObjects[index]) != FPDF_PAGEOBJ_SHADING) continue;
        if (!FPDFPage_RemoveObject(page, originalObjects[index])) {
            for (FPDF_PAGEOBJECT replacement : replacements) {
                if (replacement) FPDFPageObj_Destroy(replacement);
            }
            return false;
        }
    }

    for (int index = 0; index < objectCount; ++index) {
        FPDF_PAGEOBJECT replacement = replacements[index];
        if (!replacement) continue;
        if (!FPDFPage_InsertObjectAtIndex(page, replacement, static_cast<size_t>(index))) {
            FPDFPageObj_Destroy(replacement);
            return false;
        }
        FPDFPageObj_Destroy(originalObjects[index]);
    }
    return true;
}

static std::string GetBridgeDataPropertyUtf8(
        JNIEnv* env,
        jobject object,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit,
        const char* key
) {
    jstring value = GetBridgeDataPropertyJString(
            env,
            object,
            dataPropsField,
            jsonClass,
            jsonInit,
            key
    );
    if (!value) return std::string();
    const char* rawValue = env->GetStringUTFChars(value, nullptr);
    const std::string result = rawValue ? rawValue : "";
    if (rawValue) env->ReleaseStringUTFChars(value, rawValue);
    env->DeleteLocalRef(value);
    return result;
}

// --- HELPER 1: LINK LOGIC ---
static void processLink(JNIEnv* env, jobject obj, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID urlField) {
    jstring jUrl = (jstring)env->GetObjectField(obj, urlField);
    if (jUrl) {
        const char* url = env->GetStringUTFChars(jUrl, 0);
        FPDFAnnot_SetURI(annot, url);
        env->ReleaseStringUTFChars(jUrl, url);
    }

    FS_QUADPOINTSF qp = {rect.left, rect.top, rect.right, rect.top, rect.left, rect.bottom, rect.right, rect.bottom};
    FPDFAnnot_AppendAttachmentPoints(annot, &qp);
}

static void processStickyNoteComment(
        JNIEnv* env,
        jobject obj,
        FPDF_ANNOTATION annot,
        jfieldID commentPropsField,
        int r,
        int g,
        int b,
        int alpha,
        jclass jsonClass,
        jmethodID jsonInit
) {
    if (!annot) return;

    // Reset any prior appearance before rebuilding the sticky note from the
    // latest JSON payload.
    FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
    SetAnnotAsciiStringValue(annot, "Name", "Note");

    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, commentPropsField, jsonClass, jsonInit, "textProperties");
    if (!jJsonStr) return;

    SetAnnotWideStringValueFromJString(env, annot, "LufickCommentMeta", jJsonStr);

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json) {
        env->DeleteLocalRef(jJsonStr);
        return;
    }

    jmethodID optString = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring titleKey = env->NewStringUTF("title");
    jstring textKey = env->NewStringUTF("text");
    jstring richTextKey = env->NewStringUTF("richText");
    jstring iconKeyKey = env->NewStringUTF("iconKey");
    jstring createdAtRawKey = env->NewStringUTF("createdAtRaw");
    jstring jTitle = (jstring)env->CallObjectMethod(json, optString, titleKey);
    jstring jText = (jstring)env->CallObjectMethod(json, optString, textKey);
    jstring jRichText = (jstring)env->CallObjectMethod(json, optString, richTextKey);
    jstring jIconKey = (jstring)env->CallObjectMethod(json, optString, iconKeyKey);
    jstring jCreatedAtRaw = (jstring)env->CallObjectMethod(json, optString, createdAtRawKey);

    if (jTitle && env->GetStringLength(jTitle) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "T", jTitle);
    }
    if (jText && env->GetStringLength(jText) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "Contents", jText);
    }
    if (jRichText) {
        SetAnnotWideStringValueFromJString(env, annot, "RC", jRichText);
    }
    if (jCreatedAtRaw && env->GetStringLength(jCreatedAtRaw) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "CreationDate", jCreatedAtRaw);
        SetAnnotWideStringValueFromJString(env, annot, "M", jCreatedAtRaw);
    }

    std::string iconKey;
    if (jIconKey) {
        const char* rawIconKey = env->GetStringUTFChars(jIconKey, nullptr);
        if (rawIconKey) {
            iconKey = rawIconKey;
            env->ReleaseStringUTFChars(jIconKey, rawIconKey);
        }
    }

    const char* pdfIconName = MapStickyNoteIconKeyToPdfCommentName(iconKey);
    SetAnnotAsciiStringValue(annot, "Name", pdfIconName);

    FS_RECTF annotRect;
    if (FPDFAnnot_GetRect(annot, &annotRect)) {
        const std::u16string appearanceStream = BuildStickyNoteAppearanceStream(annotRect, iconKey, r, g, b);
        if (!appearanceStream.empty()) {
            FPDFAnnot_SetAP(
                    annot,
                    FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                    reinterpret_cast<FPDF_WIDESTRING>(appearanceStream.c_str())
            );
        }
    }

    env->DeleteLocalRef(jIconKey);
    env->DeleteLocalRef(jText);
    env->DeleteLocalRef(jRichText);
    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jCreatedAtRaw);
    env->DeleteLocalRef(createdAtRawKey);
    env->DeleteLocalRef(iconKeyKey);
    env->DeleteLocalRef(textKey);
    env->DeleteLocalRef(richTextKey);
    env->DeleteLocalRef(titleKey);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
}
// --- HELPER 2: TEXT STAMP LOGIC ---
static void processTextStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit) {
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, textPropsField, jsonClass, jsonInit, "textProperties");
    if (!jJsonStr) return;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    jmethodID getD = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");

    double rotation = 0, jsonWidth = 0, jsonHeight = 0;
    try {
        rotation = env->CallDoubleMethod(json, getD, env->NewStringUTF("rotation"));
        jsonWidth = env->CallDoubleMethod(json, getD, env->NewStringUTF("width"));
        jsonHeight = env->CallDoubleMethod(json, getD, env->NewStringUTF("height"));
    } catch (...) {}

    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    const int defaultAlpha = alpha > 0 ? alpha : 255;
    const int textR = optIntValue("textColorR", r);
    const int textG = optIntValue("textColorG", g);
    const int textB = optIntValue("textColorB", b);
    const int textA = optIntValue("textColorA", defaultAlpha);

    // Recreated text stamps carry their base box in JSON while the annotation rect
    // may already be the expanded rotated bounds. Prefer JSON dimensions when present.
    float initialWidth = (jsonWidth > 0) ? (float)jsonWidth : fabs(rect.right - rect.left);
    float initialHeight = (jsonHeight > 0) ? (float)jsonHeight : fabs(rect.top - rect.bottom);
    double angleRad = rotation * M_PI / 180.0;

    // Expanded Bounding Box Math
    float expandedWidth = (float)(initialWidth * fabs(cos(angleRad)) + initialHeight * fabs(sin(angleRad)));
    float expandedHeight = (float)(initialHeight * fabs(cos(angleRad)) + initialWidth * fabs(sin(angleRad)));
    float origCenterX = (rect.left + rect.right) / 2.0f;
    float origCenterY = (rect.bottom + rect.top) / 2.0f;

    FS_RECTF drawRect = {origCenterX - (expandedWidth / 2.0f), origCenterY + (expandedHeight / 2.0f),
                         origCenterX + (expandedWidth / 2.0f), origCenterY - (expandedHeight / 2.0f)};
    FPDFAnnot_SetRect(annot, &drawRect);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, textR, textG, textB, textA);

    appendSimplePdfStampTextObject(
            env,
            doc,
            page,
            annot,
            rect,
            json,
            jsonClass,
            r,
            g,
            b,
            alpha,
            false
    );
    const jchar* rawJsonContent = env->GetStringChars(jJsonStr, nullptr);
    FPDFAnnot_SetStringValue(annot, "Contents", (FPDF_WIDESTRING)rawJsonContent);
    SetAnnotWideStringValueFromJString(env, annot, "LufickTextStampMeta", jJsonStr);
    SetAnnotAsciiStringValue(annot, "LufickStampKind", "text");
    jstring jSignatureSubtypeKey = env->NewStringUTF("signatureSubType");
    jstring jSignatureSubtype = (jstring)env->CallObjectMethod(json, optS, jSignatureSubtypeKey);
    if (jSignatureSubtype && env->GetStringLength(jSignatureSubtype) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "LufickSignatureSubtype", jSignatureSubtype);
    }
    if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
    env->DeleteLocalRef(jSignatureSubtypeKey);
    env->ReleaseStringChars(jJsonStr, rawJsonContent);
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
}

static bool processPageLevelTextWatermark(
        JNIEnv* env,
        jobject obj,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit
) {
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, dataPropsField, jsonClass, jsonInit, "watermarkProperties");
    if (!jJsonStr) return false;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    auto optStringValue = [&](const char* key) -> jstring {
        jstring jKey = env->NewStringUTF(key);
        jstring value = (jstring)env->CallObjectMethod(json, optS, jKey);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optBoolValue = [&](const char* key, bool fallback) -> bool {
        jstring jKey = env->NewStringUTF(key);
        bool value = env->CallBooleanMethod(json, optB, jKey, fallback ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
        env->DeleteLocalRef(jKey);
        return value;
    };

    jstring jType = optStringValue("watermarkType");
    jstring jMode = optStringValue("watermarkMode");
    jstring jText = optStringValue("text");
    jstring jFont = optStringValue("font");
    jstring jFontPath = optStringValue("fontPath");

    const char* typeStr = jType ? env->GetStringUTFChars(jType, nullptr) : nullptr;
    const char* modeStr = jMode ? env->GetStringUTFChars(jMode, nullptr) : nullptr;
    const bool isTextWatermark = !typeStr || strlen(typeStr) == 0 || strcmp(typeStr, "TEXT") == 0;
    const bool isRepeatedMode = !modeStr || strlen(modeStr) == 0 || strcmp(modeStr, "REPEATED") == 0;
    if (!isTextWatermark || !isRepeatedMode || !jText || env->GetStringLength(jText) == 0) {
        if (typeStr) env->ReleaseStringUTFChars(jType, typeStr);
        if (modeStr) env->ReleaseStringUTFChars(jMode, modeStr);
        if (jType) env->DeleteLocalRef(jType);
        if (jMode) env->DeleteLocalRef(jMode);
        if (jText) env->DeleteLocalRef(jText);
        if (jFont) env->DeleteLocalRef(jFont);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }
    if (typeStr) env->ReleaseStringUTFChars(jType, typeStr);
    if (modeStr) env->ReleaseStringUTFChars(jMode, modeStr);

    const jchar* textContent = env->GetStringChars(jText, nullptr);
    const char* fontName = jFont ? env->GetStringUTFChars(jFont, nullptr) : nullptr;
    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;
    const bool isBold = optBoolValue("bold", false);
    const bool isItalic = optBoolValue("italic", false);
    const std::string resolvedFontPath = ResolvePageLevelWatermarkFontPath(fontPath, isBold, isItalic);
    const char* outlineFontPath = resolvedFontPath.empty() ? nullptr : resolvedFontPath.c_str();

    FPDF_FONT loadedFont = nullptr;
    if (outlineFontPath && strlen(outlineFontPath) > 0) {
        FILE* f = fopen(outlineFontPath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fSize = ftell(f);
            rewind(f);
            if (fSize > 0) {
                std::vector<uint8_t> buffer(fSize);
                fread(buffer.data(), 1, fSize, f);
                loadedFont = FPDFText_LoadFont(doc, buffer.data(), fSize, FPDF_FONT_TRUETYPE, true);
            }
            fclose(f);
        }
    }

    const float pageWidth = FPDF_GetPageWidth(page);
    const float pageHeight = FPDF_GetPageHeight(page);
    const float canvasWidth = fmax((float)optDoubleValue("canvasWidth", pageWidth), 0.0001f);
    const float canvasHeight = fmax((float)optDoubleValue("canvasHeight", pageHeight), 0.0001f);
    const float pageScale = fmin(pageWidth / canvasWidth, pageHeight / canvasHeight);
    const float inverseBgScale = fmax((float)optDoubleValue("inverseBgScale", 1.0), 0.0001f);
    const float watermarkPdfScale = inverseBgScale * fmax(pageScale, 0.0001f);
    const float modelTextSize = fmax((float)optDoubleValue("fontSize", 12.0), 0.1f);
    const float scale = fmax(modelTextSize * watermarkPdfScale, 1.0f);
    const double rotation = optDoubleValue("rotation", 0.0);
    const double angleRad = rotation * M_PI / 180.0;
    const double cosA = cos(angleRad);
    const double sinA = sin(angleRad);
    const int textR = optIntValue("textColorR", 0);
    const int textG = optIntValue("textColorG", 0);
    const int textB = optIntValue("textColorB", 0);
    const float rawOpacity = fmax(0.0f, fmin((float)optDoubleValue("opacity", 1.0), 1.0f));
    const float repeatedWatermarkOpacityScale = 0.65f;
    const int textA = std::max(0, std::min(
            static_cast<int>(std::lround(rawOpacity * repeatedWatermarkOpacityScale * 255.0f)),
            255
    ));
    const float letterSpacing = fmax((float)optDoubleValue("letterSpacing", 0.0), 0.0f);
    const jsize textLength = env->GetStringLength(jText);

    if (!outlineFontPath || strlen(outlineFontPath) == 0) {
        env->ReleaseStringChars(jText, textContent);
        if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
        if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
        if (jType) env->DeleteLocalRef(jType);
        if (jMode) env->DeleteLocalRef(jMode);
        if (jText) env->DeleteLocalRef(jText);
        if (jFont) env->DeleteLocalRef(jFont);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    float tL = 0.0f;
    float tB = 0.0f;
    float tR = fmax((float)textLength * 0.6f, 1.0f);
    float tT = 1.0f;
    FPDF_PAGEOBJECT probeObj = loadedFont ? FPDFPageObj_CreateTextObj(doc, loadedFont, 1.0f) : nullptr;
    if (probeObj) {
        FPDFText_SetText(probeObj, (FPDF_WIDESTRING)textContent);
        FPDFPageObj_GetBounds(probeObj, &tL, &tB, &tR, &tT);
        FPDFPageObj_Destroy(probeObj);
    }

    float textWidth = fmax((tR - tL) * scale, scale);
    const float textHeight = fmax((tT - tB) * scale, scale);
    std::vector<float> charUnitWidths;
    if (loadedFont && fabs(letterSpacing) > 0.0001f && textLength > 1) {
        charUnitWidths.reserve(textLength);
        float measuredWidth = 0.0f;
        for (jsize charIndex = 0; charIndex < textLength; charIndex++) {
            FPDF_PAGEOBJECT charProbeObj = FPDFPageObj_CreateTextObj(doc, loadedFont, 1.0f);
            if (!charProbeObj) continue;
            jchar singleChar[2] = {textContent[charIndex], 0};
            FPDFText_SetText(charProbeObj, (FPDF_WIDESTRING)singleChar);
            float cL = 0.0f, cB = 0.0f, cR = 0.0f, cT = 0.0f;
            FPDFPageObj_GetBounds(charProbeObj, &cL, &cB, &cR, &cT);
            FPDFPageObj_Destroy(charProbeObj);
            const float charWidth = fmax(cR - cL, 0.0f);
            charUnitWidths.push_back(charWidth);
            measuredWidth += charWidth * scale;
        }
        const int letterGapCount = std::max(0, (int)textLength - 1);
        measuredWidth += letterGapCount * letterSpacing * scale;
        if (measuredWidth > 0.0f) {
            textWidth = measuredWidth;
        }
    }
    const float measuredTextWidth = (float)optDoubleValue("measuredTextWidth", 0.0);
    const float measuredTextHeight = (float)optDoubleValue("measuredTextHeight", 0.0);
    const float measuredSpacingOffset = (float)optDoubleValue("measuredSpacingOffset", 0.0);
    if (measuredTextWidth > 0.0f) {
        textWidth = fmax((measuredTextWidth + measuredSpacingOffset) * watermarkPdfScale, scale);
    }
    const float measuredTileHeight = fmax(
            measuredTextHeight > 0.0f
            ? measuredTextHeight * watermarkPdfScale
            : textHeight,
            scale
    );
    const float horizontalSpacing = fmax((float)optDoubleValue("horizontalSpacing", 0.0) * watermarkPdfScale, 0.0f);
    const float verticalSpacing = fmax((float)optDoubleValue("verticalSpacing", 0.0) * watermarkPdfScale, 0.0f);
    const float tileWidth = fmax(textWidth + horizontalSpacing, scale);
    const float tileHeight = fmax(measuredTileHeight + verticalSpacing, scale);
    const float centerX = pageWidth * 0.5f;
    const float centerY = pageHeight * 0.5f;
    const float diagonal = (float)hypot(pageWidth, pageHeight);
    const float minX = centerX - diagonal;
    const float maxX = centerX + diagonal;
    const float minY = centerY - diagonal;
    const float maxY = centerY + diagonal;
    const float skewX = isItalic ? 0.25f : 0.0f;
    const int maxStampTextObjects = 1200;
    const int estimatedColumns = std::max(1, (int)ceil((maxX - minX) / fmax(tileWidth, 0.0001f)) + 1);
    const int estimatedRows = std::max(1, (int)ceil((maxY - minY) / fmax(tileHeight, 0.0001f)) + 1);
    const int estimatedTiles = estimatedColumns * estimatedRows;
    if (estimatedTiles * std::max(1, (int)textLength) > maxStampTextObjects) {
        charUnitWidths.clear();
    }
    if (AppendPageLevelTextWatermarkGlyphPaths(
            page,
            outlineFontPath,
            textContent,
            textLength,
            textR,
            textG,
            textB,
            textA,
            isBold,
            scale,
            letterSpacing,
            textHeight,
            cosA,
            sinA,
            skewX,
            minX,
            maxX,
            minY,
            maxY,
            centerX,
            centerY,
            tileWidth,
            tileHeight,
            maxStampTextObjects
    )) {
        WM_LOGE("Native edit watermark saved with font glyph paths font=%s path=%s", fontName ? fontName : "", outlineFontPath);
        env->ReleaseStringChars(jText, textContent);
        if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
        if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
        if (jType) env->DeleteLocalRef(jType);
        if (jMode) env->DeleteLocalRef(jMode);
        if (jText) env->DeleteLocalRef(jText);
        if (jFont) env->DeleteLocalRef(jFont);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return true;
    }

    env->ReleaseStringChars(jText, textContent);
    if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
    if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
    if (jType) env->DeleteLocalRef(jType);
    if (jMode) env->DeleteLocalRef(jMode);
    if (jText) env->DeleteLocalRef(jText);
    if (jFont) env->DeleteLocalRef(jFont);
    if (jFontPath) env->DeleteLocalRef(jFontPath);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
    return false;
}

static bool SaveToolkitTextWatermarksWithPdfium(
        JNIEnv* env,
        const char* inputPath,
        const char* outputPath,
        jobjectArray watermarksArray,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit
) {
    if (!inputPath || !outputPath || !watermarksArray) return false;

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) return false;

    const int watermarkCount = env->GetArrayLength(watermarksArray);
    bool appliedAny = false;
    bool failed = false;

    for (int i = 0; i < watermarkCount; i++) {
        jobject obj = env->GetObjectArrayElement(watermarksArray, i);
        if (!obj) continue;

        RawPdfWatermarkSpec spec;
        if (!CollectPageLevelTextWatermarkPatternSpec(env, obj, dataPropsField, jsonClass, jsonInit, &spec)) {
            env->DeleteLocalRef(obj);
            failed = true;
            break;
        }
        if (spec.pageIndex < 0 || spec.pageIndex >= FPDF_GetPageCount(doc)) {
            env->DeleteLocalRef(obj);
            failed = true;
            break;
        }

        FPDF_PAGE page = FPDF_LoadPage(doc, spec.pageIndex);
        if (!page) {
            env->DeleteLocalRef(obj);
            failed = true;
            break;
        }

        const bool applied = processPageLevelTextWatermark(
                env,
                obj,
                doc,
                page,
                dataPropsField,
                jsonClass,
                jsonInit
        );
        if (applied) {
            FPDFPage_GenerateContent(page);
            appliedAny = true;
        } else {
            failed = true;
        }
        FPDF_ClosePage(page);
        env->DeleteLocalRef(obj);

        if (failed) break;
    }

    int success = JNI_FALSE;
    if (appliedAny && !failed) {
        FILE* file = fopen(outputPath, "wb");
        PdfFileWriter writer{ {1, WriteBlock}, file };
        success = (file) ? FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL) : JNI_FALSE;
        if (file) fclose(file);
    }

    FPDF_CloseDocument(doc);
    return success == JNI_TRUE;
}

// --- HELPER 2B: EDIT PANEL ADD TEXT CONTENT LOGIC ---
static void processFreeText(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit) {
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, textPropsField, jsonClass, jsonInit, "textProperties");
    if (!jJsonStr) {
        return;
    }

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json) {
        env->DeleteLocalRef(jJsonStr);
        return;
    }

    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optS2 = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    auto optStringValue = [&](const char* key, const char* fallback) -> jstring {
        jstring jKey = env->NewStringUTF(key);
        jstring jFallback = env->NewStringUTF(fallback);
        jstring value = (jstring)env->CallObjectMethod(json, optS2, jKey, jFallback);
        env->DeleteLocalRef(jKey);
        env->DeleteLocalRef(jFallback);
        return value;
    };
    auto optBoolValue = [&](const char* key, jboolean fallback) -> jboolean {
        jstring jKey = env->NewStringUTF(key);
        const jboolean value = env->CallBooleanMethod(json, optB, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    jstring jText = optStringValue("text", "");
    jstring jLayoutText = optStringValue("layoutText", "");
    jstring jFont = optStringValue("font", "Helvetica");
    jstring jAlign = optStringValue("alignment", "center");
    jstring jFontPath = optStringValue("fontPath", "");

    jboolean hasUnderline = optBoolValue("underline", JNI_FALSE);
    jboolean hasStrikeout = optBoolValue("strikeout", JNI_FALSE);
    jboolean isBold = optBoolValue("bold", JNI_FALSE);
    jboolean isItalic = optBoolValue("italic", JNI_FALSE);
    jboolean hasBg = optBoolValue("hasBackground", JNI_FALSE);

    double rotation = 0, jsonSize = 0, jsonWidth = 0, jsonHeight = 0;
    double jsonFontSpacing = 0, jsonLineHeight = 0, jsonFontAscent = 0, jsonTextPadding = 0;
    rotation = optDoubleValue("rotation", 0.0);
    jsonSize = optDoubleValue("size", 12.0);
    jsonWidth = optDoubleValue("width", 0.0);
    jsonHeight = optDoubleValue("height", 0.0);
    jsonFontSpacing = optDoubleValue("fontSpacing", 0.0);
    jsonLineHeight = optDoubleValue("lineHeight", 0.0);
    jsonFontAscent = optDoubleValue("fontAscent", 0.0);
    jsonTextPadding = optDoubleValue("textPadding", 0.0);

    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    const int defaultAlpha = alpha > 0 ? alpha : 255;
    const int textR = optIntValue("textColorR", r);
    const int textG = optIntValue("textColorG", g);
    const int textB = optIntValue("textColorB", b);
    const int textA = optIntValue("textColorA", defaultAlpha);

    if (!jText || env->GetStringLength(jText) == 0) {
        if (jFont) env->DeleteLocalRef(jFont);
        if (jText) env->DeleteLocalRef(jText);
        if (jLayoutText) env->DeleteLocalRef(jLayoutText);
        if (jAlign) env->DeleteLocalRef(jAlign);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return;
    }

    jstring jRenderedText = (jLayoutText && env->GetStringLength(jLayoutText) > 0)
                            ? jLayoutText
                            : jText;
    const jsize renderedTextLength = env->GetStringLength(jRenderedText);
    const jchar* rawRenderedText = env->GetStringChars(jRenderedText, nullptr);
    std::vector<std::vector<unsigned short>> textLines;
    std::vector<unsigned short> currentLine;
    if (rawRenderedText) {
        for (jsize index = 0; index < renderedTextLength; index++) {
            const unsigned short character = rawRenderedText[index];
            if (character == '\r' || character == '\n') {
                if (character == '\r' &&
                    index + 1 < renderedTextLength &&
                    rawRenderedText[index + 1] == '\n') {
                    index++;
                }
                currentLine.push_back(0);
                textLines.push_back(currentLine);
                currentLine.clear();
            } else {
                currentLine.push_back(character);
            }
        }
    }
    currentLine.push_back(0);
    textLines.push_back(currentLine);
    const char* fontName = jFont ? env->GetStringUTFChars(jFont, nullptr) : nullptr;
    const char* alignStr = jAlign ? env->GetStringUTFChars(jAlign, nullptr) : "center";
    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;

    float initialWidth = (jsonWidth > 0) ? (float)jsonWidth : fabs(rect.right - rect.left);
    float initialHeight = (jsonHeight > 0) ? (float)jsonHeight : fabs(rect.top - rect.bottom);
    double angleRad = rotation * M_PI / 180.0;
    float origCenterX = (rect.left + rect.right) / 2.0f;
    float origCenterY = (rect.bottom + rect.top) / 2.0f;
    float centerY = origCenterY;
    double cosA = cos(angleRad), sinA = sin(angleRad);

    if (hasBg) {
        FPDF_PAGEOBJECT bg = FPDFPageObj_CreateNewRect(-initialWidth / 2.0f, -initialHeight / 2.0f, initialWidth, initialHeight);
        FPDFPageObj_SetFillColor(
                bg,
                optIntValue("bgColorR", 255),
                optIntValue("bgColorG", 255),
                optIntValue("bgColorB", 255),
                (int)(optDoubleValue("bgOpacity", 0.0) * 255)
        );
        FPDFPath_SetDrawMode(bg, 1, JNI_FALSE);
        FPDFPageObj_Transform(bg, cosA, sinA, -sinA, cosA, origCenterX, centerY);
        FPDFPage_InsertObject(page, bg);
    }

    FPDF_FONT loadedFont = nullptr;
    if (fontPath) {
        FILE* f = fopen(fontPath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long fSize = ftell(f); rewind(f);
            std::vector<uint8_t> buffer(fSize); fread(buffer.data(), 1, fSize, f); fclose(f);
            loadedFont = FPDFText_LoadFont(doc, buffer.data(), fSize, FPDF_FONT_TRUETYPE, true);
        }
    }
    if (!loadedFont) {
        const char* fallbackFontName = "Helvetica";
        if (fontName) {
            if (strcmp(fontName, "serif") == 0 || strstr(fontName, "Serif") != nullptr || strstr(fontName, "serif") != nullptr) {
                fallbackFontName = "Times-Roman";
            } else if (strstr(fontName, "Mono") != nullptr || strstr(fontName, "mono") != nullptr || strstr(fontName, "Courier") != nullptr) {
                fallbackFontName = "Courier";
            }
        }
        loadedFont = FPDFText_LoadStandardFont(doc, fallbackFontName);
    }

    struct FreeTextLineBounds {
        float left = 0.0f;
        float width = 0.0f;
        FPDF_PAGEOBJECT textObject = nullptr;
    };
    std::vector<FreeTextLineBounds> lineBounds(textLines.size());
    float maxUnitLineWidth = 0.0f;
    for (size_t lineIndex = 0; lineIndex < textLines.size(); lineIndex++) {
        if (textLines[lineIndex].size() <= 1) continue;
        FPDF_PAGEOBJECT textObject = FPDFPageObj_CreateTextObj(doc, loadedFont, 1.0f);
        if (!textObject) continue;
        if (FPDFText_SetText(textObject, (FPDF_WIDESTRING)textLines[lineIndex].data())) {
            float lineLeft = 0.0f, lineBottom = 0.0f, lineRight = 0.0f, lineTop = 0.0f;
            if (FPDFPageObj_GetBounds(textObject, &lineLeft, &lineBottom, &lineRight, &lineTop)) {
                lineBounds[lineIndex].textObject = textObject;
                lineBounds[lineIndex].left = lineLeft;
                lineBounds[lineIndex].width = fmax(0.0f, lineRight - lineLeft);
                maxUnitLineWidth = fmax(maxUnitLineWidth, lineBounds[lineIndex].width);
            } else {
                FPDFPageObj_Destroy(textObject);
            }
        } else {
            FPDFPageObj_Destroy(textObject);
        }
    }

    const float requestedFontSize = (jsonSize > 0.0) ? (float)jsonSize : 12.0f;
    const float requestedFontSpacing = (jsonFontSpacing > 0.0)
                                       ? (float)jsonFontSpacing
                                       : requestedFontSize * 1.20f;
    const float requestedLineHeight = (jsonLineHeight > 0.0)
                                      ? (float)jsonLineHeight
                                      : requestedFontSpacing;
    const float requestedFontAscent = (jsonFontAscent < 0.0)
                                      ? (float)jsonFontAscent
                                      : -requestedFontSize * 0.80f;
    const float horizontalPadding = fmax(0.0f, (float)jsonTextPadding);
    const float availableWidth = fmax(0.1f, initialWidth - horizontalPadding);
    const float requestedBlockHeight = requestedFontSpacing +
                                       fmax(0.0f, (float)textLines.size() - 1.0f) *
                                       requestedLineHeight;

    float fitScale = 1.0f;
    const float requestedMaxLineWidth = maxUnitLineWidth * requestedFontSize;
    if (requestedMaxLineWidth > availableWidth) {
        fitScale = fmin(fitScale, availableWidth / requestedMaxLineWidth);
    }
    if (requestedBlockHeight > initialHeight) {
        fitScale = fmin(fitScale, initialHeight / requestedBlockHeight);
    }
    fitScale = fmax(0.01f, fmin(1.0f, fitScale));

    const float fontSize = requestedFontSize * fitScale;
    const float fontSpacing = requestedFontSpacing * fitScale;
    const float lineHeight = requestedLineHeight * fitScale;
    const float fontAscent = requestedFontAscent * fitScale;
    const float textBlockHeight = fontSpacing +
                                  fmax(0.0f, (float)textLines.size() - 1.0f) *
                                  lineHeight;
    const float firstBaselineY = (textBlockHeight / 2.0f) + fontAscent;
    const float skewX = isItalic ? 0.25f : 0.0f;
    const float transformA = (float)(cosA * fontSize);
    const float transformB = (float)(sinA * fontSize);
    const float transformC = (float)((cosA * skewX - sinA) * fontSize);
    const float transformD = (float)((sinA * skewX + cosA) * fontSize);

    for (size_t lineIndex = 0; lineIndex < textLines.size(); lineIndex++) {
        const float baselineY = firstBaselineY - ((float)lineIndex * lineHeight);
        const float textWidth = lineBounds[lineIndex].width * fontSize;
        float alignedLeft = -textWidth / 2.0f;
        if (strcmp(alignStr, "left") == 0) {
            alignedLeft = (-initialWidth / 2.0f) + (horizontalPadding / 2.0f);
        } else if (strcmp(alignStr, "right") == 0) {
            alignedLeft = (initialWidth / 2.0f) - (horizontalPadding / 2.0f) - textWidth;
        }

        FPDF_PAGEOBJECT textObj = lineBounds[lineIndex].textObject;
        if (textObj) {
                FPDFPageObj_SetFillColor(textObj, textR, textG, textB, textA);
                const float originX = alignedLeft - (lineBounds[lineIndex].left * fontSize);
                const float translatedX = origCenterX +
                                          (float)(originX * cosA - baselineY * sinA);
                const float translatedY = centerY +
                                          (float)(originX * sinA + baselineY * cosA);
                FPDFPageObj_Transform(
                        textObj,
                        transformA,
                        transformB,
                        transformC,
                        transformD,
                        translatedX,
                        translatedY
                );

                if (isBold) {
                    FPDFPageObj_SetStrokeColor(textObj, textR, textG, textB, textA);
                    FPDFPageObj_SetStrokeWidth(textObj, fmax(0.35f, fontSize * 0.04f));
                    FPDFTextObj_SetTextRenderMode(textObj, FPDF_TEXTRENDERMODE_FILL_STROKE);
                }
                FPDFPage_InsertObject(page, textObj);
        }

        auto drawDecoration = [&](float localY) {
            if (textWidth <= 0.0f) return;
            FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(0.0f, 0.0f);
            if (!line) return;
            FPDFPath_LineTo(line, textWidth, 0.0f);
            const float translatedX = origCenterX +
                                      (float)(alignedLeft * cosA - localY * sinA);
            const float translatedY = centerY +
                                      (float)(alignedLeft * sinA + localY * cosA);
            FPDFPageObj_Transform(
                    line,
                    (float)cosA,
                    (float)sinA,
                    (float)-sinA,
                    (float)cosA,
                    translatedX,
                    translatedY
            );
            FPDFPageObj_SetStrokeColor(line, textR, textG, textB, textA);
            FPDFPageObj_SetStrokeWidth(line, fmax(0.35f, fontSize * 0.04f));
            FPDFPath_SetDrawMode(line, 0, JNI_TRUE);
            FPDFPage_InsertObject(page, line);
        };
        if (hasUnderline) drawDecoration(baselineY - (fontSize * 0.12f));
        if (hasStrikeout) drawDecoration(baselineY + (fontSize * 0.30f));
    }

    if (rawRenderedText) env->ReleaseStringChars(jRenderedText, rawRenderedText);
    if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
    if (jAlign && alignStr) env->ReleaseStringUTFChars(jAlign, alignStr);
    if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
    if (jFont) env->DeleteLocalRef(jFont);
    if (jText) env->DeleteLocalRef(jText);
    if (jLayoutText) env->DeleteLocalRef(jLayoutText);
    if (jAlign) env->DeleteLocalRef(jAlign);
    if (jFontPath) env->DeleteLocalRef(jFontPath);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
}

// for sticker tff to pdf path conversion code----
struct GlyphOutlinePoint {
    double x;
    double y;
    bool onCurve;
};

struct GlyphOutlineContour {
    std::vector<GlyphOutlinePoint> points;
};

struct GlyphOutline {
    std::vector<GlyphOutlineContour> contours;
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    bool hasBounds = false;
};

struct GlyphAffine {
    double a;
    double b;
    double c;
    double d;
    double e;
    double f;
};

struct TrueTypeTable {
    uint32_t offset;
    uint32_t length;
};

static GlyphAffine GlyphIdentityMatrix() {
    return {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
}

static GlyphAffine GlyphMultiplyMatrix(const GlyphAffine& first, const GlyphAffine& second) {
    return {
            first.a * second.a + first.c * second.b,
            first.b * second.a + first.d * second.b,
            first.a * second.c + first.c * second.d,
            first.b * second.c + first.d * second.d,
            first.a * second.e + first.c * second.f + first.e,
            first.b * second.e + first.d * second.f + first.f
    };
}

static GlyphOutlinePoint GlyphTransformPoint(const GlyphAffine& matrix, const GlyphOutlinePoint& point) {
    return {
            matrix.a * point.x + matrix.c * point.y + matrix.e,
            matrix.b * point.x + matrix.d * point.y + matrix.f,
            point.onCurve
    };
}

static bool ReadFileBytes(const char* path, std::vector<uint8_t>* outBytes) {
    if (!path || !outBytes || strlen(path) == 0) return false;

    FILE* file = fopen(path, "rb");
    if (!file) return false;

    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    rewind(file);
    if (fileSize <= 0) {
        fclose(file);
        return false;
    }

    outBytes->assign(static_cast<size_t>(fileSize), 0);
    const size_t readSize = fread(outBytes->data(), 1, outBytes->size(), file);
    fclose(file);
    return readSize == outBytes->size();
}

class TrueTypeGlyphReader {
public:
    explicit TrueTypeGlyphReader(const std::vector<uint8_t>& bytes) : data(bytes) {}

    bool load() {
        if (data.size() < 12) return false;

        uint16_t numTables = 0;
        if (!readU16(4, &numTables)) return false;
        const size_t tableDirectoryEnd = 12 + (static_cast<size_t>(numTables) * 16);
        if (tableDirectoryEnd > data.size()) return false;

        for (uint16_t i = 0; i < numTables; i++) {
            const size_t recordOffset = 12 + (static_cast<size_t>(i) * 16);
            uint32_t tableOffset = 0;
            uint32_t tableLength = 0;
            if (!readU32(recordOffset + 8, &tableOffset) ||
                !readU32(recordOffset + 12, &tableLength)) {
                return false;
            }
            if (!rangeValid(tableOffset, tableLength)) continue;

            std::string tag(reinterpret_cast<const char*>(&data[recordOffset]), 4);
            tables[tag] = {tableOffset, tableLength};
        }

        TrueTypeTable headTable;
        TrueTypeTable maxpTable;
        if (!getTable("head", &headTable) ||
            !getTable("maxp", &maxpTable) ||
            !getTable("loca", nullptr) ||
            !getTable("glyf", nullptr) ||
            !getTable("cmap", nullptr)) {
            return false;
        }

        uint16_t units = 0;
        int16_t locaFormat = 0;
        uint16_t glyphCount = 0;
        if (!readU16(headTable.offset + 18, &units) ||
            !readS16(headTable.offset + 50, &locaFormat) ||
            !readU16(maxpTable.offset + 4, &glyphCount)) {
            return false;
        }
        unitsPerEm = units > 0 ? units : 1000;
        indexToLocFormat = locaFormat;
        numGlyphs = glyphCount;
        return numGlyphs > 0 && (indexToLocFormat == 0 || indexToLocFormat == 1);
    }

    bool loadGlyphForCodepoint(uint32_t codepoint, GlyphOutline* outline) const {
        if (!outline) return false;

        uint32_t glyphIndex = 0;
        if (!mapCodepointToGlyph(codepoint, &glyphIndex) || glyphIndex == 0 || glyphIndex >= numGlyphs) {
            return false;
        }

        *outline = GlyphOutline();
        if (!parseGlyph(glyphIndex, GlyphIdentityMatrix(), outline, 0)) {
            return false;
        }
        return outline->hasBounds && !outline->contours.empty();
    }

    uint16_t getUnitsPerEm() const {
        return unitsPerEm > 0 ? unitsPerEm : 1000;
    }

private:
    const std::vector<uint8_t>& data;
    std::map<std::string, TrueTypeTable> tables;
    uint16_t unitsPerEm = 1000;
    int16_t indexToLocFormat = 0;
    uint16_t numGlyphs = 0;

    bool rangeValid(size_t offset, size_t length) const {
        return offset <= data.size() && length <= data.size() - offset;
    }

    bool readU8(size_t offset, uint8_t* out) const {
        if (!out || !rangeValid(offset, 1)) return false;
        *out = data[offset];
        return true;
    }

    bool readS8(size_t offset, int8_t* out) const {
        uint8_t value = 0;
        if (!readU8(offset, &value) || !out) return false;
        *out = static_cast<int8_t>(value);
        return true;
    }

    bool readU16(size_t offset, uint16_t* out) const {
        if (!out || !rangeValid(offset, 2)) return false;
        *out = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                     static_cast<uint16_t>(data[offset + 1]));
        return true;
    }

    bool readS16(size_t offset, int16_t* out) const {
        uint16_t value = 0;
        if (!readU16(offset, &value) || !out) return false;
        *out = static_cast<int16_t>(value);
        return true;
    }

    bool readU32(size_t offset, uint32_t* out) const {
        if (!out || !rangeValid(offset, 4)) return false;
        *out = (static_cast<uint32_t>(data[offset]) << 24) |
               (static_cast<uint32_t>(data[offset + 1]) << 16) |
               (static_cast<uint32_t>(data[offset + 2]) << 8) |
               static_cast<uint32_t>(data[offset + 3]);
        return true;
    }

    bool getTable(const char* tag, TrueTypeTable* out) const {
        auto it = tables.find(tag);
        if (it == tables.end()) return false;
        if (out) *out = it->second;
        return true;
    }

    bool mapCodepointToGlyph(uint32_t codepoint, uint32_t* glyphIndex) const {
        TrueTypeTable cmapTable;
        if (!glyphIndex || !getTable("cmap", &cmapTable) || cmapTable.length < 4) return false;

        uint16_t subtableCount = 0;
        if (!readU16(cmapTable.offset + 2, &subtableCount)) return false;
        if (cmapTable.length < 4 + static_cast<uint32_t>(subtableCount) * 8) return false;

        struct CmapCandidate {
            uint32_t offset;
            uint16_t platform;
            uint16_t encoding;
            uint16_t format;
            int score;
        };
        std::vector<CmapCandidate> candidates;
        for (uint16_t i = 0; i < subtableCount; i++) {
            const size_t recordOffset = cmapTable.offset + 4 + (static_cast<size_t>(i) * 8);
            uint16_t platform = 0;
            uint16_t encoding = 0;
            uint32_t relativeOffset = 0;
            if (!readU16(recordOffset, &platform) ||
                !readU16(recordOffset + 2, &encoding) ||
                !readU32(recordOffset + 4, &relativeOffset)) {
                continue;
            }
            if (relativeOffset >= cmapTable.length) continue;

            const uint32_t subtableOffset = cmapTable.offset + relativeOffset;
            uint16_t format = 0;
            if (!readU16(subtableOffset, &format)) continue;
            if (format != 0 && format != 4 && format != 12) continue;

            int score = 0;
            if (platform == 3 && encoding == 10) score += 40;
            if (platform == 3 && encoding == 1) score += 30;
            if (platform == 0) score += 20;
            if (format == 12) score += 12;
            if (format == 4) score += 8;
            if (format == 0) score += 1;
            candidates.push_back({subtableOffset, platform, encoding, format, score});
        }

        std::sort(candidates.begin(), candidates.end(), [](const CmapCandidate& lhs, const CmapCandidate& rhs) {
            return lhs.score > rhs.score;
        });

        for (const auto& candidate : candidates) {
            uint32_t mappedGlyph = 0;
            bool mapped = false;
            if (candidate.format == 12) {
                mapped = mapFormat12(candidate.offset, codepoint, &mappedGlyph);
            } else if (candidate.format == 4) {
                mapped = mapFormat4(candidate.offset, codepoint, &mappedGlyph);
            } else if (candidate.format == 0) {
                mapped = mapFormat0(candidate.offset, codepoint, &mappedGlyph);
            }
            if (mapped && mappedGlyph > 0) {
                *glyphIndex = mappedGlyph;
                return true;
            }
        }
        return false;
    }

    bool mapFormat0(uint32_t tableOffset, uint32_t codepoint, uint32_t* glyphIndex) const {
        if (!glyphIndex || codepoint > 255 || !rangeValid(tableOffset, 262)) return false;
        *glyphIndex = data[tableOffset + 6 + codepoint];
        return *glyphIndex > 0;
    }

    bool mapFormat4(uint32_t tableOffset, uint32_t codepoint, uint32_t* glyphIndex) const {
        if (!glyphIndex || codepoint > 0xFFFF || !rangeValid(tableOffset, 16)) return false;

        uint16_t length = 0;
        uint16_t segCountX2 = 0;
        if (!readU16(tableOffset + 2, &length) ||
            !readU16(tableOffset + 6, &segCountX2) ||
            length < 16 ||
            !rangeValid(tableOffset, length)) {
            return false;
        }

        const uint16_t segCount = segCountX2 / 2;
        const size_t endCodeOffset = tableOffset + 14;
        const size_t startCodeOffset = endCodeOffset + (static_cast<size_t>(segCount) * 2) + 2;
        const size_t idDeltaOffset = startCodeOffset + (static_cast<size_t>(segCount) * 2);
        const size_t idRangeOffsetOffset = idDeltaOffset + (static_cast<size_t>(segCount) * 2);
        if (!rangeValid(idRangeOffsetOffset, static_cast<size_t>(segCount) * 2)) return false;

        for (uint16_t i = 0; i < segCount; i++) {
            uint16_t endCode = 0;
            uint16_t startCode = 0;
            int16_t idDelta = 0;
            uint16_t idRangeOffset = 0;
            if (!readU16(endCodeOffset + (static_cast<size_t>(i) * 2), &endCode) ||
                !readU16(startCodeOffset + (static_cast<size_t>(i) * 2), &startCode) ||
                !readS16(idDeltaOffset + (static_cast<size_t>(i) * 2), &idDelta) ||
                !readU16(idRangeOffsetOffset + (static_cast<size_t>(i) * 2), &idRangeOffset)) {
                continue;
            }

            if (codepoint < startCode || codepoint > endCode) continue;

            uint32_t mappedGlyph = 0;
            if (idRangeOffset == 0) {
                mappedGlyph = (codepoint + idDelta) & 0xFFFF;
            } else {
                const size_t glyphOffset =
                        idRangeOffsetOffset +
                        (static_cast<size_t>(i) * 2) +
                        idRangeOffset +
                        ((codepoint - startCode) * 2);
                if (!rangeValid(glyphOffset, 2) || glyphOffset + 2 > tableOffset + length) {
                    return false;
                }
                uint16_t glyphValue = 0;
                if (!readU16(glyphOffset, &glyphValue)) return false;
                mappedGlyph = glyphValue == 0 ? 0 : ((glyphValue + idDelta) & 0xFFFF);
            }

            *glyphIndex = mappedGlyph;
            return mappedGlyph > 0;
        }
        return false;
    }

    bool mapFormat12(uint32_t tableOffset, uint32_t codepoint, uint32_t* glyphIndex) const {
        if (!glyphIndex || !rangeValid(tableOffset, 16)) return false;

        uint32_t length = 0;
        uint32_t groupCount = 0;
        if (!readU32(tableOffset + 4, &length) ||
            !readU32(tableOffset + 12, &groupCount) ||
            length < 16 ||
            !rangeValid(tableOffset, length)) {
            return false;
        }

        const size_t groupsOffset = tableOffset + 16;
        if (!rangeValid(groupsOffset, static_cast<size_t>(groupCount) * 12)) return false;

        for (uint32_t i = 0; i < groupCount; i++) {
            const size_t groupOffset = groupsOffset + (static_cast<size_t>(i) * 12);
            uint32_t startChar = 0;
            uint32_t endChar = 0;
            uint32_t startGlyph = 0;
            if (!readU32(groupOffset, &startChar) ||
                !readU32(groupOffset + 4, &endChar) ||
                !readU32(groupOffset + 8, &startGlyph)) {
                continue;
            }
            if (codepoint >= startChar && codepoint <= endChar) {
                *glyphIndex = startGlyph + (codepoint - startChar);
                return *glyphIndex > 0;
            }
        }
        return false;
    }

    bool glyphDataRange(uint32_t glyphIndex, uint32_t* glyphOffset, uint32_t* glyphLength) const {
        if (!glyphOffset || !glyphLength || glyphIndex >= numGlyphs) return false;

        TrueTypeTable locaTable;
        TrueTypeTable glyfTable;
        if (!getTable("loca", &locaTable) || !getTable("glyf", &glyfTable)) return false;

        uint32_t start = 0;
        uint32_t end = 0;
        if (indexToLocFormat == 0) {
            uint16_t startShort = 0;
            uint16_t endShort = 0;
            const size_t locaOffset = locaTable.offset + (static_cast<size_t>(glyphIndex) * 2);
            if (!readU16(locaOffset, &startShort) || !readU16(locaOffset + 2, &endShort)) return false;
            start = static_cast<uint32_t>(startShort) * 2;
            end = static_cast<uint32_t>(endShort) * 2;
        } else {
            const size_t locaOffset = locaTable.offset + (static_cast<size_t>(glyphIndex) * 4);
            if (!readU32(locaOffset, &start) || !readU32(locaOffset + 4, &end)) return false;
        }

        if (end <= start || end > glyfTable.length) return false;
        *glyphOffset = glyfTable.offset + start;
        *glyphLength = end - start;
        return rangeValid(*glyphOffset, *glyphLength);
    }

    void addContour(GlyphOutline* outline, const GlyphOutlineContour& contour) const {
        if (!outline || contour.points.empty()) return;
        for (const auto& point : contour.points) {
            if (!outline->hasBounds) {
                outline->minX = outline->maxX = point.x;
                outline->minY = outline->maxY = point.y;
                outline->hasBounds = true;
            } else {
                outline->minX = std::min(outline->minX, point.x);
                outline->maxX = std::max(outline->maxX, point.x);
                outline->minY = std::min(outline->minY, point.y);
                outline->maxY = std::max(outline->maxY, point.y);
            }
        }
        outline->contours.push_back(contour);
    }

    bool parseGlyph(uint32_t glyphIndex, const GlyphAffine& transform, GlyphOutline* outline, int depth) const {
        if (!outline || depth > 12) return false;

        uint32_t glyphOffset = 0;
        uint32_t glyphLength = 0;
        if (!glyphDataRange(glyphIndex, &glyphOffset, &glyphLength) || glyphLength < 10) return false;

        int16_t contourCount = 0;
        if (!readS16(glyphOffset, &contourCount)) return false;
        if (contourCount >= 0) {
            return parseSimpleGlyph(glyphOffset, glyphLength, contourCount, transform, outline);
        }
        return parseCompositeGlyph(glyphOffset, glyphLength, transform, outline, depth);
    }

    bool parseSimpleGlyph(
            uint32_t glyphOffset,
            uint32_t glyphLength,
            int16_t contourCount,
            const GlyphAffine& transform,
            GlyphOutline* outline
    ) const {
        if (contourCount <= 0) return false;

        const size_t endPointsOffset = glyphOffset + 10;
        if (!rangeValid(endPointsOffset, static_cast<size_t>(contourCount) * 2)) return false;

        std::vector<uint16_t> endPoints(static_cast<size_t>(contourCount));
        for (int i = 0; i < contourCount; i++) {
            if (!readU16(endPointsOffset + (static_cast<size_t>(i) * 2), &endPoints[i])) return false;
        }

        const uint16_t pointCount = static_cast<uint16_t>(endPoints.back() + 1);
        const size_t instructionLengthOffset = endPointsOffset + (static_cast<size_t>(contourCount) * 2);
        uint16_t instructionLength = 0;
        if (!readU16(instructionLengthOffset, &instructionLength)) return false;

        size_t cursor = instructionLengthOffset + 2 + instructionLength;
        const size_t glyphEnd = glyphOffset + glyphLength;
        if (cursor > glyphEnd) return false;

        std::vector<uint8_t> flags;
        flags.reserve(pointCount);
        while (flags.size() < pointCount && cursor < glyphEnd) {
            uint8_t flag = 0;
            if (!readU8(cursor++, &flag)) return false;
            flags.push_back(flag);
            if ((flag & 0x08) != 0) {
                uint8_t repeatCount = 0;
                if (!readU8(cursor++, &repeatCount)) return false;
                for (uint8_t repeatIndex = 0; repeatIndex < repeatCount && flags.size() < pointCount; repeatIndex++) {
                    flags.push_back(flag);
                }
            }
        }
        if (flags.size() != pointCount) return false;

        std::vector<int> xCoordinates(pointCount, 0);
        std::vector<int> yCoordinates(pointCount, 0);
        int currentX = 0;
        for (uint16_t i = 0; i < pointCount; i++) {
            int delta = 0;
            if ((flags[i] & 0x02) != 0) {
                uint8_t value = 0;
                if (!readU8(cursor++, &value)) return false;
                delta = ((flags[i] & 0x10) != 0) ? value : -static_cast<int>(value);
            } else if ((flags[i] & 0x10) == 0) {
                int16_t value = 0;
                if (!readS16(cursor, &value)) return false;
                cursor += 2;
                delta = value;
            }
            currentX += delta;
            xCoordinates[i] = currentX;
        }

        int currentY = 0;
        for (uint16_t i = 0; i < pointCount; i++) {
            int delta = 0;
            if ((flags[i] & 0x04) != 0) {
                uint8_t value = 0;
                if (!readU8(cursor++, &value)) return false;
                delta = ((flags[i] & 0x20) != 0) ? value : -static_cast<int>(value);
            } else if ((flags[i] & 0x20) == 0) {
                int16_t value = 0;
                if (!readS16(cursor, &value)) return false;
                cursor += 2;
                delta = value;
            }
            currentY += delta;
            yCoordinates[i] = currentY;
        }

        uint16_t startPoint = 0;
        for (int contourIndex = 0; contourIndex < contourCount; contourIndex++) {
            const uint16_t endPoint = endPoints[contourIndex];
            if (endPoint < startPoint || endPoint >= pointCount) return false;

            GlyphOutlineContour contour;
            contour.points.reserve(static_cast<size_t>(endPoint - startPoint) + 1);
            for (uint16_t pointIndex = startPoint; pointIndex <= endPoint; pointIndex++) {
                GlyphOutlinePoint point = {
                        static_cast<double>(xCoordinates[pointIndex]),
                        static_cast<double>(yCoordinates[pointIndex]),
                        (flags[pointIndex] & 0x01) != 0
                };
                contour.points.push_back(GlyphTransformPoint(transform, point));
            }
            addContour(outline, contour);
            startPoint = static_cast<uint16_t>(endPoint + 1);
        }

        return true;
    }

    static double readF2Dot14(int16_t value) {
        return static_cast<double>(value) / 16384.0;
    }

    bool parseCompositeGlyph(
            uint32_t glyphOffset,
            uint32_t glyphLength,
            const GlyphAffine& parentTransform,
            GlyphOutline* outline,
            int depth
    ) const {
        const size_t glyphEnd = glyphOffset + glyphLength;
        size_t cursor = glyphOffset + 10;
        bool parsedAny = false;
        bool moreComponents = true;

        while (moreComponents && cursor + 4 <= glyphEnd) {
            uint16_t flags = 0;
            uint16_t componentGlyph = 0;
            if (!readU16(cursor, &flags) || !readU16(cursor + 2, &componentGlyph)) return false;
            cursor += 4;

            int arg1 = 0;
            int arg2 = 0;
            if ((flags & 0x0001) != 0) {
                int16_t first = 0;
                int16_t second = 0;
                if (!readS16(cursor, &first) || !readS16(cursor + 2, &second)) return false;
                cursor += 4;
                arg1 = first;
                arg2 = second;
            } else {
                int8_t first = 0;
                int8_t second = 0;
                if (!readS8(cursor, &first) || !readS8(cursor + 1, &second)) return false;
                cursor += 2;
                arg1 = first;
                arg2 = second;
            }

            if ((flags & 0x0002) == 0) {
                return false;
            }

            GlyphAffine componentTransform = {1.0, 0.0, 0.0, 1.0, static_cast<double>(arg1), static_cast<double>(arg2)};
            if ((flags & 0x0008) != 0) {
                int16_t scale = 0;
                if (!readS16(cursor, &scale)) return false;
                cursor += 2;
                componentTransform.a = readF2Dot14(scale);
                componentTransform.d = readF2Dot14(scale);
            } else if ((flags & 0x0040) != 0) {
                int16_t xScale = 0;
                int16_t yScale = 0;
                if (!readS16(cursor, &xScale) || !readS16(cursor + 2, &yScale)) return false;
                cursor += 4;
                componentTransform.a = readF2Dot14(xScale);
                componentTransform.d = readF2Dot14(yScale);
            } else if ((flags & 0x0080) != 0) {
                int16_t xScale = 0;
                int16_t scale01 = 0;
                int16_t scale10 = 0;
                int16_t yScale = 0;
                if (!readS16(cursor, &xScale) ||
                    !readS16(cursor + 2, &scale01) ||
                    !readS16(cursor + 4, &scale10) ||
                    !readS16(cursor + 6, &yScale)) {
                    return false;
                }
                cursor += 8;
                componentTransform.a = readF2Dot14(xScale);
                componentTransform.c = readF2Dot14(scale01);
                componentTransform.b = readF2Dot14(scale10);
                componentTransform.d = readF2Dot14(yScale);
            }

            const GlyphAffine combinedTransform = GlyphMultiplyMatrix(parentTransform, componentTransform);
            if (!parseGlyph(componentGlyph, combinedTransform, outline, depth + 1)) {
                return false;
            }
            parsedAny = true;
            moreComponents = (flags & 0x0020) != 0;
        }

        return parsedAny;
    }
};

static GlyphOutlinePoint MidPoint(const GlyphOutlinePoint& first, const GlyphOutlinePoint& second) {
    return {
            (first.x + second.x) * 0.5,
            (first.y + second.y) * 0.5,
            true
    };
}

static void AppendQuadraticAsCubic(
        FPDF_PAGEOBJECT path,
        const GlyphOutlinePoint& start,
        const GlyphOutlinePoint& control,
        const GlyphOutlinePoint& end
) {
    const double cp1X = start.x + ((control.x - start.x) * (2.0 / 3.0));
    const double cp1Y = start.y + ((control.y - start.y) * (2.0 / 3.0));
    const double cp2X = end.x + ((control.x - end.x) * (2.0 / 3.0));
    const double cp2Y = end.y + ((control.y - end.y) * (2.0 / 3.0));
    FPDFPath_BezierTo(
            path,
            static_cast<float>(cp1X),
            static_cast<float>(cp1Y),
            static_cast<float>(cp2X),
            static_cast<float>(cp2Y),
            static_cast<float>(end.x),
            static_cast<float>(end.y)
    );
}

static bool AppendGlyphContourToPdfPath(FPDF_PAGEOBJECT path, const GlyphOutlineContour& contour) {
    if (!path || contour.points.empty()) return false;

    const int pointCount = static_cast<int>(contour.points.size());
    const GlyphOutlinePoint& first = contour.points.front();
    const GlyphOutlinePoint& last = contour.points.back();

    GlyphOutlinePoint start = first;
    int index = 1;
    int consumed = 1;
    if (!first.onCurve) {
        if (last.onCurve) {
            start = last;
            index = 0;
            consumed = 1;
        } else {
            start = MidPoint(last, first);
            index = 0;
            consumed = 0;
        }
    }

    FPDFPath_MoveTo(path, static_cast<float>(start.x), static_cast<float>(start.y));
    GlyphOutlinePoint current = start;

    while (consumed < pointCount) {
        const GlyphOutlinePoint& point = contour.points[index % pointCount];
        if (point.onCurve) {
            FPDFPath_LineTo(path, static_cast<float>(point.x), static_cast<float>(point.y));
            current = point;
            index++;
            consumed++;
        } else {
            const GlyphOutlinePoint& next = contour.points[(index + 1) % pointCount];
            if (next.onCurve) {
                AppendQuadraticAsCubic(path, current, point, next);
                current = next;
                index += 2;
                consumed += 2;
            } else {
                const GlyphOutlinePoint midpoint = MidPoint(point, next);
                AppendQuadraticAsCubic(path, current, point, midpoint);
                current = midpoint;
                index++;
                consumed++;
            }
        }
    }

    FPDFPath_Close(path);
    return true;
}

static FPDF_PAGEOBJECT CreateGlyphPathObject(const GlyphOutline& outline) {
    if (!outline.hasBounds || outline.contours.empty()) return nullptr;

    FPDF_PAGEOBJECT path = nullptr;
    for (const auto& contour : outline.contours) {
        if (contour.points.empty()) continue;
        if (!path) {
            GlyphOutlinePoint start = contour.points.front();
            if (!start.onCurve) {
                const GlyphOutlinePoint& last = contour.points.back();
                start = last.onCurve ? last : MidPoint(last, start);
            }
            path = FPDFPageObj_CreateNewPath(static_cast<float>(start.x), static_cast<float>(start.y));
            if (!path) return nullptr;
        }
        AppendGlyphContourToPdfPath(path, contour);
    }

    return path;
}

static GlyphOutlineContour TranslateGlyphContour(
        const GlyphOutlineContour& contour,
        double offsetX,
        double offsetY
) {
    GlyphOutlineContour translated;
    translated.points.reserve(contour.points.size());
    for (const auto& point : contour.points) {
        translated.points.push_back({
                point.x + offsetX,
                point.y + offsetY,
                point.onCurve
        });
    }
    return translated;
}

static FPDF_PAGEOBJECT CreateWatermarkTextPathObject(
        const std::vector<GlyphOutline>& glyphs,
        const std::vector<bool>& spaces,
        double unitsPerEm,
        double letterSpacing
) {
    if (glyphs.empty() || glyphs.size() != spaces.size()) return nullptr;

    FPDF_PAGEOBJECT path = nullptr;
    double cursorX = 0.0;
    const double letterSpacingUnits = letterSpacing * unitsPerEm;
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (spaces[i]) {
            cursorX += unitsPerEm * 0.35 + letterSpacingUnits;
            continue;
        }

        const GlyphOutline& glyph = glyphs[i];
        if (!glyph.hasBounds || glyph.contours.empty()) continue;

        const double offsetX = cursorX - glyph.minX;
        const double offsetY = -glyph.minY;
        for (const auto& contour : glyph.contours) {
            if (contour.points.empty()) continue;
            GlyphOutlineContour translated = TranslateGlyphContour(contour, offsetX, offsetY);
            if (!path) {
                GlyphOutlinePoint start = translated.points.front();
                if (!start.onCurve) {
                    const GlyphOutlinePoint& last = translated.points.back();
                    start = last.onCurve ? last : MidPoint(last, start);
                }
                path = FPDFPageObj_CreateNewPath(static_cast<float>(start.x), static_cast<float>(start.y));
                if (!path) return nullptr;
            }
            AppendGlyphContourToPdfPath(path, translated);
        }
        cursorX += (glyph.maxX - glyph.minX) + letterSpacingUnits;
    }

    return path;
}

static std::vector<uint32_t> DecodeUtf8Codepoints(const std::string& text) {
    std::vector<uint32_t> codepoints;
    for (size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 0x80) {
            codepoints.push_back(ch);
            i++;
        } else if ((ch & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoints.push_back(((ch & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F));
            i += 2;
        } else if ((ch & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoints.push_back(
                    ((ch & 0x0F) << 12) |
                    ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(text[i + 2]) & 0x3F)
            );
            i += 3;
        } else if ((ch & 0xF8) == 0xF0 && i + 3 < text.size()) {
            codepoints.push_back(
                    ((ch & 0x07) << 18) |
                    ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
                    ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(text[i + 3]) & 0x3F)
            );
            i += 4;
        } else {
            i++;
        }
    }
    return codepoints;
}

static void AppendPdfPathQuadraticAsCubic(
        std::ostringstream* stream,
        double scaleX,
        double scaleY,
        double offsetX,
        double offsetY,
        const GlyphOutlinePoint& start,
        const GlyphOutlinePoint& control,
        const GlyphOutlinePoint& end
) {
    const double cp1X = start.x + ((control.x - start.x) * (2.0 / 3.0));
    const double cp1Y = start.y + ((control.y - start.y) * (2.0 / 3.0));
    const double cp2X = end.x + ((control.x - end.x) * (2.0 / 3.0));
    const double cp2Y = end.y + ((control.y - end.y) * (2.0 / 3.0));
    *stream << FormatPdfFloat(static_cast<float>(offsetX + cp1X * scaleX)) << ' '
            << FormatPdfFloat(static_cast<float>(offsetY + cp1Y * scaleY)) << ' '
            << FormatPdfFloat(static_cast<float>(offsetX + cp2X * scaleX)) << ' '
            << FormatPdfFloat(static_cast<float>(offsetY + cp2Y * scaleY)) << ' '
            << FormatPdfFloat(static_cast<float>(offsetX + end.x * scaleX)) << ' '
            << FormatPdfFloat(static_cast<float>(offsetY + end.y * scaleY)) << " c\n";
}

static bool AppendGlyphContourToPdfStream(
        std::ostringstream* stream,
        const GlyphOutlineContour& contour,
        double scaleX,
        double scaleY,
        double offsetX,
        double offsetY
) {
    if (!stream || contour.points.empty()) return false;

    const int pointCount = static_cast<int>(contour.points.size());
    const GlyphOutlinePoint& first = contour.points.front();
    const GlyphOutlinePoint& last = contour.points.back();
    GlyphOutlinePoint start = first;
    int index = 1;
    int consumed = 1;
    if (!first.onCurve) {
        if (last.onCurve) {
            start = last;
            index = 0;
        } else {
            start = MidPoint(last, first);
            index = 0;
            consumed = 0;
        }
    }

    *stream << FormatPdfFloat(static_cast<float>(offsetX + start.x * scaleX)) << ' '
            << FormatPdfFloat(static_cast<float>(offsetY + start.y * scaleY)) << " m\n";
    GlyphOutlinePoint current = start;
    while (consumed < pointCount) {
        const GlyphOutlinePoint& point = contour.points[index % pointCount];
        if (point.onCurve) {
            *stream << FormatPdfFloat(static_cast<float>(offsetX + point.x * scaleX)) << ' '
                    << FormatPdfFloat(static_cast<float>(offsetY + point.y * scaleY)) << " l\n";
            current = point;
            index++;
            consumed++;
        } else {
            const GlyphOutlinePoint& next = contour.points[(index + 1) % pointCount];
            if (next.onCurve) {
                AppendPdfPathQuadraticAsCubic(stream, scaleX, scaleY, offsetX, offsetY, current, point, next);
                current = next;
                index += 2;
                consumed += 2;
            } else {
                const GlyphOutlinePoint midpoint = MidPoint(point, next);
                AppendPdfPathQuadraticAsCubic(stream, scaleX, scaleY, offsetX, offsetY, current, point, midpoint);
                current = midpoint;
                index++;
                consumed++;
            }
        }
    }
    *stream << "h\n";
    return true;
}

static std::string BuildPdfWatermarkTextPathPatternStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
) {
    if (spec.textPathData.empty() || spec.textPathWidth <= 0.0f || spec.textPathHeight <= 0.0f) {
        return std::string();
    }

    struct TextPathParser {
        const std::string& data;
        size_t pos = 0;

        void skipSeparators() {
            while (pos < data.size()) {
                const char c = data[pos];
                if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
                    pos++;
                } else {
                    break;
                }
            }
        }

        bool readNumber(double* out) {
            skipSeparators();
            if (pos >= data.size() || !out) return false;
            char* endPtr = nullptr;
            const char* start = data.c_str() + pos;
            const double value = std::strtod(start, &endPtr);
            if (endPtr == start) return false;
            pos = static_cast<size_t>(endPtr - data.c_str());
            *out = value;
            return true;
        }

        bool parseToPdfStream(
                std::ostringstream* stream,
                double drawX,
                double drawY,
                double scaleX,
                double scaleY,
                double pathHeight
        ) {
            if (!stream) return false;
            bool wrotePath = false;
            while (pos < data.size()) {
                skipSeparators();
                if (pos >= data.size()) break;
                const char command = data[pos++];
                if (command == 'M' || command == 'm' || command == 'L' || command == 'l') {
                    double x = 0.0;
                    double y = 0.0;
                    if (!readNumber(&x) || !readNumber(&y)) return false;
                    const double pdfX = drawX + (x * scaleX);
                    const double pdfY = drawY + ((pathHeight - y) * scaleY);
                    *stream << FormatPdfFloat(static_cast<float>(pdfX)) << ' '
                            << FormatPdfFloat(static_cast<float>(pdfY)) << ' '
                            << ((command == 'M' || command == 'm') ? "m\n" : "l\n");
                    wrotePath = true;
                } else if (command == 'Z' || command == 'z') {
                    *stream << "h\n";
                } else {
                    return false;
                }
            }
            return wrotePath;
        }
    };

    const double targetWidth = std::max(
            static_cast<double>(spec.fontSize),
            static_cast<double>(spec.contentWidth > 0.0f ? spec.contentWidth : spec.textPathWidth)
    );
    const double targetHeight = std::max(
            1.0,
            static_cast<double>(spec.contentHeight > 0.0f ? spec.contentHeight : spec.textPathHeight)
    );
    const double scaleX = targetWidth / std::max(1.0, static_cast<double>(spec.textPathWidth));
    const double scaleY = targetHeight / std::max(1.0, static_cast<double>(spec.textPathHeight));
    const double drawX = spec.baselineX;
    const double drawY = std::max(
            0.0,
            (static_cast<double>(spec.patternHeight) - targetHeight) * 0.5
    );

    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(spec.textR / 255.0f) << ' '
           << FormatPdfFloat(spec.textG / 255.0f) << ' '
           << FormatPdfFloat(spec.textB / 255.0f) << " rg\n";

    TextPathParser parser{spec.textPathData};
    if (!parser.parseToPdfStream(&stream, drawX, drawY, scaleX, scaleY, spec.textPathHeight)) {
        return std::string();
    }

    if (spec.isUnderline || spec.isStrikeout) {
        const double decorationThickness = std::max(0.5, static_cast<double>(spec.fontSize) * 0.06);
        const double maxDecorationY = std::max(0.0, static_cast<double>(spec.patternHeight) - decorationThickness);
        auto appendDecorationRect = [&](double y) {
            const double clampedY = std::max(0.0, std::min(y, maxDecorationY));
            stream << FormatPdfFloat(spec.baselineX) << ' '
                   << FormatPdfFloat(static_cast<float>(clampedY)) << ' '
                   << FormatPdfFloat(static_cast<float>(targetWidth)) << ' '
                   << FormatPdfFloat(static_cast<float>(decorationThickness)) << " re\n";
        };
        if (spec.isUnderline) {
            appendDecorationRect(drawY - (static_cast<double>(spec.fontSize) * 0.12));
        }
        if (spec.isStrikeout) {
            appendDecorationRect(drawY + (targetHeight * 0.45));
        }
    }

    stream << "f\nQ";
    return stream.str();
}

static std::string BuildPdfWatermarkFontOutlinePatternStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
) {
    if (!spec.textPathData.empty()) {
        std::string textPathStream = BuildPdfWatermarkTextPathPatternStream(spec, graphicsStateName);
        if (!textPathStream.empty()) return textPathStream;
    }
    if (spec.fontPath.empty() || spec.text.empty() || spec.fontSize <= 0.0f) return std::string();
    std::vector<uint8_t> fontBytes;
    if (!ReadFileBytes(spec.fontPath.c_str(), &fontBytes)) return std::string();
    TrueTypeGlyphReader glyphReader(fontBytes);
    if (!glyphReader.load()) return std::string();

    const std::vector<uint32_t> codepoints = DecodeUtf8Codepoints(spec.text);
    if (codepoints.empty()) return std::string();

    const double unitsPerEm = std::max(1, static_cast<int>(glyphReader.getUnitsPerEm()));
    const double glyphScale = spec.fontSize / unitsPerEm;
    const double letterSpacingUnits = spec.characterSpacing > 0.0f ? spec.characterSpacing / glyphScale : 0.0;
    std::vector<GlyphOutline> glyphs;
    std::vector<bool> spaces;
    glyphs.reserve(codepoints.size());
    spaces.reserve(codepoints.size());
    bool hasDrawableGlyph = false;
    for (uint32_t codepoint : codepoints) {
        if (codepoint == 0x20) {
            glyphs.push_back(GlyphOutline());
            spaces.push_back(true);
            continue;
        }
        GlyphOutline outline;
        if (!glyphReader.loadGlyphForCodepoint(codepoint, &outline) || !outline.hasBounds || outline.contours.empty()) {
            glyphs.push_back(GlyphOutline());
            spaces.push_back(true);
            continue;
        }
        glyphs.push_back(outline);
        spaces.push_back(false);
        hasDrawableGlyph = true;
    }
    if (!hasDrawableGlyph) return std::string();

    double runMinY = 0.0;
    double runMaxY = 0.0;
    bool hasRunBounds = false;
    for (const GlyphOutline& glyph : glyphs) {
        if (!glyph.hasBounds) continue;
        runMinY = hasRunBounds ? std::min(runMinY, glyph.minY) : glyph.minY;
        runMaxY = hasRunBounds ? std::max(runMaxY, glyph.maxY) : glyph.maxY;
        hasRunBounds = true;
    }
    if (!hasRunBounds) return std::string();

    double naturalRunWidth = 0.0;
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (spaces[i]) {
            naturalRunWidth += unitsPerEm * 0.35 + letterSpacingUnits;
            continue;
        }
        const GlyphOutline& glyph = glyphs[i];
        if (!glyph.hasBounds) continue;
        naturalRunWidth += (glyph.maxX - glyph.minX) + letterSpacingUnits;
    }
    const double naturalRunWidthPdf = naturalRunWidth * glyphScale;
    double glyphScaleX = glyphScale;
    if (naturalRunWidthPdf > 0.0001 && spec.repeatStepWidth > 0.0f) {
        const double targetContentWidth = std::max(
                static_cast<double>(spec.fontSize),
                spec.contentWidth > 0.0f
                ? static_cast<double>(spec.contentWidth)
                : static_cast<double>(spec.repeatStepWidth) - (static_cast<double>(spec.fontSize) * 0.08)
        );
        const double rawXScaleMultiplier = targetContentWidth / naturalRunWidthPdf;
        const double xScaleMultiplier = spec.isIconImage
                                        ? 1.14
                                        : std::max(0.65, std::min(rawXScaleMultiplier, 1.55));
        glyphScaleX = glyphScale * xScaleMultiplier;
    }
    double glyphScaleY = glyphScale;
    const double naturalRunHeightPdf = (runMaxY - runMinY) * glyphScale;
    if (spec.isIconImage && naturalRunHeightPdf > 0.0001) {
//        const double iconHeightScale = 0.78;
        const double targetContentHeight = std::max(
                static_cast<double>(spec.fontSize),
                spec.contentHeight > 0.0f ? static_cast<double>(spec.contentHeight) : static_cast<double>(spec.fontSize)
        );
        const double yScaleMultiplier = targetContentHeight / naturalRunHeightPdf;
        glyphScaleY = glyphScale * yScaleMultiplier;
    }

    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(spec.textR / 255.0f) << ' '
           << FormatPdfFloat(spec.textG / 255.0f) << ' '
           << FormatPdfFloat(spec.textB / 255.0f) << " rg\n";
    double cursorX = 0.0;
    const double verticalPadding = std::max(1.0, static_cast<double>(spec.fontSize) * 0.08);
    const double preferredOffsetY = spec.isIconImage
                                    ? (static_cast<double>(spec.patternHeight) - ((runMinY + runMaxY) * glyphScaleY)) * 0.5
                                    : spec.baselineY - (runMinY * glyphScaleY);
    const double minOffsetY = verticalPadding - (runMinY * glyphScaleY);
    const double maxOffsetY = static_cast<double>(spec.patternHeight) - verticalPadding - (runMaxY * glyphScaleY);
    double baselineOffsetY = preferredOffsetY;
    if (maxOffsetY >= minOffsetY) {
        baselineOffsetY = std::max(minOffsetY, std::min(preferredOffsetY, maxOffsetY));
    } else {
        baselineOffsetY = (static_cast<double>(spec.patternHeight) - ((runMinY + runMaxY) * glyphScaleY)) * 0.5;
    }
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (spaces[i]) {
            cursorX += (unitsPerEm * 0.35 + letterSpacingUnits) * glyphScaleX;
            continue;
        }
        const GlyphOutline& glyph = glyphs[i];
        const double offsetX = spec.baselineX + cursorX - (glyph.minX * glyphScaleX);
        for (const GlyphOutlineContour& contour : glyph.contours) {
            AppendGlyphContourToPdfStream(&stream, contour, glyphScaleX, glyphScaleY, offsetX, baselineOffsetY);
        }
        cursorX += ((glyph.maxX - glyph.minX) + letterSpacingUnits) * glyphScaleX;
    }
    if (spec.isUnderline || spec.isStrikeout) {
        const double decorationWidth = std::max(
                cursorX,
                static_cast<double>(spec.contentWidth > 0.0f ? spec.contentWidth : spec.fontSize)
        );
        const double decorationThickness = std::max(0.5, static_cast<double>(spec.fontSize) * 0.06);
        const double maxDecorationY = std::max(0.0, static_cast<double>(spec.patternHeight) - decorationThickness);
        auto appendDecorationRect = [&](double y) {
            const double clampedY = std::max(0.0, std::min(y, maxDecorationY));
            stream << FormatPdfFloat(spec.baselineX) << ' '
                   << FormatPdfFloat(static_cast<float>(clampedY)) << ' '
                   << FormatPdfFloat(static_cast<float>(decorationWidth)) << ' '
                   << FormatPdfFloat(static_cast<float>(decorationThickness)) << " re\n";
        };
        if (spec.isUnderline) {
            appendDecorationRect(baselineOffsetY - (static_cast<double>(spec.fontSize) * 0.12));
        }
        if (spec.isStrikeout) {
            appendDecorationRect(baselineOffsetY + (static_cast<double>(spec.fontSize) * 0.32));
        }
    }
    stream << "f\nQ";
    WM_LOGE(
            "Native compact watermark pattern uses font outlines font=%s path=%s xScale=%f naturalW=%f targetStep=%f yBounds=%f..%f offsetY=%f patternH=%f",
            spec.fontName.c_str(),
            spec.fontPath.c_str(),
            glyphScaleX / glyphScale,
            naturalRunWidthPdf,
            spec.repeatStepWidth,
            runMinY * glyphScaleY,
            runMaxY * glyphScaleY,
            baselineOffsetY,
            spec.patternHeight
    );
    return stream.str();
}

static std::string BuildPdfSingleWatermarkTextPathContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
) {
    if (spec.textPathData.empty() || spec.textPathWidth <= 0.0f || spec.textPathHeight <= 0.0f) {
        return std::string();
    }

    struct TextPathParser {
        const std::string& data;
        size_t pos = 0;

        void skipSeparators() {
            while (pos < data.size()) {
                const char c = data[pos];
                if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
                    pos++;
                } else {
                    break;
                }
            }
        }

        bool readNumber(double* out) {
            skipSeparators();
            if (pos >= data.size() || !out) return false;
            char* endPtr = nullptr;
            const char* start = data.c_str() + pos;
            const double value = std::strtod(start, &endPtr);
            if (endPtr == start) return false;
            pos = static_cast<size_t>(endPtr - data.c_str());
            *out = value;
            return true;
        }

        bool parseToPdfStream(
                std::ostringstream* stream,
                double drawX,
                double drawY,
                double scaleX,
                double scaleY,
                double pathHeight
        ) {
            if (!stream) return false;
            bool wrotePath = false;
            while (pos < data.size()) {
                skipSeparators();
                if (pos >= data.size()) break;
                const char command = data[pos++];
                if (command == 'M' || command == 'm' || command == 'L' || command == 'l') {
                    double x = 0.0;
                    double y = 0.0;
                    if (!readNumber(&x) || !readNumber(&y)) return false;
                    const double pdfX = drawX + (x * scaleX);
                    const double pdfY = drawY + ((pathHeight - y) * scaleY);
                    *stream << FormatPdfFloat(static_cast<float>(pdfX)) << ' '
                            << FormatPdfFloat(static_cast<float>(pdfY)) << ' '
                            << ((command == 'M' || command == 'm') ? "m\n" : "l\n");
                    wrotePath = true;
                } else if (command == 'Z' || command == 'z') {
                    *stream << "h\n";
                } else {
                    return false;
                }
            }
            return wrotePath;
        }
    };

    const double targetWidth = std::max(
            static_cast<double>(spec.fontSize),
            static_cast<double>(spec.contentWidth > 0.0f ? spec.contentWidth : spec.textPathWidth)
    );
    const double targetHeight = std::max(
            1.0,
            static_cast<double>(spec.contentHeight > 0.0f ? spec.contentHeight : spec.textPathHeight)
    );
    const double scaleX = targetWidth / std::max(1.0, static_cast<double>(spec.textPathWidth));
    const double scaleY = targetHeight / std::max(1.0, static_cast<double>(spec.textPathHeight));
    const double drawX = -targetWidth * 0.5;
    const double drawY = -targetHeight * 0.5;
    const double angleRad = spec.rotation * M_PI / 180.0;
    const float cosA = static_cast<float>(cos(angleRad));
    const float sinA = static_cast<float>(sin(angleRad));
    const float centerX = spec.centerX > 0.0f ? spec.centerX : (spec.pageWidth * 0.5f);
    const float centerY = spec.centerY > 0.0f ? spec.centerY : (spec.pageHeight * 0.5f);

    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(spec.textR / 255.0f) << ' '
           << FormatPdfFloat(spec.textG / 255.0f) << ' '
           << FormatPdfFloat(spec.textB / 255.0f) << " rg\n"
           << FormatPdfFloat(cosA) << ' ' << FormatPdfFloat(sinA) << ' '
           << FormatPdfFloat(-sinA) << ' ' << FormatPdfFloat(cosA) << ' '
           << FormatPdfFloat(centerX) << ' ' << FormatPdfFloat(centerY) << " cm\n";

    TextPathParser parser{spec.textPathData};
    if (!parser.parseToPdfStream(&stream, drawX, drawY, scaleX, scaleY, spec.textPathHeight)) {
        return std::string();
    }

    if (spec.isUnderline || spec.isStrikeout) {
        const double decorationThickness = std::max(0.5, static_cast<double>(spec.fontSize) * 0.06);
        auto appendDecorationRect = [&](double y) {
            stream << FormatPdfFloat(static_cast<float>(drawX)) << ' '
                   << FormatPdfFloat(static_cast<float>(y)) << ' '
                   << FormatPdfFloat(static_cast<float>(targetWidth)) << ' '
                   << FormatPdfFloat(static_cast<float>(decorationThickness)) << " re\n";
        };
        if (spec.isUnderline) {
            appendDecorationRect(drawY - (static_cast<double>(spec.fontSize) * 0.12));
        }
        if (spec.isStrikeout) {
            appendDecorationRect(drawY + (targetHeight * 0.45));
        }
    }

    stream << "f\nQ";
    return stream.str();
}

static std::string BuildPdfSingleWatermarkFontOutlineContentStream(
        const RawPdfWatermarkSpec& spec,
        const std::string& graphicsStateName
) {
    if (!spec.textPathData.empty()) {
        std::string textPathStream = BuildPdfSingleWatermarkTextPathContentStream(spec, graphicsStateName);
        if (!textPathStream.empty()) return textPathStream;
    }
    if (spec.fontPath.empty() || spec.text.empty() || spec.fontSize <= 0.0f) return std::string();
    std::vector<uint8_t> fontBytes;
    if (!ReadFileBytes(spec.fontPath.c_str(), &fontBytes)) return std::string();
    TrueTypeGlyphReader glyphReader(fontBytes);
    if (!glyphReader.load()) return std::string();

    const std::vector<uint32_t> codepoints = DecodeUtf8Codepoints(spec.text);
    if (codepoints.empty()) return std::string();

    const double unitsPerEm = std::max(1, static_cast<int>(glyphReader.getUnitsPerEm()));
    const double glyphScale = spec.fontSize / unitsPerEm;
    const double letterSpacingUnits = spec.characterSpacing > 0.0f ? spec.characterSpacing / glyphScale : 0.0;
    std::vector<GlyphOutline> glyphs;
    std::vector<bool> spaces;
    glyphs.reserve(codepoints.size());
    spaces.reserve(codepoints.size());
    bool hasDrawableGlyph = false;
    for (uint32_t codepoint : codepoints) {
        if (codepoint == 0x20) {
            glyphs.push_back(GlyphOutline());
            spaces.push_back(true);
            continue;
        }
        GlyphOutline outline;
        if (!glyphReader.loadGlyphForCodepoint(codepoint, &outline) || !outline.hasBounds || outline.contours.empty()) {
            glyphs.push_back(GlyphOutline());
            spaces.push_back(true);
            continue;
        }
        glyphs.push_back(outline);
        spaces.push_back(false);
        hasDrawableGlyph = true;
    }
    if (!hasDrawableGlyph) return std::string();

    double runMinY = 0.0;
    double runMaxY = 0.0;
    bool hasRunBounds = false;
    for (const GlyphOutline& glyph : glyphs) {
        if (!glyph.hasBounds) continue;
        runMinY = hasRunBounds ? std::min(runMinY, glyph.minY) : glyph.minY;
        runMaxY = hasRunBounds ? std::max(runMaxY, glyph.maxY) : glyph.maxY;
        hasRunBounds = true;
    }
    if (!hasRunBounds) return std::string();

    double naturalRunWidth = 0.0;
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (spaces[i]) {
            naturalRunWidth += unitsPerEm * 0.35 + letterSpacingUnits;
            continue;
        }
        const GlyphOutline& glyph = glyphs[i];
        if (!glyph.hasBounds) continue;
        naturalRunWidth += (glyph.maxX - glyph.minX) + letterSpacingUnits;
    }
    const double naturalRunWidthPdf = naturalRunWidth * glyphScale;
    double glyphScaleX = glyphScale;
    if (naturalRunWidthPdf > 0.0001 && spec.contentWidth > 0.0f) {
        const double targetContentWidth = std::max(static_cast<double>(spec.fontSize), static_cast<double>(spec.contentWidth));
        const double rawXScaleMultiplier = targetContentWidth / naturalRunWidthPdf;
        const double xScaleMultiplier = spec.isIconImage
                                        ? 1.15
                                        : std::max(0.65, std::min(rawXScaleMultiplier, 1.55));
        glyphScaleX = glyphScale * xScaleMultiplier;
    }
    double glyphScaleY = glyphScale;
    const double naturalRunHeightPdf = (runMaxY - runMinY) * glyphScale;
    if (spec.isIconImage && naturalRunHeightPdf > 0.0001) {
        const double iconHeightScale = 1.0;
        const double targetContentHeight = std::max(
                static_cast<double>(spec.fontSize),
                spec.contentHeight > 0.0f ? static_cast<double>(spec.contentHeight) : static_cast<double>(spec.fontSize)
        ) * iconHeightScale;
        const double yScaleMultiplier = targetContentHeight / naturalRunHeightPdf;
        glyphScaleY = glyphScale * yScaleMultiplier;
    }
    const double visualRunWidth = naturalRunWidth * glyphScaleX;
    const double drawX = -visualRunWidth * 0.5;
    const double drawY = spec.isIconImage ? -((runMinY + runMaxY) * glyphScaleY) * 0.5 : 0.0;
    const double angleRad = spec.rotation * M_PI / 180.0;
    const float cosA = static_cast<float>(cos(angleRad));
    const float sinA = static_cast<float>(sin(angleRad));
    const float centerX = spec.centerX > 0.0f ? spec.centerX : (spec.pageWidth * 0.5f);
    const float centerY = spec.centerY > 0.0f ? spec.centerY : (spec.pageHeight * 0.5f);

    std::ostringstream stream;
    stream << "q\n"
           << "/" << graphicsStateName << " gs\n"
           << FormatPdfFloat(spec.textR / 255.0f) << ' '
           << FormatPdfFloat(spec.textG / 255.0f) << ' '
           << FormatPdfFloat(spec.textB / 255.0f) << " rg\n"
           << FormatPdfFloat(cosA) << ' ' << FormatPdfFloat(sinA) << ' '
           << FormatPdfFloat(-sinA) << ' ' << FormatPdfFloat(cosA) << ' '
           << FormatPdfFloat(centerX) << ' ' << FormatPdfFloat(centerY) << " cm\n";

    double cursorX = 0.0;
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (spaces[i]) {
            cursorX += (unitsPerEm * 0.35 + letterSpacingUnits) * glyphScaleX;
            continue;
        }
        const GlyphOutline& glyph = glyphs[i];
        const double offsetX = drawX + cursorX - (glyph.minX * glyphScaleX);
        for (const GlyphOutlineContour& contour : glyph.contours) {
            AppendGlyphContourToPdfStream(&stream, contour, glyphScaleX, glyphScaleY, offsetX, drawY);
        }
        cursorX += ((glyph.maxX - glyph.minX) + letterSpacingUnits) * glyphScaleX;
    }
    if (spec.isUnderline || spec.isStrikeout) {
        const double decorationWidth = std::max(cursorX, static_cast<double>(spec.fontSize));
        const double decorationThickness = std::max(0.5, static_cast<double>(spec.fontSize) * 0.06);
        auto appendDecorationRect = [&](double y) {
            stream << FormatPdfFloat(static_cast<float>(drawX)) << ' '
                   << FormatPdfFloat(static_cast<float>(y)) << ' '
                   << FormatPdfFloat(static_cast<float>(decorationWidth)) << ' '
                   << FormatPdfFloat(static_cast<float>(decorationThickness)) << " re\n";
        };
        if (spec.isUnderline) {
            appendDecorationRect(-(static_cast<double>(spec.fontSize) * 0.12));
        }
        if (spec.isStrikeout) {
            appendDecorationRect(static_cast<double>(spec.fontSize) * 0.32);
        }
    }
    stream << "f\nQ";
    WM_LOGE(
            "Native single watermark uses font outlines font=%s path=%s xScale=%f yScale=%f naturalW=%f visualW=%f rotation=%f underline=%d strikeout=%d",
            spec.fontName.c_str(),
            spec.fontPath.c_str(),
            glyphScaleX / glyphScale,
            glyphScaleY / glyphScale,
            naturalRunWidthPdf,
            visualRunWidth,
            spec.rotation,
            spec.isUnderline ? 1 : 0,
            spec.isStrikeout ? 1 : 0
    );
    return stream.str();
}

static bool AppendPageLevelTextWatermarkGlyphPaths(
        FPDF_PAGE page,
        const char* fontPath,
        const jchar* textContent,
        jsize textLength,
        int textR,
        int textG,
        int textB,
        int textA,
        bool isBold,
        double scale,
        double letterSpacing,
        double textHeight,
        double cosA,
        double sinA,
        double skewX,
        float minX,
        float maxX,
        float minY,
        float maxY,
        float centerX,
        float centerY,
        float tileWidth,
        float tileHeight,
        int maxStampObjects
) {
    if (!page || !fontPath || strlen(fontPath) == 0 || !textContent || textLength <= 0) {
        return false;
    }

    std::vector<uint8_t> fontBytes;
    if (!ReadFileBytes(fontPath, &fontBytes)) {
        return false;
    }

    TrueTypeGlyphReader glyphReader(fontBytes);
    if (!glyphReader.load()) {
        return false;
    }

    std::vector<uint32_t> codepoints;
    codepoints.reserve(textLength);
    for (jsize charIndex = 0; charIndex < textLength; charIndex++) {
        const uint16_t first = static_cast<uint16_t>(textContent[charIndex]);
        if (first >= 0xD800 && first <= 0xDBFF && charIndex + 1 < textLength) {
            const uint16_t second = static_cast<uint16_t>(textContent[charIndex + 1]);
            if (second >= 0xDC00 && second <= 0xDFFF) {
                codepoints.push_back(
                        0x10000 +
                        (((static_cast<uint32_t>(first) - 0xD800) << 10) |
                         (static_cast<uint32_t>(second) - 0xDC00))
                );
                charIndex++;
                continue;
            }
        }
        codepoints.push_back(first);
    }
    if (codepoints.empty()) return false;

    const double unitsPerEm = std::max(1, (int)glyphReader.getUnitsPerEm());
    const double glyphScale = scale / unitsPerEm;
    std::vector<GlyphOutline> glyphRun;
    std::vector<bool> spaceRun;
    glyphRun.reserve(codepoints.size());
    spaceRun.reserve(codepoints.size());
    bool hasDrawableGlyph = false;
    for (uint32_t codepoint : codepoints) {
        if (codepoint == 0x20) {
            glyphRun.push_back(GlyphOutline());
            spaceRun.push_back(true);
            continue;
        }

        GlyphOutline loadedOutline;
        if (!glyphReader.loadGlyphForCodepoint(codepoint, &loadedOutline)) {
            glyphRun.push_back(GlyphOutline());
            spaceRun.push_back(true);
            continue;
        }
        glyphRun.push_back(loadedOutline);
        spaceRun.push_back(false);
        hasDrawableGlyph = true;
    }
    if (!hasDrawableGlyph) return false;

    int objectCount = 0;
    bool appendedAny = false;

    for (float gridY = minY; gridY <= maxY && objectCount < maxStampObjects; gridY += tileHeight) {
        for (float gridX = minX; gridX <= maxX && objectCount < maxStampObjects; gridX += tileWidth) {
            const float dx = gridX - centerX;
            const float dy = gridY - centerY;
            const float finalX = centerX + (float)(dx * cosA - dy * sinA);
            const float finalY = centerY + (float)(dx * sinA + dy * cosA);

            FPDF_PAGEOBJECT pathObj = CreateWatermarkTextPathObject(
                    glyphRun,
                    spaceRun,
                    unitsPerEm,
                    letterSpacing
            );
            if (!pathObj) continue;

            FPDFPageObj_SetFillColor(pathObj, textR, textG, textB, textA);
            if (isBold) {
                FPDFPageObj_SetStrokeColor(pathObj, textR, textG, textB, textA);
                FPDFPageObj_SetStrokeWidth(pathObj, textHeight * 0.05f);
            }
            FPDFPath_SetDrawMode(pathObj, FPDF_FILLMODE_WINDING, isBold ? JNI_TRUE : JNI_FALSE);
            FPDFPageObj_Transform(
                    pathObj,
                    (float)(cosA * glyphScale),
                    (float)(sinA * glyphScale),
                    (float)((-sinA + skewX) * glyphScale),
                    (float)(cosA * glyphScale),
                    finalX,
                    finalY
            );

            FPDFPage_InsertObject(page, pathObj);
            objectCount++;
            appendedAny = true;
        }
    }

    return appendedAny;
}
//------------------------------------------------------------------------------------------------------

static bool processSvgPathStamp(
        JNIEnv* env,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        jobject json,
        jstring jJsonStr,
        jmethodID optS,
        jmethodID optD,
        jmethodID optI,
        jmethodID optB,
        int r,
        int g,
        int b,
        int alpha,
        bool saveAsPageContent
) {
    if (!env || !json || !jJsonStr || !optS || !optD || !optI || !optB) {
        return false;
    }
    if (saveAsPageContent ? !page : !annot) {
        return false;
    }

    auto optStringValue = [&](const char* key) -> jstring {
        jstring jKey = env->NewStringUTF(key);
        jstring value = (jstring)env->CallObjectMethod(json, optS, jKey);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optBoolValue = [&](const char* key, bool fallback) -> bool {
        jstring jKey = env->NewStringUTF(key);
        const bool value = env->CallBooleanMethod(
                json,
                optB,
                jKey,
                fallback ? JNI_TRUE : JNI_FALSE
        ) == JNI_TRUE;
        env->DeleteLocalRef(jKey);
        return value;
    };

    jstring jPathData = optStringValue("pathData");
    if (!jPathData) {
        jPathData = optStringValue("svgPathData");
    }
    const char* rawPathData = jPathData ? env->GetStringUTFChars(jPathData, nullptr) : nullptr;
    const std::string pathData = rawPathData ? rawPathData : "";
    if (rawPathData) env->ReleaseStringUTFChars(jPathData, rawPathData);
    if (jPathData) env->DeleteLocalRef(jPathData);
    if (pathData.empty()) {
        return false;
    }

    struct SvgParser {
        const std::string& data;
        size_t pos = 0;
        char command = 0;
        FPDF_PAGEOBJECT path = nullptr;
        bool hasPath = false;
        bool hasCurrent = false;
        double currentX = 0.0;
        double currentY = 0.0;
        double startX = 0.0;
        double startY = 0.0;
        double lastCubicX = 0.0;
        double lastCubicY = 0.0;
        double lastQuadX = 0.0;
        double lastQuadY = 0.0;
        char previousCurve = 0;
        double minX = 0.0;
        double minY = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;

        bool isCommand(char c) const {
            return c == 'M' || c == 'm' || c == 'L' || c == 'l' ||
                   c == 'H' || c == 'h' || c == 'V' || c == 'v' ||
                   c == 'C' || c == 'c' || c == 'S' || c == 's' ||
                   c == 'Q' || c == 'q' || c == 'T' || c == 't' ||
                   c == 'A' || c == 'a' || c == 'Z' || c == 'z';
        }

        void skipSeparators() {
            while (pos < data.size()) {
                const char c = data[pos];
                if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
                    pos++;
                } else {
                    break;
                }
            }
        }

        bool hasNumberAhead() {
            skipSeparators();
            if (pos >= data.size()) return false;
            const char c = data[pos];
            return c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c));
        }

        bool readNumber(double* out) {
            skipSeparators();
            if (pos >= data.size()) return false;
            char* endPtr = nullptr;
            const char* start = data.c_str() + pos;
            const double value = std::strtod(start, &endPtr);
            if (endPtr == start) return false;
            pos = static_cast<size_t>(endPtr - data.c_str());
            *out = value;
            return true;
        }

        void includePoint(double x, double y) {
            if (!hasPath) {
                minX = maxX = x;
                minY = maxY = y;
                hasPath = true;
            } else {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }

        bool ensurePath(double x, double y) {
            if (!path) {
                path = FPDFPageObj_CreateNewPath(static_cast<float>(x), static_cast<float>(y));
                if (!path) return false;
            } else {
                FPDFPath_MoveTo(path, static_cast<float>(x), static_cast<float>(y));
            }
            currentX = startX = x;
            currentY = startY = y;
            hasCurrent = true;
            includePoint(x, y);
            previousCurve = 0;
            return true;
        }

        void lineTo(double x, double y) {
            if (!path && !ensurePath(x, y)) return;
            FPDFPath_LineTo(path, static_cast<float>(x), static_cast<float>(y));
            currentX = x;
            currentY = y;
            includePoint(x, y);
            previousCurve = 0;
        }

        void cubicTo(double x1, double y1, double x2, double y2, double x, double y) {
            if (!path && !ensurePath(currentX, currentY)) return;
            FPDFPath_BezierTo(
                    path,
                    static_cast<float>(x1),
                    static_cast<float>(y1),
                    static_cast<float>(x2),
                    static_cast<float>(y2),
                    static_cast<float>(x),
                    static_cast<float>(y)
            );
            includePoint(x1, y1);
            includePoint(x2, y2);
            includePoint(x, y);
            currentX = x;
            currentY = y;
            lastCubicX = x2;
            lastCubicY = y2;
            previousCurve = 'C';
        }

        void quadTo(double x1, double y1, double x, double y) {
            const double c1x = currentX + (2.0 / 3.0) * (x1 - currentX);
            const double c1y = currentY + (2.0 / 3.0) * (y1 - currentY);
            const double c2x = x + (2.0 / 3.0) * (x1 - x);
            const double c2y = y + (2.0 / 3.0) * (y1 - y);
            cubicTo(c1x, c1y, c2x, c2y, x, y);
            lastQuadX = x1;
            lastQuadY = y1;
            previousCurve = 'Q';
        }

        void arcTo(double rx, double ry, double xAxisRotation, bool largeArc, bool sweep, double x, double y) {
            if (rx == 0.0 || ry == 0.0 ||
                (fabs(currentX - x) < 0.0001 && fabs(currentY - y) < 0.0001)) {
                lineTo(x, y);
                return;
            }

            rx = fabs(rx);
            ry = fabs(ry);
            const double phi = xAxisRotation * M_PI / 180.0;
            const double cosPhi = cos(phi);
            const double sinPhi = sin(phi);
            const double dx2 = (currentX - x) / 2.0;
            const double dy2 = (currentY - y) / 2.0;
            const double x1p = (cosPhi * dx2) + (sinPhi * dy2);
            const double y1p = (-sinPhi * dx2) + (cosPhi * dy2);

            double rxSq = rx * rx;
            double rySq = ry * ry;
            const double x1pSq = x1p * x1p;
            const double y1pSq = y1p * y1p;
            const double radiusScale = (x1pSq / rxSq) + (y1pSq / rySq);
            if (radiusScale > 1.0) {
                const double scale = sqrt(radiusScale);
                rx *= scale;
                ry *= scale;
                rxSq = rx * rx;
                rySq = ry * ry;
            }

            const double sign = (largeArc == sweep) ? -1.0 : 1.0;
            const double denom = (rxSq * y1pSq) + (rySq * x1pSq);
            double coef = 0.0;
            if (denom > 0.0) {
                coef = sign * sqrt(std::max(0.0, ((rxSq * rySq) - (rxSq * y1pSq) - (rySq * x1pSq)) / denom));
            }
            const double cxp = coef * ((rx * y1p) / ry);
            const double cyp = coef * (-(ry * x1p) / rx);
            const double cx = (cosPhi * cxp) - (sinPhi * cyp) + ((currentX + x) / 2.0);
            const double cy = (sinPhi * cxp) + (cosPhi * cyp) + ((currentY + y) / 2.0);

            auto vectorAngle = [](double ux, double uy, double vx, double vy) {
                const double dot = (ux * vx) + (uy * vy);
                const double len = sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
                if (len <= 0.0) return 0.0;
                const double clamped = std::max(-1.0, std::min(1.0, dot / len));
                double angle = acos(clamped);
                if ((ux * vy - uy * vx) < 0.0) angle = -angle;
                return angle;
            };

            const double ux = (x1p - cxp) / rx;
            const double uy = (y1p - cyp) / ry;
            const double vx = (-x1p - cxp) / rx;
            const double vy = (-y1p - cyp) / ry;
            double startAngle = vectorAngle(1.0, 0.0, ux, uy);
            double sweepAngle = vectorAngle(ux, uy, vx, vy);
            if (!sweep && sweepAngle > 0.0) sweepAngle -= 2.0 * M_PI;
            if (sweep && sweepAngle < 0.0) sweepAngle += 2.0 * M_PI;

            const int segments = std::max(4, static_cast<int>(ceil(fabs(sweepAngle) / (M_PI / 12.0))));
            for (int i = 1; i <= segments; i++) {
                const double theta = startAngle + (sweepAngle * (static_cast<double>(i) / segments));
                const double cosTheta = cos(theta);
                const double sinTheta = sin(theta);
                const double px = cx + (rx * cosPhi * cosTheta) - (ry * sinPhi * sinTheta);
                const double py = cy + (rx * sinPhi * cosTheta) + (ry * cosPhi * sinTheta);
                lineTo(px, py);
            }
            currentX = x;
            currentY = y;
            previousCurve = 0;
        }

        void closePath() {
            if (path) {
                FPDFPath_Close(path);
                currentX = startX;
                currentY = startY;
                includePoint(currentX, currentY);
            }
            previousCurve = 0;
        }

        bool parse() {
            while (pos < data.size()) {
                skipSeparators();
                if (pos >= data.size()) break;
                if (isCommand(data[pos])) {
                    command = data[pos++];
                } else if (command == 0) {
                    return false;
                }

                const bool relative = std::islower(static_cast<unsigned char>(command));
                const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
                if (upper == 'Z') {
                    closePath();
                    continue;
                }

                if (upper == 'M') {
                    bool first = true;
                    while (hasNumberAhead()) {
                        double x = 0.0, y = 0.0;
                        if (!readNumber(&x) || !readNumber(&y)) return false;
                        if (relative) {
                            x += currentX;
                            y += currentY;
                        }
                        if (first) {
                            if (!ensurePath(x, y)) return false;
                            first = false;
                        } else {
                            lineTo(x, y);
                        }
                    }
                    continue;
                }

                while (hasNumberAhead()) {
                    if (upper == 'L') {
                        double x = 0.0, y = 0.0;
                        if (!readNumber(&x) || !readNumber(&y)) return false;
                        if (relative) { x += currentX; y += currentY; }
                        lineTo(x, y);
                    } else if (upper == 'H') {
                        double x = 0.0;
                        if (!readNumber(&x)) return false;
                        if (relative) x += currentX;
                        lineTo(x, currentY);
                    } else if (upper == 'V') {
                        double y = 0.0;
                        if (!readNumber(&y)) return false;
                        if (relative) y += currentY;
                        lineTo(currentX, y);
                    } else if (upper == 'C') {
                        double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                        if (!readNumber(&x1) || !readNumber(&y1) ||
                            !readNumber(&x2) || !readNumber(&y2) ||
                            !readNumber(&x) || !readNumber(&y)) return false;
                        if (relative) {
                            x1 += currentX; y1 += currentY;
                            x2 += currentX; y2 += currentY;
                            x += currentX; y += currentY;
                        }
                        cubicTo(x1, y1, x2, y2, x, y);
                    } else if (upper == 'S') {
                        double x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                        if (!readNumber(&x2) || !readNumber(&y2) || !readNumber(&x) || !readNumber(&y)) return false;
                        double x1 = currentX;
                        double y1 = currentY;
                        if (previousCurve == 'C') {
                            x1 = (2.0 * currentX) - lastCubicX;
                            y1 = (2.0 * currentY) - lastCubicY;
                        }
                        if (relative) {
                            x2 += currentX; y2 += currentY;
                            x += currentX; y += currentY;
                        }
                        cubicTo(x1, y1, x2, y2, x, y);
                    } else if (upper == 'Q') {
                        double x1 = 0.0, y1 = 0.0, x = 0.0, y = 0.0;
                        if (!readNumber(&x1) || !readNumber(&y1) || !readNumber(&x) || !readNumber(&y)) return false;
                        if (relative) {
                            x1 += currentX; y1 += currentY;
                            x += currentX; y += currentY;
                        }
                        quadTo(x1, y1, x, y);
                    } else if (upper == 'T') {
                        double x = 0.0, y = 0.0;
                        if (!readNumber(&x) || !readNumber(&y)) return false;
                        double x1 = currentX;
                        double y1 = currentY;
                        if (previousCurve == 'Q') {
                            x1 = (2.0 * currentX) - lastQuadX;
                            y1 = (2.0 * currentY) - lastQuadY;
                        }
                        if (relative) { x += currentX; y += currentY; }
                        quadTo(x1, y1, x, y);
                    } else if (upper == 'A') {
                        double rx = 0.0, ry = 0.0, xAxisRotation = 0.0, largeArc = 0.0, sweep = 0.0, x = 0.0, y = 0.0;
                        if (!readNumber(&rx) || !readNumber(&ry) || !readNumber(&xAxisRotation) ||
                            !readNumber(&largeArc) || !readNumber(&sweep) ||
                            !readNumber(&x) || !readNumber(&y)) return false;
                        if (relative) { x += currentX; y += currentY; }
                        arcTo(rx, ry, xAxisRotation, largeArc != 0.0, sweep != 0.0, x, y);
                    } else {
                        return false;
                    }
                }
            }
            return path && hasPath;
        }
    };

    SvgParser parser{pathData};
    if (!parser.parse() || !parser.path) {
        if (parser.path) FPDFPageObj_Destroy(parser.path);
        return false;
    }

    auto clampColor = [](int value) -> int {
        return std::max(0, std::min(255, value));
    };
    const int defaultAlpha = alpha > 0 ? alpha : 255;
    const int iconR = clampColor(optIntValue("iconColorR", optIntValue("textColorR", r)));
    const int iconG = clampColor(optIntValue("iconColorG", optIntValue("textColorG", g)));
    const int iconB = clampColor(optIntValue("iconColorB", optIntValue("textColorB", b)));
    const int iconA = clampColor(optIntValue("iconColorA", optIntValue("textColorA", defaultAlpha)));

    const double rectWidth = fabs(rect.right - rect.left);
    const double rectHeight = fabs(rect.top - rect.bottom);
    const double svgWidth = parser.maxX - parser.minX;
    const double svgHeight = parser.maxY - parser.minY;
    if (rectWidth <= 0.0 || rectHeight <= 0.0 || svgWidth <= 0.0001 || svgHeight <= 0.0001) {
        FPDFPageObj_Destroy(parser.path);
        return false;
    }

    const double rotation = optDoubleValue("rotation", 0.0);
    const double baseWidth = optDoubleValue("baseWidth", rectWidth);
    const double baseHeight = optDoubleValue("baseHeight", rectHeight);
    const double angleRad = rotation * M_PI / 180.0;
    const double absCos = fabs(cos(angleRad));
    const double absSin = fabs(sin(angleRad));
    const double storedExpandedWidth = (baseWidth * absCos) + (baseHeight * absSin);
    const double storedExpandedHeight = (baseWidth * absSin) + (baseHeight * absCos);
    double fitScale = 1.0;
    if (storedExpandedWidth > 0.0 && storedExpandedHeight > 0.0) {
        fitScale = std::min(rectWidth / storedExpandedWidth, rectHeight / storedExpandedHeight);
    }
    const double drawWidth = std::max(baseWidth * fitScale, 1.0);
    const double drawHeight = std::max(baseHeight * fitScale, 1.0);
    const double scale = std::max(std::min(drawWidth / svgWidth, drawHeight / svgHeight), 0.0001);
    const double centerX = (rect.left + rect.right) / 2.0;
    const double centerY = (rect.top + rect.bottom) / 2.0;
    const bool flipHorizontal = optBoolValue(
            "effectiveFlipHorizontal",
            optBoolValue("flipHorizontal", false)
    );
    const bool flipVertical = optBoolValue(
            "effectiveFlipVertical",
            optBoolValue("flipVertical", false)
    );

    const double pathCenterX = ((parser.minX + parser.maxX) / 2.0) * scale;
    const double pathCenterY = ((parser.minY + parser.maxY) / 2.0) * scale;
    const double cosA = cos(angleRad);
    const double sinA = sin(angleRad);
    const double flipX = flipHorizontal ? -1.0 : 1.0;
    const double flipY = flipVertical ? 1.0 : -1.0;
    const double matrixA = cosA * scale * flipX;
    const double matrixB = sinA * scale * flipX;
    const double matrixC = -sinA * scale * flipY;
    const double matrixD = cosA * scale * flipY;
    const double flippedPathCenterX = pathCenterX * flipX;
    const double flippedPathCenterY = pathCenterY * flipY;
    const double matrixE = centerX - ((flippedPathCenterX * cosA) - (flippedPathCenterY * sinA));
    const double matrixF = centerY - ((flippedPathCenterX * sinA) + (flippedPathCenterY * cosA));

    FPDFPageObj_SetFillColor(parser.path, iconR, iconG, iconB, iconA);
    FPDFPath_SetDrawMode(parser.path, FPDF_FILLMODE_ALTERNATE, JNI_FALSE);
    FPDFPageObj_Transform(parser.path, matrixA, matrixB, matrixC, matrixD, matrixE, matrixF);
    if (saveAsPageContent) {
        FPDFPage_InsertObject(page, parser.path);
        return true;
    }

    FPDFAnnot_SetRect(annot, &rect);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, iconR, iconG, iconB, iconA);
    FPDFAnnot_SetBorder(annot, 0, 0, 0);

    if (!FPDFAnnot_AppendObject(annot, parser.path)) {
        FPDFPageObj_Destroy(parser.path);
        return false;
    }

    FPDFAnnot_UpdateObject(annot, parser.path);
    SetAnnotWideStringValueFromJString(env, annot, "LufickImageMeta", jJsonStr);
    SetAnnotWideStringValueFromJString(env, annot, "LufickShapeElementMeta", jJsonStr);
    SetAnnotAsciiStringValue(annot, "LufickStampKind", "shape_element_svg");
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
    return true;
}

static bool processStickerStamp(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        jobject json,
        jstring jJsonStr,
        jmethodID optS,
        jmethodID optD,
        jmethodID optI,
        jmethodID optB,
        int r,
        int g,
        int b,
        int alpha,
        bool saveAsPageContent
) {
    if (!env || !doc || !json || !jJsonStr || !optS || !optD || !optI || !optB) {
        return false;
    }
    if (saveAsPageContent ? !page : !annot) {
        return false;
    }

    auto optStringValue = [&](const char* key) -> jstring {
        jstring jKey = env->NewStringUTF(key);
        jstring value = (jstring)env->CallObjectMethod(json, optS, jKey);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optBoolValue = [&](const char* key, bool fallback) -> bool {
        jstring jKey = env->NewStringUTF(key);
        const bool value = env->CallBooleanMethod(
                json,
                optB,
                jKey,
                fallback ? JNI_TRUE : JNI_FALSE
        ) == JNI_TRUE;
        env->DeleteLocalRef(jKey);
        return value;
    };

    jstring jGlyph = optStringValue("glyph");
    jstring jFontPath = optStringValue("fontPath");
    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;

    const std::string resolvedFontPath = fontPath ? fontPath : "";
    const std::u16string glyph = JStringToUtf16(env, jGlyph);
    uint32_t glyphCodepoint = static_cast<uint32_t>(std::max(optIntValue("glyphCode", 0), 0));
    if (glyphCodepoint == 0 && !glyph.empty()) {
        const uint16_t first = static_cast<uint16_t>(glyph[0]);
        if (first >= 0xD800 && first <= 0xDBFF && glyph.size() > 1) {
            const uint16_t second = static_cast<uint16_t>(glyph[1]);
            if (second >= 0xDC00 && second <= 0xDFFF) {
                glyphCodepoint = 0x10000 +
                                 (((static_cast<uint32_t>(first) - 0xD800) << 10) |
                                  (static_cast<uint32_t>(second) - 0xDC00));
            }
        } else {
            glyphCodepoint = first;
        }
    }

    auto cleanupStrings = [&]() {
        if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        if (jGlyph) env->DeleteLocalRef(jGlyph);
    };

    if (glyphCodepoint == 0 || resolvedFontPath.empty()) {
        cleanupStrings();
        return false;
    }

    cleanupStrings();

    std::vector<uint8_t> fontBytes;
    if (!ReadFileBytes(resolvedFontPath.c_str(), &fontBytes)) {
        return false;
    }
    TrueTypeGlyphReader glyphReader(fontBytes);
    GlyphOutline glyphOutline;
    if (!glyphReader.load() || !glyphReader.loadGlyphForCodepoint(glyphCodepoint, &glyphOutline)) {
        return false;
    }

    const int defaultAlpha = alpha > 0 ? alpha : 255;
    auto clampColor = [](int value) -> int {
        return std::max(0, std::min(255, value));
    };
    const int iconR = clampColor(optIntValue("iconColorR", optIntValue("textColorR", r)));
    const int iconG = clampColor(optIntValue("iconColorG", optIntValue("textColorG", g)));
    const int iconB = clampColor(optIntValue("iconColorB", optIntValue("textColorB", b)));
    const int iconA = clampColor(optIntValue("iconColorA", optIntValue("textColorA", defaultAlpha)));

    const double rectWidth = fabs(rect.right - rect.left);
    const double rectHeight = fabs(rect.top - rect.bottom);
    if (rectWidth <= 0.0 || rectHeight <= 0.0) {
        return false;
    }

    const double rotation = optDoubleValue("rotation", 0.0);
    const double baseWidth = optDoubleValue("baseWidth", rectWidth);
    const double baseHeight = optDoubleValue("baseHeight", rectHeight);
    const double angleRad = rotation * M_PI / 180.0;
    const double absCos = fabs(cos(angleRad));
    const double absSin = fabs(sin(angleRad));
    const double storedExpandedWidth = (baseWidth * absCos) + (baseHeight * absSin);
    const double storedExpandedHeight = (baseWidth * absSin) + (baseHeight * absCos);
    double fitScale = 1.0;
    if (storedExpandedWidth > 0.0 && storedExpandedHeight > 0.0) {
        fitScale = std::min(rectWidth / storedExpandedWidth, rectHeight / storedExpandedHeight);
    }
    const double drawWidth = std::max(baseWidth * fitScale, 1.0);
    const double drawHeight = std::max(baseHeight * fitScale, 1.0);
    const double centerX = (rect.left + rect.right) / 2.0;
    const double centerY = (rect.top + rect.bottom) / 2.0;
    const bool flipHorizontal = optBoolValue(
            "effectiveFlipHorizontal",
            optBoolValue("flipHorizontal", false)
    );
    const bool flipVertical = optBoolValue(
            "effectiveFlipVertical",
            optBoolValue("flipVertical", false)
    );

    FPDF_PAGEOBJECT pathObj = CreateGlyphPathObject(glyphOutline);
    if (!pathObj) {
        return false;
    }

    FPDFPageObj_SetFillColor(pathObj, iconR, iconG, iconB, iconA);
    FPDFPath_SetDrawMode(pathObj, FPDF_FILLMODE_WINDING, JNI_FALSE);

    double tL = glyphOutline.minX;
    double tB = glyphOutline.minY;
    double tR = glyphOutline.maxX;
    double tT = glyphOutline.maxY;
    double glyphWidth = tR - tL;
    double glyphHeight = tT - tB;
    if (glyphWidth <= 0.0001 || glyphHeight <= 0.0001) {
        tL = 0.0f;
        tB = 0.0f;
        tR = 1.0f;
        tT = 1.0f;
        glyphWidth = 1.0;
        glyphHeight = 1.0;
    }

    const double scale = std::max(
            std::min(drawWidth / glyphWidth, drawHeight / glyphHeight),
            0.0001
    );
    const double glyphCenterX = ((tL + tR) / 2.0) * scale;
    const double glyphCenterY = ((tB + tT) / 2.0) * scale;
    const double cosA = cos(angleRad);
    const double sinA = sin(angleRad);
    const double flipX = flipHorizontal ? -1.0 : 1.0;
    const double flipY = flipVertical ? -1.0 : 1.0;
    const double flippedGlyphCenterX = glyphCenterX * flipX;
    const double flippedGlyphCenterY = glyphCenterY * flipY;
    const double matrixA = cosA * scale * flipX;
    const double matrixB = sinA * scale * flipX;
    const double matrixC = -sinA * scale * flipY;
    const double matrixD = cosA * scale * flipY;
    const double matrixE = centerX - ((flippedGlyphCenterX * cosA) - (flippedGlyphCenterY * sinA));
    const double matrixF = centerY - ((flippedGlyphCenterX * sinA) + (flippedGlyphCenterY * cosA));

    FPDFPageObj_Transform(pathObj, matrixA, matrixB, matrixC, matrixD, matrixE, matrixF);
    if (saveAsPageContent) {
        FPDFPage_InsertObject(page, pathObj);
        return true;
    }

    FPDFAnnot_SetRect(annot, &rect);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, iconR, iconG, iconB, iconA);
    FPDFAnnot_SetBorder(annot, 0, 0, 0);

    if (!FPDFAnnot_AppendObject(annot, pathObj)) {
        FPDFPageObj_Destroy(pathObj);
        return false;
    }

    FPDFAnnot_UpdateObject(annot, pathObj);
    SetAnnotWideStringValueFromJString(env, annot, "LufickImageMeta", jJsonStr);
    SetAnnotWideStringValueFromJString(env, annot, "LufickStickerMeta", jJsonStr);
    SetAnnotAsciiStringValue(annot, "LufickStampKind", "sticker");
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
    return true;
}

static int ResolvePdfImageOpacityAlpha(double rawOpacity) {
    const double normalizedOpacity = rawOpacity <= 1.0
                                     ? rawOpacity
                                     : rawOpacity / 255.0;
    return std::max(
            0,
            std::min(static_cast<int>(std::lround(normalizedOpacity * 255.0)), 255));
}

static bool processImageOrPresetStamp(
        JNIEnv* env,
        jobject obj,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        jfieldID imagePropsField,
        jclass jsonClass,
        jmethodID jsonInit,
        bool saveAsPageContent
) {
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, imagePropsField, jsonClass, jsonInit, "imageProperties");
    if (!jJsonStr) return false;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json) {
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    jstring jImagePathKey = env->NewStringUTF("flattenedAssetPath");
    jstring jStampKindKey = env->NewStringUTF("stampKind");
    jstring jAssetFormatKey = env->NewStringUTF("assetFormat");
    jstring jSignatureSubtypeKey = env->NewStringUTF("signatureSubType");
    jstring jImagePath = (jstring)env->CallObjectMethod(json, optS, jImagePathKey);
    jstring jStampKind = (jstring)env->CallObjectMethod(json, optS, jStampKindKey);
    jstring jAssetFormat = (jstring)env->CallObjectMethod(json, optS, jAssetFormatKey);
    jstring jSignatureSubtype = (jstring)env->CallObjectMethod(json, optS, jSignatureSubtypeKey);
    env->DeleteLocalRef(jImagePathKey);
    env->DeleteLocalRef(jStampKindKey);
    env->DeleteLocalRef(jAssetFormatKey);
    env->DeleteLocalRef(jSignatureSubtypeKey);

    const char* imagePath = jImagePath ? env->GetStringUTFChars(jImagePath, nullptr) : nullptr;
    const char* stampKind = jStampKind ? env->GetStringUTFChars(jStampKind, nullptr) : nullptr;
    const char* assetFormat = jAssetFormat ? env->GetStringUTFChars(jAssetFormat, nullptr) : nullptr;
    const std::string resolvedStampKind =
            (stampKind && strlen(stampKind) > 0) ? stampKind : "image";
    const std::string resolvedAssetFormat =
            (assetFormat && strlen(assetFormat) > 0) ? assetFormat : "";
    auto toLowerAscii = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    auto releaseJsonStrings = [&]() {
        if (assetFormat) env->ReleaseStringUTFChars(jAssetFormat, assetFormat);
        if (jAssetFormat) env->DeleteLocalRef(jAssetFormat);
        if (stampKind) env->ReleaseStringUTFChars(jStampKind, stampKind);
        if (jStampKind) env->DeleteLocalRef(jStampKind);
        if (imagePath) env->ReleaseStringUTFChars(jImagePath, imagePath);
        if (jImagePath) env->DeleteLocalRef(jImagePath);
    };

    const std::string normalizedStampKind = toLowerAscii(resolvedStampKind);
    const std::string normalizedAssetFormat = toLowerAscii(resolvedAssetFormat);
    jstring jPresetStampKey = env->NewStringUTF("presetStamp");
    const bool isPresetStamp = env->CallBooleanMethod(json, optB, jPresetStampKey, false);
    env->DeleteLocalRef(jPresetStampKey);
    const bool shouldSaveAsPresetStamp =
            normalizedStampKind == "preset_stamp" ||
            normalizedStampKind == "preset stamp" ||
            isPresetStamp;
    if (normalizedAssetFormat == "svg-path" ||
        normalizedAssetFormat == "svg" ||
        normalizedStampKind == "shape_element_svg") {
        const bool savedSvgPath = processSvgPathStamp(
                env,
                page,
                annot,
                rect,
                json,
                jJsonStr,
                optS,
                optD,
                optI,
                optB,
                0,
                0,
                0,
                255,
                saveAsPageContent
        );
        releaseJsonStrings();
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return savedSvgPath;
    }
    if (normalizedStampKind == "sticker" ||
        normalizedAssetFormat == "font-glyph" ||
        normalizedAssetFormat == "glyph-path") {
        const bool savedSticker = processStickerStamp(
                env,
                doc,
                page,
                annot,
                rect,
                json,
                jJsonStr,
                optS,
                optD,
                optI,
                optB,
                0,
                0,
                0,
                255,
                saveAsPageContent
        );
        releaseJsonStrings();
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return savedSticker;
    }

    if (!imagePath || strlen(imagePath) == 0) {
        releaseJsonStrings();
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    jstring jRotationKey = env->NewStringUTF("rotation");
    jstring jBaseWidthKey = env->NewStringUTF("baseWidth");
    jstring jBaseHeightKey = env->NewStringUTF("baseHeight");
    jstring jStretchToBoundsKey = env->NewStringUTF("stretchToBounds");
    jstring jOpacityKey = env->NewStringUTF("opacity");
    const double rectWidth = fabs(rect.right - rect.left);
    const double rectHeight = fabs(rect.top - rect.bottom);
    const double rotation = env->CallDoubleMethod(json, optD, jRotationKey, 0.0);
    const double baseWidth = env->CallDoubleMethod(json, optD, jBaseWidthKey, rectWidth);
    const double baseHeight = env->CallDoubleMethod(json, optD, jBaseHeightKey, rectHeight);
    const bool stretchToBounds = env->CallBooleanMethod(json, optB, jStretchToBoundsKey, false);
    const int opacityAlpha = ResolvePdfImageOpacityAlpha(
            env->CallDoubleMethod(json, optD, jOpacityKey, 1.0));
    env->DeleteLocalRef(jRotationKey);
    env->DeleteLocalRef(jBaseWidthKey);
    env->DeleteLocalRef(jBaseHeightKey);
    env->DeleteLocalRef(jStretchToBoundsKey);
    env->DeleteLocalRef(jOpacityKey);

    FPDF_PAGEOBJECT imageObj = FPDFPageObj_NewImageObj(doc);
    if (!imageObj) {
        releaseJsonStrings();
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    auto endsWith = [](const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    const std::string normalizedPath = toLowerAscii(imagePath);
    bool preferJpegInline =
            normalizedAssetFormat == "jpg" ||
            normalizedAssetFormat == "jpeg";
    if (!preferJpegInline && normalizedAssetFormat.empty()) {
        preferJpegInline =
                endsWith(normalizedPath, ".jpg") ||
                endsWith(normalizedPath, ".jpeg");
    }

    const bool loaded = preferJpegInline
                        ? LoadJpegFileIntoImageObject(imagePath, imageObj)
                        : LoadBitmapFileIntoImageObject(env, imagePath, imageObj);
    releaseJsonStrings();

    if (!loaded) {
        FPDFPageObj_Destroy(imageObj);
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    const double angleRad = rotation * M_PI / 180.0;
    const double absCos = fabs(cos(angleRad));
    const double absSin = fabs(sin(angleRad));
    const double storedExpandedWidth = (baseWidth * absCos) + (baseHeight * absSin);
    const double storedExpandedHeight = (baseWidth * absSin) + (baseHeight * absCos);
    double fitScale = 1.0;
    if (storedExpandedWidth > 0.0 && storedExpandedHeight > 0.0) {
        fitScale = std::min(rectWidth / storedExpandedWidth, rectHeight / storedExpandedHeight);
    }
    const double drawWidth = std::max(stretchToBounds ? baseWidth : baseWidth * fitScale, 1.0);
    const double drawHeight = std::max(stretchToBounds ? baseHeight : baseHeight * fitScale, 1.0);
    const double centerX = (rect.left + rect.right) / 2.0;
    const double centerY = (rect.top + rect.bottom) / 2.0;
    const double cosA = cos(angleRad);
    const double sinA = sin(angleRad);
    const double a = cosA * drawWidth;
    const double b = sinA * drawWidth;
    const double c = -sinA * drawHeight;
    const double d = cosA * drawHeight;
    const double e = centerX - ((a + c) / 2.0);
    const double f = centerY - ((b + d) / 2.0);

    FPDFPageObj_SetFillColor(imageObj, 255, 255, 255, opacityAlpha);
    FPDFImageObj_SetMatrix(imageObj, a, b, c, d, e, f);
    if (saveAsPageContent) {
        FPDFPage_InsertObject(page, imageObj);
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return true;
    }

    if (!annot) {
        FPDFPageObj_Destroy(imageObj);
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    if (!FPDFAnnot_AppendObject(annot, imageObj)) {
        FPDFPageObj_Destroy(imageObj);
        if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    FPDFAnnot_UpdateObject(annot, imageObj);
    const jchar* rawJsonContent = env->GetStringChars(jJsonStr, nullptr);
    FPDFAnnot_SetStringValue(annot, "LufickImageMeta", (FPDF_WIDESTRING)rawJsonContent);
    SetAnnotAsciiStringValue(
            annot,
            "LufickStampKind",
            shouldSaveAsPresetStamp ? "preset_stamp" : resolvedStampKind.c_str()
    );
    if (shouldSaveAsPresetStamp) {
        FPDFAnnot_SetStringValue(annot, "LufickPresetStampMeta", (FPDF_WIDESTRING)rawJsonContent);
        SetAnnotAsciiStringValue(annot, "LufickPresetStamp", "1");
    }
    if (jSignatureSubtype && env->GetStringLength(jSignatureSubtype) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "LufickSignatureSubtype", jSignatureSubtype);
        if (JStringToUtf16(env, jSignatureSubtype) == u"Sign_Image") {
            SetAnnotAsciiStringValue(annot, "LufickSignImage", "1");
        }
    }
    env->ReleaseStringChars(jJsonStr, rawJsonContent);
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
    if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
    return true;
}

static bool processPdfShape(
        JNIEnv* env,
        jobject obj,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        int typeInt,
        jfieldID shapePropsField,
        int r,
        int g,
        int b,
        int alpha,
        jclass jsonClass,
        jmethodID jsonInit
) {
    if (!annot) return false;

    jstring jJsonStr = shapePropsField
            ? GetBridgeDataPropertyJString(env, obj, shapePropsField, jsonClass, jsonInit, "shapeProperties")
            : nullptr;
    jobject json = jJsonStr ? env->NewObject(jsonClass, jsonInit, jJsonStr) : nullptr;

    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        if (!json || !optD) return fallback;
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optIntValue = [&](const char* key, int fallback) -> int {
        if (!json || !optI) return fallback;
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    FS_RECTF baseRect;
    baseRect.left = static_cast<float>(optDoubleValue("baseLeft", rect.left));
    baseRect.top = static_cast<float>(optDoubleValue("baseTop", rect.top));
    baseRect.right = static_cast<float>(optDoubleValue("baseRight", rect.right));
    baseRect.bottom = static_cast<float>(optDoubleValue("baseBottom", rect.bottom));

    const float rotation = static_cast<float>(optDoubleValue("rotation", 0.0));
    const float strokeWidth = static_cast<float>(optDoubleValue("strokeWidth", 1.0));
    const float cornerRadius = static_cast<float>(optDoubleValue("cornerRadius", 0.0));
    const float dashWidth = static_cast<float>(optDoubleValue("dashWidth", 0.0));
    const float dashGap = static_cast<float>(optDoubleValue("dashGap", 0.0));
    const int strokeR = optIntValue("strokeR", r);
    const int strokeG = optIntValue("strokeG", g);
    const int strokeB = optIntValue("strokeB", b);
    const int strokeA = std::max(0, std::min(optIntValue("strokeA", alpha), 255));
    const int fillR = optIntValue("fillR", 0);
    const int fillG = optIntValue("fillG", 0);
    const int fillB = optIntValue("fillB", 0);
    const int requestedFillA = std::max(0, std::min(optIntValue("fillA", 0), 255));
    const int fillA = requestedFillA > 0 ? strokeA : 0;
    bool arrowStart = false;
    bool arrowEnd = typeInt == 18;
    if (json && optB) {
        jstring arrowStartKey = env->NewStringUTF("arrowStart");
        jstring arrowEndKey = env->NewStringUTF("arrowEnd");
        arrowStart = env->CallBooleanMethod(json, optB, arrowStartKey, false);
        arrowEnd = env->CallBooleanMethod(json, optB, arrowEndKey, typeInt == 18);
        env->DeleteLocalRef(arrowStartKey);
        env->DeleteLocalRef(arrowEndKey);
    }
    const std::vector<PdfShapePoint> customPoints = ReadPdfShapePointsFromJson(env, json, jsonClass);

    FPDFAnnot_SetBorder(annot, 0, 0, fmax(strokeWidth, 0.0f));
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, strokeR, strokeG, strokeB, strokeA);
    if (fillA > 0) {
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, fillR, fillG, fillB, fillA);
    }

    if (jJsonStr) {
        SetAnnotWideStringValueFromJString(env, annot, "LufickPdfShapeMeta", jJsonStr);
    }
    SetAnnotAsciiStringValue(annot, "LufickPdfShapeType", GetPdfShapeName(typeInt));
    SetAnnotAsciiStringValue(annot, "LufickStampKind", "shape");
    if (IsDirectNativePdfBoxShape(typeInt) && fabs(rotation) >= 0.001f) {
        SetAnnotAsciiStringValue(annot, GetPdfBoxShapePatchMarkerKey(typeInt), "1");
        SetPdfShapeFloatMarker(annot, "LufickPdfBoxShapeRotation", rotation);
        SetPdfShapeFloatMarker(annot, "LufickPdfBoxShapeBaseLeft", baseRect.left);
        SetPdfShapeFloatMarker(annot, "LufickPdfBoxShapeBaseTop", baseRect.top);
        SetPdfShapeFloatMarker(annot, "LufickPdfBoxShapeBaseRight", baseRect.right);
        SetPdfShapeFloatMarker(annot, "LufickPdfBoxShapeBaseBottom", baseRect.bottom);
        if (strokeA < 255) {
            std::ostringstream alphaMarker;
            alphaMarker << "LufickPdfBoxShapeAlpha" << strokeA;
            SetAnnotAsciiStringValue(annot, alphaMarker.str().c_str(), "1");
            static const std::string opacityPatchPad(128, ' ');
            SetAnnotAsciiStringValue(annot, "LufickPdfBoxShapeOpacityPad", opacityPatchPad.c_str());
        }
    }
    if (NeedsSavedPdfShapeDictionaryPatch(typeInt)) {
        SetAnnotAsciiStringValue(annot, GetPdfShapePatchMarkerKey(typeInt), "1");
        std::ostringstream alphaMarker;
        alphaMarker << "LufickPdfShapeAlpha" << strokeA;
        SetAnnotAsciiStringValue(annot, alphaMarker.str().c_str(), "1");
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeRotation", rotation);
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeBaseLeft", baseRect.left);
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeBaseTop", baseRect.top);
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeBaseRight", baseRect.right);
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeBaseBottom", baseRect.bottom);
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeArrowStart", arrowStart ? 1.0f : 0.0f);
        SetPdfShapeFloatMarker(annot, "LufickPdfShapeArrowEnd", arrowEnd ? 1.0f : 0.0f);
        if ((typeInt == 15 || typeInt == 16 || typeInt == 17 || typeInt == 18) && customPoints.size() >= 2) {
            const size_t maxPointCount = (typeInt == 17 || typeInt == 18) ? 2 : 64;
            const size_t pointCount = std::min(customPoints.size(), maxPointCount);
            for (size_t index = 0; index < pointCount; index++) {
                std::ostringstream xMarker;
                xMarker << "LufickPdfShapePoint" << index << "X";
                std::ostringstream yMarker;
                yMarker << "LufickPdfShapePoint" << index << "Y";
                SetPdfShapeFloatMarker(annot, xMarker.str().c_str(), customPoints[index].x);
                SetPdfShapeFloatMarker(annot, yMarker.str().c_str(), customPoints[index].y);
            }
        }
        static const std::string patchPad(8192, ' ');
        SetAnnotAsciiStringValue(annot, "LufickPdfShapePatchPad", patchPad.c_str());
    }

    const bool useRotatedAlphaObject =
            !NeedsSavedPdfShapeDictionaryPatch(typeInt) &&
            fabs(rotation) >= 0.001f &&
            (strokeA < 255 || (fillA > 0 && fillA < 255));
    const bool appendedRotatedAlphaObject = useRotatedAlphaObject && AppendPdfShapeAppearanceObject(
            annot,
            typeInt,
            baseRect,
            rotation,
            strokeR,
            strokeG,
            strokeB,
            strokeA,
            fillR,
            fillG,
            fillB,
            fillA,
            strokeWidth,
            cornerRadius,
            dashWidth,
            dashGap
    );

    if (appendedRotatedAlphaObject) {
        // The appended annotation object keeps RGBA alpha; replacing it with a
        // raw AP stream would lose opacity for rotated box-based shapes.
    } else if (ShouldUseCustomPdfShapeAppearanceStream(typeInt, strokeA, fillA, rotation)) {
        const std::u16string appearanceStream = BuildPdfShapeAppearanceStream(
                typeInt,
                baseRect,
                rotation,
                strokeR,
                strokeG,
                strokeB,
                strokeA,
                fillR,
                fillG,
                fillB,
                fillA,
                strokeWidth,
                arrowStart,
                arrowEnd,
                customPoints.empty() ? nullptr : &customPoints
        );
        if (!appearanceStream.empty()) {
            FPDFAnnot_SetAP(
                    annot,
                    FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                    reinterpret_cast<FPDF_WIDESTRING>(appearanceStream.c_str())
            );
        }
    } else {
        FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);
    }

    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
    if (json) env->DeleteLocalRef(json);
    if (jJsonStr) env->DeleteLocalRef(jJsonStr);
    return true;
}

static bool processPdfShapeContent(
        JNIEnv* env,
        jobject obj,
        FPDF_PAGE page,
        FS_RECTF rect,
        int typeInt,
        jfieldID shapePropsField,
        int r,
        int g,
        int b,
        int alpha,
        jclass jsonClass,
        jmethodID jsonInit
) {
    if (!page || !IsPdfShapeNativeType(typeInt)) return false;

    jstring jJsonStr = shapePropsField
            ? GetBridgeDataPropertyJString(env, obj, shapePropsField, jsonClass, jsonInit, "shapeProperties")
            : nullptr;
    jobject json = jJsonStr ? env->NewObject(jsonClass, jsonInit, jJsonStr) : nullptr;

    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        if (!json || !optD) return fallback;
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(json, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    auto optIntValue = [&](const char* key, int fallback) -> int {
        if (!json || !optI) return fallback;
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    FS_RECTF baseRect;
    baseRect.left = static_cast<float>(optDoubleValue("baseLeft", rect.left));
    baseRect.top = static_cast<float>(optDoubleValue("baseTop", rect.top));
    baseRect.right = static_cast<float>(optDoubleValue("baseRight", rect.right));
    baseRect.bottom = static_cast<float>(optDoubleValue("baseBottom", rect.bottom));

    const float rotation = static_cast<float>(optDoubleValue("rotation", 0.0));
    const float strokeWidth = static_cast<float>(optDoubleValue("strokeWidth", 1.0));
    const float cornerRadius = static_cast<float>(optDoubleValue("cornerRadius", 0.0));
    const float dashWidth = static_cast<float>(optDoubleValue("dashWidth", 0.0));
    const float dashGap = static_cast<float>(optDoubleValue("dashGap", 0.0));
    (void)dashWidth;
    (void)dashGap;
    const int strokeR = optIntValue("strokeR", r);
    const int strokeG = optIntValue("strokeG", g);
    const int strokeB = optIntValue("strokeB", b);
    const int strokeA = std::max(0, std::min(optIntValue("strokeA", alpha), 255));
    const int fillR = optIntValue("fillR", 0);
    const int fillG = optIntValue("fillG", 0);
    const int fillB = optIntValue("fillB", 0);
    const int requestedFillA = std::max(0, std::min(optIntValue("fillA", 0), 255));
    const int fillA = requestedFillA > 0 ? strokeA : 0;
    bool arrowStart = false;
    bool arrowEnd = typeInt == 18;
    if (json && optB) {
        jstring arrowStartKey = env->NewStringUTF("arrowStart");
        jstring arrowEndKey = env->NewStringUTF("arrowEnd");
        arrowStart = env->CallBooleanMethod(json, optB, arrowStartKey, false);
        arrowEnd = env->CallBooleanMethod(json, optB, arrowEndKey, typeInt == 18);
        env->DeleteLocalRef(arrowStartKey);
        env->DeleteLocalRef(arrowEndKey);
    }
    const std::vector<PdfShapePoint> customPoints = ReadPdfShapePointsFromJson(env, json, jsonClass);

    std::vector<PdfShapePoint> linePoints;
    FPDF_PAGEOBJECT shapePath = CreatePdfShapeContentPath(
            typeInt,
            baseRect,
            rotation,
            cornerRadius,
            customPoints,
            &linePoints
    );
    const bool allowFill = typeInt != 16 && typeInt != 17 && typeInt != 18;
    bool inserted = InsertPdfShapeContentPath(
            page,
            shapePath,
            allowFill,
            strokeR,
            strokeG,
            strokeB,
            strokeA,
            fillR,
            fillG,
            fillB,
            fillA,
            strokeWidth
    );

    if (inserted && (typeInt == 16 || typeInt == 17 || typeInt == 18) && linePoints.size() >= 2) {
        if (arrowStart) {
            inserted = InsertPdfShapeArrowHeadContent(
                    page,
                    linePoints[1],
                    linePoints[0],
                    strokeR,
                    strokeG,
                    strokeB,
                    strokeA,
                    strokeWidth
            ) || inserted;
        }
        if (arrowEnd) {
            inserted = InsertPdfShapeArrowHeadContent(
                    page,
                    linePoints[linePoints.size() - 2],
                    linePoints.back(),
                    strokeR,
                    strokeG,
                    strokeB,
                    strokeA,
                    strokeWidth
            ) || inserted;
        }
    }

    if (json) env->DeleteLocalRef(json);
    if (jJsonStr) env->DeleteLocalRef(jJsonStr);
    return inserted;
}

static bool jsonStringContainsSimplePdfStamp(JNIEnv* env, jstring jsonString) {
    if (!jsonString) return false;
    const char* rawJson = env->GetStringUTFChars(jsonString, nullptr);
    const bool result =
            rawJson &&
            strstr(rawJson, "\"stampKind\":\"simple_pdf_stamp\"") != nullptr;
    if (rawJson) env->ReleaseStringUTFChars(jsonString, rawJson);
    return result;
}

static bool isSimplePdfStampBridgeAnnotation(
        JNIEnv* env,
        jobject obj,
        jfieldID dataPropsField,
        jclass jsonClass,
        jmethodID jsonInit
) {
    jstring simpleProps = GetBridgeDataPropertyJString(env, obj, dataPropsField, jsonClass, jsonInit, "simplePdfStampProperties");
    const bool isSimpleStamp = jsonStringContainsSimplePdfStamp(env, simpleProps);
    if (simpleProps) env->DeleteLocalRef(simpleProps);
    if (isSimpleStamp) return true;

    jstring rawDataProps = dataPropsField ? (jstring)env->GetObjectField(obj, dataPropsField) : nullptr;
    const char* rawData = rawDataProps ? env->GetStringUTFChars(rawDataProps, nullptr) : nullptr;
    const bool hasSimpleStampKey =
            rawData &&
            strstr(rawData, "simplePdfStampProperties") != nullptr;
    if (rawData) env->ReleaseStringUTFChars(rawDataProps, rawData);
    if (rawDataProps) env->DeleteLocalRef(rawDataProps);
    return hasSimpleStampKey;
}

static bool appendSimplePdfStampShapeObject(
        JNIEnv* env,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        jobject shapeJson,
        jclass jsonClass,
        bool saveAsPageContent = false
) {
    if (!shapeJson) return false;
    if (saveAsPageContent ? !page : !annot) return false;

    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID toStringMethod = env->GetMethodID(jsonClass, "toString", "()Ljava/lang/String;");
    jstring jShapeString = (jstring)env->CallObjectMethod(shapeJson, toStringMethod);
    const char* shapeString = jShapeString ? env->GetStringUTFChars(jShapeString, nullptr) : nullptr;
    const bool isSpike = shapeString && (
            strstr(shapeString, "\"shapeType\":\"spike\"") != nullptr ||
            strstr(shapeString, "\"shapeType\": \"spike\"") != nullptr);
    const int typeInt = shapeString ? GetPdfShapeTypeFromMeta(shapeString, 12) : 12;
    if (shapeString) env->ReleaseStringUTFChars(jShapeString, shapeString);
    if (jShapeString) env->DeleteLocalRef(jShapeString);

    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(shapeJson, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(shapeJson, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };

    const float rawLeft = static_cast<float>(optDoubleValue("baseLeft", 0.0));
    const float rawTop = static_cast<float>(optDoubleValue("baseTop", 0.0));
    const float rawRight = static_cast<float>(optDoubleValue("baseRight", 0.0));
    const float rawBottom = static_cast<float>(optDoubleValue("baseBottom", 0.0));
    FS_RECTF baseRect;
    baseRect.left = fmin(rawLeft, rawRight);
    baseRect.right = fmax(rawLeft, rawRight);
    baseRect.bottom = fmin(rawTop, rawBottom);
    baseRect.top = fmax(rawTop, rawBottom);
    const float rotation = static_cast<float>(optDoubleValue("rotation", 0.0));
    const float strokeWidth = static_cast<float>(optDoubleValue("strokeWidth", 1.0));
    const float cornerRadius = static_cast<float>(optDoubleValue("cornerRadius", 0.0));
    const float dashWidth = static_cast<float>(optDoubleValue("dashWidth", 0.0));
    const float dashGap = static_cast<float>(optDoubleValue("dashGap", 0.0));
    const float startAngle = static_cast<float>(optDoubleValue("startAngle", 0.0));
    const float sweepAngle = static_cast<float>(optDoubleValue("sweepAngle", 360.0));
    const int spikeCount = std::max(2, std::min(optIntValue("spikeCount", 12), 180));

    const int strokeR = optIntValue("strokeR", 0);
    const int strokeG = optIntValue("strokeG", 0);
    const int strokeB = optIntValue("strokeB", 0);
    const int strokeA = std::max(0, std::min(optIntValue("strokeA", 255), 255));
    const int fillR = optIntValue("fillR", 0);
    const int fillG = optIntValue("fillG", 0);
    const int fillB = optIntValue("fillB", 0);
    const int fillA = std::max(0, std::min(optIntValue("fillA", 0), 255));

    if (saveAsPageContent) {
        return AppendPdfShapeAppearanceObject(
                nullptr,
                typeInt,
                baseRect,
                rotation,
                strokeR,
                strokeG,
                strokeB,
                strokeA,
                fillR,
                fillG,
                fillB,
                fillA,
                strokeWidth,
                cornerRadius,
                dashWidth,
                dashGap,
                startAngle,
                sweepAngle,
                isSpike,
                spikeCount,
                page
        );
    }

    return AppendPdfShapeAppearanceObject(
            annot,
            typeInt,
            baseRect,
            rotation,
            strokeR,
            strokeG,
            strokeB,
            strokeA,
            fillR,
            fillG,
            fillB,
            fillA,
            strokeWidth,
            cornerRadius,
            dashWidth,
            dashGap,
            startAngle,
            sweepAngle,
            isSpike,
            spikeCount
    );
}

static bool appendSimplePdfStampTextObject(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        jobject textJson,
        jclass jsonClass,
        int r,
        int g,
        int b,
        int alpha,
        bool saveAsPageContent = false
) {
    if (!doc || !textJson) return false;
    if (saveAsPageContent ? !page : !annot) return false;

    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optB = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");

    auto optStringValue = [&](const char* key, const char* fallback) -> jstring {
        jstring jKey = env->NewStringUTF(key);
        jstring jFallback = env->NewStringUTF(fallback);
        jstring value = (jstring)env->CallObjectMethod(textJson, optS, jKey, jFallback);
        env->DeleteLocalRef(jKey);
        env->DeleteLocalRef(jFallback);
        return value;
    };
    auto optDoubleValue = [&](const char* key, double fallback) -> double {
        jstring jKey = env->NewStringUTF(key);
        const double value = env->CallDoubleMethod(textJson, optD, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(textJson, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    auto optBoolValue = [&](const char* key, bool fallback) -> bool {
        jstring jKey = env->NewStringUTF(key);
        const bool value = env->CallBooleanMethod(textJson, optB, jKey, fallback ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
        env->DeleteLocalRef(jKey);
        return value;
    };

    jstring jText = optStringValue("text", "");
    jstring jLayoutText = optStringValue("layoutText", "");
    jstring jFont = optStringValue("font", "");
    jstring jAlign = optStringValue("alignment", "center");
    jstring jFontPath = optStringValue("fontPath", "");
    jstring jTextSource = (jLayoutText && env->GetStringLength(jLayoutText) > 0) ? jLayoutText : jText;
    const char16_t* textContent = jTextSource ? (const char16_t*)env->GetStringChars(jTextSource, nullptr) : nullptr;
    const char* fontName = jFont ? env->GetStringUTFChars(jFont, nullptr) : nullptr;
    const char* alignStr = jAlign ? env->GetStringUTFChars(jAlign, nullptr) : "center";
    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;
    if (!textContent) {
        if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
        if (alignStr && jAlign) env->ReleaseStringUTFChars(jAlign, alignStr);
        if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
        if (jFontPath) env->DeleteLocalRef(jFontPath);
        if (jAlign) env->DeleteLocalRef(jAlign);
        if (jFont) env->DeleteLocalRef(jFont);
        if (jLayoutText) env->DeleteLocalRef(jLayoutText);
        if (jText) env->DeleteLocalRef(jText);
        return false;
    }

    const int defaultAlpha = alpha > 0 ? alpha : 255;
    const int textR = optIntValue("textColorR", r);
    const int textG = optIntValue("textColorG", g);
    const int textB = optIntValue("textColorB", b);
    const int textA = optIntValue("textColorA", defaultAlpha);
    const bool hasBg = optBoolValue("hasBackground", false);
    const bool isBold = optBoolValue("bold", false);
    const bool isItalic = optBoolValue("italic", false);
    const bool hasUnderline = optBoolValue("underline", false);
    const bool hasStrikeout = optBoolValue("strikeout", false);
    const double rotation = optDoubleValue("rotation", 0.0);
    const float initialWidth = static_cast<float>(optDoubleValue("width", fabs(rect.right - rect.left)));
    const float initialHeight = static_cast<float>(optDoubleValue("height", fabs(rect.top - rect.bottom)));
    const float origCenterX = (rect.left + rect.right) / 2.0f;
    const float centerY = (rect.bottom + rect.top) / 2.0f;
    const double angleRad = rotation * M_PI / 180.0;
    const double cosA = cos(angleRad);
    const double sinA = sin(angleRad);

    if (hasBg) {
        FPDF_PAGEOBJECT bgObj = FPDFPageObj_CreateNewRect(-initialWidth / 2.0f, -initialHeight / 2.0f, initialWidth, initialHeight);
        if (bgObj) {
            FPDFPageObj_SetFillColor(
                    bgObj,
                    optIntValue("bgColorR", 255),
                    optIntValue("bgColorG", 255),
                    optIntValue("bgColorB", 255),
                    static_cast<int>(optDoubleValue("bgOpacity", 0.0) * 255.0)
            );
            FPDFPath_SetDrawMode(bgObj, 1, JNI_FALSE);
            FPDFPageObj_Transform(bgObj, cosA, sinA, -sinA, cosA, origCenterX, centerY);
            if (saveAsPageContent) {
                FPDFPage_InsertObject(page, bgObj);
            } else {
                FPDFAnnot_AppendObject(annot, bgObj);
                FPDFAnnot_UpdateObject(annot, bgObj);
            }
        }
    }

    FPDF_FONT loadedFont = nullptr;
    if (fontPath && strlen(fontPath) > 0) {
        FILE* f = fopen(fontPath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fSize = ftell(f);
            rewind(f);
            std::vector<uint8_t> buffer(fSize);
            fread(buffer.data(), 1, fSize, f);
            fclose(f);
            loadedFont = FPDFText_LoadFont(doc, buffer.data(), fSize, FPDF_FONT_TRUETYPE, true);
        }
    }
    if (!loadedFont) {
        const char* fallbackFont = "Helvetica";
        if (fontName) {
            if (strcmp(fontName, "serif") == 0 || strstr(fontName, "Serif") != nullptr || strstr(fontName, "serif") != nullptr) {
                fallbackFont = "Times-Roman";
            } else if (strstr(fontName, "Mono") != nullptr || strstr(fontName, "mono") != nullptr || strstr(fontName, "Courier") != nullptr) {
                fallbackFont = "Courier";
            }
        }
        loadedFont = FPDFText_LoadStandardFont(doc, fallbackFont);
    }

    bool appended = false;
    std::vector<std::u16string> layoutLines;
    std::u16string currentLine;
    const int textLength = env->GetStringLength(jTextSource);
    for (int i = 0; i < textLength; i++) {
        const char16_t ch = textContent[i];
        if (ch == u'\n') {
            layoutLines.push_back(currentLine);
            currentLine.clear();
        } else if (ch != u'\r') {
            currentLine.push_back(ch);
        }
    }
    layoutLines.push_back(currentLine);
    if (layoutLines.empty()) layoutLines.push_back(u"");

    const float scale = static_cast<float>(optDoubleValue("size", 12.0));
    const float canvasWidth = fmax(static_cast<float>(optDoubleValue("canvasWidth", initialWidth)), 1.0f);
    const float canvasHeight = fmax(static_cast<float>(optDoubleValue("canvasHeight", initialHeight)), 1.0f);
    const float fontSpacingPx = fmax(static_cast<float>(optDoubleValue("fontSpacingPx", canvasHeight)), 0.0001f);
    const float fontAscentPx = static_cast<float>(optDoubleValue("fontAscentPx", -fontSpacingPx * 0.8f));
    const float lineSpacing = fmax(static_cast<float>(optDoubleValue("lineSpacing", 1.0)), 0.1f);
    const float textPaddingPx = fmax(
            static_cast<float>(
                    optDoubleValue("textPaddingPx", optDoubleValue("textPadding", 0.0))
            ),
            0.0f
    );
    const float pxToPageX = initialWidth / canvasWidth;
    const float pxToPageY = initialHeight / canvasHeight;
    const float totalTextHeightPx = (layoutLines.size() * fontSpacingPx) +
            ((static_cast<int>(layoutLines.size()) - 1) > 0 ? (layoutLines.size() - 1) * fontSpacingPx * (lineSpacing - 1.0f) : 0.0f);
    float baselineYPx = ((canvasHeight - totalTextHeightPx) / 2.0f) - fontAscentPx;
    const float skewX = isItalic ? 0.25f : 0.0f;
    float decorationLeft = 0.0f;
    float decorationWidth = 0.0f;
    float decorationY = 0.0f;
    float decorationHeight = scale;

    for (size_t lineIndex = 0; lineIndex < layoutLines.size(); lineIndex++) {
        const std::u16string& lineText = layoutLines[lineIndex];
        if (!lineText.empty()) {
            FPDF_PAGEOBJECT textObj = FPDFPageObj_CreateTextObj(doc, loadedFont, 1.0f);
            if (textObj) {
                FPDFText_SetText(textObj, (FPDF_WIDESTRING)lineText.c_str());
                FPDFPageObj_SetFillColor(textObj, textR, textG, textB, textA);
                float tL, tB, tR, tT;
                FPDFPageObj_GetBounds(textObj, &tL, &tB, &tR, &tT);
                const float textW = (tR - tL) * scale;
                const float textH = (tT - tB) * scale;
                const float lineWidthPx = textW / fmax(pxToPageX, 0.0001f);
                float xPx = (canvasWidth - lineWidthPx) / 2.0f;
                if (strcmp(alignStr, "left") == 0) xPx = textPaddingPx / 2.0f;
                else if (strcmp(alignStr, "right") == 0) xPx = canvasWidth - lineWidthPx - (textPaddingPx / 2.0f);
                const float localX = (xPx * pxToPageX) - (initialWidth / 2.0f) - (tL * scale);
                const float localY = (initialHeight / 2.0f) - (baselineYPx * pxToPageY) - (tB * scale);

                FPDFPageObj_Transform(textObj, cosA * scale, sinA * scale, (-sinA + skewX) * scale, cosA * scale,
                                      origCenterX + (localX * cosA - localY * sinA),
                                      centerY + (localX * sinA + localY * cosA));
                if (isBold) {
                    FPDFPageObj_SetStrokeColor(textObj, textR, textG, textB, textA);
                    FPDFPageObj_SetStrokeWidth(textObj, textH * 0.05f);
                    FPDFTextObj_SetTextRenderMode(textObj, FPDF_TEXTRENDERMODE_FILL_STROKE);
                }
                const bool lineAppended = saveAsPageContent
                                          ? true
                                          : FPDFAnnot_AppendObject(annot, textObj) != 0;
                if (lineAppended) {
                    if (saveAsPageContent) {
                        FPDFPage_InsertObject(page, textObj);
                    } else {
                        FPDFAnnot_UpdateObject(annot, textObj);
                    }
                    appended = true;
                    decorationLeft = xPx * pxToPageX;
                    decorationWidth = textW;
                    decorationY = localY;
                    decorationHeight = textH;
                } else {
                    FPDFPageObj_Destroy(textObj);
                }
            }
        }
        if (lineIndex < layoutLines.size() - 1) {
            baselineYPx += fontSpacingPx * lineSpacing;
        }
    }

    auto drawLine = [&](float baselineOffset) {
        if (!appended || decorationWidth <= 0.0f) return;
        FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(0, 0);
        if (!line) return;
        const float lineY = decorationY + (baselineOffset * decorationHeight);
        FPDFPath_LineTo(line, decorationWidth, 0);
        FPDFPageObj_Transform(line, cosA, sinA, -sinA, cosA,
                              origCenterX + ((decorationLeft - initialWidth / 2.0f) * cosA - lineY * sinA),
                              centerY + ((decorationLeft - initialWidth / 2.0f) * sinA + lineY * cosA));
        FPDFPageObj_SetStrokeColor(line, textR, textG, textB, textA);
        FPDFPageObj_SetStrokeWidth(line, decorationHeight * 0.05f);
        FPDFPath_SetDrawMode(line, 0, JNI_TRUE);
        if (saveAsPageContent) {
            FPDFPage_InsertObject(page, line);
        } else if (FPDFAnnot_AppendObject(annot, line)) {
            FPDFAnnot_UpdateObject(annot, line);
        } else {
            FPDFPageObj_Destroy(line);
        }
    };
    if (hasUnderline) drawLine(-0.15f);
    if (hasStrikeout) drawLine(0.30f);

    env->ReleaseStringChars(jTextSource, (const jchar*)textContent);
    if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
    if (alignStr && jAlign) env->ReleaseStringUTFChars(jAlign, alignStr);
    if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
    if (jFontPath) env->DeleteLocalRef(jFontPath);
    if (jAlign) env->DeleteLocalRef(jAlign);
    if (jFont) env->DeleteLocalRef(jFont);
    if (jLayoutText) env->DeleteLocalRef(jLayoutText);
    if (jText) env->DeleteLocalRef(jText);
    return appended;
}

static bool processSimplePdfStamp(
        JNIEnv* env,
        jobject obj,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        int typeInt,
        jfieldID dataPropsField,
        int r,
        int g,
        int b,
        int alpha,
        jclass jsonClass,
        jmethodID jsonInit,
        bool saveAsPageContent
) {
    (void)typeInt;
    if (saveAsPageContent ? !page : !annot) return false;
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, dataPropsField, jsonClass, jsonInit, "simplePdfStampProperties");
    if (!jJsonStr) return false;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json) {
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    jmethodID optJSONArray = env->GetMethodID(jsonClass, "optJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");
    jstring childrenKey = env->NewStringUTF("children");
    jobject children = env->CallObjectMethod(json, optJSONArray, childrenKey);
    env->DeleteLocalRef(childrenKey);
    if (!children) {
        env->DeleteLocalRef(json);
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    jclass arrayClass = env->FindClass("org/json/JSONArray");
    jmethodID arrayLength = env->GetMethodID(arrayClass, "length", "()I");
    jmethodID arrayGetJSONObject = env->GetMethodID(arrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jmethodID childOptString = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID childOptDouble = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID childOptJSONObject = env->GetMethodID(jsonClass, "optJSONObject", "(Ljava/lang/String;)Lorg/json/JSONObject;");
    jstring kindKey = env->NewStringUTF("kind");
    jstring emptyValue = env->NewStringUTF("");
    jstring textPropsKey = env->NewStringUTF("textProperties");
    jstring shapePropsKey = env->NewStringUTF("shapeProperties");

    FS_RECTF paddedRect = rect;
    const float containerWidth = fabs(rect.right - rect.left);
    const float containerHeight = fabs(rect.top - rect.bottom);
    const float containerPadding = fmax(1.0f, fmin(containerWidth, containerHeight) * 0.04f);
    paddedRect.left -= containerPadding;
    paddedRect.right += containerPadding;
    paddedRect.top += containerPadding;
    paddedRect.bottom -= containerPadding;

    if (!saveAsPageContent) {
        FPDFAnnot_SetRect(annot, &paddedRect);
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
        FPDFAnnot_SetBorder(annot, 0, 0, 0);
    }

    int appendedCount = 0;
    const int childCount = env->CallIntMethod(children, arrayLength);
    for (int index = 0; index < childCount; index++) {
        jobject child = env->CallObjectMethod(children, arrayGetJSONObject, index);
        if (!child) continue;
        jstring kindValue = (jstring)env->CallObjectMethod(child, childOptString, kindKey, emptyValue);
        const char* kind = kindValue ? env->GetStringUTFChars(kindValue, nullptr) : "";

        auto childDoubleValue = [&](const char* key, double fallback) -> double {
            jstring jKey = env->NewStringUTF(key);
            const double value = env->CallDoubleMethod(child, childOptDouble, jKey, fallback);
            env->DeleteLocalRef(jKey);
            return value;
        };
        const float rawLeft = static_cast<float>(childDoubleValue("left", rect.left));
        const float rawTop = static_cast<float>(childDoubleValue("top", rect.top));
        const float rawRight = static_cast<float>(childDoubleValue("right", rect.right));
        const float rawBottom = static_cast<float>(childDoubleValue("bottom", rect.bottom));
        FS_RECTF childRect;
        childRect.left = fmin(rawLeft, rawRight);
        childRect.right = fmax(rawLeft, rawRight);
        childRect.bottom = fmin(rawTop, rawBottom);
        childRect.top = fmax(rawTop, rawBottom);

        bool appended = false;
        if (strcmp(kind, "text") == 0) {
            jobject textProps = env->CallObjectMethod(child, childOptJSONObject, textPropsKey);
            appended = appendSimplePdfStampTextObject(env, doc, page, annot, childRect, textProps, jsonClass, r, g, b, alpha, saveAsPageContent);
            if (textProps) env->DeleteLocalRef(textProps);
        } else if (strcmp(kind, "shape") == 0) {
            jobject shapeProps = env->CallObjectMethod(child, childOptJSONObject, shapePropsKey);
            appended = appendSimplePdfStampShapeObject(env, page, annot, shapeProps, jsonClass, saveAsPageContent);
            if (shapeProps) env->DeleteLocalRef(shapeProps);
        }
        if (appended) appendedCount++;

        if (kindValue && kind) env->ReleaseStringUTFChars(kindValue, kind);
        if (kindValue) env->DeleteLocalRef(kindValue);
        env->DeleteLocalRef(child);
    }

    env->DeleteLocalRef(kindKey);
    env->DeleteLocalRef(emptyValue);
    env->DeleteLocalRef(textPropsKey);
    env->DeleteLocalRef(shapePropsKey);
    env->DeleteLocalRef(children);
    env->DeleteLocalRef(json);

    if (appendedCount <= 0) {
        env->DeleteLocalRef(jJsonStr);
        return false;
    }

    if (!saveAsPageContent) {
        SetAnnotWideStringValueFromJString(env, annot, "LufickSimplePdfStampMeta", jJsonStr);
        SetAnnotAsciiStringValue(annot, "LufickStampKind", "simple_pdf_stamp");
        SetAnnotAsciiStringValue(annot, "LufickSimplePdfStamp", "1");
        FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
    }
    env->DeleteLocalRef(jJsonStr);
    return true;
}

// --- HELPER 3: FREEHAND LOGIC ---
static void processFreeHand(JNIEnv* env, jobject obj, FPDF_PAGE page, jfieldID fhField, int r, int g, int b, jclass jsonClass, jmethodID jsonInit, bool saveAsPageContent) {
    jstring jJsonStr = GetBridgeDataPropertyJString(env, obj, fhField, jsonClass, jsonInit, "fhDrawingProperties");
    if (!jJsonStr) return;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json) {
        env->DeleteLocalRef(jJsonStr);
        return;
    }
    jmethodID getS = env->GetMethodID(jsonClass, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID getD = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID getA = env->GetMethodID(jsonClass, "getJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");
    jmethodID optA = env->GetMethodID(jsonClass, "optJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    jstring jModeKey = env->NewStringUTF("mode");
    jstring jPointsKey = env->NewStringUTF("points");
    jstring jStrokesKey = env->NewStringUTF("strokes");
    jstring jSegmentsKey = env->NewStringUTF("segments");
    jstring jStrokeWidthKey = env->NewStringUTF("strokeWidth");
    jstring jAlphaKey = env->NewStringUTF("alpha");
    jstring jLineJoinKey = env->NewStringUTF("lineJoin");
    jstring jLineCapKey = env->NewStringUTF("lineCap");
    jstring jTypeKey = env->NewStringUTF("type");
    jstring jXKey = env->NewStringUTF("x");
    jstring jYKey = env->NewStringUTF("y");
    jstring jCloseKey = env->NewStringUTF("close");
    jstring jSignatureSubtypeKey = env->NewStringUTF("signatureSubType");
    jstring jEmptyValue = env->NewStringUTF("");

    jstring jMode = (jstring)env->CallObjectMethod(json, getS, jModeKey);
    jstring jSignatureSubtype = optS
            ? (jstring)env->CallObjectMethod(json, optS, jSignatureSubtypeKey, jEmptyValue)
            : nullptr;
    const char* modeStr = jMode ? env->GetStringUTFChars(jMode, nullptr) : "BRUSH_PENS";
    bool isSignatureDraw = false;
    if (jSignatureSubtype && env->GetStringLength(jSignatureSubtype) > 0) {
        const char* signatureSubtypeStr = env->GetStringUTFChars(jSignatureSubtype, nullptr);
        if (signatureSubtypeStr) {
            isSignatureDraw = strcmp(signatureSubtypeStr, "Sign_draw") == 0;
            env->ReleaseStringUTFChars(jSignatureSubtype, signatureSubtypeStr);
        }
    }

    jclass arrayClass = env->FindClass("org/json/JSONArray");
    jmethodID lenM = env->GetMethodID(arrayClass, "length", "()I");
    jmethodID getObjM = env->GetMethodID(arrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    const float strokeWidth = (float)env->CallDoubleMethod(json, optD, jStrokeWidthKey, 1.0);
    const int alphaValue = env->CallIntMethod(json, optI, jAlphaKey, 255);
    const int lineJoin = env->CallIntMethod(json, optI, jLineJoinKey, FPDF_LINEJOIN_ROUND);
    const int lineCap = env->CallIntMethod(json, optI, jLineCapKey, FPDF_LINECAP_ROUND);
    const bool isHighlighter = strcmp(modeStr, "HIGHLIGHTER") == 0;
    auto appendInkPoint = [](std::vector<FS_POINTF>& inkPoints, float x, float y) {
        if (inkPoints.empty() ||
            fabs(inkPoints.back().x - x) > 0.01f ||
            fabs(inkPoints.back().y - y) > 0.01f) {
            inkPoints.push_back({x, y});
        }
    };

    auto finalizeInkAnnot = [&](FPDF_PAGEOBJECT path, const std::vector<FS_POINTF>& inkPoints) {
        if (!path || inkPoints.size() < 2) {
            if (path) FPDFPageObj_Destroy(path);
            return;
        }

        float minX = inkPoints[0].x;
        float maxX = inkPoints[0].x;
        float minY = inkPoints[0].y;
        float maxY = inkPoints[0].y;
        for (const auto& point : inkPoints) {
            minX = fmin(minX, point.x);
            maxX = fmax(maxX, point.x);
            minY = fmin(minY, point.y);
            maxY = fmax(maxY, point.y);
        }

        const float effectiveStrokeWidth = strokeWidth > 0.0f ? strokeWidth : 1.0f;

        FPDFPageObj_SetStrokeWidth(path, effectiveStrokeWidth);
        FPDFPageObj_SetLineJoin(path, lineJoin);
        FPDFPageObj_SetLineCap(path, lineCap);
        if (isHighlighter) {
            FPDFPageObj_SetStrokeColor(path, r, g, b, 125);
            FPDFPageObj_SetBlendMode(path, "Multiply");
        } else {
            FPDFPageObj_SetStrokeColor(path, r, g, b, alphaValue);
        }
        FPDFPath_SetDrawMode(path, 0, JNI_TRUE);

        if (saveAsPageContent) {
            FPDFPage_InsertObject(page, path);
            return;
        }

        const float rectMargin = fmax((effectiveStrokeWidth * 0.5f) + 0.5f, 1.0f);
        FS_RECTF rect = {
                minX - rectMargin,
                minY - rectMargin,
                maxX + rectMargin,
                maxY + rectMargin
        };

        FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
        if (!annot) {
            FPDFPageObj_Destroy(path);
            return;
        }

        FPDFAnnot_SetBorder(annot, 0, 0, effectiveStrokeWidth);
        FPDFAnnot_SetRect(annot, &rect);
        const int inkStrokeIndex = FPDFAnnot_AddInkStroke(annot, inkPoints.data(), inkPoints.size());
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, isHighlighter ? 125 : alphaValue);
        bool appendedObject = false;
        if (!isHighlighter) {
            FPDFPageObj_Destroy(path);
        } else {
            appendedObject = FPDFAnnot_AppendObject(annot, path);
            if (!appendedObject) {
                FPDFPageObj_Destroy(path);
            } else {
                const int objectCount = FPDFAnnot_GetObjectCount(annot);
                if (objectCount > 0) {
                    FPDF_PAGEOBJECT annotObject = FPDFAnnot_GetObject(annot, objectCount - 1);
                    if (annotObject) {
                        FPDFAnnot_UpdateObject(annot, annotObject);
                    }
                }
            }
        }

        if (inkStrokeIndex < 0 || (isHighlighter && !appendedObject)) {
            const int annotIndex = FPDFPage_GetAnnotIndex(page, annot);
            FPDFPage_CloseAnnot(annot);
            if (annotIndex >= 0) {
                FPDFPage_RemoveAnnot(page, annotIndex);
            }
            return;
        }

        if (jSignatureSubtype && env->GetStringLength(jSignatureSubtype) > 0) {
            SetAnnotWideStringValueFromJString(env, annot, "LufickSignatureSubtype", jSignatureSubtype);
        }
        FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
        FPDFPage_CloseAnnot(annot);
    };

    jobject strokesArray = optA ? env->CallObjectMethod(json, optA, jStrokesKey) : nullptr;
    if (isSignatureDraw && strokesArray) {
        const int strokeCount = env->CallIntMethod(strokesArray, lenM);
        std::vector<std::vector<FS_POINTF>> inkStrokes;
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f;
        bool hasBounds = false;

        for (int strokeIndex = 0; strokeIndex < strokeCount; strokeIndex++) {
            jobject stroke = env->CallObjectMethod(strokesArray, getObjM, strokeIndex);
            if (!stroke) continue;
            jobject strokePointsArray = optA ? env->CallObjectMethod(stroke, optA, jPointsKey) : nullptr;
            const int pointsCount = strokePointsArray ? env->CallIntMethod(strokePointsArray, lenM) : 0;
            std::vector<FS_POINTF> inkPoints;

            for (int pointIndex = 0; pointIndex < pointsCount; pointIndex++) {
                jobject point = env->CallObjectMethod(strokePointsArray, getObjM, pointIndex);
                if (!point) continue;
                const float x = (float)env->CallDoubleMethod(point, getD, jXKey);
                const float y = (float)env->CallDoubleMethod(point, getD, jYKey);
                appendInkPoint(inkPoints, x, y);
                if (!hasBounds) {
                    minX = maxX = x;
                    minY = maxY = y;
                    hasBounds = true;
                } else {
                    minX = fmin(minX, x);
                    maxX = fmax(maxX, x);
                    minY = fmin(minY, y);
                    maxY = fmax(maxY, y);
                }
                env->DeleteLocalRef(point);
            }

            if (inkPoints.size() >= 2) {
                inkStrokes.push_back(std::move(inkPoints));
            }
            if (strokePointsArray) env->DeleteLocalRef(strokePointsArray);
            env->DeleteLocalRef(stroke);
        }

        if (!inkStrokes.empty() && hasBounds) {
            const float effectiveStrokeWidth = strokeWidth > 0.0f ? strokeWidth : 1.0f;
            if (saveAsPageContent) {
                int insertedPathCount = 0;
                for (const auto& inkPoints : inkStrokes) {
                    if (inkPoints.size() < 2) continue;

                    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(inkPoints[0].x, inkPoints[0].y);
                    if (!path) continue;

                    for (size_t pointIndex = 1; pointIndex < inkPoints.size(); pointIndex++) {
                        FPDFPath_LineTo(path, inkPoints[pointIndex].x, inkPoints[pointIndex].y);
                    }

                    FPDFPageObj_SetStrokeWidth(path, effectiveStrokeWidth);
                    FPDFPageObj_SetLineJoin(path, lineJoin);
                    FPDFPageObj_SetLineCap(path, lineCap);
                    FPDFPageObj_SetStrokeColor(path, r, g, b, alphaValue);
                    FPDFPath_SetDrawMode(path, 0, JNI_TRUE);
                    FPDFPage_InsertObject(page, path);
                    insertedPathCount++;
                }
                LOGE("PDF_EDIT_NATIVE processFreeHand signature draw pageContent insertedPaths=%d", insertedPathCount);
            } else {
                const float rectMargin = fmax((effectiveStrokeWidth * 0.5f) + 0.5f, 1.0f);
                FS_RECTF rect = {
                        minX - rectMargin,
                        minY - rectMargin,
                        maxX + rectMargin,
                        maxY + rectMargin
                };

                FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
                if (annot) {
                    FPDFAnnot_SetBorder(annot, 0, 0, effectiveStrokeWidth);
                    FPDFAnnot_SetRect(annot, &rect);
                    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alphaValue);
                    bool addedAllStrokes = true;
                    for (const auto& inkPoints : inkStrokes) {
                        if (FPDFAnnot_AddInkStroke(annot, inkPoints.data(), inkPoints.size()) < 0) {
                            addedAllStrokes = false;
                            break;
                        }
                    }
                    if (addedAllStrokes) {
                        SetAnnotWideStringValueFromJString(env, annot, "LufickSignatureSubtype", jSignatureSubtype);
                        FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
                        FPDFPage_CloseAnnot(annot);
                    } else {
                        const int annotIndex = FPDFPage_GetAnnotIndex(page, annot);
                        FPDFPage_CloseAnnot(annot);
                        if (annotIndex >= 0) {
                            FPDFPage_RemoveAnnot(page, annotIndex);
                        }
                    }
                }
            }
        }
    } else {
    jobject segmentsArray = optA ? env->CallObjectMethod(json, optA, jSegmentsKey) : nullptr;
    if (segmentsArray) {
        const int segmentCount = env->CallIntMethod(segmentsArray, lenM);
        if (segmentCount > 0) {
            jobject firstSegment = env->CallObjectMethod(segmentsArray, getObjM, 0);
            if (firstSegment) {
                const float startX = (float)env->CallDoubleMethod(firstSegment, getD, jXKey);
                const float startY = (float)env->CallDoubleMethod(firstSegment, getD, jYKey);
                FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(startX, startY);
                std::vector<FS_POINTF> inkPoints;
                inkPoints.push_back({startX, startY});
                float currentX = startX;
                float currentY = startY;

                for (int index = 1; index < segmentCount; index++) {
                    jobject segment = env->CallObjectMethod(segmentsArray, getObjM, index);
                    if (!segment) continue;

                    const int type = env->CallIntMethod(segment, optI, jTypeKey, FPDF_SEGMENT_LINETO);
                    const float x = (float)env->CallDoubleMethod(segment, getD, jXKey);
                    const float y = (float)env->CallDoubleMethod(segment, getD, jYKey);
                    const bool close = env->CallIntMethod(segment, optI, jCloseKey, 0) != 0;

                    if (type == FPDF_SEGMENT_MOVETO) {
                        FPDFPath_MoveTo(path, x, y);
                        if (inkPoints.empty() ||
                            fabs(inkPoints.back().x - x) > 0.01f ||
                            fabs(inkPoints.back().y - y) > 0.01f) {
                            inkPoints.push_back({x, y});
                        }
                        currentX = x;
                        currentY = y;
                    } else if (type == FPDF_SEGMENT_BEZIERTO && index + 2 < segmentCount) {
                        jobject control2 = env->CallObjectMethod(segmentsArray, getObjM, index + 1);
                        jobject endPoint = env->CallObjectMethod(segmentsArray, getObjM, index + 2);
                        const bool hasBezierGroup =
                                control2 && endPoint &&
                                env->CallIntMethod(control2, optI, jTypeKey, FPDF_SEGMENT_UNKNOWN) == FPDF_SEGMENT_BEZIERTO &&
                                env->CallIntMethod(endPoint, optI, jTypeKey, FPDF_SEGMENT_UNKNOWN) == FPDF_SEGMENT_BEZIERTO;

                        if (hasBezierGroup) {
                            const float cp1X = x;
                            const float cp1Y = y;
                            const float cp2X = (float)env->CallDoubleMethod(control2, getD, jXKey);
                            const float cp2Y = (float)env->CallDoubleMethod(control2, getD, jYKey);
                            const float endX = (float)env->CallDoubleMethod(endPoint, getD, jXKey);
                            const float endY = (float)env->CallDoubleMethod(endPoint, getD, jYKey);
                            FPDFPath_BezierTo(path, cp1X, cp1Y, cp2X, cp2Y, endX, endY);
                            for (int sampleIndex = 1; sampleIndex <= 8; sampleIndex++) {
                                const float t = sampleIndex / 8.0f;
                                const float omt = 1.0f - t;
                                const float sampleX =
                                        omt * omt * omt * currentX +
                                        3.0f * omt * omt * t * cp1X +
                                        3.0f * omt * t * t * cp2X +
                                        t * t * t * endX;
                                const float sampleY =
                                        omt * omt * omt * currentY +
                                        3.0f * omt * omt * t * cp1Y +
                                        3.0f * omt * t * t * cp2Y +
                                        t * t * t * endY;
                                if (inkPoints.empty() ||
                                    fabs(inkPoints.back().x - sampleX) > 0.01f ||
                                    fabs(inkPoints.back().y - sampleY) > 0.01f) {
                                    inkPoints.push_back({sampleX, sampleY});
                                }
                            }
                            if (env->CallIntMethod(endPoint, optI, jCloseKey, 0) != 0) {
                                FPDFPath_Close(path);
                            }
                            currentX = endX;
                            currentY = endY;
                            env->DeleteLocalRef(control2);
                            env->DeleteLocalRef(endPoint);
                            env->DeleteLocalRef(segment);
                            index += 2;
                            continue;
                        }

                        if (control2) env->DeleteLocalRef(control2);
                        if (endPoint) env->DeleteLocalRef(endPoint);
                        FPDFPath_LineTo(path, x, y);
                        if (inkPoints.empty() ||
                            fabs(inkPoints.back().x - x) > 0.01f ||
                            fabs(inkPoints.back().y - y) > 0.01f) {
                            inkPoints.push_back({x, y});
                        }
                        currentX = x;
                        currentY = y;
                        if (close) FPDFPath_Close(path);
                    } else {
                        FPDFPath_LineTo(path, x, y);
                        if (inkPoints.empty() ||
                            fabs(inkPoints.back().x - x) > 0.01f ||
                            fabs(inkPoints.back().y - y) > 0.01f) {
                            inkPoints.push_back({x, y});
                        }
                        currentX = x;
                        currentY = y;
                        if (close) FPDFPath_Close(path);
                    }

                    env->DeleteLocalRef(segment);
                }

                finalizeInkAnnot(path, inkPoints);
                env->DeleteLocalRef(firstSegment);
            }
        }
    } else {
        jobject pointsArray = env->CallObjectMethod(json, getA, jPointsKey);
        const int pointsCount = pointsArray ? env->CallIntMethod(pointsArray, lenM) : 0;

        if (pointsCount >= 2) {
            jobject ptStart = env->CallObjectMethod(pointsArray, getObjM, 0);
            if (ptStart) {
                const float startX = (float)env->CallDoubleMethod(ptStart, getD, jXKey);
                const float startY = (float)env->CallDoubleMethod(ptStart, getD, jYKey);
                FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(startX, startY);
                std::vector<FS_POINTF> inkPoints;
                appendInkPoint(inkPoints, startX, startY);
                float segmentStartX = startX;
                float segmentStartY = startY;
                float prevX = startX;
                float prevY = startY;

                for (int i = 1; i < pointsCount; i++) {
                    jobject pt = env->CallObjectMethod(pointsArray, getObjM, i);
                    if (!pt) continue;
                    const float curX = (float)env->CallDoubleMethod(pt, getD, jXKey);
                    const float curY = (float)env->CallDoubleMethod(pt, getD, jYKey);

                    if (i == pointsCount - 1) {
                        FPDFPath_LineTo(path, curX, curY);
                        appendInkPoint(inkPoints, curX, curY);
                    } else {
                        const float midX = (prevX + curX) * 0.5f;
                        const float midY = (prevY + curY) * 0.5f;

                        const float cp1X = segmentStartX + (2.0f / 3.0f) * (prevX - segmentStartX);
                        const float cp1Y = segmentStartY + (2.0f / 3.0f) * (prevY - segmentStartY);
                        const float cp2X = midX + (2.0f / 3.0f) * (prevX - midX);
                        const float cp2Y = midY + (2.0f / 3.0f) * (prevY - midY);

                        FPDFPath_BezierTo(path, cp1X, cp1Y, cp2X, cp2Y, midX, midY);
                        for (int sampleIndex = 1; sampleIndex <= 8; sampleIndex++) {
                            const float t = sampleIndex / 8.0f;
                            const float omt = 1.0f - t;
                            const float sampleX =
                                    omt * omt * omt * segmentStartX +
                                    3.0f * omt * omt * t * cp1X +
                                    3.0f * omt * t * t * cp2X +
                                    t * t * t * midX;
                            const float sampleY =
                                    omt * omt * omt * segmentStartY +
                                    3.0f * omt * omt * t * cp1Y +
                                    3.0f * omt * t * t * cp2Y +
                                    t * t * t * midY;
                            appendInkPoint(inkPoints, sampleX, sampleY);
                        }
                        segmentStartX = midX;
                        segmentStartY = midY;
                    }
                    prevX = curX;
                    prevY = curY;
                    env->DeleteLocalRef(pt);
                }
                finalizeInkAnnot(path, inkPoints);
                env->DeleteLocalRef(ptStart);
            }
        }
        if (pointsArray) env->DeleteLocalRef(pointsArray);
    }
    if (segmentsArray) env->DeleteLocalRef(segmentsArray);
    }

    if (strokesArray) env->DeleteLocalRef(strokesArray);
    if (jMode) {
        env->ReleaseStringUTFChars(jMode, modeStr);
        env->DeleteLocalRef(jMode);
    }
    env->DeleteLocalRef(jModeKey);
    env->DeleteLocalRef(jPointsKey);
    env->DeleteLocalRef(jStrokesKey);
    env->DeleteLocalRef(jSegmentsKey);
    env->DeleteLocalRef(jStrokeWidthKey);
    env->DeleteLocalRef(jAlphaKey);
    env->DeleteLocalRef(jLineJoinKey);
    env->DeleteLocalRef(jLineCapKey);
    env->DeleteLocalRef(jTypeKey);
    env->DeleteLocalRef(jXKey);
    env->DeleteLocalRef(jYKey);
    env->DeleteLocalRef(jCloseKey);
    if (jSignatureSubtype) env->DeleteLocalRef(jSignatureSubtype);
    env->DeleteLocalRef(jSignatureSubtypeKey);
    env->DeleteLocalRef(jEmptyValue);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
}

// --- HELPER 4: REGION HIGHLIGHT ---
static FS_RECTF GetRegionHighlightAppearanceRect(FS_RECTF rect) {
    return {
            fmin(rect.left, rect.right),
            fmax(rect.top, rect.bottom),
            fmax(rect.left, rect.right),
            fmin(rect.top, rect.bottom)
    };
}

static void SetRegionHighlightAppearance(
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        int r,
        int g,
        int b,
        int fillAlpha
) {
    const FS_RECTF appearanceRect = GetRegionHighlightAppearanceRect(rect);
    FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);
    for (int objectIndex = FPDFAnnot_GetObjectCount(annot) - 1; objectIndex >= 0; objectIndex--) {
        FPDFAnnot_RemoveObject(annot, objectIndex);
    }
    AppendPdfShapeAppearanceObject(
            annot,
            12,
            appearanceRect,
            0.0f,
            r,
            g,
            b,
            0,
            r,
            g,
            b,
            fillAlpha,
            0.0f,
            0.0f,
            0.0f,
            0.0f
    );
}

static void processRegionHighlight(
        JNIEnv* env,
        jobject obj,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        int r,
        int g,
        int b,
        int alpha
) {
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
    FPDFAnnot_SetColor(annot,FPDFANNOT_COLORTYPE_InteriorColor,r, g, b, alpha);
    const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
    FPDFAnnot_SetStringValue(annot,"BM",(FPDF_WIDESTRING)blendMode);
    FPDFAnnot_SetBorder(annot, 0, 0, 0);
    SetAnnotAsciiStringValue(annot, "LufickAreaMarkup", "1");
    const std::string areaAlpha = std::to_string(std::max(0, std::min(alpha, 255)));
    SetAnnotAsciiStringValue(annot, "LufickAreaMarkupAlpha", areaAlpha.c_str());
    SetRegionHighlightAppearance(annot, rect, r, g, b, alpha);
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
}

static float GetDefaultTextMarkupStrokeRatio(int typeInt) {
    switch (typeInt) {
        case 1:
        case 8:
            return 0.10f;
        case 2:
            return 0.05f;
        default:
            return 0.10f;
    }
}

static float GetClampedTextMarkupStrokeRatio(int typeInt, double strokeRatioValue) {
    return static_cast<float>(fmax(0.02, fmin(strokeRatioValue, 0.30)));
}

static float GetSquigglyRenderThickness(float rectHeight, float strokeRatio) {
    const float clampedStrokeRatio = GetClampedTextMarkupStrokeRatio(8, strokeRatio);
    const float ratioProgress = static_cast<float>(fmax(
            0.0,
            fmin((clampedStrokeRatio - 0.02f) / (0.30f - 0.02f), 1.0)
    ));
    const float previewRatio = 0.02f + ((0.18f - 0.02f) * sqrtf(ratioProgress));
    return fmax(fmax(rectHeight, 1.0f) * previewRatio, 0.5f);
}

static void AppendStraightTextMarkupAppearance(
        FPDF_ANNOTATION annot,
        float left,
        float right,
        float lineY,
        int r,
        int g,
        int b,
        int alpha,
        float strokeWidth) {
    FPDF_PAGEOBJECT lineObj = FPDFPageObj_CreateNewPath(fmin(left, right), lineY);
    if (!lineObj) return;

    FPDFPath_LineTo(lineObj, fmax(left, right), lineY);
    FPDFPageObj_SetStrokeColor(lineObj, r, g, b, alpha);
    FPDFPageObj_SetFillColor(lineObj, r, g, b, alpha);
    FPDFPageObj_SetStrokeWidth(lineObj, strokeWidth);
    FPDFPath_SetDrawMode(lineObj, 0, JNI_TRUE);
    FPDFAnnot_AppendObject(annot, lineObj);
    FPDFAnnot_UpdateObject(annot, lineObj);
}

static float GetSquigglyAppearanceStrokeWidth(float thickness) {
    return fmax(thickness * 0.55f, 0.5f);
}

static float GetSquigglyAppearanceWaveHeight(float thickness) {
    const float strokeWidth = GetSquigglyAppearanceStrokeWidth(thickness);
    return fmax(thickness, strokeWidth);
}

static float GetSquigglyAppearanceStep(float thickness) {
    return fmax(thickness * 1.80f, 4.0f);
}

static float GetSquigglyPathPeakY(float rectBottom, float rectTop, float thickness) {
    const float strokeWidth = GetSquigglyAppearanceStrokeWidth(thickness);
    const float waveHeight = GetSquigglyAppearanceWaveHeight(thickness);
    const float baseY = rectBottom + (strokeWidth * 0.5f);
    return fmin(rectTop, baseY + waveHeight);
}

static float GetSquigglyAttachmentTop(float rectBottom, float rectTop, float thickness) {
    const float strokeWidth = GetSquigglyAppearanceStrokeWidth(thickness);
    const float peakY = GetSquigglyPathPeakY(rectBottom, rectTop, thickness);
    return fmin(rectTop, peakY + (strokeWidth * 0.5f));
}

static void AppendSquigglyTextMarkupAppearance(
        FPDF_ANNOTATION annot,
        float left,
        float right,
        float rectBottom,
        float rectTop,
        int r,
        int g,
        int b,
        int alpha,
        float thickness) {
    const float minX = fmin(left, right);
    const float maxX = fmax(left, right);
    if (maxX <= minX) return;

    const float strokeWidth = GetSquigglyAppearanceStrokeWidth(thickness);
    const float baseY = rectBottom + (strokeWidth * 0.5f);
    const float peakY = GetSquigglyPathPeakY(rectBottom, rectTop, thickness);
    const float step = GetSquigglyAppearanceStep(thickness);

    FPDF_PAGEOBJECT waveObj = FPDFPageObj_CreateNewPath(minX, baseY);
    if (!waveObj) return;

    if ((maxX - minX) <= step) {
        const float midX = (minX + maxX) * 0.5f;
        FPDFPath_LineTo(waveObj, midX, peakY);
        FPDFPath_LineTo(waveObj, maxX, baseY);
    } else {
        float x = minX;
        while (x < maxX) {
            const float midX = fmin(x + (step * 0.5f), maxX);
            const float nextX = fmin(x + step, maxX);
            FPDFPath_LineTo(waveObj, midX, peakY);
            FPDFPath_LineTo(waveObj, nextX, baseY);
            x = nextX;
        }
    }

    FPDFPageObj_SetStrokeColor(waveObj, r, g, b, alpha);
    FPDFPageObj_SetFillColor(waveObj, r, g, b, alpha);
    FPDFPageObj_SetStrokeWidth(waveObj, strokeWidth);
    FPDFPageObj_SetLineJoin(waveObj, FPDF_LINEJOIN_ROUND);
    FPDFPageObj_SetLineCap(waveObj, FPDF_LINECAP_ROUND);
    FPDFPath_SetDrawMode(waveObj, 0, JNI_TRUE);
    FPDFAnnot_AppendObject(annot, waveObj);
    FPDFAnnot_UpdateObject(annot, waveObj);
}

static void ClearAnnotationAppearanceObjects(FPDF_ANNOTATION annot) {
    if (!annot) return;

    for (int objectIndex = FPDFAnnot_GetObjectCount(annot) - 1; objectIndex >= 0; objectIndex--) {
        FPDFAnnot_RemoveObject(annot, objectIndex);
    }
}

static void AppendFallbackTextMarkupAppearance(
        FPDF_ANNOTATION annot,
        int typeInt,
        FS_RECTF rect,
        int r,
        int g,
        int b,
        int alpha) {
    if (!annot || !(typeInt == 1 || typeInt == 2 || typeInt == 8)) return;

    const float rectTop = fmax(rect.top, rect.bottom);
    const float rectBottom = fmin(rect.top, rect.bottom);
    const float rectHeight = fmax(rectTop - rectBottom, 0.5f);
    const float strokeRatio = GetDefaultTextMarkupStrokeRatio(typeInt);
    const float thickness = (typeInt == 8)
            ? GetSquigglyRenderThickness(rectHeight, strokeRatio)
            : fmax(rectHeight * strokeRatio, 0.5f);

    if (typeInt == 1) {
        const float lineY = rectBottom + (thickness * 0.5f);
        AppendStraightTextMarkupAppearance(annot, rect.left, rect.right, lineY, r, g, b, alpha, thickness);
        return;
    }

    if (typeInt == 2) {
        const float lineY = (rectTop + rectBottom) * 0.5f;
        AppendStraightTextMarkupAppearance(annot, rect.left, rect.right, lineY, r, g, b, alpha, thickness);
        return;
    }

    AppendSquigglyTextMarkupAppearance(
            annot,
            rect.left,
            rect.right,
            rectBottom,
            rectTop,
            r,
            g,
            b,
            alpha,
            thickness
    );
}


static bool ReadAppStickyNoteAppearanceColor(
        FPDF_ANNOTATION annot,
        unsigned int* r,
        unsigned int* g,
        unsigned int* b,
        unsigned int* a
) {
    if (ReadAnnotStringValueUtf16(annot, "LufickCommentMeta").empty()) return false;
    const unsigned long byteLength = FPDFAnnot_GetAP(
            annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
    if (byteLength <= sizeof(char16_t) || byteLength % sizeof(char16_t) != 0) return false;
    std::u16string appearance(byteLength / sizeof(char16_t), u'\0');
    if (FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                        reinterpret_cast<FPDF_WCHAR*>(&appearance[0]), byteLength) != byteLength) {
        return false;
    }
    appearance.resize(appearance.size() - 1);
    std::istringstream stream(Utf16ToSimpleUtf8(appearance));
    std::string token;
    float components[3] = {0.0f, 0.0f, 0.0f};
    int componentCount = 0;
    while (stream >> token) {
        if ((token == "RG" || token == "rg") && componentCount == 3) {
            *r = PdfColorComponentToByte(components[0]);
            *g = PdfColorComponentToByte(components[1]);
            *b = PdfColorComponentToByte(components[2]);
            float opacity = 1.0f;
            if (!FPDFAnnot_GetNumberValue(annot, "CA", &opacity) || !std::isfinite(opacity)) {
                opacity = 1.0f;
            }
            *a = static_cast<unsigned int>(std::lround(
                    std::max(0.0f, std::min(opacity, 1.0f)) * 255.0f));
            return true;
        }
        char* end = nullptr;
        const float component = std::strtof(token.c_str(), &end);
        if (end != token.c_str() && *end == '\0' && std::isfinite(component) &&
            component >= 0.0f && component <= 1.0f && componentCount < 3) {
            components[componentCount++] = component;
        } else {
            componentCount = 0;
        }
    }
    return false;
}

static void ResolveAnnotAppearanceColors(
        FPDF_ANNOTATION annot,
        bool* hasStrokeColor,
        unsigned int* strokeR,
        unsigned int* strokeG,
        unsigned int* strokeB,
        unsigned int* strokeA,
        bool* hasFillColor,
        unsigned int* fillR,
        unsigned int* fillG,
        unsigned int* fillB,
        unsigned int* fillA
) {
    if (!annot) return;

    const int objectCount = FPDFAnnot_GetObjectCount(annot);
    for (int objectIndex = 0; objectIndex < objectCount; objectIndex++) {
        FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
        if (!pageObject) continue;

        const bool isPathObject = FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_PATH;
        bool pathHasFill = true;
        bool pathHasStroke = true;
        if (isPathObject) {
            int fillMode = FPDF_FILLMODE_NONE;
            FPDF_BOOL isStroked = false;
            if (FPDFPath_GetDrawMode(pageObject, &fillMode, &isStroked)) {
                pathHasFill = fillMode != FPDF_FILLMODE_NONE;
                pathHasStroke = isStroked;
            }
        }

        unsigned int objR = 0, objG = 0, objB = 0, objA = 0;
        if (pathHasFill &&
            !*hasFillColor &&
            FPDFPageObj_GetFillColor(pageObject, &objR, &objG, &objB, &objA) &&
            objA > 0) {
            *hasFillColor = true;
            *fillR = objR;
            *fillG = objG;
            *fillB = objB;
            *fillA = objA;
        }

        objR = objG = objB = objA = 0;
        if (pathHasStroke &&
            !*hasStrokeColor &&
            FPDFPageObj_GetStrokeColor(pageObject, &objR, &objG, &objB, &objA) &&
            objA > 0) {
            *hasStrokeColor = true;
            *strokeR = objR;
            *strokeG = objG;
            *strokeB = objB;
            *strokeA = objA;
        }

        if (*hasFillColor && *hasStrokeColor) {
            break;
        }
    }
}

static float ResolveAnnotAppearanceStrokeWidth(
        FPDF_ANNOTATION annot,
        float fallbackWidth,
        bool* sawPathObject = nullptr,
        bool* sawStrokedPath = nullptr
) {
    float resolvedWidth = fallbackWidth > 0.0f ? fallbackWidth : 0.0f;
    if (sawPathObject) *sawPathObject = false;
    if (sawStrokedPath) *sawStrokedPath = false;
    if (!annot) return resolvedWidth;

    const int objectCount = FPDFAnnot_GetObjectCount(annot);
    for (int objectIndex = 0; objectIndex < objectCount; objectIndex++) {
        FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
        if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_PATH) continue;
        if (sawPathObject) *sawPathObject = true;

        int fillMode = 0;
        FPDF_BOOL isStroked = false;
        if (FPDFPath_GetDrawMode(pageObject, &fillMode, &isStroked) && !isStroked) {
            continue;
        }

        unsigned int strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 0;
        if (FPDFPageObj_GetStrokeColor(pageObject, &strokeR, &strokeG, &strokeB, &strokeA) &&
            strokeA == 0) {
            continue;
        }

        float appearanceStrokeWidth = 0.0f;
        if (FPDFPageObj_GetStrokeWidth(pageObject, &appearanceStrokeWidth) &&
            appearanceStrokeWidth > 0.0f) {
            if (sawStrokedPath) *sawStrokedPath = true;
            return appearanceStrokeWidth;
        }
    }

    return resolvedWidth;
}

static float ResolveAnnotVisibleStrokeWidth(FPDF_ANNOTATION annot) {
    if (!annot) return 0.0f;

    float horizontalRadius = 0.0f;
    float verticalRadius = 0.0f;
    float borderWidth = 0.0f;
    if (!FPDFAnnot_GetBorder(annot, &horizontalRadius, &verticalRadius, &borderWidth) ||
        borderWidth <= 0.0f) {
        borderWidth = 0.0f;
    }
    bool sawPathObject = false;
    bool sawStrokedPath = false;
    const float strokeWidth = ResolveAnnotAppearanceStrokeWidth(
            annot,
            borderWidth,
            &sawPathObject,
            &sawStrokedPath
    );
    return sawPathObject && !sawStrokedPath ? 0.0f : strokeWidth;
}

static int GetPdfBoxShapeTypeFromAnnotBounds(FPDF_ANNOTATION annot) {
    FS_RECTF shapeRect = {0, 0, 0, 0};
    if (!annot || !FPDFAnnot_GetRect(annot, &shapeRect)) {
        return 12;
    }

    const float width = fabs(shapeRect.right - shapeRect.left);
    const float height = fabs(shapeRect.top - shapeRect.bottom);
    const float largerSide = fmax(width, height);
    if (largerSide > 0.0f && fabs(width - height) <= fmax(1.0f, largerSide * 0.03f)) {
        return 13;
    }
    return 12;
}

// helper for annotation update // todo testing code not final...
static void ApplyExistingAnnotationColor(
        FPDF_ANNOTATION annot,
        int typeInt,
        int r,
        int g,
        int b,
        int alpha
) {
    if (!annot) return;

    const bool isRegionHighlight = typeInt == 7;
    if (isRegionHighlight) {
        FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
        const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
        FPDFAnnot_SetStringValue(annot, "BM", (FPDF_WIDESTRING)blendMode);
        FPDFAnnot_SetBorder(annot, 0, 0, 0);
        SetAnnotAsciiStringValue(annot, "LufickAreaMarkup", "1");
        const std::string areaAlpha = std::to_string(std::max(0, std::min(alpha, 255)));
        SetAnnotAsciiStringValue(annot, "LufickAreaMarkupAlpha", areaAlpha.c_str());
        FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
        FS_RECTF regionRect;
        if (FPDFAnnot_GetRect(annot, &regionRect)) {
            SetRegionHighlightAppearance(annot, regionRect, r, g, b, alpha);
        }
        return;
    }
    FPDFAnnot_SetColor(
            annot,
            FPDFANNOT_COLORTYPE_Color,
            r,
            g,
            b,
            isRegionHighlight ? 255 : alpha
    );
    if (typeInt == 0 || typeInt == 4 || isRegionHighlight) {
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
    }
    if (typeInt == 4) {
        SetAnnotAsciiStringValue(annot, "LufickPdfRedaction", "1");
    }
    if (typeInt == 0 || isRegionHighlight) {
        const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
        FPDFAnnot_SetStringValue(annot, "BM", (FPDF_WIDESTRING)blendMode);
    }
    const int objectCount = FPDFAnnot_GetObjectCount(annot);
    for (int objectIndex = 0; objectIndex < objectCount; objectIndex++) {
        FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
        if (!pageObject) continue;

        FPDFPageObj_SetStrokeColor(pageObject, r, g, b, alpha);
        FPDFPageObj_SetFillColor(pageObject, r, g, b, alpha);
        FPDFAnnot_UpdateObject(annot, pageObject);
    }
}

static int GetLufickNativePathStableId(FPDF_PAGEOBJECT pathObj) {
    if (!pathObj) return -1;
    const int markCount = FPDFPageObj_CountMarks(pathObj);
    for (int markIndex = 0; markIndex < markCount; ++markIndex) {
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(
                pathObj,
                static_cast<unsigned long>(markIndex)
        );
        if (!mark) continue;

        unsigned long nameLength = 0;
        if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &nameLength) || nameLength < 2) {
            continue;
        }
        std::vector<FPDF_WCHAR> nameBuffer((nameLength / sizeof(FPDF_WCHAR)) + 1, 0);
        if (!FPDFPageObjMark_GetName(
                mark,
                nameBuffer.data(),
                nameLength,
                &nameLength
        )) {
            continue;
        }
        const std::u16string markName(
                reinterpret_cast<const char16_t*>(nameBuffer.data())
        );
        if (markName != u"LufickPath") continue;

        int stablePathId = -1;
        if (FPDFPageObjMark_GetParamIntValue(mark, "MCID", &stablePathId)) {
            return stablePathId;
        }
    }
    return -1;
}

static bool BuildFreehandPropsFromPathObject(
        FPDF_PAGEOBJECT pathObj,
        std::string* outProps,
        unsigned int* outR = nullptr,
        unsigned int* outG = nullptr,
        unsigned int* outB = nullptr,
        unsigned int* outA = nullptr
) {
    if (!pathObj || !outProps || FPDFPageObj_GetType(pathObj) != FPDF_PAGEOBJ_PATH) {
        return false;
    }

    const int segmentCount = FPDFPath_CountSegments(pathObj);
    if (segmentCount <= 0) return false;

    std::ostringstream freehandProps;
    freehandProps << "{\"segments\":[";
    bool wroteSegment = false;
    for (int segmentIndex = 0; segmentIndex < segmentCount; segmentIndex++) {
        FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(pathObj, segmentIndex);
        if (!segment) continue;

        float segmentX = 0.0f;
        float segmentY = 0.0f;
        if (!FPDFPathSegment_GetPoint(segment, &segmentX, &segmentY)) continue;

        const int segmentType = FPDFPathSegment_GetType(segment);
        if (segmentType == FPDF_SEGMENT_UNKNOWN) continue;

        if (wroteSegment) freehandProps << ",";
        freehandProps << "{"
                      << "\"type\":" << segmentType << ","
                      << "\"x\":" << segmentX << ","
                      << "\"y\":" << segmentY;
        if (FPDFPathSegment_GetClose(segment)) {
            freehandProps << ",\"close\":1";
        }
        freehandProps << "}";
        wroteSegment = true;
    }
    if (!wroteSegment) return false;

    float strokeWidth = 1.0f;
    FPDFPageObj_GetStrokeWidth(pathObj, &strokeWidth);
    int fillMode = FPDF_FILLMODE_NONE;
    FPDF_BOOL isStroked = false;
    FPDFPath_GetDrawMode(pathObj, &fillMode, &isStroked);
    bool hasStroke = isStroked == JNI_TRUE;
    bool hasFill = fillMode != FPDF_FILLMODE_NONE;
    if (!hasStroke && !hasFill) return false;

    unsigned int strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 0;
    unsigned int fillR = 0, fillG = 0, fillB = 0, fillA = 0;
    const bool canReadStrokeColor = hasStroke &&
            FPDFPageObj_GetStrokeColor(pathObj, &strokeR, &strokeG, &strokeB, &strokeA);
    const bool canReadFillColor = hasFill &&
            FPDFPageObj_GetFillColor(pathObj, &fillR, &fillG, &fillB, &fillA);
    const bool hasStrokeColor = hasStroke && canReadStrokeColor;
    const bool hasFillColor = hasFill && canReadFillColor;
    if (!hasStrokeColor && !hasFillColor) return false;

    const unsigned int r = hasStrokeColor ? strokeR : fillR;
    const unsigned int g = hasStrokeColor ? strokeG : fillG;
    const unsigned int b = hasStrokeColor ? strokeB : fillB;
    const unsigned int a = hasStrokeColor ? strokeA : fillA;
    const int lineJoin = FPDFPageObj_GetLineJoin(pathObj);
    const int lineCap = FPDFPageObj_GetLineCap(pathObj);
    float dashPhase = 0.0f;
    FPDFPageObj_GetDashPhase(pathObj, &dashPhase);
    const int dashCount = FPDFPageObj_GetDashCount(pathObj);
    std::vector<float> dashArray;
    if (dashCount > 0) {
        dashArray.resize(static_cast<size_t>(dashCount));
        if (!FPDFPageObj_GetDashArray(
                pathObj,
                dashArray.data(),
                static_cast<size_t>(dashCount)
        )) {
            dashArray.clear();
        }
    }
    freehandProps << "],"
                  << "\"strokeWidth\":" << strokeWidth << ","
                  << "\"alpha\":" << a << ","
                  << "\"hasStroke\":" << (hasStrokeColor ? "true" : "false") << ","
                  << "\"strokeR\":" << strokeR << ","
                  << "\"strokeG\":" << strokeG << ","
                  << "\"strokeB\":" << strokeB << ","
                  << "\"strokeA\":" << strokeA << ","
                  << "\"hasFill\":" << (hasFillColor ? "true" : "false") << ","
                  << "\"fillR\":" << fillR << ","
                  << "\"fillG\":" << fillG << ","
                  << "\"fillB\":" << fillB << ","
                  << "\"fillA\":" << fillA << ","
                  << "\"mode\":\"" << (a == 125 ? "HIGHLIGHTER" : "BRUSH_PENS") << "\","
                  << "\"lineJoin\":" << lineJoin << ","
                  << "\"lineCap\":" << lineCap << ","
                  << "\"dashPhase\":" << dashPhase << ","
                  << "\"dashArray\":[";
    for (size_t dashIndex = 0; dashIndex < dashArray.size(); ++dashIndex) {
        if (dashIndex > 0) freehandProps << ",";
        freehandProps << dashArray[dashIndex];
    }
    freehandProps << "]"
                  << "}";

    *outProps = freehandProps.str();
    if (outR) *outR = r;
    if (outG) *outG = g;
    if (outB) *outB = b;
    if (outA) *outA = a;
    return true;
}

static void ResolveInkAnnotStrokeStyle(
        FPDF_ANNOTATION annot,
        float* outStrokeWidth,
        int* outLineJoin,
        int* outLineCap
) {
    if (outStrokeWidth) *outStrokeWidth = 1.0f;
    if (outLineJoin) *outLineJoin = FPDF_LINEJOIN_ROUND;
    if (outLineCap) *outLineCap = FPDF_LINECAP_ROUND;
    if (!annot) return;

    const int annotObjectCount = FPDFAnnot_GetObjectCount(annot);
    for (int objectIndex = 0; objectIndex < annotObjectCount; objectIndex++) {
        FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
        if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_PATH) continue;

        if (outStrokeWidth) {
            float strokeWidth = 1.0f;
            if (FPDFPageObj_GetStrokeWidth(pageObject, &strokeWidth) && strokeWidth > 0.0f) {
                *outStrokeWidth = strokeWidth;
            }
        }
        if (outLineJoin) *outLineJoin = FPDFPageObj_GetLineJoin(pageObject);
        if (outLineCap) *outLineCap = FPDFPageObj_GetLineCap(pageObject);
        return;
    }

    if (outStrokeWidth) {
        float horizontalRadius = 0.0f;
        float verticalRadius = 0.0f;
        float borderWidth = 1.0f;
        if (FPDFAnnot_GetBorder(annot, &horizontalRadius, &verticalRadius, &borderWidth) &&
            borderWidth > 0.0f) {
            *outStrokeWidth = borderWidth;
        }
    }
}

static bool GetInkAnnotPageBounds(
        FPDF_ANNOTATION annot,
        float* outLeft,
        float* outBottom,
        float* outRight,
        float* outTop
) {
    if (!annot || !outLeft || !outBottom || !outRight || !outTop) return false;

    const unsigned long inkPathCount = FPDFAnnot_GetInkListCount(annot);
    if (inkPathCount == 0) return false;

    bool hasPoint = false;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;

    for (unsigned long pathIndex = 0; pathIndex < inkPathCount; pathIndex++) {
        const unsigned long pointCount = FPDFAnnot_GetInkListPath(annot, pathIndex, nullptr, 0);
        if (pointCount == 0) continue;

        std::vector<FS_POINTF> points(pointCount);
        if (FPDFAnnot_GetInkListPath(annot, pathIndex, points.data(), pointCount) != pointCount) {
            continue;
        }

        for (const auto& point : points) {
            if (!hasPoint) {
                minX = maxX = point.x;
                minY = maxY = point.y;
                hasPoint = true;
            } else {
                minX = fmin(minX, point.x);
                maxX = fmax(maxX, point.x);
                minY = fmin(minY, point.y);
                maxY = fmax(maxY, point.y);
            }
        }
    }

    if (!hasPoint) return false;

    float strokeWidth = 1.0f;
    ResolveInkAnnotStrokeStyle(annot, &strokeWidth, nullptr, nullptr);
    const float effectiveStrokeWidth = strokeWidth > 0.0f ? strokeWidth : 1.0f;
    const float padding = fmax((effectiveStrokeWidth * 0.5f) + 1.0f, 1.5f);

    *outLeft = minX - padding;
    *outBottom = minY - padding;
    *outRight = maxX + padding;
    *outTop = maxY + padding;
    return true;
}

static bool BuildFreehandPropsFromInkAnnot(
        FPDF_ANNOTATION annot,
        std::string* outProps,
        unsigned int* outR = nullptr,
        unsigned int* outG = nullptr,
        unsigned int* outB = nullptr,
        unsigned int* outA = nullptr
) {
    if (!annot || !outProps) return false;

    unsigned int r = outR ? *outR : 0;
    unsigned int g = outG ? *outG : 0;
    unsigned int b = outB ? *outB : 0;
    unsigned int a = outA ? *outA : 255;
    bool hasStrokeColor = FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &r, &g, &b, &a);
    unsigned int fillR = 0;
    unsigned int fillG = 0;
    unsigned int fillB = 0;
    unsigned int fillA = 0;
    bool hasFillColor = false;
    ResolveAnnotAppearanceColors(
            annot,
            &hasStrokeColor,
            &r,
            &g,
            &b,
            &a,
            &hasFillColor,
            &fillR,
            &fillG,
            &fillB,
            &fillA
    );
    float strokeWidth = 1.0f;
    int lineJoin = FPDF_LINEJOIN_ROUND;
    int lineCap = FPDF_LINECAP_ROUND;
    ResolveInkAnnotStrokeStyle(annot, &strokeWidth, &lineJoin, &lineCap);

    const unsigned long inkPathCount = FPDFAnnot_GetInkListCount(annot);
    if (inkPathCount > 0) {
        std::vector<std::vector<FS_POINTF>> validPaths;
        for (unsigned long pathIndex = 0; pathIndex < inkPathCount; pathIndex++) {
            const unsigned long pointCount = FPDFAnnot_GetInkListPath(annot, pathIndex, nullptr, 0);
            if (pointCount < 2) continue;
            std::vector<FS_POINTF> points(pointCount);
            if (FPDFAnnot_GetInkListPath(annot, pathIndex, points.data(), pointCount) == pointCount) {
                validPaths.push_back(std::move(points));
            }
        }
        if (!validPaths.empty()) {
            std::ostringstream freehandProps;
            freehandProps << "{";
            if (validPaths.size() == 1) {
                freehandProps << "\"points\":[";
                const auto& points = validPaths[0];
                for (size_t pointIndex = 0; pointIndex < points.size(); pointIndex++) {
                    if (pointIndex > 0) freehandProps << ",";
                    freehandProps << "{"
                                  << "\"x\":" << points[pointIndex].x << ","
                                  << "\"y\":" << points[pointIndex].y
                                  << "}";
                }
                freehandProps << "]";
            } else {
                freehandProps << "\"strokes\":[";
                for (size_t pathIndex = 0; pathIndex < validPaths.size(); pathIndex++) {
                    if (pathIndex > 0) freehandProps << ",";
                    freehandProps << "{\"points\":[";
                    const auto& points = validPaths[pathIndex];
                    for (size_t pointIndex = 0; pointIndex < points.size(); pointIndex++) {
                        if (pointIndex > 0) freehandProps << ",";
                        freehandProps << "{"
                                      << "\"x\":" << points[pointIndex].x << ","
                                      << "\"y\":" << points[pointIndex].y
                                      << "}";
                    }
                    freehandProps << "]}";
                }
                freehandProps << "]";
            }
            freehandProps << ","
                          << "\"strokeWidth\":" << (strokeWidth > 0.0f ? strokeWidth : 1.0f) << ","
                          << "\"alpha\":" << a << ","
                          << "\"mode\":\"" << (a == 125 ? "HIGHLIGHTER" : "BRUSH_PENS") << "\","
                          << "\"lineJoin\":" << lineJoin << ","
                          << "\"lineCap\":" << lineCap
                          << "}";

            *outProps = freehandProps.str();
            if (outR) *outR = r;
            if (outG) *outG = g;
            if (outB) *outB = b;
            if (outA) *outA = a;
            return true;
        }
    }

    const int annotObjectCount = FPDFAnnot_GetObjectCount(annot);
    for (int objectIndex = 0; objectIndex < annotObjectCount; objectIndex++) {
        FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
        if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_PATH) continue;
        if (BuildFreehandPropsFromPathObject(pageObject, outProps, outR, outG, outB, outA)) {
            return true;
        }
    }

    return false;
}

static void ApplyExistingTextMarkupStyle(
        JNIEnv* env,
        FPDF_ANNOTATION annot,
        int typeInt,
        int r,
        int g,
        int b,
        int alpha,
        const std::string& markupRectsJson,
        jclass jsonArrayClass,
        jmethodID jsonArrayInit,
        jmethodID jsonArrayLength,
        jmethodID jsonArrayGetObject,
        jmethodID jsonGetDouble,
        jmethodID jsonOptDouble
) {
    if (!annot) return;

    if (!markupRectsJson.empty()) {
        SetAnnotAsciiStringValue(annot, "LufickMarkupMeta", markupRectsJson.c_str());
    }

    if (!(typeInt == 1 || typeInt == 2 || typeInt == 8) || markupRectsJson.empty()) {
        ApplyExistingAnnotationColor(annot, typeInt, r, g, b, alpha);
        return;
    }

    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);

    jstring jMarkupRects = env->NewStringUTF(markupRectsJson.c_str());
    if (!jMarkupRects) return;

    jobject rectsArray = env->NewObject(jsonArrayClass, jsonArrayInit, jMarkupRects);
    if (!rectsArray) {
        env->DeleteLocalRef(jMarkupRects);
        return;
    }

    jstring jLeftKey = env->NewStringUTF("left");
    jstring jTopKey = env->NewStringUTF("top");
    jstring jRightKey = env->NewStringUTF("right");
    jstring jBottomKey = env->NewStringUTF("bottom");
    jstring jStrokeWidthRatioKey = env->NewStringUTF("strokeWidthRatio");
    if (!jLeftKey || !jTopKey || !jRightKey || !jBottomKey || !jStrokeWidthRatioKey) {
        if (jLeftKey) env->DeleteLocalRef(jLeftKey);
        if (jTopKey) env->DeleteLocalRef(jTopKey);
        if (jRightKey) env->DeleteLocalRef(jRightKey);
        if (jBottomKey) env->DeleteLocalRef(jBottomKey);
        if (jStrokeWidthRatioKey) env->DeleteLocalRef(jStrokeWidthRatioKey);
        env->DeleteLocalRef(rectsArray);
        env->DeleteLocalRef(jMarkupRects);
        return;
    }

    const jchar* rawMarkupMeta = env->GetStringChars(jMarkupRects, nullptr);
    if (rawMarkupMeta) {
        env->ReleaseStringChars(jMarkupRects, rawMarkupMeta);
    }

    ClearAnnotationAppearanceObjects(annot);

    const int rectCount = env->CallIntMethod(rectsArray, jsonArrayLength);
    const size_t existingQuadCount = FPDFAnnot_CountAttachmentPoints(annot);
    bool appended = false;
    bool hasBounds = false;
    FS_RECTF bounds = {0.0f, 0.0f, 0.0f, 0.0f};
    int quadWriteIndex = 0;

    for (int rectIndex = 0; rectIndex < rectCount; rectIndex++) {
        jobject rectObj = env->CallObjectMethod(rectsArray, jsonArrayGetObject, rectIndex);
        if (!rectObj) continue;

        float quadLeft = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, jLeftKey);
        const float quadTop = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, jTopKey);
        float quadRight = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, jRightKey);
        const float quadBottom = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, jBottomKey);
        const float rectTop = fmax(quadTop, quadBottom);
        const float rectBottom = fmin(quadTop, quadBottom);
        const float rectHeight = fmax(rectTop - rectBottom, 0.5f);
        const float strokeRatio = GetClampedTextMarkupStrokeRatio(
                typeInt,
                env->CallDoubleMethod(
                        rectObj,
                        jsonOptDouble,
                        jStrokeWidthRatioKey,
                        GetDefaultTextMarkupStrokeRatio(typeInt)
                )
        );
        const float thickness = (typeInt == 8)
                ? GetSquigglyRenderThickness(rectHeight, strokeRatio)
                : fmax(rectHeight * strokeRatio, 0.5f);

        if (typeInt == 1) {
            const float lineY = rectBottom + (thickness * 0.5f);
            AppendStraightTextMarkupAppearance(
                    annot,
                    quadLeft,
                    quadRight,
                    lineY,
                    r,
                    g,
                    b,
                    alpha,
                    thickness
            );
        } else if (typeInt == 2) {
            const float centerY = (rectTop + rectBottom) * 0.5f;
            AppendStraightTextMarkupAppearance(
                    annot,
                    quadLeft,
                    quadRight,
                    centerY,
                    r,
                    g,
                    b,
                    alpha,
                    thickness
            );
        } else {
            const float appearanceTop = GetSquigglyAttachmentTop(rectBottom, rectTop, thickness);
            AppendSquigglyTextMarkupAppearance(
                    annot,
                    quadLeft,
                    quadRight,
                    rectBottom,
                    appearanceTop,
                    r,
                    g,
                    b,
                    alpha,
                    thickness
            );
        }

        FS_QUADPOINTSF qp = {
                fmin(quadLeft, quadRight), rectTop,
                fmax(quadLeft, quadRight), rectTop,
                fmin(quadLeft, quadRight), rectBottom,
                fmax(quadLeft, quadRight), rectBottom
        };
        if (static_cast<size_t>(quadWriteIndex) < existingQuadCount) {
            FPDFAnnot_SetAttachmentPoints(annot, quadWriteIndex, &qp);
        } else {
            FPDFAnnot_AppendAttachmentPoints(annot, &qp);
        }
        quadWriteIndex++;

        const FS_RECTF pieceBounds = {
                fmin(quadLeft, quadRight),
                rectBottom,
                fmax(quadLeft, quadRight),
                rectTop
        };
        if (!hasBounds) {
            bounds = pieceBounds;
            hasBounds = true;
        } else {
            bounds.left = fmin(bounds.left, pieceBounds.left);
            bounds.right = fmax(bounds.right, pieceBounds.right);
            bounds.bottom = fmin(bounds.bottom, pieceBounds.bottom);
            bounds.top = fmax(bounds.top, pieceBounds.top);
        }

        appended = true;
        env->DeleteLocalRef(rectObj);
    }

    if (hasBounds) {
        FPDFAnnot_SetRect(annot, &bounds);
    }
    if (!appended) {
        FS_RECTF rect = {0.0f, 0.0f, 0.0f, 0.0f};
        if (FPDFAnnot_GetRect(annot, &rect)) {
            AppendFallbackTextMarkupAppearance(annot, typeInt, rect, r, g, b, alpha);
        }
    }

    env->DeleteLocalRef(jStrokeWidthRatioKey);
    env->DeleteLocalRef(jBottomKey);
    env->DeleteLocalRef(jRightKey);
    env->DeleteLocalRef(jTopKey);
    env->DeleteLocalRef(jLeftKey);
    env->DeleteLocalRef(rectsArray);
    env->DeleteLocalRef(jMarkupRects);
}

static bool ApplyNativeAnnotationEditActions(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        jobjectArray highlightsArray,
        FPDF_PAGE providedPage = nullptr,
        int providedPageIndex = -1,
        bool processNewObjects = true
) {
    if (!doc || !highlightsArray) return false;
    LOGE(
            "PDF_EDIT_NATIVE ApplyNativeAnnotationEditActions start count=%d providedPage=%p providedPageIndex=%d processNewObjects=%d",
            env->GetArrayLength(highlightsArray),
            providedPage,
            providedPageIndex,
            processNewObjects ? 1 : 0
    );

    jclass highlightClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfAnnotationNative");
    if (!highlightClass) return false;

    jfieldID typeField = env->GetFieldID(highlightClass, "type", "I");
    jfieldID pageField = env->GetFieldID(highlightClass, "pageIndex", "I");
    jfieldID leftField = env->GetFieldID(highlightClass, "left", "F");
    jfieldID topField = env->GetFieldID(highlightClass, "top", "F");
    jfieldID rightField = env->GetFieldID(highlightClass, "right", "F");
    jfieldID bottomField = env->GetFieldID(highlightClass, "bottom", "F");
    jfieldID rField = env->GetFieldID(highlightClass, "r", "I");
    jfieldID gField = env->GetFieldID(highlightClass, "g", "I");
    jfieldID bField = env->GetFieldID(highlightClass, "b", "I");
    jfieldID alphaField = env->GetFieldID(highlightClass, "alpha", "I");
    jfieldID urlField = env->GetFieldID(highlightClass, "linkUrl", "Ljava/lang/String;");
    jfieldID markupRectsField = env->GetFieldID(highlightClass, "markupRectsJson", "Ljava/lang/String;");
    jfieldID dataPropsField = env->GetFieldID(highlightClass, "dataProperties", "Ljava/lang/String;");
    jfieldID drawingBitmapField = env->GetFieldID(highlightClass, "drawingBitmap", "Landroid/graphics/Bitmap;");
    jfieldID nativeSourceIdField = env->GetFieldID(highlightClass, "nativeSourceId", "I");
    jfieldID nativeEditActionField = env->GetFieldID(highlightClass, "nativeEditAction", "I");

    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jclass jsonArrayClass = env->FindClass("org/json/JSONArray");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayInit = env->GetMethodID(jsonArrayClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayLength = env->GetMethodID(jsonArrayClass, "length", "()I");
    jmethodID jsonArrayGetObject = env->GetMethodID(jsonArrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jmethodID jsonGetDouble = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID jsonOptDouble = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID jsonOptInt = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID jsonOptBoolean = env->GetMethodID(jsonClass, "optBoolean", "(Ljava/lang/String;Z)Z");
    jmethodID jsonOptString = env->GetMethodID(
            jsonClass,
            "optString",
            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    auto toLowerAsciiForNativeEdit = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };

    std::map<int, std::vector<int>> removalMap;
    struct FreehandRemovalTarget {
        int objectIndex;
        int objectType;
        float left;
        float top;
        float right;
        float bottom;
        int r;
        int g;
        int b;
        int a;
    };
    std::map<int, std::vector<FreehandRemovalTarget>> objectRemovalMap;
    struct AnnotColorUpdate {
        int annotIndex;
        int typeInt;
        int r;
        int g;
        int b;
        int alpha;
        std::string markupRectsJson;
    };
    std::map<int, std::vector<AnnotColorUpdate>> colorUpdateMap;
    struct AnnotRectUpdate {
        int annotIndex;
        float left;
        float top;
        float right;
        float bottom;
    };
    std::map<int, std::vector<AnnotRectUpdate>> rectUpdateMap;
    struct FileAttachmentUpdate {
        int annotIndex;
        float left;
        float top;
        float right;
        float bottom;
        int r;
        int g;
        int b;
        int alpha;
        bool applyAppearanceUpdate;
        std::string iconName;
        std::string filePath;
        std::u16string fileName;
        std::u16string mimeType;
    };
    std::map<int, std::vector<FileAttachmentUpdate>> fileAttachmentUpdateMap;
    struct ContentImageUpdate {
        int objectIndex;
        float left;
        float top;
        float right;
        float bottom;
        float rotation;
        float baseWidth;
        float baseHeight;
        bool stretchToBounds;
        int opacityAlpha;
        std::string replacementImagePath;
        bool preferJpegInline;
        FPDF_BITMAP replacementBitmap;
    };
    std::map<int, std::vector<ContentImageUpdate>> contentImageUpdateMap;
    struct ContentPathStyleUpdate {
        int objectIndex;
        bool hasStroke;
        int strokeR;
        int strokeG;
        int strokeB;
        int strokeAlpha;
        bool hasFill;
        int fillR;
        int fillG;
        int fillB;
        int fillAlpha;
    };
    struct ContentPathTransformUpdate {
        int objectIndex;
        int objectType;
        float left;
        float top;
        float right;
        float bottom;
        bool hasSourceBounds;
        float sourceLeft;
        float sourceTop;
        float sourceRight;
        float sourceBottom;
    };
    std::map<int, std::vector<ContentPathStyleUpdate>> contentPathStyleUpdateMap;
    std::map<int, std::vector<ContentPathTransformUpdate>> contentPathTransformUpdateMap;
    bool removeAllAnnotations = false;

    int highlightCount = env->GetArrayLength(highlightsArray);
    for (int i = 0; i < highlightCount; i++) {
        jobject obj = env->GetObjectArrayElement(highlightsArray, i);
        if (!obj) continue;

        int pageIndex = env->GetIntField(obj, pageField);
        int nativeSourceId = env->GetIntField(obj, nativeSourceIdField);
        int nativeEditAction = env->GetIntField(obj, nativeEditActionField);
        if (nativeEditAction == 5) {
            removeAllAnnotations = true;
        } else if (nativeSourceId >= 0 && nativeEditAction == 1) {
            const int objectType = env->GetIntField(obj, typeField);
            if (objectType == 6 || objectType == 19 || objectType == 20) {
                objectRemovalMap[pageIndex].push_back({
                    nativeSourceId,
                    objectType,
                    env->GetFloatField(obj, leftField),
                    env->GetFloatField(obj, topField),
                    env->GetFloatField(obj, rightField),
                    env->GetFloatField(obj, bottomField),
                    env->GetIntField(obj, rField),
                    env->GetIntField(obj, gField),
                    env->GetIntField(obj, bField),
                    env->GetIntField(obj, alphaField)
                });
            } else {
                removalMap[pageIndex].push_back(nativeSourceId);
            }
        } else if (nativeSourceId >= 0 && nativeEditAction == 8) {
            const int objectType = env->GetIntField(obj, typeField);
            objectRemovalMap[pageIndex].push_back({
                nativeSourceId,
                objectType,
                env->GetFloatField(obj, leftField),
                env->GetFloatField(obj, topField),
                env->GetFloatField(obj, rightField),
                env->GetFloatField(obj, bottomField),
                env->GetIntField(obj, rField),
                env->GetIntField(obj, gField),
                env->GetIntField(obj, bField),
                env->GetIntField(obj, alphaField)
            });
        } else if (nativeSourceId >= 0 && nativeEditAction == 2) {
            std::string markupRectsJson;
            jstring jMarkupRects = (jstring)env->GetObjectField(obj, markupRectsField);
            if (jMarkupRects) {
                const char* rawMarkupRects = env->GetStringUTFChars(jMarkupRects, nullptr);
                if (rawMarkupRects) {
                    markupRectsJson = rawMarkupRects;
                    env->ReleaseStringUTFChars(jMarkupRects, rawMarkupRects);
                }
                env->DeleteLocalRef(jMarkupRects);
            }
            colorUpdateMap[pageIndex].push_back({
                                                          nativeSourceId,
                                                          env->GetIntField(obj, typeField),
                                                          env->GetIntField(obj, rField),
                                                          env->GetIntField(obj, gField),
                                                          env->GetIntField(obj, bField),
                                                           env->GetIntField(obj, alphaField),
                                                           markupRectsJson
                                                   });
        } else if (nativeSourceId >= 0 && nativeEditAction == 3) {
            rectUpdateMap[pageIndex].push_back({
                nativeSourceId,
                env->GetFloatField(obj, leftField),
                env->GetFloatField(obj, topField),
                env->GetFloatField(obj, rightField),
                env->GetFloatField(obj, bottomField)
            });
        } else if (nativeSourceId >= 0 && nativeEditAction == 11) {
            std::string iconName = "Paperclip";
            std::string filePath;
            bool applyAppearanceUpdate = false;
            std::u16string fileName;
            std::u16string mimeType;
            jstring attachmentProps = GetBridgeDataPropertyJString(
                    env, obj, dataPropsField, jsonClass, jsonInit, "attachmentProperties");
            if (attachmentProps) {
                jobject attachmentJson = env->NewObject(jsonClass, jsonInit, attachmentProps);
                if (attachmentJson && !env->ExceptionCheck()) {
                    auto readUtf16 = [&](const char* key) -> std::u16string {
                        jstring jsonKey = env->NewStringUTF(key);
                        jstring emptyValue = env->NewStringUTF("");
                        jstring value = static_cast<jstring>(env->CallObjectMethod(
                                attachmentJson, jsonOptString, jsonKey, emptyValue));
                        std::u16string result;
                        if (value) {
                            const jsize length = env->GetStringLength(value);
                            const jchar* chars = env->GetStringChars(value, nullptr);
                            if (chars) {
                                result.assign(
                                        reinterpret_cast<const char16_t*>(chars),
                                        static_cast<size_t>(length));
                                env->ReleaseStringChars(value, chars);
                            }
                            env->DeleteLocalRef(value);
                        }
                        env->DeleteLocalRef(emptyValue);
                        env->DeleteLocalRef(jsonKey);
                        return result;
                    };
                    auto readUtf8 = [&](const char* key) -> std::string {
                        const std::u16string value = readUtf16(key);
                        return Utf16ToSimpleUtf8(value);
                    };
                    const std::string requestedIcon = readUtf8("iconName");
                    if (!requestedIcon.empty()) iconName = requestedIcon;
                    applyAppearanceUpdate = readUtf8("applyAppearanceUpdate") == "1";
                    filePath = readUtf8("replacementFilePath");
                    fileName = readUtf16("fileName");
                    mimeType = readUtf16("mimeType");
                    env->DeleteLocalRef(attachmentJson);
                } else {
                    env->ExceptionClear();
                    if (attachmentJson) env->DeleteLocalRef(attachmentJson);
                }
                env->DeleteLocalRef(attachmentProps);
            }
            fileAttachmentUpdateMap[pageIndex].push_back({
                nativeSourceId,
                env->GetFloatField(obj, leftField),
                env->GetFloatField(obj, topField),
                env->GetFloatField(obj, rightField),
                env->GetFloatField(obj, bottomField),
                env->GetIntField(obj, rField),
                env->GetIntField(obj, gField),
                env->GetIntField(obj, bField),
                env->GetIntField(obj, alphaField),
                applyAppearanceUpdate,
                iconName,
                filePath,
                fileName,
                mimeType
            });
        } else if (nativeSourceId >= 0 && (nativeEditAction == 6 || nativeEditAction == 7)) {
            FPDF_BITMAP replacementBitmap = nullptr;
            if (nativeEditAction == 7) {
                jobject bitmap = env->GetObjectField(obj, drawingBitmapField);
                if (bitmap) {
                    replacementBitmap = ConvertToFPDFBitmap(env, bitmap);
                    env->DeleteLocalRef(bitmap);
                }
            }
            const float objectLeft = env->GetFloatField(obj, leftField);
            const float objectTop = env->GetFloatField(obj, topField);
            const float objectRight = env->GetFloatField(obj, rightField);
            const float objectBottom = env->GetFloatField(obj, bottomField);
            float rotation = 0.0f;
            float baseWidth = fabsf(objectRight - objectLeft);
            float baseHeight = fabsf(objectTop - objectBottom);
            bool stretchToBounds = true;
            int opacityAlpha = -1;
            std::string replacementImagePath;
            bool preferJpegInline = false;
            jstring imageProps = GetBridgeDataPropertyJString(
                    env, obj, dataPropsField, jsonClass, jsonInit, "imageProperties");
            if (imageProps) {
                jobject imageJson = env->NewObject(jsonClass, jsonInit, imageProps);
                if (imageJson) {
                    jstring rotationKey = env->NewStringUTF("rotation");
                    jstring baseWidthKey = env->NewStringUTF("baseWidth");
                    jstring baseHeightKey = env->NewStringUTF("baseHeight");
                    jstring stretchKey = env->NewStringUTF("stretchToBounds");
                    jstring opacityKey = env->NewStringUTF("opacity");
                    jstring replacementPathKey = env->NewStringUTF("flattenedAssetPath");
                    jstring assetFormatKey = env->NewStringUTF("assetFormat");
                    rotation = static_cast<float>(env->CallDoubleMethod(
                            imageJson, jsonOptDouble, rotationKey, 0.0));
                    baseWidth = static_cast<float>(env->CallDoubleMethod(
                            imageJson, jsonOptDouble, baseWidthKey, static_cast<double>(baseWidth)));
                    baseHeight = static_cast<float>(env->CallDoubleMethod(
                            imageJson, jsonOptDouble, baseHeightKey, static_cast<double>(baseHeight)));
                    stretchToBounds = env->CallBooleanMethod(
                            imageJson, jsonOptBoolean, stretchKey, JNI_TRUE) == JNI_TRUE;
                    const double rawOpacity = env->CallDoubleMethod(
                            imageJson, jsonOptDouble, opacityKey, -1.0);
                    if (rawOpacity >= 0.0) {
                        opacityAlpha = ResolvePdfImageOpacityAlpha(rawOpacity);
                    }
                    jstring replacementPathValue = static_cast<jstring>(env->CallObjectMethod(
                            imageJson, jsonOptString, replacementPathKey, nullptr));
                    jstring assetFormatValue = static_cast<jstring>(env->CallObjectMethod(
                            imageJson, jsonOptString, assetFormatKey, nullptr));
                    if (replacementPathValue) {
                        const char* value = env->GetStringUTFChars(replacementPathValue, nullptr);
                        if (value) {
                            replacementImagePath = value;
                            env->ReleaseStringUTFChars(replacementPathValue, value);
                        }
                    }
                    std::string assetFormat;
                    if (assetFormatValue) {
                        const char* value = env->GetStringUTFChars(assetFormatValue, nullptr);
                        if (value) {
                            assetFormat = toLowerAsciiForNativeEdit(value);
                            env->ReleaseStringUTFChars(assetFormatValue, value);
                        }
                    }
                    const std::string normalizedPath =
                            toLowerAsciiForNativeEdit(replacementImagePath);
                    preferJpegInline =
                            assetFormat == "jpg" ||
                            assetFormat == "jpeg" ||
                            (normalizedPath.size() >= 4 &&
                             normalizedPath.compare(normalizedPath.size() - 4, 4, ".jpg") == 0) ||
                            (normalizedPath.size() >= 5 &&
                             normalizedPath.compare(normalizedPath.size() - 5, 5, ".jpeg") == 0);
                    if (replacementPathValue) env->DeleteLocalRef(replacementPathValue);
                    if (assetFormatValue) env->DeleteLocalRef(assetFormatValue);
                    env->DeleteLocalRef(assetFormatKey);
                    env->DeleteLocalRef(replacementPathKey);
                    env->DeleteLocalRef(opacityKey);
                    env->DeleteLocalRef(stretchKey);
                    env->DeleteLocalRef(baseHeightKey);
                    env->DeleteLocalRef(baseWidthKey);
                    env->DeleteLocalRef(rotationKey);
                    env->DeleteLocalRef(imageJson);
                }
                env->DeleteLocalRef(imageProps);
            }
            contentImageUpdateMap[pageIndex].push_back({
                nativeSourceId,
                objectLeft,
                objectTop,
                objectRight,
                objectBottom,
                rotation,
                baseWidth,
                baseHeight,
                stretchToBounds,
                opacityAlpha,
                replacementImagePath,
                preferJpegInline,
                replacementBitmap
            });
        } else if (nativeSourceId >= 0 && nativeEditAction == 9) {
            const int defaultR = env->GetIntField(obj, rField);
            const int defaultG = env->GetIntField(obj, gField);
            const int defaultB = env->GetIntField(obj, bField);
            const int defaultAlpha = env->GetIntField(obj, alphaField);
            bool hasStroke = true;
            int strokeR = defaultR;
            int strokeG = defaultG;
            int strokeB = defaultB;
            int strokeAlpha = defaultAlpha;
            bool hasFill = false;
            int fillR = defaultR;
            int fillG = defaultG;
            int fillB = defaultB;
            int fillAlpha = defaultAlpha;

            jstring pathProperties = static_cast<jstring>(
                    env->GetObjectField(obj, dataPropsField)
            );
            if (pathProperties) {
                jobject pathJson = env->NewObject(jsonClass, jsonInit, pathProperties);
                if (pathJson) {
                    auto optPathInt = [&](const char* key, int fallback) -> int {
                        jstring jsonKey = env->NewStringUTF(key);
                        const int value = env->CallIntMethod(
                                pathJson, jsonOptInt, jsonKey, fallback);
                        env->DeleteLocalRef(jsonKey);
                        return value;
                    };
                    auto optPathBoolean = [&](const char* key, bool fallback) -> bool {
                        jstring jsonKey = env->NewStringUTF(key);
                        const bool value = env->CallBooleanMethod(
                                pathJson, jsonOptBoolean, jsonKey, fallback ? JNI_TRUE : JNI_FALSE);
                        env->DeleteLocalRef(jsonKey);
                        return value;
                    };
                    hasStroke = optPathBoolean("hasStroke", true);
                    strokeR = optPathInt("strokeR", defaultR);
                    strokeG = optPathInt("strokeG", defaultG);
                    strokeB = optPathInt("strokeB", defaultB);
                    strokeAlpha = optPathInt("strokeA", defaultAlpha);
                    hasFill = optPathBoolean("hasFill", false);
                    fillR = optPathInt("fillR", defaultR);
                    fillG = optPathInt("fillG", defaultG);
                    fillB = optPathInt("fillB", defaultB);
                    fillAlpha = optPathInt("fillA", defaultAlpha);
                    env->DeleteLocalRef(pathJson);
                }
                env->DeleteLocalRef(pathProperties);
            }
            contentPathStyleUpdateMap[pageIndex].push_back({
                nativeSourceId,
                hasStroke,
                strokeR,
                strokeG,
                strokeB,
                strokeAlpha,
                hasFill,
                fillR,
                fillG,
                fillB,
                fillAlpha
            });
        } else if (nativeSourceId >= 0 && nativeEditAction == 10) {
            const int objectType = env->GetIntField(obj, typeField);
            bool hasSourceBounds = false;
            float sourceLeft = 0.0f;
            float sourceTop = 0.0f;
            float sourceRight = 0.0f;
            float sourceBottom = 0.0f;
            if (objectType == 20) {
                jstring formProperties = static_cast<jstring>(
                        env->GetObjectField(obj, dataPropsField)
                );
                if (formProperties) {
                    jobject formJson = env->NewObject(jsonClass, jsonInit, formProperties);
                    if (formJson) {
                        jstring sourceLeftKey = env->NewStringUTF("sourceLeft");
                        jstring sourceTopKey = env->NewStringUTF("sourceTop");
                        jstring sourceRightKey = env->NewStringUTF("sourceRight");
                        jstring sourceBottomKey = env->NewStringUTF("sourceBottom");
                        sourceLeft = static_cast<float>(env->CallDoubleMethod(
                                formJson,
                                jsonOptDouble,
                                sourceLeftKey,
                                static_cast<jdouble>(NAN)
                        ));
                        sourceTop = static_cast<float>(env->CallDoubleMethod(
                                formJson,
                                jsonOptDouble,
                                sourceTopKey,
                                static_cast<jdouble>(NAN)
                        ));
                        sourceRight = static_cast<float>(env->CallDoubleMethod(
                                formJson,
                                jsonOptDouble,
                                sourceRightKey,
                                static_cast<jdouble>(NAN)
                        ));
                        sourceBottom = static_cast<float>(env->CallDoubleMethod(
                                formJson,
                                jsonOptDouble,
                                sourceBottomKey,
                                static_cast<jdouble>(NAN)
                        ));
                        hasSourceBounds =
                                std::isfinite(sourceLeft) &&
                                std::isfinite(sourceTop) &&
                                std::isfinite(sourceRight) &&
                                std::isfinite(sourceBottom) &&
                                fabsf(sourceRight - sourceLeft) >= 0.0001f &&
                                fabsf(sourceTop - sourceBottom) >= 0.0001f;
                        env->DeleteLocalRef(sourceBottomKey);
                        env->DeleteLocalRef(sourceRightKey);
                        env->DeleteLocalRef(sourceTopKey);
                        env->DeleteLocalRef(sourceLeftKey);
                        env->DeleteLocalRef(formJson);
                    }
                    env->DeleteLocalRef(formProperties);
                }
            }
            contentPathTransformUpdateMap[pageIndex].push_back({
                nativeSourceId,
                objectType,
                env->GetFloatField(obj, leftField),
                env->GetFloatField(obj, topField),
                env->GetFloatField(obj, rightField),
                env->GetFloatField(obj, bottomField),
                hasSourceBounds,
                sourceLeft,
                sourceTop,
                sourceRight,
                sourceBottom
            });
        }
        env->DeleteLocalRef(obj);
    }

    if (removeAllAnnotations) {
        LOGE("PDF_EDIT_NATIVE ApplyNativeAnnotationEditActions removeAllAnnotations requested");
        const int pageCount = FPDF_GetPageCount(doc);
        for (int pageIndex = 0; pageIndex < pageCount; pageIndex++) {
            bool shouldClosePage = false;
            FPDF_PAGE page = nullptr;
            if (providedPage && providedPageIndex == pageIndex) {
                page = providedPage;
            } else {
                page = FPDF_LoadPage(doc, pageIndex);
                shouldClosePage = true;
            }
            if (!page) continue;

            for (int annotIndex = FPDFPage_GetAnnotCount(page) - 1; annotIndex >= 0; annotIndex--) {
                FPDFPage_RemoveAnnot(page, annotIndex);
            }

            if (shouldClosePage) {
                FPDF_ClosePage(page);
            }
        }
        return true;
    }

    std::map<int, bool> touchedPages;
    for (const auto& entry : removalMap) touchedPages[entry.first] = true;
    for (const auto& entry : objectRemovalMap) touchedPages[entry.first] = true;
    for (const auto& entry : colorUpdateMap) touchedPages[entry.first] = true;
    for (const auto& entry : rectUpdateMap) touchedPages[entry.first] = true;
    for (const auto& entry : fileAttachmentUpdateMap) touchedPages[entry.first] = true;
    for (const auto& entry : contentImageUpdateMap) touchedPages[entry.first] = true;
    for (const auto& entry : contentPathStyleUpdateMap) touchedPages[entry.first] = true;
    for (const auto& entry : contentPathTransformUpdateMap) touchedPages[entry.first] = true;

    bool allPagesSucceeded = true;
    for (const auto& pageEntry : touchedPages) {
        int pageIndex = pageEntry.first;
        bool shouldClosePage = false;
        FPDF_PAGE page = (providedPage && pageIndex == providedPageIndex)
                         ? providedPage
                         : FPDF_LoadPage(doc, pageIndex);
        if (!page) {
            for (const auto& update : contentImageUpdateMap[pageIndex]) {
                if (update.replacementBitmap) FPDFBitmap_Destroy(update.replacementBitmap);
            }
            continue;
        }
        if (!(providedPage && pageIndex == providedPageIndex)) {
            shouldClosePage = true;
        }
        LOGE(
                "PDF_EDIT_NATIVE ApplyNativeAnnotationEditActions page=%d before annotCount=%d objectCount=%d removals=%zu objectRemovals=%zu colorUpdates=%zu rectUpdates=%zu processNewObjects=%d",
                pageIndex,
                FPDFPage_GetAnnotCount(page),
                FPDFPage_CountObjects(page),
                removalMap[pageIndex].size(),
                objectRemovalMap[pageIndex].size(),
                colorUpdateMap[pageIndex].size(),
                rectUpdateMap[pageIndex].size(),
                processNewObjects ? 1 : 0
        );

        if (!ConvertNativeShadingsToEditableProxies(doc, page)) {
            LOGE(
                    "PDF_EDIT_NATIVE failed to preserve shading objects on page=%d",
                    pageIndex
            );
            for (const auto& update : contentImageUpdateMap[pageIndex]) {
                if (update.replacementBitmap) FPDFBitmap_Destroy(update.replacementBitmap);
            }
            if (shouldClosePage) FPDF_ClosePage(page);
            allPagesSucceeded = false;
            continue;
        }

        auto updatesIt = colorUpdateMap.find(pageIndex);
        if (updatesIt != colorUpdateMap.end()) {
            for (const auto& update : updatesIt->second) {
                if (update.annotIndex < 0 || update.annotIndex >= FPDFPage_GetAnnotCount(page)) continue;
                FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, update.annotIndex);
                if (!annot) continue;
                ApplyExistingTextMarkupStyle(
                        env,
                        annot,
                        update.typeInt,
                        update.r,
                        update.g,
                        update.b,
                        update.alpha,
                        update.markupRectsJson,
                        jsonArrayClass,
                        jsonArrayInit,
                        jsonArrayLength,
                        jsonArrayGetObject,
                        jsonGetDouble,
                        jsonOptDouble
                );
                FPDFPage_CloseAnnot(annot);
            }
        }

        auto rectUpdatesIt = rectUpdateMap.find(pageIndex);
        if (rectUpdatesIt != rectUpdateMap.end()) {
            for (const auto& update : rectUpdatesIt->second) {
                if (update.annotIndex < 0 || update.annotIndex >= FPDFPage_GetAnnotCount(page)) continue;
                FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, update.annotIndex);
                if (!annot) continue;
                FS_RECTF rect;
                rect.left = fmin(update.left, update.right);
                rect.right = fmax(update.left, update.right);
                rect.bottom = fmin(update.top, update.bottom);
                rect.top = fmax(update.top, update.bottom);
                FPDFAnnot_SetRect(annot, &rect);
                FPDFPage_CloseAnnot(annot);
            }
        }

        auto attachmentUpdatesIt = fileAttachmentUpdateMap.find(pageIndex);
        if (attachmentUpdatesIt != fileAttachmentUpdateMap.end()) {
            for (const auto& update : attachmentUpdatesIt->second) {
                if (update.annotIndex < 0 || update.annotIndex >= FPDFPage_GetAnnotCount(page)) continue;
                FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, update.annotIndex);
                if (!annot || FPDFAnnot_GetSubtype(annot) != FPDF_ANNOT_FILEATTACHMENT) {
                    if (annot) FPDFPage_CloseAnnot(annot);
                    continue;
                }
                FS_RECTF rect = {
                    fmin(update.left, update.right),
                    fmax(update.top, update.bottom),
                    fmax(update.left, update.right),
                    fmin(update.top, update.bottom)
                };
                FPDFAnnot_SetRect(annot, &rect);
                if (update.applyAppearanceUpdate) {
                    FPDFAnnot_SetColor(
                            annot, FPDFANNOT_COLORTYPE_Color,
                            update.r, update.g, update.b, update.alpha);
                    FPDFAnnot_SetColor(
                            annot, FPDFANNOT_COLORTYPE_InteriorColor,
                            update.r, update.g, update.b, update.alpha);
                    SetAnnotAsciiStringValue(annot, "Name", update.iconName.c_str());
                }

                FPDF_ATTACHMENT attachment = FPDFAnnot_GetFileAttachment(annot);
                if (attachment && !update.filePath.empty()) {
                    if (!update.fileName.empty()) {
                        FPDF_ATTACHMENT replacement = FPDFAnnot_AddFileAttachment(
                                annot,
                                reinterpret_cast<FPDF_WIDESTRING>(update.fileName.c_str()));
                        if (replacement) attachment = replacement;
                    }
                    std::ifstream input(update.filePath, std::ios::binary);
                    std::vector<unsigned char> bytes(
                            (std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
                    if (input.good() || input.eof()) {
                        FPDFAttachment_SetFile(
                                attachment,
                                doc,
                                bytes.empty() ? nullptr : bytes.data(),
                                static_cast<unsigned long>(bytes.size()));
                    }
                }
                if (attachment && !update.fileName.empty()) {
                    const auto wideName = reinterpret_cast<FPDF_WIDESTRING>(update.fileName.c_str());
                    FPDFAnnot_SetStringValue(annot, "Contents", wideName);
                }
                if (attachment && !update.mimeType.empty()) {
                    const auto wideMime = reinterpret_cast<FPDF_WIDESTRING>(update.mimeType.c_str());
                    FPDFAttachment_SetStringValue(attachment, "Subtype", wideMime);
                    FPDFAnnot_SetStringValue(annot, "LufickAttachmentMime", wideMime);
                }
                if (update.applyAppearanceUpdate) {
                    const std::u16string appearance = BuildFileAttachmentAppearanceStream(
                            rect,
                            update.iconName,
                            update.r,
                            update.g,
                            update.b);
                    if (!appearance.empty()) {
                        FPDFAnnot_SetAP(
                                annot,
                                FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                reinterpret_cast<FPDF_WIDESTRING>(appearance.c_str()));
                    }
                }
                FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
                FPDFPage_CloseAnnot(annot);
            }
        }

        auto contentImageUpdatesIt = contentImageUpdateMap.find(pageIndex);
        if (contentImageUpdatesIt != contentImageUpdateMap.end()) {
            for (const auto& update : contentImageUpdatesIt->second) {
                if (update.objectIndex >= 0 && update.objectIndex < FPDFPage_CountObjects(page)) {
                    FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, update.objectIndex);
                    if (pageObject && FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_IMAGE) {
                        FS_MATRIX originalMatrix{};
                        const bool hasOriginalMatrix =
                                FPDFPageObj_GetMatrix(pageObject, &originalMatrix) == JNI_TRUE;
                        bool replacedFromAsset = false;
                        if (!update.replacementImagePath.empty()) {
                            replacedFromAsset = update.preferJpegInline
                                    ? LoadJpegFileIntoImageObject(
                                            update.replacementImagePath.c_str(),
                                            pageObject)
                                    : LoadBitmapFileIntoImageObject(
                                            env,
                                            update.replacementImagePath.c_str(),
                                            pageObject);
                        }
                        if (!replacedFromAsset && update.replacementBitmap) {
                            FPDFImageObj_SetBitmap(&page, 1, pageObject, update.replacementBitmap);
                        }
                        const float left = fmin(update.left, update.right);
                        const float right = fmax(update.left, update.right);
                        const float bottom = fmin(update.top, update.bottom);
                        const float top = fmax(update.top, update.bottom);
                        const double angleRadians = update.rotation * M_PI / 180.0;
                        const double cosAngle = cos(angleRadians);
                        const double sinAngle = sin(angleRadians);
                        const double safeBaseWidth = std::max(static_cast<double>(update.baseWidth), 1.0);
                        const double safeBaseHeight = std::max(static_cast<double>(update.baseHeight), 1.0);
                        const double expandedWidth = safeBaseWidth * fabs(cosAngle) +
                                                     safeBaseHeight * fabs(sinAngle);
                        const double expandedHeight = safeBaseWidth * fabs(sinAngle) +
                                                      safeBaseHeight * fabs(cosAngle);
                        const double fitScale = std::min(
                                static_cast<double>(right - left) / std::max(expandedWidth, 1.0),
                                static_cast<double>(top - bottom) / std::max(expandedHeight, 1.0));
                        const double drawWidth = std::max(
                                update.stretchToBounds ? safeBaseWidth : safeBaseWidth * fitScale,
                                1.0);
                        const double drawHeight = std::max(
                                update.stretchToBounds ? safeBaseHeight : safeBaseHeight * fitScale,
                                1.0);
                        const double a = cosAngle * drawWidth;
                        const double b = sinAngle * drawWidth;
                        const double c = -sinAngle * drawHeight;
                        const double d = cosAngle * drawHeight;
                        const double centerX = (left + right) * 0.5;
                        const double centerY = (bottom + top) * 0.5;
                        const double e = centerX - (a + c) * 0.5;
                        const double f = centerY - (b + d) * 0.5;
                        FPDFImageObj_SetMatrix(
                                pageObject,
                                a,
                                b,
                                c,
                                d,
                                e,
                                f
                        );
                        if (update.opacityAlpha >= 0) {
                            unsigned int r = 255, g = 255, b = 255, a = 255;
                            FPDFPageObj_GetFillColor(pageObject, &r, &g, &b, &a);
                            FPDFPageObj_SetFillColor(
                                    pageObject,
                                    r,
                                    g,
                                    b,
                                    static_cast<unsigned int>(update.opacityAlpha)
                            );
                        }
                        const double determinant =
                                (originalMatrix.a * originalMatrix.d) -
                                (originalMatrix.b * originalMatrix.c);
                        if (hasOriginalMatrix && fabs(determinant) > 0.000001) {
                            const double inverseA = originalMatrix.d / determinant;
                            const double inverseB = -originalMatrix.b / determinant;
                            const double inverseC = -originalMatrix.c / determinant;
                            const double inverseD = originalMatrix.a / determinant;
                            const double inverseE =
                                    ((originalMatrix.c * originalMatrix.f) -
                                     (originalMatrix.d * originalMatrix.e)) / determinant;
                            const double inverseF =
                                    ((originalMatrix.b * originalMatrix.e) -
                                     (originalMatrix.a * originalMatrix.f)) / determinant;
                            FPDFPageObj_TransformClipPath(
                                    pageObject,
                                    (a * inverseA) + (c * inverseB),
                                    (b * inverseA) + (d * inverseB),
                                    (a * inverseC) + (c * inverseD),
                                    (b * inverseC) + (d * inverseD),
                                    (a * inverseE) + (c * inverseF) + e,
                                    (b * inverseE) + (d * inverseF) + f
                            );
                        }
                    }
                }
                if (update.replacementBitmap) FPDFBitmap_Destroy(update.replacementBitmap);
            }
        }

        auto contentPathStyleIt = contentPathStyleUpdateMap.find(pageIndex);
        if (contentPathStyleIt != contentPathStyleUpdateMap.end()) {
            for (const auto& update : contentPathStyleIt->second) {
                if (update.objectIndex < 0 || update.objectIndex >= FPDFPage_CountObjects(page)) continue;
                FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, update.objectIndex);
                if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_PATH) continue;

                int fillMode = FPDF_FILLMODE_NONE;
                FPDF_BOOL isStroked = false;
                if (!FPDFPath_GetDrawMode(pageObject, &fillMode, &isStroked)) continue;
                if (isStroked && update.hasStroke) {
                    FPDFPageObj_SetStrokeColor(
                            pageObject,
                            update.strokeR,
                            update.strokeG,
                            update.strokeB,
                            update.strokeAlpha
                    );
                }
                if (fillMode != FPDF_FILLMODE_NONE && update.hasFill) {
                    FPDFPageObj_SetFillColor(
                            pageObject,
                            update.fillR,
                            update.fillG,
                            update.fillB,
                            update.fillAlpha
                    );
                }
            }
        }

        auto contentPathTransformIt = contentPathTransformUpdateMap.find(pageIndex);
        if (contentPathTransformIt != contentPathTransformUpdateMap.end()) {
            for (const auto& update : contentPathTransformIt->second) {
                if (update.objectIndex < 0 || update.objectIndex >= FPDFPage_CountObjects(page)) continue;
                FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, update.objectIndex);
                const int expectedObjectType = update.objectType == 19
                                               ? FPDF_PAGEOBJ_SHADING
                                               : (update.objectType == 20
                                                  ? FPDF_PAGEOBJ_FORM
                                                  : FPDF_PAGEOBJ_PATH);
                const bool objectTypeMatches = update.objectType == 19
                                               ? IsEditableShadingObject(pageObject)
                                               : pageObject &&
                                                 FPDFPageObj_GetType(pageObject) == expectedObjectType;
                if (!objectTypeMatches) continue;

                float sourceLeft = 0.0f, sourceBottom = 0.0f, sourceRight = 0.0f, sourceTop = 0.0f;
                if (update.objectType == 20 && update.hasSourceBounds) {
                    sourceLeft = fmin(update.sourceLeft, update.sourceRight);
                    sourceRight = fmax(update.sourceLeft, update.sourceRight);
                    sourceBottom = fmin(update.sourceTop, update.sourceBottom);
                    sourceTop = fmax(update.sourceTop, update.sourceBottom);
                } else if (!FPDFPageObj_GetBounds(
                        pageObject,
                        &sourceLeft,
                        &sourceBottom,
                        &sourceRight,
                        &sourceTop
                )) {
                    continue;
                }
                const float sourceWidth = sourceRight - sourceLeft;
                const float sourceHeight = sourceTop - sourceBottom;
                if (fabs(sourceWidth) < 0.0001f || fabs(sourceHeight) < 0.0001f) continue;

                const float targetLeft = fmin(update.left, update.right);
                const float targetRight = fmax(update.left, update.right);
                const float targetBottom = fmin(update.top, update.bottom);
                const float targetTop = fmax(update.top, update.bottom);
                const float scaleX = (targetRight - targetLeft) / sourceWidth;
                const float scaleY = (targetTop - targetBottom) / sourceHeight;
                FPDFPageObj_Transform(
                        pageObject,
                        scaleX,
                        0.0f,
                        0.0f,
                        scaleY,
                        targetLeft - (sourceLeft * scaleX),
                        targetBottom - (sourceBottom * scaleY)
                );
                if (expectedObjectType == FPDF_PAGEOBJ_PATH) {
                    FPDFPageObj_TransformClipPath(
                            pageObject,
                            scaleX,
                            0.0f,
                            0.0f,
                            scaleY,
                            targetLeft - (sourceLeft * scaleX),
                            targetBottom - (sourceBottom * scaleY)
                    );
                }
            }
        }

        std::vector<FreehandRemovalTarget> residualObjectRemovalTargets;
        std::vector<int> annotBackedFreehandRemovalIds;
        auto objectRemovalsIt = objectRemovalMap.find(pageIndex);
        if (objectRemovalsIt != objectRemovalMap.end()) {
            auto targets = objectRemovalsIt->second;
            std::sort(targets.begin(), targets.end(), [](const FreehandRemovalTarget& first, const FreehandRemovalTarget& second) {
                return first.objectIndex > second.objectIndex;
            });
            targets.erase(std::unique(targets.begin(), targets.end(), [](const FreehandRemovalTarget& first, const FreehandRemovalTarget& second) {
                return first.objectIndex == second.objectIndex;
            }), targets.end());

            for (const auto& target : targets) {
                bool isAnnotBackedInk = false;
                if (target.objectType == 6 &&
                    target.objectIndex >= 0 && target.objectIndex < FPDFPage_GetAnnotCount(page)) {
                    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, target.objectIndex);
                    if (annot) {
                        isAnnotBackedInk = FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_INK;
                        FPDFPage_CloseAnnot(annot);
                    }
                }

                if (isAnnotBackedInk) {
                    annotBackedFreehandRemovalIds.push_back(target.objectIndex);
                } else {
                    residualObjectRemovalTargets.push_back(target);
                }
            }
        }

        auto removalsIt = removalMap.find(pageIndex);
        if (removalsIt != removalMap.end() || !annotBackedFreehandRemovalIds.empty()) {
            std::vector<int> ids = removalsIt != removalMap.end()
                    ? removalsIt->second
                    : std::vector<int>();
            ids.insert(ids.end(), annotBackedFreehandRemovalIds.begin(), annotBackedFreehandRemovalIds.end());
            std::sort(ids.begin(), ids.end(), std::greater<int>());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            for (int annotIndex : ids) {
                if (annotIndex >= 0 && annotIndex < FPDFPage_GetAnnotCount(page)) {
                    FPDFPage_RemoveAnnot(page, annotIndex);
                }
            }
        }

        if (!residualObjectRemovalTargets.empty()) {
            for (const auto& target : residualObjectRemovalTargets) {
                bool removed = false;
                const float targetLeft = fmin(target.left, target.right);
                const float targetRight = fmax(target.left, target.right);
                const float targetBottom = fmin(target.top, target.bottom);
                const float targetTop = fmax(target.top, target.bottom);

                if (target.objectType == 6 &&
                    target.objectIndex >= 0 && target.objectIndex < FPDFPage_GetAnnotCount(page)) {
                    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, target.objectIndex);
                    if (annot) {
                        const int subtype = FPDFAnnot_GetSubtype(annot);
                        if (subtype == FPDF_ANNOT_INK) {
                            FPDFPage_CloseAnnot(annot);
                            if (FPDFPage_RemoveAnnot(page, target.objectIndex)) {
                                removed = true;
                            }
                        } else {
                            FPDFPage_CloseAnnot(annot);
                        }
                    }
                }

                if (!removed && target.objectIndex >= 0 && target.objectIndex < FPDFPage_CountObjects(page)) {
                    FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, target.objectIndex);
                    const int expectedObjectType = target.objectType == 9
                                                   ? FPDF_PAGEOBJ_IMAGE
                                                   : (target.objectType == 19
                                                      ? FPDF_PAGEOBJ_SHADING
                                                      : (target.objectType == 20
                                                         ? FPDF_PAGEOBJ_FORM
                                                         : FPDF_PAGEOBJ_PATH));
                    const bool objectTypeMatches = target.objectType == 19
                                                   ? IsEditableShadingObject(pageObject)
                                                   : pageObject &&
                                                     FPDFPageObj_GetType(pageObject) == expectedObjectType;
                    if (objectTypeMatches &&
                        FPDFPage_RemoveObject(page, pageObject)) {
                        FPDFPageObj_Destroy(pageObject);
                        removed = true;
                    }
                }
                if (!removed && (target.objectType == 19 || target.objectType == 20)) {
                    continue;
                }
                if (!removed) {
                    float bestScore = 0.0f;
                    int bestIndex = -1;

                    const int objectCount = FPDFPage_CountObjects(page);
                    for (int objectIndex = 0; objectIndex < objectCount; objectIndex++) {
                        FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, objectIndex);
                        const int expectedObjectType = target.objectType == 9
                                                       ? FPDF_PAGEOBJ_IMAGE
                                                       : (target.objectType == 19
                                                          ? FPDF_PAGEOBJ_SHADING
                                                          : (target.objectType == 20
                                                             ? FPDF_PAGEOBJ_FORM
                                                             : FPDF_PAGEOBJ_PATH));
                        const bool objectTypeMatches = target.objectType == 19
                                                       ? IsEditableShadingObject(pageObject)
                                                       : pageObject &&
                                                         FPDFPageObj_GetType(pageObject) == expectedObjectType;
                        if (!objectTypeMatches) continue;

                        float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
                        if (!FPDFPageObj_GetBounds(pageObject, &left, &bottom, &right, &top)) continue;

                        const float objLeft = std::min(left, right);
                        const float objRight = std::max(left, right);
                        const float objBottom = std::min(bottom, top);
                        const float objTop = std::max(bottom, top);

                        const float overlapLeft = std::max(targetLeft, objLeft);
                        const float overlapBottom = std::max(targetBottom, objBottom);
                        const float overlapRight = std::min(targetRight, objRight);
                        const float overlapTop = std::min(targetTop, objTop);
                        if (overlapRight <= overlapLeft || overlapBottom <= overlapTop) continue;

                        unsigned int strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 255;
                        FPDFPageObj_GetStrokeColor(pageObject, &strokeR, &strokeG, &strokeB, &strokeA);
                        const bool colorMatches =
                            static_cast<int>(strokeR) == target.r &&
                            static_cast<int>(strokeG) == target.g &&
                            static_cast<int>(strokeB) == target.b;
                        const float overlapArea = (overlapRight - overlapLeft) * (overlapTop - overlapBottom);
                        const float score = colorMatches ? overlapArea * 2.0f : overlapArea;
                        if (score > bestScore) {
                            bestScore = score;
                            bestIndex = objectIndex;
                        }
                    }

                    if (bestIndex >= 0 && bestIndex < FPDFPage_CountObjects(page)) {
                        FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, bestIndex);
                        if (pageObject && FPDFPage_RemoveObject(page, pageObject)) {
                            FPDFPageObj_Destroy(pageObject);
                        }
                    }
                }
            }
        }

        if (processNewObjects && (providedPage != nullptr || providedPageIndex >= 0)) {
            for (int i = 0; i < highlightCount; i++) {
                jobject obj = env->GetObjectArrayElement(highlightsArray, i);
                if (!obj) continue;
                int objPageIndex = env->GetIntField(obj, pageField);
                int nativeEditAction = env->GetIntField(obj, nativeEditActionField);
                int typeInt = env->GetIntField(obj, typeField);
                LOGE(
                        "PDF_EDIT_NATIVE ApplyNativeAnnotationEditActions newObjectCandidate index=%d page=%d targetPage=%d type=%d action=%d",
                        i,
                        objPageIndex,
                        pageIndex,
                        typeInt,
                        nativeEditAction
                );

                if (objPageIndex != pageIndex || nativeEditAction != 0) {
                    env->DeleteLocalRef(obj);
                    continue;
                }

                float left = env->GetFloatField(obj, leftField);
                float top = env->GetFloatField(obj, topField);
                float right = env->GetFloatField(obj, rightField);
                float bottom = env->GetFloatField(obj, bottomField);
                int r = env->GetIntField(obj, rField);
                int g = env->GetIntField(obj, gField);
                int b = env->GetIntField(obj, bField);
                int alpha = env->GetIntField(obj, alphaField);

                FS_RECTF rect;
                rect.left = fmin(left, right);
                rect.right = fmax(left, right);
                rect.bottom = fmin(top, bottom);
                rect.top = fmax(top, bottom);

                if (typeInt == 6) {
                    processFreeHand(env, obj, page, dataPropsField, r, g, b, jsonClass, jsonInit);
                } else {
                    if (typeInt == 11) {
                        processFreeText(env, obj, doc, page, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        env->DeleteLocalRef(obj);
                        continue;
                    }

                    int pdfType = (typeInt == 1) ? FPDF_ANNOT_UNDERLINE :
                                   (typeInt == 2) ? FPDF_ANNOT_STRIKEOUT :
                                   (typeInt == 8) ? FPDF_ANNOT_SQUIGGLY :
                                  (typeInt == 3) ? FPDF_ANNOT_LINK :
                               (typeInt == 10) ? FPDF_ANNOT_TEXT :
                               (typeInt == 21) ? FPDF_ANNOT_FILEATTACHMENT :
                                   (typeInt == 11) ? FPDF_ANNOT_FREETEXT :
                                   (typeInt == 4 || typeInt == 7) ? FPDF_ANNOT_SQUARE :
                                   FPDF_ANNOT_HIGHLIGHT;
                    FPDF_ANNOTATION annot = IsPdfShapeNativeType(typeInt)
                                            ? CreatePdfShapeAnnotation(page, typeInt)
                                            : FPDFPage_CreateAnnot(page, (typeInt == 5 || typeInt == 9) ? FPDF_ANNOT_STAMP : pdfType);
                    if (annot) {
                        FPDFAnnot_SetRect(annot, &rect);
                        if (typeInt == 3) processLink(env, obj, page, annot, rect, urlField);
                        else if (typeInt == 4) {
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
                            SetAnnotAsciiStringValue(annot, "LufickPdfRedaction", "1");
                        }
                        else if (typeInt == 10) processStickyNoteComment(env, obj, annot, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        else if (isSimplePdfStampBridgeAnnotation(env, obj, dataPropsField, jsonClass, jsonInit)) {
                            processSimplePdfStamp(env, obj, doc, page, annot, rect, typeInt, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        }
                        else if (typeInt == 5) processTextStamp(env, obj, doc, page, annot, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        else if (typeInt == 11) processFreeText(env, obj, doc, page, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        else if (typeInt == 9) processImageOrPresetStamp(env, obj, doc, page, annot, rect, dataPropsField, jsonClass, jsonInit);
                        else if (typeInt == 7) {
                            processRegionHighlight(env, obj, page, annot, rect, r, g, b, alpha);
                        }
                        else if (IsPdfShapeNativeType(typeInt)) {
                            processPdfShape(env, obj, annot, rect, typeInt, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        }
                        else {
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
                            if (typeInt == 1 || typeInt == 2 || typeInt == 8) {
                                ClearAnnotationAppearanceObjects(annot);
                            }

                            jstring jMarkupRects = (jstring)env->GetObjectField(obj, markupRectsField);
                            bool appended = false;
                            if (jMarkupRects) {
                                jobject rectsArray = env->NewObject(jsonArrayClass, jsonArrayInit, jMarkupRects);
                                if (rectsArray) {
                                    int rectCount = env->CallIntMethod(rectsArray, jsonArrayLength);
                                    for (int rectIndex = 0; rectIndex < rectCount; rectIndex++) {
                                        jobject rectObj = env->CallObjectMethod(rectsArray, jsonArrayGetObject, rectIndex);
                                        if (!rectObj) continue;

                                        float quadLeft = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("left"));
                                        float quadTop = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("top"));
                                        float quadRight = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("right"));
                                        float quadBottom = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("bottom"));
                                        float rectTop = fmax(quadTop, quadBottom);
                                        float rectBottom = fmin(quadTop, quadBottom);
                                        if (typeInt == 1 || typeInt == 2 || typeInt == 8) {
                                            double strokeRatioValue = env->CallDoubleMethod(
                                                    rectObj,
                                                    jsonOptDouble,
                                                    env->NewStringUTF("strokeWidthRatio"),
                                                    GetDefaultTextMarkupStrokeRatio(typeInt)
                                            );
                                            float rectHeight = fmax(rectTop - rectBottom, 0.5f);
                                            float strokeRatio = GetClampedTextMarkupStrokeRatio(typeInt, strokeRatioValue);
                                            float thickness = (typeInt == 8)
                                                    ? GetSquigglyRenderThickness(rectHeight, strokeRatio)
                                                    : fmax(rectHeight * strokeRatio, 0.5f);
                                            if (typeInt == 1) {
                                                float lineY = rectBottom + (thickness * 0.5f);
                                                AppendStraightTextMarkupAppearance(
                                                        annot,
                                                        quadLeft,
                                                        quadRight,
                                                        lineY,
                                                        r,
                                                        g,
                                                        b,
                                                        alpha,
                                                        thickness
                                                );
                                            } else {
                                                if (typeInt == 2) {
                                                    float centerY = (rectTop + rectBottom) * 0.5f;
                                                    AppendStraightTextMarkupAppearance(
                                                            annot,
                                                            quadLeft,
                                                            quadRight,
                                                            centerY,
                                                            r,
                                                            g,
                                                            b,
                                                            alpha,
                                                            thickness
                                                    );
                                                } else {
                                                    const float appearanceTop =
                                                            GetSquigglyAttachmentTop(rectBottom, rectTop, thickness);
                                                    AppendSquigglyTextMarkupAppearance(
                                                            annot,
                                                            quadLeft,
                                                            quadRight,
                                                            rectBottom,
                                                            appearanceTop,
                                                            r,
                                                            g,
                                                            b,
                                                            alpha,
                                                            thickness
                                                    );
                                                }
                                            }
                                        }

                                        FS_QUADPOINTSF qp = {
                                                fmin(quadLeft, quadRight), rectTop,
                                                fmax(quadLeft, quadRight), rectTop,
                                                fmin(quadLeft, quadRight), rectBottom,
                                                fmax(quadLeft, quadRight), rectBottom
                                        };
                                        FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                                        appended = true;
                                        env->DeleteLocalRef(rectObj);
                                    }
                                    env->DeleteLocalRef(rectsArray);
                                }

                                const jchar* rawMarkupMeta = env->GetStringChars(jMarkupRects, nullptr);
                                FPDFAnnot_SetStringValue(annot, "LufickMarkupMeta", (FPDF_WIDESTRING)rawMarkupMeta);
                                env->ReleaseStringChars(jMarkupRects, rawMarkupMeta);
                            }
                            if (!appended) {
                                AppendFallbackTextMarkupAppearance(annot, typeInt, rect, r, g, b, alpha);
                                FS_QUADPOINTSF qp = {rect.left, rect.top, rect.right, rect.top, rect.left, rect.bottom, rect.right, rect.bottom};
                                FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                            }

                            if (typeInt == 0) {
                                FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
                                const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
                                FPDFAnnot_SetStringValue(annot, "BM", (FPDF_WIDESTRING)blendMode);
                            }
                        }
                        FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
                        FPDFPage_CloseAnnot(annot);
                    }
                }

                env->DeleteLocalRef(obj);
            }
        }

        FPDFPage_GenerateContent(page);
        LOGE(
                "PDF_EDIT_NATIVE ApplyNativeAnnotationEditActions page=%d afterGenerate annotCount=%d objectCount=%d shouldClose=%d",
                pageIndex,
                FPDFPage_GetAnnotCount(page),
                FPDFPage_CountObjects(page),
                shouldClosePage ? 1 : 0
        );
        if (shouldClosePage) {
            FPDF_ClosePage(page);
        }
    }

    LOGE("PDF_EDIT_NATIVE ApplyNativeAnnotationEditActions finish touchedPages=%zu", touchedPages.size());
    return allPagesSucceeded;
}

JNIEXPORT jboolean JNICALL //TODO Main Method of saving.
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveAnnotations(
        JNIEnv* env, jobject thiz, jstring inputPath_, jstring outputPath_, jobjectArray highlightsArray) {

    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) {
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    jclass highlightClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfAnnotationNative");
    jfieldID typeField = env->GetFieldID(highlightClass, "type", "I");
    jfieldID pageField = env->GetFieldID(highlightClass, "pageIndex", "I");
    jfieldID leftField = env->GetFieldID(highlightClass, "left", "F");
    jfieldID topField = env->GetFieldID(highlightClass, "top", "F");
    jfieldID rightField = env->GetFieldID(highlightClass, "right", "F");
    jfieldID bottomField = env->GetFieldID(highlightClass, "bottom", "F");
    jfieldID rField = env->GetFieldID(highlightClass, "r", "I");
    jfieldID gField = env->GetFieldID(highlightClass, "g", "I");
    jfieldID bField = env->GetFieldID(highlightClass, "b", "I");
    jfieldID alphaField = env->GetFieldID(highlightClass, "alpha", "I");
    jfieldID urlField = env->GetFieldID(highlightClass, "linkUrl", "Ljava/lang/String;");
    jfieldID markupRectsField = env->GetFieldID(highlightClass, "markupRectsJson", "Ljava/lang/String;");
    jfieldID dataPropsField = env->GetFieldID(highlightClass, "dataProperties", "Ljava/lang/String;");
    jfieldID nativeSourceIdField = env->GetFieldID(highlightClass, "nativeSourceId", "I");
    jfieldID nativeEditActionField = env->GetFieldID(highlightClass, "nativeEditAction", "I");

    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jclass jsonArrayClass = env->FindClass("org/json/JSONArray");
    jmethodID jsonArrayInit = env->GetMethodID(jsonArrayClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayLength = env->GetMethodID(jsonArrayClass, "length", "()I");
    jmethodID jsonArrayGetObject = env->GetMethodID(jsonArrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jmethodID jsonGetDouble = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID jsonOptDouble = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    int highlightCount = env->GetArrayLength(highlightsArray);

    ApplyNativeAnnotationEditActions(env, doc, highlightsArray);

    FPDF_PAGE currentPage = nullptr;
    int lastPageIndex = -1;

    for (int i = 0; i < highlightCount; i++) {
        jobject obj = env->GetObjectArrayElement(highlightsArray, i);
        int typeInt = env->GetIntField(obj, typeField);
        int pageIndex = env->GetIntField(obj, pageField);
        int nativeSourceId = env->GetIntField(obj, nativeSourceIdField);
        int nativeEditAction = env->GetIntField(obj, nativeEditActionField);

        if (nativeEditAction == 1 || nativeEditAction == 2 || nativeEditAction == 5 || nativeSourceId >= 0) {
            env->DeleteLocalRef(obj);
            continue;
        }

        if (pageIndex != lastPageIndex) {
            if (currentPage) { FPDFPage_GenerateContent(currentPage); FPDF_ClosePage(currentPage); }
            currentPage = FPDF_LoadPage(doc, pageIndex);
            lastPageIndex = pageIndex;
        }

        if (currentPage) {
            float left = env->GetFloatField(obj, leftField);
            float top = env->GetFloatField(obj, topField);
            float right = env->GetFloatField(obj, rightField);
            float bottom = env->GetFloatField(obj, bottomField);
            int r = env->GetIntField(obj, rField);
            int g = env->GetIntField(obj, gField);
            int b = env->GetIntField(obj, bField);
            int alpha = env->GetIntField(obj, alphaField);

            FS_RECTF rect;
            rect.left = fmin(left, right); rect.right = fmax(left, right);
            rect.bottom = fmin(top, bottom); rect.top = fmax(top, bottom);

            if (typeInt == 6) { // Freehand is not a standard Annotation type in your logic
                processFreeHand(env, obj, currentPage, dataPropsField, r, g, b, jsonClass, jsonInit);
            } else {
                if (typeInt == 11) {
                    processFreeText(env, obj, doc, currentPage, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    env->DeleteLocalRef(obj);
                    continue;
                }

                    int pdfType = (typeInt == 1) ? FPDF_ANNOT_UNDERLINE :
                                  (typeInt == 2) ? FPDF_ANNOT_STRIKEOUT :
                                  (typeInt == 8) ? FPDF_ANNOT_SQUIGGLY :
                                  (typeInt == 3) ? FPDF_ANNOT_LINK :
                                  (typeInt == 10) ? FPDF_ANNOT_TEXT :
                                  (typeInt == 21) ? FPDF_ANNOT_FILEATTACHMENT :
                                  (typeInt == 11) ? FPDF_ANNOT_FREETEXT :
                                  (typeInt == 4 || typeInt == 7) ? FPDF_ANNOT_SQUARE :
                                  FPDF_ANNOT_HIGHLIGHT;

                FPDF_ANNOTATION annot = IsPdfShapeNativeType(typeInt)
                                        ? CreatePdfShapeAnnotation(currentPage, typeInt)
                                        : FPDFPage_CreateAnnot(currentPage, (typeInt == 5 || typeInt == 9) ? FPDF_ANNOT_STAMP : pdfType);
                if (annot) {
                    FPDFAnnot_SetRect(annot, &rect);
                    if (typeInt == 3) processLink(env, obj, currentPage, annot, rect, urlField);
                    else if (typeInt == 4) { // Redaction
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
                        SetAnnotAsciiStringValue(annot, "LufickPdfRedaction", "1");
                    }
                    else if (typeInt == 10) processStickyNoteComment(env, obj, annot, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    else if (typeInt == 21) {
                        ProcessFileAttachment(
                                env, obj, doc, annot, dataPropsField, jsonClass, jsonInit,
                                r, g, b, alpha);
                    }
                    else if (isSimplePdfStampBridgeAnnotation(env, obj, dataPropsField, jsonClass, jsonInit)) {
                        processSimplePdfStamp(env, obj, doc, currentPage, annot, rect, typeInt, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    }
                    else if (typeInt == 5) processTextStamp(env, obj, doc, currentPage, annot, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    else if (typeInt == 11) processFreeText(env, obj, doc, currentPage, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    else if (typeInt == 9) processImageOrPresetStamp(env, obj, doc, currentPage, annot, rect, dataPropsField, jsonClass, jsonInit);
                    else if (typeInt == 7) {
                        processRegionHighlight(env, obj, currentPage, annot, rect, r, g, b, alpha);
                    }
                    else if (IsPdfShapeNativeType(typeInt)) {
                        processPdfShape(env, obj, annot, rect, typeInt, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    }
                    else { // Highlight / Underline / Strikeout / Squiggly
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
                        if (typeInt == 1 || typeInt == 2 || typeInt == 8) {
                            ClearAnnotationAppearanceObjects(annot);
                        }
                        if (typeInt == 0) {
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
                            const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
                            FPDFAnnot_SetStringValue(annot, "BM", (FPDF_WIDESTRING)blendMode);
                        }
                        jstring jMarkupRects = (jstring)env->GetObjectField(obj, markupRectsField);
                        bool appended = false;
                        bool hasBounds = false;
                        FS_RECTF bounds = {0.0f, 0.0f, 0.0f, 0.0f};
                        if (jMarkupRects) {
                            jobject rectsArray = env->NewObject(jsonArrayClass, jsonArrayInit, jMarkupRects);
                            if (rectsArray) {
                                int rectCount = env->CallIntMethod(rectsArray, jsonArrayLength);
                                for (int rectIndex = 0; rectIndex < rectCount; rectIndex++) {
                                    jobject rectObj = env->CallObjectMethod(rectsArray, jsonArrayGetObject, rectIndex);
                                    if (!rectObj) continue;

                                    float quadLeft = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("left"));
                                    float quadTop = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("top"));
                                    float quadRight = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("right"));
                                    float quadBottom = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("bottom"));
                                    float rectTop = fmax(quadTop, quadBottom);
                                    float rectBottom = fmin(quadTop, quadBottom);
                                    if (typeInt == 1 || typeInt == 2 || typeInt == 8) {
                                        double strokeRatioValue = env->CallDoubleMethod(
                                                rectObj,
                                                jsonOptDouble,
                                                env->NewStringUTF("strokeWidthRatio"),
                                                GetDefaultTextMarkupStrokeRatio(typeInt)
                                        );
                                        float rectHeight = fmax(rectTop - rectBottom, 0.5f);
                                        float strokeRatio = GetClampedTextMarkupStrokeRatio(typeInt, strokeRatioValue);
                                        float thickness = (typeInt == 8)
                                                ? GetSquigglyRenderThickness(rectHeight, strokeRatio)
                                                : fmax(rectHeight * strokeRatio, 0.5f);
                                        if (typeInt == 1) {
                                            float lineY = rectBottom + (thickness * 0.5f);
                                            AppendStraightTextMarkupAppearance(
                                                    annot,
                                                    quadLeft,
                                                    quadRight,
                                                    lineY,
                                                    r,
                                                    g,
                                                    b,
                                                    alpha,
                                                    thickness
                                            );
                                        } else {
                                            if (typeInt == 2) {
                                                float centerY = (rectTop + rectBottom) * 0.5f;
                                                AppendStraightTextMarkupAppearance(
                                                        annot,
                                                        quadLeft,
                                                        quadRight,
                                                        centerY,
                                                        r,
                                                        g,
                                                        b,
                                                        alpha,
                                                        thickness
                                                );
                                        } else {
                                            const float appearanceTop =
                                                    GetSquigglyAttachmentTop(rectBottom, rectTop, thickness);
                                            AppendSquigglyTextMarkupAppearance(
                                                    annot,
                                                    quadLeft,
                                                    quadRight,
                                                    rectBottom,
                                                    appearanceTop,
                                                    r,
                                                    g,
                                                    b,
                                                    alpha,
                                                    thickness
                                            );
                                            }
                                        }
                                    }

                                    FS_QUADPOINTSF qp = {
                                            fmin(quadLeft, quadRight), rectTop,
                                            fmax(quadLeft, quadRight), rectTop,
                                            fmin(quadLeft, quadRight), rectBottom,
                                            fmax(quadLeft, quadRight), rectBottom
                                    };
                                    FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                                    const FS_RECTF pieceBounds = {
                                            fmin(quadLeft, quadRight),
                                            fmin(rectTop, rectBottom),
                                            fmax(quadLeft, quadRight),
                                            fmax(rectTop, rectBottom)
                                    };
                                    if (!hasBounds) {
                                        bounds = pieceBounds;
                                        hasBounds = true;
                                    } else {
                                        bounds.left = fmin(bounds.left, pieceBounds.left);
                                        bounds.right = fmax(bounds.right, pieceBounds.right);
                                        bounds.bottom = fmin(bounds.bottom, pieceBounds.bottom);
                                        bounds.top = fmax(bounds.top, pieceBounds.top);
                                    }
                                    appended = true;
                                    env->DeleteLocalRef(rectObj);
                                }
                                env->DeleteLocalRef(rectsArray);
                            }

                            const jchar* rawMarkupMeta = env->GetStringChars(jMarkupRects, nullptr);
                            FPDFAnnot_SetStringValue(annot, "LufickMarkupMeta", (FPDF_WIDESTRING)rawMarkupMeta);
                            env->ReleaseStringChars(jMarkupRects, rawMarkupMeta);
                        }
                        if (!appended) {
                            AppendFallbackTextMarkupAppearance(annot, typeInt, rect, r, g, b, alpha);
                            FS_QUADPOINTSF qp = {rect.left, rect.top, rect.right, rect.top, rect.left, rect.bottom, rect.right, rect.bottom};
                            FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                        } else if (hasBounds) {
                            FPDFAnnot_SetRect(annot, &bounds);
                        }
                    }
                    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
                    FPDFPage_CloseAnnot(annot);
                }
            }
        }
        env->DeleteLocalRef(obj);
    }

    if (currentPage) { FPDFPage_GenerateContent(currentPage); FPDF_ClosePage(currentPage); }

    // Save Logic
    FILE* file = fopen(outputPath, "wb");
    PdfFileWriter writer{ {1, WriteBlock}, file };
    int success = (file) ? FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL) : JNI_FALSE;
    if (file) fclose(file);
    if (success && !PatchSavedPdfShapeNativeDictionaries(outputPath)) {
        success = JNI_FALSE;
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
    return success ? JNI_TRUE : JNI_FALSE;
}

static bool processRedactionContent(
        FPDF_PAGE page,
        FS_RECTF rect,
        int r,
        int g,
        int b,
        int alpha
) {
    if (!page) return false;

    const float redactionLeft = fmin(rect.left, rect.right);
    const float redactionRight = fmax(rect.left, rect.right);
    const float redactionBottom = fmin(rect.bottom, rect.top);
    const float redactionTop = fmax(rect.bottom, rect.top);
    const float width = redactionRight - redactionLeft;
    const float height = redactionTop - redactionBottom;
    if (width <= 0.0f || height <= 0.0f) return false;

    FPDF_PAGEOBJECT redactionObject = FPDFPageObj_CreateNewRect(
            redactionLeft,
            redactionBottom,
            width,
            height
    );
    if (!redactionObject) return false;

    FPDFPageObj_SetFillColor(redactionObject, r, g, b, alpha);
    FPDFPath_SetDrawMode(redactionObject, FPDF_FILLMODE_WINDING, JNI_FALSE);
    FPDFPage_InsertObject(page, redactionObject);
    LOGE(
            "PDF_EDIT_NATIVE processRedactionContent rect=[%f,%f,%f,%f]",
            redactionLeft,
            redactionBottom,
            redactionRight,
            redactionTop
    );
    return true;
}

static std::string GetPageObjectMarkName(FPDF_PAGEOBJECTMARK mark) {
    if (!mark) return std::string();
    unsigned long byteLength = 0;
    if (!FPDFPageObjMark_GetName(mark, nullptr, 0, &byteLength) ||
        byteLength < sizeof(FPDF_WCHAR)) {
        return std::string();
    }
    std::vector<FPDF_WCHAR> buffer((byteLength / sizeof(FPDF_WCHAR)) + 1, 0);
    if (!FPDFPageObjMark_GetName(mark, buffer.data(), byteLength, &byteLength)) {
        return std::string();
    }
    return Utf16ToSimpleUtf8(std::u16string(
            reinterpret_cast<const char16_t*>(buffer.data())));
}

static bool PageObjectHasExactMark(
        FPDF_PAGEOBJECT object,
        const std::string& expectedMarkName) {
    if (!object || expectedMarkName.empty()) return false;
    const int markCount = FPDFPageObj_CountMarks(object);
    for (int markIndex = 0; markIndex < markCount; ++markIndex) {
        if (GetPageObjectMarkName(
                FPDFPageObj_GetMark(object, static_cast<unsigned long>(markIndex))) ==
            expectedMarkName) {
            return true;
        }
    }
    return false;
}

static bool PageObjectHasMarkPrefix(
        FPDF_PAGEOBJECT object,
        const char* expectedPrefix) {
    if (!object || !expectedPrefix || !*expectedPrefix) return false;
    const size_t prefixLength = strlen(expectedPrefix);
    const int markCount = FPDFPageObj_CountMarks(object);
    for (int markIndex = 0; markIndex < markCount; ++markIndex) {
        const std::string markName = GetPageObjectMarkName(
                FPDFPageObj_GetMark(object, static_cast<unsigned long>(markIndex)));
        if (markName.size() >= prefixLength &&
            markName.compare(0, prefixLength, expectedPrefix) == 0) {
            return true;
        }
    }
    return false;
}

static FPDF_PAGEOBJECT FindTextEditPreviewObject(
        FPDF_PAGE page,
        const std::string& markPrefix,
        const std::string& exactMarkName,
        std::vector<FPDF_PAGEOBJECT>* staleObjects) {
    if (!page) return nullptr;
    FPDF_PAGEOBJECT exactObject = nullptr;
    const int objectCount = FPDFPage_CountObjects(page);
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectIndex);
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT) continue;
        const int markCount = FPDFPageObj_CountMarks(object);
        for (int markIndex = 0; markIndex < markCount; ++markIndex) {
            const std::string markName = GetPageObjectMarkName(
                    FPDFPageObj_GetMark(object, static_cast<unsigned long>(markIndex)));
            if (markName.rfind(markPrefix, 0) != 0) continue;
            if (markName == exactMarkName) {
                exactObject = object;
            } else if (staleObjects) {
                staleObjects->push_back(object);
            }
            break;
        }
    }
    return exactObject;
}

static void RemoveTextEditPreviewObjects(FPDF_PAGE page, const std::string& markPrefix) {
    if (!page) return;
    std::vector<FPDF_PAGEOBJECT> objectsToRemove;
    const int objectCount = FPDFPage_CountObjects(page);
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectIndex);
        if (!object) continue;
        const int markCount = FPDFPageObj_CountMarks(object);
        for (int markIndex = 0; markIndex < markCount; ++markIndex) {
            const std::string markName = GetPageObjectMarkName(
                    FPDFPageObj_GetMark(object, static_cast<unsigned long>(markIndex)));
            if (markName.rfind(markPrefix, 0) == 0) {
                objectsToRemove.push_back(object);
                break;
            }
        }
    }
    for (FPDF_PAGEOBJECT object : objectsToRemove) {
        if (FPDFPage_RemoveObject(page, object)) FPDFPageObj_Destroy(object);
    }
}

struct TextEditNativeDecorationPaths {
    std::vector<FPDF_PAGEOBJECT> underline;
    std::vector<FPDF_PAGEOBJECT> strikeout;
};

static TextEditNativeDecorationPaths FindTextEditNativeDecorationPaths(
        FPDF_PAGE page,
        FPDF_PAGEOBJECT textObject,
        float textLeft,
        float textBottom,
        float textRight,
        float textTop,
        const std::string& markPrefix) {
    TextEditNativeDecorationPaths result;
    if (!page || !textObject || FPDFPageObj_GetType(textObject) != FPDF_PAGEOBJ_TEXT) return result;
    const int objectCount = FPDFPage_CountObjects(page);
    bool hasMarkedPaths = false;
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectIndex);
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH) continue;
        const int markCount = FPDFPageObj_CountMarks(object);
        for (int markIndex = 0; markIndex < markCount; ++markIndex) {
            const std::string markName = GetPageObjectMarkName(
                    FPDFPageObj_GetMark(object, static_cast<unsigned long>(markIndex)));
            if (markName.rfind(markPrefix, 0) != 0) continue;
            hasMarkedPaths = true;
            if (markName.find("underline", markPrefix.size()) != std::string::npos) {
                result.underline.push_back(object);
            } else if (markName.find("strikeout", markPrefix.size()) != std::string::npos) {
                result.strikeout.push_back(object);
            }
            break;
        }
    }
    if (hasMarkedPaths) return result;

    const float textWidth = fabsf(textRight - textLeft);
    const float textHeight = fabsf(textTop - textBottom);
    if (textWidth <= 0.01f || textHeight <= 0.01f) return result;
    unsigned int textR = 0, textG = 0, textB = 0, textA = 255;
    FPDFPageObj_GetFillColor(textObject, &textR, &textG, &textB, &textA);

    int underlineOrdinal = 0;
    int strikeoutOrdinal = 0;
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectIndex);
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH) continue;
        float pathLeft = 0.0f, pathBottom = 0.0f, pathRight = 0.0f, pathTop = 0.0f;
        if (!FPDFPageObj_GetBounds(
                object,
                &pathLeft,
                &pathBottom,
                &pathRight,
                &pathTop)) continue;
        const float pathWidth = fabsf(pathRight - pathLeft);
        const float pathHeight = fabsf(pathTop - pathBottom);
        if (pathHeight > textHeight * 0.22f) continue;
        const float overlap = fmaxf(
                0.0f,
                fminf(textRight, pathRight) - fmaxf(textLeft, pathLeft));
        if (overlap < fminf(textWidth, pathWidth) * 0.72f) continue;
        unsigned int pathR = 0, pathG = 0, pathB = 0, pathA = 255;
        FPDFPageObj_GetStrokeColor(object, &pathR, &pathG, &pathB, &pathA);
        if (abs(static_cast<int>(pathR) - static_cast<int>(textR)) > 12 ||
            abs(static_cast<int>(pathG) - static_cast<int>(textG)) > 12 ||
            abs(static_cast<int>(pathB) - static_cast<int>(textB)) > 12) continue;
        const float pathNormal = (pathBottom + pathTop) * 0.5f;
        const float underlineNormal = textBottom + textHeight * 0.08f;
        const float strikeoutNormal = textBottom + textHeight * 0.52f;
        const float tolerance = fmaxf(textHeight * 0.18f, 0.75f);
        const float underlineDistance = fabsf(pathNormal - underlineNormal);
        const float strikeoutDistance = fabsf(pathNormal - strikeoutNormal);
        if (underlineDistance <= tolerance && underlineDistance <= strikeoutDistance) {
            FPDFPageObj_AddMark(
                    object,
                    (markPrefix + "underline_" + std::to_string(underlineOrdinal++)).c_str());
            result.underline.push_back(object);
        } else if (strikeoutDistance <= tolerance) {
            FPDFPageObj_AddMark(
                    object,
                    (markPrefix + "strikeout_" + std::to_string(strikeoutOrdinal++)).c_str());
            result.strikeout.push_back(object);
        }
    }
    return result;
}

static void UpdateTextEditNativeDecorationPaths(
        FPDF_PAGE page,
        const std::vector<FPDF_PAGEOBJECT>& paths,
        const std::vector<FPDF_PAGEOBJECT>& currentTextObjects,
        bool keepDecoration) {
    if (!page || paths.empty()) return;
    if (!keepDecoration || currentTextObjects.empty()) {
        for (FPDF_PAGEOBJECT path : paths) {
            if (FPDFPage_RemoveObject(page, path)) FPDFPageObj_Destroy(path);
        }
        return;
    }
    FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
    if (!FPDFPageObj_GetMatrix(currentTextObjects.front(), &matrix)) return;
    float baselineX = matrix.a;
    float baselineY = matrix.b;
    const float baselineLength = hypotf(baselineX, baselineY);
    if (baselineLength <= 0.0001f) return;
    baselineX /= baselineLength;
    baselineY /= baselineLength;
    float desiredMin = FLT_MAX;
    float desiredMax = -FLT_MAX;
    for (FPDF_PAGEOBJECT textObject : currentTextObjects) {
        FS_QUADPOINTSF quad = {};
        if (!textObject || !FPDFPageObj_GetRotatedBounds(textObject, &quad)) continue;
        const float points[4][2] = {
                {quad.x1, quad.y1}, {quad.x2, quad.y2},
                {quad.x3, quad.y3}, {quad.x4, quad.y4}
        };
        for (const auto& point : points) {
            const float projection = point[0] * baselineX + point[1] * baselineY;
            desiredMin = fminf(desiredMin, projection);
            desiredMax = fmaxf(desiredMax, projection);
        }
    }
    if (desiredMax - desiredMin <= 0.01f) return;
    float sourceMin = FLT_MAX;
    float sourceMax = -FLT_MAX;
    for (FPDF_PAGEOBJECT path : paths) {
        FS_QUADPOINTSF quad = {};
        if (!path || !FPDFPageObj_GetRotatedBounds(path, &quad)) continue;
        const float points[4][2] = {
                {quad.x1, quad.y1}, {quad.x2, quad.y2},
                {quad.x3, quad.y3}, {quad.x4, quad.y4}
        };
        for (const auto& point : points) {
            const float projection = point[0] * baselineX + point[1] * baselineY;
            sourceMin = fminf(sourceMin, projection);
            sourceMax = fmaxf(sourceMax, projection);
        }
    }
    const float sourceWidth = sourceMax - sourceMin;
    if (sourceWidth <= 0.01f) return;
    const float scale = (desiredMax - desiredMin) / sourceWidth;
    const float cross = (scale - 1.0f) * baselineX * baselineY;
    const float transformA = scale * baselineX * baselineX + baselineY * baselineY;
    const float transformB = cross;
    const float transformC = cross;
    const float transformD = scale * baselineY * baselineY + baselineX * baselineX;
    const float shift = desiredMin - scale * sourceMin;
    const float transformE = baselineX * shift;
    const float transformF = baselineY * shift;
    for (FPDF_PAGEOBJECT path : paths) {
        if (path) FPDFPageObj_Transform(
                path,
                transformA,
                transformB,
                transformC,
                transformD,
                transformE,
                transformF);
    }
}

static void ApplyTextEditLiveFontStyle(
        FPDF_PAGEOBJECT textObject,
        bool resetAppearance,
        bool isBold,
        bool isItalic,
        int sourceFontWeight,
        float sourceItalicAngle,
        float fontSize,
        unsigned int r,
        unsigned int g,
        unsigned int b,
        unsigned int a) {
    if (!textObject || !resetAppearance) return;
    const bool sourceIsBold = sourceFontWeight >= 600;
    if (isBold && !sourceIsBold) {
        FPDFPageObj_SetStrokeColor(textObject, r, g, b, a);
        FPDFPageObj_SetStrokeWidth(textObject, fmax(fontSize * 0.025f, 0.2f));
        FPDFTextObj_SetTextRenderMode(textObject, FPDF_TEXTRENDERMODE_FILL_STROKE);
    } else if (!isBold && !sourceIsBold) {
        FPDFTextObj_SetTextRenderMode(textObject, FPDF_TEXTRENDERMODE_FILL);
    }

    if (isItalic && fabsf(sourceItalicAngle) <= 0.1f) {
        FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
        if (FPDFPageObj_GetMatrix(textObject, &matrix)) {
            const float italicShear = tanf(12.0f * static_cast<float>(M_PI) / 180.0f);
            matrix.c += matrix.a * italicShear;
            matrix.d += matrix.b * italicShear;
            FPDFPageObj_SetMatrix(textObject, &matrix);
        }
    }
}

static void AppendTextEditLiveDecoration(
        FPDF_PAGE page,
        const std::vector<FPDF_PAGEOBJECT>& textObjects,
        bool underline,
        bool strikeout,
        unsigned int r,
        unsigned int g,
        unsigned int b,
        unsigned int a,
        const std::string& markPrefix,
        int objectOrdinal) {
    if (!page || textObjects.empty() || (!underline && !strikeout)) return;
    FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
    if (!FPDFPageObj_GetMatrix(textObjects.front(), &matrix)) return;

    float baselineX = matrix.a;
    float baselineY = matrix.b;
    float baselineLength = hypotf(baselineX, baselineY);
    if (baselineLength <= 0.0001f) return;
    baselineX /= baselineLength;
    baselineY /= baselineLength;
    const float normalX = -baselineY;
    const float normalY = baselineX;
    struct DecorationLineBounds {
        float minBaseline = FLT_MAX;
        float maxBaseline = -FLT_MAX;
        float minNormal = FLT_MAX;
        float maxNormal = -FLT_MAX;
        float baselineNormal = 0.0f;
    };
    std::vector<DecorationLineBounds> lineBounds;
    for (FPDF_PAGEOBJECT textObject : textObjects) {
        if (!textObject) continue;
        FS_QUADPOINTSF quad = {};
        if (!FPDFPageObj_GetRotatedBounds(textObject, &quad)) continue;
        const float points[4][2] = {
                {quad.x1, quad.y1}, {quad.x2, quad.y2},
                {quad.x3, quad.y3}, {quad.x4, quad.y4}
        };
        DecorationLineBounds objectBounds;
        FS_MATRIX objectMatrix = {1, 0, 0, 1, 0, 0};
        if (!FPDFPageObj_GetMatrix(textObject, &objectMatrix)) continue;
        objectBounds.baselineNormal =
                objectMatrix.e * normalX + objectMatrix.f * normalY;
        for (const auto& point : points) {
            const float baselineProjection = point[0] * baselineX + point[1] * baselineY;
            const float normalProjection = point[0] * normalX + point[1] * normalY;
            objectBounds.minBaseline = fminf(objectBounds.minBaseline, baselineProjection);
            objectBounds.maxBaseline = fmaxf(objectBounds.maxBaseline, baselineProjection);
            objectBounds.minNormal = fminf(objectBounds.minNormal, normalProjection);
            objectBounds.maxNormal = fmaxf(objectBounds.maxNormal, normalProjection);
        }
        auto matchingLine = std::find_if(
                lineBounds.begin(),
                lineBounds.end(),
                [&](const DecorationLineBounds& candidate) {
                    // All glyph objects created for one visual line share the same baseline
                    // matrix even though their glyph bounds differ (for example, i versus g).
                    // Use a height-relative tolerance because transformed/embedded fonts can
                    // report slightly different baseline origins for adjacent glyph objects.
                    const float candidateHeight = candidate.maxNormal - candidate.minNormal;
                    const float objectHeight = objectBounds.maxNormal - objectBounds.minNormal;
                    const float baselineTolerance = fmaxf(
                            0.5f,
                            fminf(candidateHeight, objectHeight) * 0.25f);
                    return fabsf(candidate.baselineNormal - objectBounds.baselineNormal) <=
                            baselineTolerance;
                });
        if (matchingLine == lineBounds.end()) {
            lineBounds.push_back(objectBounds);
        } else {
            matchingLine->minBaseline = fminf(
                    matchingLine->minBaseline,
                    objectBounds.minBaseline);
            matchingLine->maxBaseline = fmaxf(
                    matchingLine->maxBaseline,
                    objectBounds.maxBaseline);
            matchingLine->minNormal = fminf(matchingLine->minNormal, objectBounds.minNormal);
            matchingLine->maxNormal = fmaxf(matchingLine->maxNormal, objectBounds.maxNormal);
        }
    }

    auto appendLine = [&](const DecorationLineBounds& bounds, float normalRatio,
                          const char* suffix, int lineOrdinal) {
        const float textHeight = fmaxf(bounds.maxNormal - bounds.minNormal, 1.0f);
        if (bounds.maxBaseline - bounds.minBaseline <= 0.01f) return;
        const float lineNormal = bounds.minNormal + textHeight * normalRatio;
        const float startX = baselineX * bounds.minBaseline + normalX * lineNormal;
        const float startY = baselineY * bounds.minBaseline + normalY * lineNormal;
        const float endX = baselineX * bounds.maxBaseline + normalX * lineNormal;
        const float endY = baselineY * bounds.maxBaseline + normalY * lineNormal;
        FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(startX, startY);
        if (!line) return;
        FPDFPath_LineTo(line, endX, endY);
        FPDFPageObj_SetStrokeColor(line, r, g, b, a);
        FPDFPageObj_SetStrokeWidth(line, fmaxf(textHeight * 0.055f, 0.35f));
        FPDFPath_SetDrawMode(line, 0, JNI_TRUE);
        FPDFPageObj_AddMark(
                line,
                (markPrefix + suffix + "_" + std::to_string(objectOrdinal) + "_" +
                 std::to_string(lineOrdinal)).c_str());
        FPDFPage_InsertObject(page, line);
    };
    for (size_t lineIndex = 0; lineIndex < lineBounds.size(); ++lineIndex) {
        if (underline) appendLine(lineBounds[lineIndex], 0.08f, "underline",
                                  static_cast<int>(lineIndex));
        if (strikeout) appendLine(lineBounds[lineIndex], 0.52f, "strikeout",
                                  static_cast<int>(lineIndex));
    }
}

static bool TextEditNeedsCompatibleFont(JNIEnv* env, jstring originalText, jstring replacementText) {
    if (!env || !replacementText) return false;
    const jsize replacementLength = env->GetStringLength(replacementText);
    const jsize originalLength = originalText ? env->GetStringLength(originalText) : 0;
    const jchar* replacementChars = env->GetStringChars(replacementText, nullptr);
    const jchar* originalChars = originalText ? env->GetStringChars(originalText, nullptr) : nullptr;
    bool needsCompatibleFont = false;
    for (jsize index = 0; replacementChars && index < replacementLength; ++index) {
        const uint16_t ch = replacementChars[index];
        const bool complexScript =
                (ch >= 0x0590 && ch <= 0x08FF) ||
                (ch >= 0x0900 && ch <= 0x0D7F) ||
                (ch >= 0x0E00 && ch <= 0x0E7F) ||
                (ch >= 0x2E80 && ch <= 0x9FFF) ||
                (ch >= 0xAC00 && ch <= 0xD7AF);
        const bool existedInOriginal = originalChars &&
                std::find(originalChars, originalChars + originalLength, ch) != originalChars + originalLength;
        if (complexScript || !existedInOriginal) {
            needsCompatibleFont = true;
            break;
        }
    }
    if (originalChars) env->ReleaseStringChars(originalText, originalChars);
    if (replacementChars) env->ReleaseStringChars(replacementText, replacementChars);
    return needsCompatibleFont;
}

static void CopyTextEditObjectAppearance(
        FPDF_PAGEOBJECT sourceObject,
        FPDF_PAGEOBJECT replacementObject) {
    if (!sourceObject || !replacementObject) return;
    FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
    if (FPDFPageObj_GetMatrix(sourceObject, &matrix)) {
        FPDFPageObj_SetMatrix(replacementObject, &matrix);
    }
    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (FPDFPageObj_GetFillColor(sourceObject, &r, &g, &b, &a)) {
        FPDFPageObj_SetFillColor(replacementObject, r, g, b, a);
    }
    if (FPDFPageObj_GetStrokeColor(sourceObject, &r, &g, &b, &a)) {
        FPDFPageObj_SetStrokeColor(replacementObject, r, g, b, a);
    }
    float strokeWidth = 0.0f;
    if (FPDFPageObj_GetStrokeWidth(sourceObject, &strokeWidth)) {
        FPDFPageObj_SetStrokeWidth(replacementObject, strokeWidth);
    }
    FPDFTextObj_SetTextRenderMode(
            replacementObject,
            FPDFTextObj_GetTextRenderMode(sourceObject));
}

static float MeasureTextEditRunAdvance(
        FPDF_DOCUMENT doc,
        FPDF_FONT font,
        float fontSize,
        const std::u16string& text) {
    if (!doc || !font || text.empty()) return 0.0f;
    char16_t sentinel = u'M';
    for (char16_t value : text) {
        if (value != u' ' && value != u'\t' && value != u'\r' && value != u'\n') {
            sentinel = value;
            break;
        }
    }

    auto measureBoundsWidth = [&](const std::u16string& value) -> float {
        FPDF_PAGEOBJECT measurementObject = FPDFPageObj_CreateTextObj(doc, font, fontSize);
        if (!measurementObject) return 0.0f;
        std::vector<unsigned short> utf16(value.begin(), value.end());
        utf16.push_back(0);
        float width = 0.0f;
        if (FPDFText_SetText(
                measurementObject,
                reinterpret_cast<FPDF_WIDESTRING>(utf16.data()))) {
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
            if (FPDFPageObj_GetBounds(
                    measurementObject,
                    &left,
                    &bottom,
                    &right,
                    &top)) {
                width = fmaxf(0.0f, right - left);
            }
        }
        FPDFPageObj_Destroy(measurementObject);
        return width;
    };

    std::u16string surroundedText;
    surroundedText.reserve(text.size() + 2);
    surroundedText.push_back(sentinel);
    surroundedText.append(text);
    surroundedText.push_back(sentinel);
    const std::u16string sentinelPair(2, sentinel);
    const float surroundedWidth = measureBoundsWidth(surroundedText);
    const float sentinelPairWidth = measureBoundsWidth(sentinelPair);
    return fmaxf(0.0f, surroundedWidth - sentinelPairWidth);
}

static FPDF_FONT LoadTextEditFallbackFont(
        FPDF_DOCUMENT doc,
        const char* fontPath,
        bool useCache) {
    if (!doc || !fontPath || strlen(fontPath) == 0) return nullptr;
    if (useCache) {
        Mutex::Autolock lock(sTextEditFontCacheLock);
        const auto documentEntry = sTextEditFontCache.find(doc);
        if (documentEntry != sTextEditFontCache.end()) {
            const auto fontEntry = documentEntry->second.find(fontPath);
            if (fontEntry != documentEntry->second.end()) return fontEntry->second;
        }
    }

    FILE* fontFile = fopen(fontPath, "rb");
    if (!fontFile) return nullptr;
    fseek(fontFile, 0, SEEK_END);
    const long fontSizeBytes = ftell(fontFile);
    rewind(fontFile);
    if (fontSizeBytes <= 0) {
        fclose(fontFile);
        return nullptr;
    }
    std::vector<uint8_t> fontBytes(static_cast<size_t>(fontSizeBytes));
    const size_t bytesRead = fread(fontBytes.data(), 1, fontBytes.size(), fontFile);
    fclose(fontFile);
    if (bytesRead != fontBytes.size()) return nullptr;
    const bool isType1 = (fontBytes.size() >= 2 &&
                          ((fontBytes[0] == 0x80 && fontBytes[1] == 0x01) ||
                           (fontBytes[0] == '%' && fontBytes[1] == '!')));
    FPDF_FONT font = FPDFText_LoadFont(
            doc,
            fontBytes.data(),
            static_cast<uint32_t>(fontBytes.size()),
            isType1 ? FPDF_FONT_TYPE1 : FPDF_FONT_TRUETYPE,
            true);
    if (font && useCache) {
        Mutex::Autolock lock(sTextEditFontCacheLock);
        sTextEditFontCache[doc][fontPath] = font;
    }
    return font;
}

static FPDF_PAGEOBJECT CreateTextEditRunObject(
        FPDF_DOCUMENT doc,
        FPDF_PAGEOBJECT sourceObject,
        FPDF_FONT font,
        const std::u16string& text,
        float advance,
        bool requestedBold,
        bool requestedItalic,
        float* outWidth,
        float fontSizeScale = 1.0f,
        bool exactSourceFont = false) {
    if (!doc || !sourceObject || !font || text.empty()) return nullptr;
    float sourceFontSize = 12.0f;
    FPDFTextObj_GetFontSize(sourceObject, &sourceFontSize);
    const float runFontSize = sourceFontSize *
            (std::isfinite(fontSizeScale)
             ? std::max(0.25f, std::min(4.0f, fontSizeScale))
             : 1.0f);
    FPDF_PAGEOBJECT runObject = FPDFPageObj_CreateTextObj(doc, font, runFontSize);
    if (!runObject) return nullptr;
    std::vector<unsigned short> utf16(text.begin(), text.end());
    utf16.push_back(0);
    if (!FPDFText_SetText(runObject, reinterpret_cast<FPDF_WIDESTRING>(utf16.data()))) {
        FPDFPageObj_Destroy(runObject);
        return nullptr;
    }
    float width = MeasureTextEditRunAdvance(doc, font, runFontSize, text);
    if (width <= 0.0f) {
        float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
        width = FPDFPageObj_GetBounds(runObject, &left, &bottom, &right, &top)
                ? fmaxf(0.0f, right - left)
                : 0.0f;
    }
    CopyTextEditObjectAppearance(sourceObject, runObject);
    FPDF_FONT sourceFont = FPDFTextObj_GetFont(sourceObject);
    const bool usesFallbackFont = !exactSourceFont && font != sourceFont;
    FPDF_FONT appearanceFont = exactSourceFont ? font : sourceFont;
    const int sourceFontWeight = FPDFFont_GetWeight(appearanceFont);
    int sourceItalicAngle = 0;
    FPDFFont_GetItalicAngle(appearanceFont, &sourceItalicAngle);
    std::string sourceFontName;
    const size_t sourceFontNameLength = FPDFFont_GetBaseFontName(appearanceFont, nullptr, 0);
    if (sourceFontNameLength > 0) {
        std::vector<char> nameBuffer(sourceFontNameLength);
        if (FPDFFont_GetBaseFontName(appearanceFont, nameBuffer.data(), sourceFontNameLength) > 0) {
            sourceFontName.assign(nameBuffer.data());
            std::transform(
                    sourceFontName.begin(),
                    sourceFontName.end(),
                    sourceFontName.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        }
    }
    const bool sourceIsBold = sourceFontWeight >= 600 ||
            sourceFontName.find("bold") != std::string::npos ||
            sourceFontName.find("black") != std::string::npos ||
            sourceFontName.find("semibold") != std::string::npos ||
            sourceFontName.find("demibold") != std::string::npos ||
            sourceFontName.find("medi") != std::string::npos;
    const bool sourceIsItalic = sourceItalicAngle != 0 ||
            sourceFontName.find("italic") != std::string::npos ||
            sourceFontName.find("oblique") != std::string::npos ||
            sourceFontName.find("ital") != std::string::npos ||
            sourceFontName.find("slant") != std::string::npos;
    if (requestedBold && (!sourceIsBold || usesFallbackFont)) {
        unsigned int r = 0, g = 0, b = 0, a = 255;
        FPDFPageObj_GetFillColor(sourceObject, &r, &g, &b, &a);
        FPDFPageObj_SetStrokeColor(runObject, r, g, b, a);
        FPDFTextObj_SetTextRenderMode(runObject, FPDF_TEXTRENDERMODE_FILL_STROKE);
        FPDFPageObj_SetStrokeWidth(runObject, fmax(runFontSize * 0.025f, 0.2f));
    }
    FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
    if (FPDFPageObj_GetMatrix(sourceObject, &matrix)) {
        if (requestedItalic && (!sourceIsItalic || usesFallbackFont)) {
            const float italicShear = tanf(
                    (sourceIsItalic ? fabsf(static_cast<float>(sourceItalicAngle)) : 12.0f) *
                    static_cast<float>(M_PI) / 180.0f);
            matrix.c += matrix.a * italicShear;
            matrix.d += matrix.b * italicShear;
        }
        matrix.e += matrix.a * advance;
        matrix.f += matrix.b * advance;
        FPDFPageObj_SetMatrix(runObject, &matrix);
    }
    if (outWidth) *outWidth = width;
    return runObject;
}

static bool CreateSegmentedCompatibleTextEditObjects(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_PAGEOBJECT sourceObject,
        jstring originalText,
        jstring replacementText,
        const char* fontPath,
        bool cacheFallbackFont,
        const std::string& markPrefix,
        std::vector<FPDF_PAGEOBJECT>* outObjects) {
    if (!env || !doc || !page || !sourceObject || !originalText || !replacementText ||
        !fontPath || strlen(fontPath) == 0 || !outObjects) return false;

    const jsize originalLength = env->GetStringLength(originalText);
    const jsize replacementLength = env->GetStringLength(replacementText);
    const jchar* originalChars = env->GetStringChars(originalText, nullptr);
    const jchar* replacementChars = env->GetStringChars(replacementText, nullptr);
    if (!originalChars || !replacementChars) {
        if (originalChars) env->ReleaseStringChars(originalText, originalChars);
        if (replacementChars) env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    size_t prefixLength = 0;
    while (prefixLength < static_cast<size_t>(originalLength) &&
           prefixLength < static_cast<size_t>(replacementLength) &&
           originalChars[prefixLength] == replacementChars[prefixLength]) {
        ++prefixLength;
    }
    size_t suffixLength = 0;
    while (suffixLength + prefixLength < static_cast<size_t>(originalLength) &&
           suffixLength + prefixLength < static_cast<size_t>(replacementLength) &&
           originalChars[originalLength - 1 - suffixLength] ==
                   replacementChars[replacementLength - 1 - suffixLength]) {
        ++suffixLength;
    }
    const std::u16string prefix(
            reinterpret_cast<const char16_t*>(originalChars),
            prefixLength);
    const std::u16string changed(
            reinterpret_cast<const char16_t*>(replacementChars + prefixLength),
            static_cast<size_t>(replacementLength) - prefixLength - suffixLength);
    const std::u16string suffix(
            reinterpret_cast<const char16_t*>(originalChars + originalLength - suffixLength),
            suffixLength);
    env->ReleaseStringChars(originalText, originalChars);
    env->ReleaseStringChars(replacementText, replacementChars);

    FPDF_FONT fallbackFont = LoadTextEditFallbackFont(doc, fontPath, cacheFallbackFont);
    FPDF_FONT originalFont = FPDFTextObj_GetFont(sourceObject);
    if (!fallbackFont || !originalFont) return false;
    const int sourceFontWeight = FPDFFont_GetWeight(originalFont);
    int sourceItalicAngle = 0;
    FPDFFont_GetItalicAngle(originalFont, &sourceItalicAngle);
    const bool sourceBold = sourceFontWeight >= 600;
    const bool sourceItalic = sourceItalicAngle != 0;

    float advance = 0.0f;
    auto appendRun = [&](FPDF_FONT font, const std::u16string& text, const char* suffixName) -> bool {
        if (text.empty()) return true;
        float runWidth = 0.0f;
        FPDF_PAGEOBJECT runObject = CreateTextEditRunObject(
                doc,
                sourceObject,
                font,
                text,
                advance,
                sourceBold,
                sourceItalic,
                &runWidth);
        if (!runObject) return false;
        FPDFPageObj_AddMark(runObject, (markPrefix + suffixName).c_str());
        FPDFPage_InsertObject(page, runObject);
        outObjects->push_back(runObject);
        advance += runWidth;
        return true;
    };

    if (!appendRun(originalFont, prefix, "prefix") ||
        !appendRun(fallbackFont, changed, "changed") ||
        !appendRun(originalFont, suffix, "suffix")) {
        for (FPDF_PAGEOBJECT object : *outObjects) {
            if (FPDFPage_RemoveObject(page, object)) FPDFPageObj_Destroy(object);
        }
        outObjects->clear();
        return false;
    }
    return !outObjects->empty();
}

static bool CreatePositionedSourceTextEditObjects(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_PAGEOBJECT sourceObject,
        jstring originalText,
        jstring replacementText,
        const char* encodedAdvances,
        const std::string& markPrefix,
        std::vector<FPDF_PAGEOBJECT>* outObjects) {
    if (!env || !doc || !page || !sourceObject || !originalText || !replacementText ||
        !encodedAdvances || !*encodedAdvances || !outObjects) return false;
    const jsize originalLength = env->GetStringLength(originalText);
    const jsize replacementLength = env->GetStringLength(replacementText);
    const jchar* originalChars = env->GetStringChars(originalText, nullptr);
    const jchar* replacementChars = env->GetStringChars(replacementText, nullptr);
    if (!originalChars || !replacementChars) {
        if (originalChars) env->ReleaseStringChars(originalText, originalChars);
        if (replacementChars) env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    std::vector<float> positions;
    std::stringstream positionStream(encodedAdvances);
    std::string positionValue;
    while (std::getline(positionStream, positionValue, ',')) {
        char* end = nullptr;
        const float value = strtof(positionValue.c_str(), &end);
        if (!end || end == positionValue.c_str() || !std::isfinite(value)) {
            positions.clear();
            break;
        }
        positions.push_back(value);
    }
    if (positions.size() != static_cast<size_t>(originalLength) + 1) {
        env->ReleaseStringChars(originalText, originalChars);
        env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    size_t prefixLength = 0;
    while (prefixLength < static_cast<size_t>(originalLength) &&
           prefixLength < static_cast<size_t>(replacementLength) &&
           originalChars[prefixLength] == replacementChars[prefixLength]) ++prefixLength;
    size_t suffixLength = 0;
    while (suffixLength + prefixLength < static_cast<size_t>(originalLength) &&
           suffixLength + prefixLength < static_cast<size_t>(replacementLength) &&
           originalChars[originalLength - 1 - suffixLength] ==
                   replacementChars[replacementLength - 1 - suffixLength]) ++suffixLength;
    size_t originalSuffixStart = static_cast<size_t>(originalLength) - suffixLength;
    size_t replacementChangeEnd = static_cast<size_t>(replacementLength) - suffixLength;
    auto isWordSeparator = [](jchar value) {
        return value == u' ' || value == u'\t' || value == u'\r' || value == u'\n';
    };
    // Rebuild the touched word as one run. Splitting an edited word at the exact diff
    // boundary loses the source font's kerning between its unchanged and changed pieces.
    size_t wordStart = prefixLength;
    while (wordStart > 0 && !isWordSeparator(originalChars[wordStart - 1])) --wordStart;
    size_t wordEnd = originalSuffixStart;
    while (wordEnd < static_cast<size_t>(originalLength) &&
           !isWordSeparator(originalChars[wordEnd])) ++wordEnd;
    replacementChangeEnd += wordEnd - originalSuffixStart;
    prefixLength = wordStart;
    originalSuffixStart = wordEnd;
    FPDF_FONT sourceFont = FPDFTextObj_GetFont(sourceObject);
    if (!sourceFont) {
        env->ReleaseStringChars(originalText, originalChars);
        env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    const int sourceWeight = FPDFFont_GetWeight(sourceFont);
    int sourceItalicAngle = 0;
    FPDFFont_GetItalicAngle(sourceFont, &sourceItalicAngle);
    const float baseAdvance = positions.front();
    int ordinal = 0;
    auto appendRun = [&](const std::u16string& text, float advance, const char* name,
                         float* widthOut) -> bool {
        if (text.empty()) return true;
        float width = 0.0f;
        FPDF_PAGEOBJECT object = CreateTextEditRunObject(
                doc, sourceObject, sourceFont, text, advance,
                sourceWeight >= 600, sourceItalicAngle != 0, &width);
        if (!object) return false;
        FPDFPageObj_AddMark(
                object,
                (markPrefix + name + "_" + std::to_string(ordinal++)).c_str());
        FPDFPage_InsertObject(page, object);
        outObjects->push_back(object);
        if (widthOut) *widthOut = width;
        return true;
    };
    auto appendOriginalGlyphs = [&](size_t start, size_t end, float offset) -> bool {
        size_t index = start;
        while (index < end) {
            if (isWordSeparator(originalChars[index])) {
                ++index;
                continue;
            }
            const size_t glyphStart = index;
            size_t glyphLength = 1;
            if (originalChars[index] >= 0xD800 && originalChars[index] <= 0xDBFF &&
                index + 1 < end && originalChars[index + 1] >= 0xDC00 &&
                originalChars[index + 1] <= 0xDFFF) {
                glyphLength = 2;
            }
            const std::u16string glyph(
                    reinterpret_cast<const char16_t*>(originalChars + glyphStart),
                    glyphLength);
            if (!appendRun(
                    glyph,
                    positions[glyphStart] - baseAdvance + offset,
                    "source",
                    nullptr)) {
                return false;
            }
            index += glyphLength;
        }
        return true;
    };
    bool success = appendOriginalGlyphs(0, prefixLength, 0.0f);
    float replacementWidth = 0.0f;
    if (success && replacementChangeEnd > prefixLength) {
        const std::u16string changed(
                reinterpret_cast<const char16_t*>(replacementChars + prefixLength),
                replacementChangeEnd - prefixLength);
        success = appendRun(
                changed,
                positions[prefixLength] - baseAdvance,
                "changed",
                &replacementWidth);
    }
    const float originalChangedWidth =
            positions[originalSuffixStart] - positions[prefixLength];
    const float suffixOffset = replacementWidth - originalChangedWidth;
    if (success) {
        success = appendOriginalGlyphs(
                originalSuffixStart,
                static_cast<size_t>(originalLength),
                suffixOffset);
    }
    env->ReleaseStringChars(originalText, originalChars);
    env->ReleaseStringChars(replacementText, replacementChars);
    if (!success || outObjects->empty()) {
        for (FPDF_PAGEOBJECT object : *outObjects) {
            if (FPDFPage_RemoveObject(page, object)) FPDFPageObj_Destroy(object);
        }
        outObjects->clear();
        return false;
    }
    return true;
}

static void CollectEditableTextObjectsFromObject(
        FPDF_PAGEOBJECT object,
        std::vector<FPDF_PAGEOBJECT>* textObjects) {
    if (!object || !textObjects) return;
    const int objectType = FPDFPageObj_GetType(object);
    if (objectType == FPDF_PAGEOBJ_TEXT) {
        if (!PageObjectHasExactMark(object, "LufickStyleOverlay") &&
            !PageObjectHasMarkPrefix(object, "LufickTextEditPreview_") &&
            !PageObjectHasMarkPrefix(object, "LufickTextEditFinal_")) {
            textObjects->push_back(object);
        }
        return;
    }
    if (objectType != FPDF_PAGEOBJ_FORM) return;
    const int childCount = FPDFFormObj_CountObjects(object);
    for (int childIndex = 0; childIndex < childCount; ++childIndex) {
        CollectEditableTextObjectsFromObject(
                FPDFFormObj_GetObject(object, static_cast<unsigned long>(childIndex)),
                textObjects);
    }
}

static std::vector<FPDF_PAGEOBJECT> CollectEditableTextObjects(FPDF_PAGE page) {
    std::vector<FPDF_PAGEOBJECT> textObjects;
    if (!page) return textObjects;
    const int objectCount = FPDFPage_CountObjects(page);
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        CollectEditableTextObjectsFromObject(
                FPDFPage_GetObject(page, objectIndex),
                &textObjects);
    }
    return textObjects;
}

static std::string TextEditFontName(FPDF_FONT font) {
    if (!font) return "";
    const size_t length = FPDFFont_GetBaseFontName(font, nullptr, 0);
    if (length == 0) return "";
    std::vector<char> buffer(length);
    if (FPDFFont_GetBaseFontName(font, buffer.data(), length) == 0) return "";
    std::string name(buffer.data());
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return name;
}

static std::string TextEditFontFamily(FPDF_FONT font) {
    std::string name = TextEditFontName(font);
    const size_t subset = name.find('+');
    if (subset != std::string::npos) name = name.substr(subset + 1);
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char value) {
        return !std::isalnum(value);
    }), name.end());
    static const char* suffixes[] = {
            "semibolditalic", "demibolditalic", "mediumitalic", "bolditalic",
            "semiboldoblique", "demiboldoblique", "boldoblique", "mediumoblique",
            "reguital", "mediital", "regularitalic", "regularoblique",
            "semibold", "demibold", "medium", "bold", "black",
            "regular", "roman", "regu", "medi", "italic", "oblique", "ital", "slant"
    };
    bool removed = true;
    while (removed) {
        removed = false;
        for (const char* suffix : suffixes) {
            const size_t suffixLength = strlen(suffix);
            if (name.size() > suffixLength &&
                name.compare(name.size() - suffixLength, suffixLength, suffix) == 0) {
                name.erase(name.size() - suffixLength);
                removed = true;
                break;
            }
        }
    }
    return name;
}

static bool TextEditFontIsBold(FPDF_FONT font) {
    const std::string name = TextEditFontName(font);
    return FPDFFont_GetWeight(font) >= 600 ||
           name.find("bold") != std::string::npos ||
           name.find("black") != std::string::npos ||
           name.find("semibold") != std::string::npos ||
           name.find("demibold") != std::string::npos ||
           name.find("medi") != std::string::npos;
}

static bool TextEditFontIsItalic(FPDF_FONT font) {
    int angle = 0;
    FPDFFont_GetItalicAngle(font, &angle);
    const std::string name = TextEditFontName(font);
    return angle != 0 || name.find("italic") != std::string::npos ||
           name.find("oblique") != std::string::npos ||
           name.find("ital") != std::string::npos ||
           name.find("slant") != std::string::npos;
}

static FPDF_FONT FindTextEditEmbeddedStyleFont(
        FPDF_PAGE page,
        FPDF_FONT sourceFont,
        bool bold,
        bool italic) {
    if (!page || !sourceFont) return sourceFont;
    if (TextEditFontIsBold(sourceFont) == bold && TextEditFontIsItalic(sourceFont) == italic) {
        return sourceFont;
    }
    const std::string family = TextEditFontFamily(sourceFont);
    if (family.empty()) return sourceFont;
    for (FPDF_PAGEOBJECT object : CollectEditableTextObjects(page)) {
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT) continue;
        FPDF_FONT candidate = FPDFTextObj_GetFont(object);
        if (candidate && TextEditFontFamily(candidate) == family &&
            TextEditFontIsBold(candidate) == bold &&
            TextEditFontIsItalic(candidate) == italic) {
            return candidate;
        }
    }
    return sourceFont;
}

struct TextEditCharacterStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikeout = false;
    bool hasColor = false;
    uint32_t color = 0xFF000000u;
    float sizeScale = 1.0f;

    TextEditCharacterStyle() = default;
    TextEditCharacterStyle(bool boldValue, bool italicValue, bool underlineValue, bool strikeoutValue,
                           bool hasColorValue = false, uint32_t colorValue = 0xFF000000u,
                           float sizeScaleValue = 1.0f)
            : bold(boldValue),
              italic(italicValue),
              underline(underlineValue),
              strikeout(strikeoutValue),
              hasColor(hasColorValue),
              color(colorValue), sizeScale(sizeScaleValue) {}

    bool operator==(const TextEditCharacterStyle& other) const {
        return bold == other.bold && italic == other.italic &&
               underline == other.underline && strikeout == other.strikeout &&
               hasColor == other.hasColor && (!hasColor || color == other.color) &&
               sizeScale == other.sizeScale;
    }
};

struct StyledTextEditObject {
    FPDF_PAGEOBJECT object = nullptr;
    TextEditCharacterStyle style;

    StyledTextEditObject(FPDF_PAGEOBJECT objectValue, const TextEditCharacterStyle& styleValue)
            : object(objectValue), style(styleValue) {}
};

static std::vector<TextEditCharacterStyle> ParseTextEditCharacterStyles(
        const char* encodedStyles,
        size_t textLength,
        const TextEditCharacterStyle& fallbackStyle) {
    std::vector<TextEditCharacterStyle> styles(textLength, fallbackStyle);
    if (!encodedStyles || strlen(encodedStyles) == 0) return styles;
    std::stringstream stream(encodedStyles);
    std::string encodedRun;
    while (std::getline(stream, encodedRun, ';')) {
        int start = 0, end = 0, bold = 0, italic = 0, underline = 0, strikeout = 0;
        long long color = static_cast<long long>(INT_MIN);
        float sizeScale = 1.0f;
        const int parsed = sscanf(
                encodedRun.c_str(),
                "%d,%d,%d,%d,%d,%d,%lld,%f",
                &start,
                &end,
                &bold,
                &italic,
                &underline,
                &strikeout,
                &color,
                &sizeScale);
        if (parsed < 6) continue;
        const int safeStart = std::max(0, std::min(start, static_cast<int>(textLength)));
        const int safeEnd = std::max(safeStart, std::min(end, static_cast<int>(textLength)));
        const TextEditCharacterStyle style = {
                bold != 0,
                italic != 0,
                underline != 0,
                strikeout != 0,
                parsed >= 7 && color != static_cast<long long>(INT_MIN),
                static_cast<uint32_t>(color),
                std::isfinite(sizeScale) ? std::max(0.25f, std::min(4.0f, sizeScale)) : 1.0f
        };
        for (int index = safeStart; index < safeEnd; ++index) styles[index] = style;
    }
    return styles;
}

static bool CreateStyledTextEditObjects(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_PAGEOBJECT sourceObject,
        jstring originalText,
        jstring replacementText,
        const char* fallbackFontPath,
        bool useCompatibleFont,
        bool replaceUnchangedFont,
        int replacementFontRangeStart,
        int replacementFontRangeEnd,
        const char* encodedVisualLineEnds,
        const char* encodedVisualLineOffsets,
        const char* encodedVisualLineYOffsets,
        float visualLineHeight,
        bool cacheFallbackFont,
        bool preserveOriginalGlyphAdvances,
        const char* encodedStyles,
        const char* encodedFontRuns,
        const char* encodedAdvances,
        const std::string& markPrefix,
        std::vector<StyledTextEditObject>* outObjects) {
    if (!env || !doc || !page || !sourceObject || !replacementText || !outObjects) return false;
    const jsize replacementLength = env->GetStringLength(replacementText);
    const jchar* replacementChars = env->GetStringChars(replacementText, nullptr);
    if (!replacementChars || replacementLength <= 0) {
        if (replacementChars) env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    const jsize originalLength = originalText ? env->GetStringLength(originalText) : 0;
    const jchar* originalChars = originalText ? env->GetStringChars(originalText, nullptr) : nullptr;
    size_t prefixLength = 0;
    while (originalChars && prefixLength < static_cast<size_t>(originalLength) &&
           prefixLength < static_cast<size_t>(replacementLength) &&
           originalChars[prefixLength] == replacementChars[prefixLength]) {
        ++prefixLength;
    }
    size_t suffixLength = 0;
    while (originalChars && suffixLength + prefixLength < static_cast<size_t>(originalLength) &&
           suffixLength + prefixLength < static_cast<size_t>(replacementLength) &&
           originalChars[originalLength - 1 - suffixLength] ==
                   replacementChars[replacementLength - 1 - suffixLength]) {
        ++suffixLength;
    }
    FPDF_FONT originalFont = FPDFTextObj_GetFont(sourceObject);
    if (!originalFont) {
        if (originalChars) env->ReleaseStringChars(originalText, originalChars);
        env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    FPDF_FONT fallbackFont = nullptr;
    if (useCompatibleFont) {
        if ((!fallbackFontPath || strlen(fallbackFontPath) == 0) &&
            (!encodedFontRuns || strlen(encodedFontRuns) == 0)) {
            if (originalChars) env->ReleaseStringChars(originalText, originalChars);
            env->ReleaseStringChars(replacementText, replacementChars);
            return false;
        }
        if (fallbackFontPath && strlen(fallbackFontPath) > 0) {
            fallbackFont = LoadTextEditFallbackFont(doc, fallbackFontPath, cacheFallbackFont);
        }
        if (!fallbackFont && (!encodedFontRuns || strlen(encodedFontRuns) == 0)) {
            if (originalChars) env->ReleaseStringChars(originalText, originalChars);
            env->ReleaseStringChars(replacementText, replacementChars);
            return false;
        }
    }

    struct TextEditFontRun {
        size_t start;
        size_t end;
        FPDF_FONT font;
    };
    std::vector<TextEditFontRun> fontRuns;
    if (encodedFontRuns && *encodedFontRuns) {
        std::stringstream stream(encodedFontRuns);
        std::string encodedRun;
        while (std::getline(stream, encodedRun, ';')) {
            const size_t firstComma = encodedRun.find(',');
            const size_t secondComma = firstComma == std::string::npos
                    ? std::string::npos : encodedRun.find(',', firstComma + 1);
            if (firstComma == std::string::npos || secondComma == std::string::npos) continue;
            const std::string startToken = encodedRun.substr(0, firstComma);
            const std::string endToken = encodedRun.substr(
                    firstComma + 1,
                    secondComma - firstComma - 1);
            char* startEnd = nullptr;
            char* endEnd = nullptr;
            const long start = strtol(startToken.c_str(), &startEnd, 10);
            const long end = strtol(endToken.c_str(), &endEnd, 10);
            const std::string path = encodedRun.substr(secondComma + 1);
            if (!startEnd || *startEnd != '\0' || !endEnd || *endEnd != '\0' ||
                start < 0 || end <= start ||
                end > replacementLength || path.empty()) continue;
            FPDF_FONT runFont = LoadTextEditFallbackFont(doc, path.c_str(), cacheFallbackFont);
            if (!runFont) {
                if (originalChars) env->ReleaseStringChars(originalText, originalChars);
                env->ReleaseStringChars(replacementText, replacementChars);
                return false;
            }
            fontRuns.push_back({static_cast<size_t>(start), static_cast<size_t>(end), runFont});
        }
    }

    float sourceFontSize = 12.0f;
    FPDFTextObj_GetFontSize(sourceObject, &sourceFontSize);
    unsigned int sourceR = 0, sourceG = 0, sourceB = 0, sourceA = 255;
    FPDFPageObj_GetFillColor(sourceObject, &sourceR, &sourceG, &sourceB, &sourceA);
    const TextEditCharacterStyle sourceStyle = {
            TextEditFontIsBold(originalFont),
            TextEditFontIsItalic(originalFont),
            false,
            false,
            true,
            ((sourceA & 0xFFu) << 24u) | ((sourceR & 0xFFu) << 16u) |
                    ((sourceG & 0xFFu) << 8u) | (sourceB & 0xFFu)
    };
    const std::vector<TextEditCharacterStyle> styles = ParseTextEditCharacterStyles(
            encodedStyles,
            static_cast<size_t>(replacementLength),
            sourceStyle);
    auto isSourceFamilyFont = [&](FPDF_FONT font) -> bool {
        return font && font == originalFont;
    };
    std::vector<float> positions;
    if (encodedAdvances && *encodedAdvances) {
        std::stringstream positionStream(encodedAdvances);
        std::string value;
        while (std::getline(positionStream, value, ',')) {
            char* end = nullptr;
            const float position = strtof(value.c_str(), &end);
            if (!end || end == value.c_str() || !std::isfinite(position)) {
                positions.clear();
                break;
            }
            positions.push_back(position);
        }
    }
    if (!originalChars || positions.size() != static_cast<size_t>(originalLength) + 1) {
        if (originalChars) env->ReleaseStringChars(originalText, originalChars);
        env->ReleaseStringChars(replacementText, replacementChars);
        return false;
    }
    const size_t originalSuffixStart = static_cast<size_t>(originalLength) - suffixLength;
    const size_t changedEnd = static_cast<size_t>(replacementLength) - suffixLength;
    const float baseAdvance = positions.front();
    const bool hasReplacementFontRange = replacementFontRangeStart >= 0 &&
            replacementFontRangeEnd > replacementFontRangeStart;
    auto usesReplacementFontAt = [&](size_t index, bool isChangedText) -> bool {
        for (const TextEditFontRun& run : fontRuns) {
            if (index >= run.start && index < run.end) return true;
        }
        if (!useCompatibleFont) return false;
        if (!replaceUnchangedFont && !fontRuns.empty()) return false;
        if (!replaceUnchangedFont) return isChangedText;
        if (!hasReplacementFontRange) return true;
        return index >= static_cast<size_t>(replacementFontRangeStart) &&
               index < static_cast<size_t>(replacementFontRangeEnd);
    };
    auto fontAt = [&](size_t index, bool isChangedText) -> FPDF_FONT {
        for (const TextEditFontRun& run : fontRuns) {
            if (index >= run.start && index < run.end) return run.font;
        }
        if (usesReplacementFontAt(index, isChangedText)) return fallbackFont;
        const TextEditCharacterStyle& style = styles[index];
        return preserveOriginalGlyphAdvances
                ? originalFont
                : FindTextEditEmbeddedStyleFont(page, originalFont, style.bold, style.italic);
    };
    int runOrdinal = 0;
    auto isTextEditWhitespace = [](jchar value) {
        return value == u' ' || value == u'\t' || value == u'\r' || value == u'\n';
    };
    auto appendRun = [&](size_t runStart, size_t runEnd, FPDF_FONT font, float advance,
                         float lineOffset) -> float {
        if (runStart >= runEnd) return 0.0f;
        const TextEditCharacterStyle runStyle = styles[runStart];
        const std::u16string runText(
                reinterpret_cast<const char16_t*>(replacementChars + runStart),
                runEnd - runStart);
        float runWidth = 0.0f;
        FPDF_PAGEOBJECT runObject = CreateTextEditRunObject(
                doc,
                sourceObject,
                font,
                runText,
                advance,
                runStyle.bold,
                runStyle.italic,
                &runWidth,
                runStyle.sizeScale,
                isSourceFamilyFont(font));
        if (!runObject) {
            for (const StyledTextEditObject& created : *outObjects) {
                if (FPDFPage_RemoveObject(page, created.object)) FPDFPageObj_Destroy(created.object);
            }
            outObjects->clear();
            return -1.0f;
        }
        if (runStyle.hasColor) {
            const unsigned int runR = (runStyle.color >> 16u) & 0xFFu;
            const unsigned int runG = (runStyle.color >> 8u) & 0xFFu;
            const unsigned int runB = runStyle.color & 0xFFu;
            const unsigned int runA = (runStyle.color >> 24u) & 0xFFu;
            FPDFPageObj_SetFillColor(runObject, runR, runG, runB, runA);
            FPDFPageObj_SetStrokeColor(runObject, runR, runG, runB, runA);
        }
        if (std::isfinite(lineOffset) && lineOffset != 0.0f) {
            FS_MATRIX runMatrix = {1, 0, 0, 1, 0, 0};
            if (FPDFPageObj_GetMatrix(runObject, &runMatrix)) {
                runMatrix.e -= runMatrix.c * lineOffset;
                runMatrix.f -= runMatrix.d * lineOffset;
                FPDFPageObj_SetMatrix(runObject, &runMatrix);
            }
        }
        FPDFPageObj_AddMark(
                runObject,
                (markPrefix + "styled_" + std::to_string(runOrdinal++)).c_str());
        FPDFPage_InsertObject(page, runObject);
        outObjects->emplace_back(runObject, runStyle);
        return runWidth;
    };
    auto appendExactGlyphRange = [&](size_t replacementStart, size_t replacementEnd,
                                     size_t originalStart, float offset) -> bool {
        size_t replacementIndex = replacementStart;
        size_t originalIndex = originalStart;
        while (replacementIndex < replacementEnd) {
            if (!preserveOriginalGlyphAdvances) {
                if (isTextEditWhitespace(replacementChars[replacementIndex])) {
                    ++replacementIndex;
                    ++originalIndex;
                    continue;
                }
                const size_t chunkStart = replacementIndex;
                const size_t originalChunkStart = originalIndex;
                const TextEditCharacterStyle chunkStyle = styles[chunkStart];
                const FPDF_FONT chunkFont = fontAt(chunkStart, false);
                while (replacementIndex < replacementEnd &&
                       !isTextEditWhitespace(replacementChars[replacementIndex]) &&
                       styles[replacementIndex] == chunkStyle &&
                       fontAt(replacementIndex, false) == chunkFont) {
                    ++replacementIndex;
                    ++originalIndex;
                }
                if (appendRun(
                        chunkStart,
                        replacementIndex,
                        chunkFont,
                        positions[originalChunkStart] - baseAdvance + offset,
                        0.0f) < 0.0f) {
                    return false;
                }
                continue;
            }
            const jchar value = replacementChars[replacementIndex];
            size_t glyphLength = 1;
            if (value >= 0xD800 && value <= 0xDBFF &&
                replacementIndex + 1 < replacementEnd &&
                replacementChars[replacementIndex + 1] >= 0xDC00 &&
                replacementChars[replacementIndex + 1] <= 0xDFFF) {
                glyphLength = 2;
            }
            if (!isTextEditWhitespace(value)) {
                if (appendRun(
                        replacementIndex,
                        replacementIndex + glyphLength,
                        fontAt(replacementIndex, false),
                        positions[originalIndex] - baseAdvance + offset,
                        0.0f) < 0.0f) {
                    return false;
                }
            }
            replacementIndex += glyphLength;
            originalIndex += glyphLength;
        }
        return true;
    };

    std::vector<size_t> visualLineEnds;
    if (encodedVisualLineEnds && *encodedVisualLineEnds) {
        std::stringstream lineStream(encodedVisualLineEnds);
        std::string encodedEnd;
        size_t previousEnd = 0;
        while (std::getline(lineStream, encodedEnd, ',')) {
            char* parseEnd = nullptr;
            const long value = strtol(encodedEnd.c_str(), &parseEnd, 10);
            if (!parseEnd || parseEnd == encodedEnd.c_str() || value <= static_cast<long>(previousEnd) ||
                value > replacementLength) {
                visualLineEnds.clear();
                break;
            }
            previousEnd = static_cast<size_t>(value);
            visualLineEnds.push_back(previousEnd);
        }
        if (visualLineEnds.empty() || visualLineEnds.back() != static_cast<size_t>(replacementLength)) {
            visualLineEnds.clear();
        }
    }
    bool success = true;
    const bool textChanged = originalLength != replacementLength ||
            !std::equal(originalChars, originalChars + originalLength, replacementChars);
    if (visualLineEnds.size() > 1 || (encodedVisualLineYOffsets && *encodedVisualLineYOffsets)) {
        std::vector<float> lineOffsets;
        std::vector<float> lineYOffsets;
        if (encodedVisualLineYOffsets && *encodedVisualLineYOffsets) {
            std::stringstream stream(encodedVisualLineYOffsets);
            std::string value;
            while (std::getline(stream, value, ',')) {
                char* end = nullptr;
                const float offset = strtof(value.c_str(), &end);
                if (!end || end == value.c_str() || *end != '\0' || !std::isfinite(offset)) {
                    success = false;
                    break;
                }
                lineYOffsets.push_back(offset);
            }
            if (lineYOffsets.size() != visualLineEnds.size()) success = false;
            if (!success) LOGE("PDF_EDIT_NATIVE invalid paragraph baseline offsets");
        }
        if (encodedVisualLineOffsets && *encodedVisualLineOffsets) {
            std::stringstream offsetStream(encodedVisualLineOffsets);
            std::string value;
            while (std::getline(offsetStream, value, ',')) {
                char* end = nullptr;
                const float offset = strtof(value.c_str(), &end);
                if (!end || end == value.c_str() || *end != '\0' || !std::isfinite(offset)) {
                    success = false;
                    break;
                }
                lineOffsets.push_back(offset);
            }
            if (lineOffsets.size() != visualLineEnds.size()) success = false;
            if (!success) LOGE("PDF_EDIT_NATIVE invalid block reflow offsets");
        }
        size_t lineStart = 0;
        const float resolvedLineHeight = std::isfinite(visualLineHeight) && visualLineHeight != 0.0f
                ? visualLineHeight : sourceFontSize * 1.2f;
        for (size_t lineIndex = 0; success && lineIndex < visualLineEnds.size(); ++lineIndex) {
            const size_t lineEnd = visualLineEnds[lineIndex];
            float lineAdvance = lineOffsets.empty() ? 0.0f : lineOffsets[lineIndex];
            size_t runStart = lineStart;
            while (success && runStart < lineEnd) {
                const TextEditCharacterStyle runStyle = styles[runStart];
                const bool isChangedText = runStart >= prefixLength && runStart < changedEnd;
                const FPDF_FONT runFont = fontAt(runStart, isChangedText);
                const bool runUsesReplacementFont = !isSourceFamilyFont(runFont);
                size_t runEnd = runStart + 1;
                if (!runUsesReplacementFont && replacementChars[runStart] >= 0xD800 &&
                    replacementChars[runStart] <= 0xDBFF && runEnd < lineEnd &&
                    replacementChars[runEnd] >= 0xDC00 && replacementChars[runEnd] <= 0xDFFF) {
                    ++runEnd;
                }
                while (runEnd < lineEnd && styles[runEnd] == runStyle) {
                    const bool nextChanged = runEnd >= prefixLength && runEnd < changedEnd;
                    if (nextChanged != isChangedText ||
                        fontAt(runEnd, nextChanged) != runFont) break;
                    ++runEnd;
                }
                const float lineYOffset = lineYOffsets.empty()
                        ? static_cast<float>(lineIndex) * resolvedLineHeight
                        : lineYOffsets[lineIndex];
                if (!runUsesReplacementFont && !isChangedText) {
                    const size_t originalRunStart = runStart < prefixLength
                            ? runStart
                            : originalSuffixStart + (runStart - changedEnd);
                    const size_t originalRunEnd = originalRunStart + (runEnd - runStart);
                    if (originalRunEnd >= positions.size()) {
                        success = false;
                    } else {
                        size_t chunkStart = runStart;
                        while (success && chunkStart < runEnd) {
                            if (isTextEditWhitespace(replacementChars[chunkStart])) {
                                ++chunkStart;
                                continue;
                            }
                            size_t chunkEnd = chunkStart + 1;
                            while (chunkEnd < runEnd &&
                                   !isTextEditWhitespace(replacementChars[chunkEnd])) {
                                ++chunkEnd;
                            }
                            const size_t originalChunkStart = originalRunStart +
                                    (chunkStart - runStart);
                            const float chunkAdvance = lineAdvance +
                                    (positions[originalChunkStart] -
                                     positions[originalRunStart]) * runStyle.sizeScale;
                            if (appendRun(
                                    chunkStart,
                                    chunkEnd,
                                    runFont,
                                    chunkAdvance,
                                    lineYOffset) < 0.0f) {
                                success = false;
                            }
                            chunkStart = chunkEnd;
                        }
                        lineAdvance += (positions[originalRunEnd] -
                                positions[originalRunStart]) * runStyle.sizeScale;
                    }
                } else {
                    const float width = appendRun(
                            runStart,
                            runEnd,
                            runFont,
                            lineAdvance,
                            lineYOffset);
                    if (width < 0.0f) success = false;
                    else lineAdvance += width;
                }
                runStart = runEnd;
            }
            lineStart = lineEnd;
        }
    } else if (!textChanged && replaceUnchangedFont && hasReplacementFontRange) {
        const size_t fontChangeStart = std::min(
                static_cast<size_t>(replacementFontRangeStart),
                static_cast<size_t>(replacementLength));
        const size_t fontChangeEnd = std::min(
                static_cast<size_t>(replacementFontRangeEnd),
                static_cast<size_t>(replacementLength));
        success = appendExactGlyphRange(0, fontChangeStart, 0, 0.0f);
        float changedAdvance = positions[fontChangeStart] - baseAdvance;
        size_t runStart = fontChangeStart;
        while (success && runStart < fontChangeEnd) {
            const TextEditCharacterStyle runStyle = styles[runStart];
            size_t runEnd = runStart + 1;
            while (runEnd < fontChangeEnd && styles[runEnd] == runStyle) ++runEnd;
            const float width = appendRun(
                    runStart,
                    runEnd,
                    fallbackFont,
                    changedAdvance,
                    0.0f);
            if (width < 0.0f) success = false;
            else {
                changedAdvance += width;
                runStart = runEnd;
            }
        }
        const float originalChangedWidth =
                positions[fontChangeEnd] - positions[fontChangeStart];
        const float replacementChangedWidth =
                changedAdvance - (positions[fontChangeStart] - baseAdvance);
        const float suffixOffset = replacementChangedWidth - originalChangedWidth;
        if (success) {
            success = appendExactGlyphRange(
                    fontChangeEnd,
                    static_cast<size_t>(replacementLength),
                    fontChangeEnd,
                    suffixOffset);
        }
    } else if (!textChanged) {
        success = appendExactGlyphRange(
                0,
                static_cast<size_t>(replacementLength),
                0,
                0.0f);
    } else {
        success = appendExactGlyphRange(0, prefixLength, 0, 0.0f);
        float changedAdvance = positions[prefixLength] - baseAdvance;
        size_t runStart = prefixLength;
        while (success && runStart < changedEnd) {
            const TextEditCharacterStyle runStyle = styles[runStart];
            const FPDF_FONT runFont = fontAt(runStart, true);
            size_t runEnd = runStart + 1;
            while (runEnd < changedEnd && styles[runEnd] == runStyle &&
                   fontAt(runEnd, true) == runFont) ++runEnd;
            const float width = appendRun(
                    runStart,
                    runEnd,
                    runFont,
                    changedAdvance,
                    0.0f);
            if (width < 0.0f) {
                success = false;
            } else {
                changedAdvance += width;
                runStart = runEnd;
            }
        }
        const float originalChangedWidth =
                positions[originalSuffixStart] - positions[prefixLength];
        const float replacementChangedWidth =
                changedAdvance - (positions[prefixLength] - baseAdvance);
        const float suffixOffset = replacementChangedWidth - originalChangedWidth;
        if (success) {
            success = appendExactGlyphRange(
                    changedEnd,
                    static_cast<size_t>(replacementLength),
                    originalSuffixStart,
                    suffixOffset);
        }
    }
    if (!success) {
        for (const StyledTextEditObject& created : *outObjects) {
            if (FPDFPage_RemoveObject(page, created.object)) FPDFPageObj_Destroy(created.object);
        }
        outObjects->clear();
    }
    env->ReleaseStringChars(originalText, originalChars);
    env->ReleaseStringChars(replacementText, replacementChars);
    return success && !outObjects->empty();
}


static bool ApplyNativeTextContentEdits(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        jobjectArray textEditsArray,
        FPDF_PAGE providedPage,
        int providedPageIndex,
        bool validateOriginalText,
        bool cacheFallbackFonts = false) {
    if (!textEditsArray) return true;

    jclass editClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfTextEditNative");
    if (!editClass) return false;
    jfieldID pageField = env->GetFieldID(editClass, "pageIndex", "I");
    jfieldID objectField = env->GetFieldID(editClass, "objectIndex", "I");
    jfieldID originalTextField = env->GetFieldID(editClass, "originalText", "Ljava/lang/String;");
    jfieldID leftField = env->GetFieldID(editClass, "left", "F");
    jfieldID bottomField = env->GetFieldID(editClass, "bottom", "F");
    jfieldID rightField = env->GetFieldID(editClass, "right", "F");
    jfieldID topField = env->GetFieldID(editClass, "top", "F");
    jfieldID newTextField = env->GetFieldID(editClass, "newText", "Ljava/lang/String;");
    jfieldID scaleXField = env->GetFieldID(editClass, "scaleX", "F");
    jfieldID translateXField = env->GetFieldID(editClass, "translateX", "F");
    jfieldID translateYField = env->GetFieldID(editClass, "translateY", "F");
    jfieldID fontSizeField = env->GetFieldID(editClass, "fontSize", "F");
    jfieldID fontWeightField = env->GetFieldID(editClass, "fontWeight", "I");
    jfieldID italicAngleField = env->GetFieldID(editClass, "italicAngle", "F");
    jfieldID redField = env->GetFieldID(editClass, "r", "I");
    jfieldID greenField = env->GetFieldID(editClass, "g", "I");
    jfieldID blueField = env->GetFieldID(editClass, "b", "I");
    jfieldID alphaField = env->GetFieldID(editClass, "a", "I");
    jfieldID matrixAField = env->GetFieldID(editClass, "matrixA", "F");
    jfieldID matrixBField = env->GetFieldID(editClass, "matrixB", "F");
    jfieldID matrixCField = env->GetFieldID(editClass, "matrixC", "F");
    jfieldID matrixDField = env->GetFieldID(editClass, "matrixD", "F");
    jfieldID matrixEField = env->GetFieldID(editClass, "matrixE", "F");
    jfieldID matrixFField = env->GetFieldID(editClass, "matrixF", "F");
    jfieldID boldField = env->GetFieldID(editClass, "isBold", "Z");
    jfieldID italicField = env->GetFieldID(editClass, "isItalic", "Z");
    jfieldID underlineField = env->GetFieldID(editClass, "isUnderline", "Z");
    jfieldID strikeoutField = env->GetFieldID(editClass, "isStrikeout", "Z");
    jfieldID styleChangedField = env->GetFieldID(editClass, "styleChanged", "Z");
    jfieldID styleRunsField = env->GetFieldID(editClass, "styleRuns", "Ljava/lang/String;");
    jfieldID fontRunsField = env->GetFieldID(editClass, "fontRuns", "Ljava/lang/String;");
    jfieldID resetAppearanceField = env->GetFieldID(
            editClass,
            "resetNativeAppearance",
            "Z");
    jfieldID fontPathField = env->GetFieldID(editClass, "fontPath", "Ljava/lang/String;");
    jfieldID sourceFontSupportsReplacementField = env->GetFieldID(
            editClass,
            "sourceFontSupportsReplacement",
            "Z");
    jfieldID forceReplacementFontField = env->GetFieldID(
            editClass,
            "forceReplacementFont",
            "Z");
    jfieldID fontRangeStartField = env->GetFieldID(editClass, "fontRangeStart", "I");
    jfieldID fontRangeEndField = env->GetFieldID(editClass, "fontRangeEnd", "I");
    jfieldID visualLineEndsField = env->GetFieldID(
            editClass,
            "visualLineEnds",
            "Ljava/lang/String;");
    jfieldID visualLineHeightField = env->GetFieldID(editClass, "visualLineHeight", "F");
    jfieldID visualLineOffsetsField = env->GetFieldID(editClass, "visualLineOffsets", "Ljava/lang/String;");
    jfieldID visualLineYOffsetsField = env->GetFieldID(editClass, "visualLineYOffsets", "Ljava/lang/String;");
    jfieldID originalCursorAdvancesField = env->GetFieldID(
            editClass,
            "originalCursorAdvances",
            "Ljava/lang/String;");
    bool allApplied = true;
    std::vector<std::pair<int, std::string>> pendingMarkedTextRemovals;

    const int editCount = env->GetArrayLength(textEditsArray);
    for (int editIndex = 0; editIndex < editCount; ++editIndex) {
        jobject edit = env->GetObjectArrayElement(textEditsArray, editIndex);
        if (!edit) {
            allApplied = false;
            continue;
        }
        const int pageIndex = env->GetIntField(edit, pageField);
        const int objectIndex = env->GetIntField(edit, objectField);
        const bool usesProvidedPage = providedPage && pageIndex == providedPageIndex;
        FPDF_PAGE page = usesProvidedPage ? providedPage : FPDF_LoadPage(doc, pageIndex);
        const std::string liveStyleMarkPrefix = "LufickTextEditPreviewStyle_" +
                std::to_string(pageIndex) + "_" + std::to_string(objectIndex) + "_";
        if (page && usesProvidedPage) {
            RemoveTextEditPreviewObjects(page, liveStyleMarkPrefix);
        }
        const std::vector<FPDF_PAGEOBJECT> editableTextObjects =
                CollectEditableTextObjects(page);
        if (!page || objectIndex < 0 ||
            objectIndex >= static_cast<int>(editableTextObjects.size())) {
            if (page && !usesProvidedPage) FPDF_ClosePage(page);
            env->DeleteLocalRef(edit);
            allApplied = false;
            continue;
        }

        FPDF_PAGEOBJECT textObject = editableTextObjects[objectIndex];
        const std::string liveSourceMark = "LufickTextEditPreviewSource_" +
                std::to_string(pageIndex) + "_" + std::to_string(objectIndex);
        const bool isRetainedLiveSource = usesProvidedPage &&
                PageObjectHasExactMark(textObject, liveSourceMark);
        float actualLeft = 0.0f, actualBottom = 0.0f, actualRight = 0.0f, actualTop = 0.0f;
        const bool isTextObject = textObject && FPDFPageObj_GetType(textObject) == FPDF_PAGEOBJ_TEXT;
        const bool hasBounds = isTextObject &&
                FPDFPageObj_GetBounds(textObject, &actualLeft, &actualBottom, &actualRight, &actualTop);
        const float expectedLeft = env->GetFloatField(edit, leftField);
        const float expectedBottom = env->GetFloatField(edit, bottomField);
        const float expectedRight = env->GetFloatField(edit, rightField);
        const float expectedTop = env->GetFloatField(edit, topField);
        const float boundsTolerance = 0.5f;
        const bool boundsMatch = hasBounds &&
                std::fabs(actualLeft - expectedLeft) <= boundsTolerance &&
                std::fabs(actualBottom - expectedBottom) <= boundsTolerance &&
                std::fabs(actualRight - expectedRight) <= boundsTolerance &&
                std::fabs(actualTop - expectedTop) <= boundsTolerance;

        jstring expectedText = (jstring)env->GetObjectField(edit, originalTextField);
        jstring replacementText = (jstring)env->GetObjectField(edit, newTextField);
        jstring replacementFontPath = (jstring)env->GetObjectField(edit, fontPathField);
        jstring encodedStyleRuns = (jstring)env->GetObjectField(edit, styleRunsField);
        jstring encodedFontRuns = (jstring)env->GetObjectField(edit, fontRunsField);
        jstring encodedAdvances = (jstring)env->GetObjectField(
                edit,
                originalCursorAdvancesField);
        jstring encodedVisualLineEnds = (jstring)env->GetObjectField(edit, visualLineEndsField);
        jstring encodedVisualLineOffsets = (jstring)env->GetObjectField(edit, visualLineOffsetsField);
        jstring encodedVisualLineYOffsets = (jstring)env->GetObjectField(edit, visualLineYOffsetsField);
        const char* encodedVisualLineYOffsetChars = encodedVisualLineYOffsets
                ? env->GetStringUTFChars(encodedVisualLineYOffsets, nullptr) : nullptr;
        const char* encodedVisualLineOffsetChars = encodedVisualLineOffsets
                ? env->GetStringUTFChars(encodedVisualLineOffsets, nullptr) : nullptr;
        const char* replacementFontPathChars = replacementFontPath
                                               ? env->GetStringUTFChars(replacementFontPath, nullptr)
                                               : nullptr;
        const char* encodedStyleRunChars = encodedStyleRuns
                                            ? env->GetStringUTFChars(encodedStyleRuns, nullptr)
                                            : nullptr;
        const char* encodedFontRunChars = encodedFontRuns
                                           ? env->GetStringUTFChars(encodedFontRuns, nullptr)
                                           : nullptr;
        const char* encodedAdvanceChars = encodedAdvances
                                          ? env->GetStringUTFChars(encodedAdvances, nullptr)
                                           : nullptr;
        const char* encodedVisualLineEndChars = encodedVisualLineEnds
                                                ? env->GetStringUTFChars(encodedVisualLineEnds, nullptr)
                                                : nullptr;
        bool textMatches = !validateOriginalText || isRetainedLiveSource;
        if (validateOriginalText && !isRetainedLiveSource && isTextObject && expectedText) {
            FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
            if (textPage) {
                const unsigned long textBytes = FPDFTextObj_GetText(textObject, textPage, nullptr, 0);
                if (textBytes >= sizeof(FPDF_WCHAR)) {
                    const size_t bufferLength = textBytes / sizeof(FPDF_WCHAR);
                    std::vector<FPDF_WCHAR> buffer(bufferLength);
                    FPDFTextObj_GetText(textObject, textPage, buffer.data(), textBytes);
                    const jsize actualLength = static_cast<jsize>(bufferLength - 1);
                    const jsize expectedLength = env->GetStringLength(expectedText);
                    const jchar* expectedChars = env->GetStringChars(expectedText, nullptr);
                    textMatches = expectedChars && actualLength == expectedLength &&
                            std::equal(expectedChars, expectedChars + expectedLength, buffer.begin());
                    if (expectedChars) env->ReleaseStringChars(expectedText, expectedChars);
                }
                FPDFText_ClosePage(textPage);
            }
        }

        // Object index plus complete original text is the stable validation. Character-derived
        // visual block bounds can legitimately cover only part of this owner object.
        bool applied = isTextObject && textMatches && replacementText;
        if (applied) {
            const bool resetAppearance = env->GetBooleanField(
                    edit,
                    resetAppearanceField) == JNI_TRUE;
            const bool isBold = env->GetBooleanField(edit, boldField) == JNI_TRUE;
            const bool isItalic = env->GetBooleanField(edit, italicField) == JNI_TRUE;
            const bool isUnderline = env->GetBooleanField(edit, underlineField) == JNI_TRUE;
            const bool isStrikeout = env->GetBooleanField(edit, strikeoutField) == JNI_TRUE;
            TextEditNativeDecorationPaths nativeDecorationPaths;
            if (usesProvidedPage) {
                const std::string nativeDecorationMarkPrefix =
                        "LufickTextEditSourceDecoration_" +
                        std::to_string(pageIndex) + "_" + std::to_string(objectIndex) + "_";
                nativeDecorationPaths = FindTextEditNativeDecorationPaths(
                        page,
                        textObject,
                        expectedLeft,
                        expectedBottom,
                        expectedRight,
                        expectedTop,
                        nativeDecorationMarkPrefix);
            }
            const bool hasCharacterStyleChanges = env->GetBooleanField(
                    edit,
                    styleChangedField) == JNI_TRUE;
            const float requestedFontSize = env->GetFloatField(edit, fontSizeField);
            const int sourceFontWeight = env->GetIntField(edit, fontWeightField);
            const float sourceItalicAngle = env->GetFloatField(edit, italicAngleField);
            const unsigned int textR = static_cast<unsigned int>(env->GetIntField(edit, redField));
            const unsigned int textG = static_cast<unsigned int>(env->GetIntField(edit, greenField));
            const unsigned int textB = static_cast<unsigned int>(env->GetIntField(edit, blueField));
            const unsigned int textA = static_cast<unsigned int>(env->GetIntField(edit, alphaField));
            if (resetAppearance) {
                FS_MATRIX originalMatrix = {
                        env->GetFloatField(edit, matrixAField),
                        env->GetFloatField(edit, matrixBField),
                        env->GetFloatField(edit, matrixCField),
                        env->GetFloatField(edit, matrixDField),
                        env->GetFloatField(edit, matrixEField),
                        env->GetFloatField(edit, matrixFField)
                };
                FPDFPageObj_SetMatrix(textObject, &originalMatrix);
            }
            const jsize replacementLength = env->GetStringLength(replacementText);
            const bool sourceFontSupportsReplacement = env->GetBooleanField(
                    edit,
                    sourceFontSupportsReplacementField) == JNI_TRUE;
            const bool forceReplacementFont = env->GetBooleanField(
                    edit,
                    forceReplacementFontField) == JNI_TRUE;
            const int replacementFontRangeStart = env->GetIntField(edit, fontRangeStartField);
            const int replacementFontRangeEnd = env->GetIntField(edit, fontRangeEndField);
            const float visualLineHeight = env->GetFloatField(edit, visualLineHeightField);
            const bool hasDefaultReplacementFont = replacementFontPathChars &&
                    strlen(replacementFontPathChars) > 0;
            const bool hasScriptFontRuns = encodedFontRunChars && *encodedFontRunChars;
            const bool useCompatibleFont = replacementLength > 0 &&
                    (hasScriptFontRuns || (hasDefaultReplacementFont &&
                     (forceReplacementFont ||
                      (!sourceFontSupportsReplacement &&
                       TextEditNeedsCompatibleFont(env, expectedText, replacementText)))));
            const bool hasVisualReflow = (encodedVisualLineEndChars &&
                    strchr(encodedVisualLineEndChars, ',') != nullptr) ||
                    (encodedVisualLineYOffsetChars && *encodedVisualLineYOffsetChars);
            bool usedCompatibleFontObject = false;
            bool stylesAppliedPerRun = false;
            std::vector<StyledTextEditObject> liveStyleObjects;
            if (replacementLength == 0 && !usesProvidedPage) {
                // FPDFText_SetText() traps for an empty string in this Pdfium build. Final save
                // removes the object after all indexed replacements have been applied, so an
                // earlier removal cannot shift the object indices of later edits.
                const std::string removalMark = "LufickTextEditOriginal_" +
                        std::to_string(pageIndex) + "_" + std::to_string(objectIndex);
                FPDFPageObj_AddMark(textObject, removalMark.c_str());
                pendingMarkedTextRemovals.emplace_back(pageIndex, removalMark);
                applied = FPDFPage_GenerateContent(page) != 0;
            } else if (hasCharacterStyleChanges || hasVisualReflow) {
                const std::string runMarkPrefix = "LufickTextEditPreview_" +
                        std::to_string(pageIndex) + "_" + std::to_string(objectIndex) + "_";
                if (usesProvidedPage) RemoveTextEditPreviewObjects(page, runMarkPrefix);
                applied = CreateStyledTextEditObjects(
                        env,
                        doc,
                        page,
                        textObject,
                        expectedText,
                        replacementText,
                        replacementFontPathChars,
                        useCompatibleFont,
                        forceReplacementFont,
                        replacementFontRangeStart,
                        replacementFontRangeEnd,
                        encodedVisualLineEndChars,
                        encodedVisualLineOffsetChars,
                        encodedVisualLineYOffsetChars,
                        visualLineHeight,
                        cacheFallbackFonts,
                        usesProvidedPage,
                        encodedStyleRunChars,
                        encodedFontRunChars,
                        encodedAdvanceChars,
                        runMarkPrefix,
                        &liveStyleObjects);
                if (applied) {
                    const float scaleX = env->GetFloatField(edit, scaleXField);
                    const float translateX = env->GetFloatField(edit, translateXField);
                    const float translateY = env->GetFloatField(edit, translateYField);
                    for (const StyledTextEditObject& replacementObject : liveStyleObjects) {
                        if (scaleX == 1.0f && translateX == 0.0f && translateY == 0.0f) continue;
                        FPDFPageObj_Transform(
                                replacementObject.object,
                                scaleX,
                                0,
                                0,
                                1,
                                translateX,
                                translateY);
                    }
                    usedCompatibleFontObject = true;
                    stylesAppliedPerRun = true;
                    if (usesProvidedPage) {
                        const unsigned short previewPlaceholder[2] = {
                                static_cast<unsigned short>(' '), 0
                        };
                        applied = FPDFText_SetText(
                                textObject,
                                reinterpret_cast<FPDF_WIDESTRING>(previewPlaceholder)) != 0;
                    } else {
                        const std::string removalMark = "LufickTextEditOriginal_" +
                                std::to_string(pageIndex) + "_" + std::to_string(objectIndex);
                        FPDFPageObj_AddMark(textObject, removalMark.c_str());
                        pendingMarkedTextRemovals.emplace_back(pageIndex, removalMark);
                    }
                }
            } else if (useCompatibleFont) {
                const std::string runMarkPrefix = (usesProvidedPage
                        ? "LufickTextEditPreview_"
                        : "LufickTextEditFinal_") +
                        std::to_string(pageIndex) + "_" + std::to_string(objectIndex) + "_";
                if (usesProvidedPage) RemoveTextEditPreviewObjects(page, runMarkPrefix);
                applied = CreateStyledTextEditObjects(
                        env,
                        doc,
                        page,
                        textObject,
                        expectedText,
                        replacementText,
                        replacementFontPathChars,
                        true,
                        forceReplacementFont,
                        replacementFontRangeStart,
                        replacementFontRangeEnd,
                        encodedVisualLineEndChars,
                        encodedVisualLineOffsetChars,
                        encodedVisualLineYOffsetChars,
                        visualLineHeight,
                        cacheFallbackFonts,
                        usesProvidedPage,
                        encodedStyleRunChars,
                        encodedFontRunChars,
                        encodedAdvanceChars,
                        runMarkPrefix,
                        &liveStyleObjects);
                if (applied) {
                    const float scaleX = env->GetFloatField(edit, scaleXField);
                    const float translateX = env->GetFloatField(edit, translateXField);
                    const float translateY = env->GetFloatField(edit, translateYField);
                    for (const StyledTextEditObject& styledObject : liveStyleObjects) {
                        FPDF_PAGEOBJECT replacementObject = styledObject.object;
                        if (scaleX == 1.0f && translateX == 0.0f && translateY == 0.0f) continue;
                        FPDFPageObj_Transform(
                                replacementObject,
                                scaleX,
                                0,
                                0,
                                1,
                                translateX,
                                translateY);
                    }
                    usedCompatibleFontObject = true;
                    stylesAppliedPerRun = true;
                    if (usesProvidedPage) {
                        const unsigned short previewPlaceholder[2] = {
                                static_cast<unsigned short>(' '), 0
                        };
                        applied = FPDFText_SetText(
                                textObject,
                                reinterpret_cast<FPDF_WIDESTRING>(previewPlaceholder)) != 0;
                    } else {
                        const std::string removalMark = "LufickTextEditOriginal_" +
                                std::to_string(pageIndex) + "_" + std::to_string(objectIndex);
                        FPDFPageObj_AddMark(textObject, removalMark.c_str());
                        pendingMarkedTextRemovals.emplace_back(pageIndex, removalMark);
                    }
                }
            } else {
                bool usedPositionedPreview = false;
                if (usesProvidedPage) {
                    const std::string markPrefix = "LufickTextEditPreview_" +
                            std::to_string(pageIndex) + "_" + std::to_string(objectIndex) + "_";
                    std::vector<FPDF_PAGEOBJECT> stalePreviewObjects;
                    FindTextEditPreviewObject(page, markPrefix, markPrefix + "none", &stalePreviewObjects);
                    for (FPDF_PAGEOBJECT staleObject : stalePreviewObjects) {
                        if (FPDFPage_RemoveObject(page, staleObject)) {
                            FPDFPageObj_Destroy(staleObject);
                        }
                    }
                    std::vector<FPDF_PAGEOBJECT> positionedObjects;
                    usedPositionedPreview = CreatePositionedSourceTextEditObjects(
                            env,
                            doc,
                            page,
                            textObject,
                            expectedText,
                            replacementText,
                            encodedAdvanceChars,
                            markPrefix,
                            &positionedObjects);
                    if (usedPositionedPreview) {
                        const float scaleX = env->GetFloatField(edit, scaleXField);
                        const float translateX = env->GetFloatField(edit, translateXField);
                        const float translateY = env->GetFloatField(edit, translateYField);
                        for (FPDF_PAGEOBJECT positionedObject : positionedObjects) {
                            if (scaleX != 1.0f || translateX != 0.0f || translateY != 0.0f) {
                                FPDFPageObj_Transform(
                                        positionedObject,
                                        scaleX,
                                        0,
                                        0,
                                        1,
                                        translateX,
                                        translateY);
                            }
                            liveStyleObjects.emplace_back(
                                    positionedObject,
                                    TextEditCharacterStyle(
                                            isBold,
                                            isItalic,
                                            isUnderline,
                                            isStrikeout));
                        }
                        const unsigned short previewPlaceholder[2] = {
                                static_cast<unsigned short>(' '), 0
                        };
                        applied = FPDFText_SetText(
                                textObject,
                                reinterpret_cast<FPDF_WIDESTRING>(previewPlaceholder)) != 0;
                        usedCompatibleFontObject = applied;
                    }
                }
                const jchar* replacementChars = usedPositionedPreview
                                                ? nullptr
                                                : env->GetStringChars(replacementText, nullptr);
                std::vector<unsigned short> replacement(
                        static_cast<size_t>(std::max<jsize>(replacementLength, 1)) + 1,
                        0);
                if (usedPositionedPreview) {
                    // The positioned preview objects already contain the replacement text.
                } else if (replacementLength == 0) {
                    // Keep the preview object alive and visually blank. A later keystroke can
                    // replace this space on the same native object without changing its index.
                    replacement[0] = static_cast<unsigned short>(' ');
                } else {
                    for (jsize i = 0; i < replacementLength; ++i) {
                        replacement[i] = static_cast<unsigned short>(replacementChars[i]);
                    }
                }
                if (!usedPositionedPreview) {
                    applied = FPDFText_SetText(
                            textObject,
                            reinterpret_cast<FPDF_WIDESTRING>(replacement.data())) != 0;
                }
                if (applied && !usedPositionedPreview) {
                    liveStyleObjects.emplace_back(
                            textObject,
                            TextEditCharacterStyle(
                                    isBold,
                                    isItalic,
                                    isUnderline,
                                    isStrikeout));
                }
                if (replacementChars) {
                    env->ReleaseStringChars(replacementText, replacementChars);
                }
            }
            if (applied) {
                if (usesProvidedPage && !isRetainedLiveSource) {
                    FPDFPageObj_AddMark(textObject, liveSourceMark.c_str());
                }
                if (!usedCompatibleFontObject && (replacementLength > 0 || usesProvidedPage)) {
                    const float scaleX = env->GetFloatField(edit, scaleXField);
                    const float translateX = env->GetFloatField(edit, translateXField);
                    const float translateY = env->GetFloatField(edit, translateYField);
                    if (scaleX != 1.0f || translateX != 0.0f || translateY != 0.0f) {
                        FPDFPageObj_Transform(textObject, scaleX, 0, 0, 1, translateX, translateY);
                    }
                }
                if (usesProvidedPage) {
                    std::vector<FPDF_PAGEOBJECT> currentTextObjects;
                    currentTextObjects.reserve(liveStyleObjects.size());
                    bool hasCurrentUnderline = false;
                    bool hasCurrentStrikeout = false;
                    for (const StyledTextEditObject& styledObject : liveStyleObjects) {
                        currentTextObjects.push_back(styledObject.object);
                        hasCurrentUnderline = hasCurrentUnderline || styledObject.style.underline;
                        hasCurrentStrikeout = hasCurrentStrikeout || styledObject.style.strikeout;
                    }
                    UpdateTextEditNativeDecorationPaths(
                            page,
                            nativeDecorationPaths.underline,
                            currentTextObjects,
                            replacementLength > 0 && hasCurrentUnderline);
                    UpdateTextEditNativeDecorationPaths(
                            page,
                            nativeDecorationPaths.strikeout,
                            currentTextObjects,
                            replacementLength > 0 && hasCurrentStrikeout);
                    const bool usesNativeUnderline = !nativeDecorationPaths.underline.empty();
                    const bool usesNativeStrikeout = !nativeDecorationPaths.strikeout.empty();
                    for (const StyledTextEditObject& styledObject : liveStyleObjects) {
                        FPDF_PAGEOBJECT liveObject = styledObject.object;
                        ApplyTextEditLiveFontStyle(
                                liveObject,
                                resetAppearance && !stylesAppliedPerRun,
                                styledObject.style.bold,
                                styledObject.style.italic,
                                sourceFontWeight,
                                sourceItalicAngle,
                                requestedFontSize,
                                textR,
                                textG,
                                textB,
                                textA);
                    }
                    int decorationOrdinal = 0;
                    size_t decorationStart = 0;
                    const unsigned int originalDecorationColor =
                            ((textA & 0xFFu) << 24u) | ((textR & 0xFFu) << 16u) |
                            ((textG & 0xFFu) << 8u) | (textB & 0xFFu);
                    auto decorationColorForStyle = [&](const TextEditCharacterStyle& style) {
                        return style.hasColor ? style.color : originalDecorationColor;
                    };
                    while (replacementLength > 0 && decorationStart < liveStyleObjects.size()) {
                        const TextEditCharacterStyle& style =
                                liveStyleObjects[decorationStart].style;
                        if (!style.underline && !style.strikeout) {
                            ++decorationStart;
                            continue;
                        }
                        const unsigned int decorationColor = decorationColorForStyle(style);
                        size_t decorationEnd = decorationStart + 1;
                        while (decorationEnd < liveStyleObjects.size()) {
                            const TextEditCharacterStyle& nextStyle =
                                    liveStyleObjects[decorationEnd].style;
                            if (nextStyle.underline != style.underline ||
                                nextStyle.strikeout != style.strikeout ||
                                decorationColorForStyle(nextStyle) != decorationColor) break;
                            ++decorationEnd;
                        }
                        std::vector<FPDF_PAGEOBJECT> decorationObjects;
                        decorationObjects.reserve(decorationEnd - decorationStart);
                        for (size_t index = decorationStart; index < decorationEnd; ++index) {
                            decorationObjects.push_back(liveStyleObjects[index].object);
                        }
                        AppendTextEditLiveDecoration(
                                page,
                                decorationObjects,
                                style.underline && !usesNativeUnderline,
                                style.strikeout && !usesNativeStrikeout,
                                (decorationColor >> 16u) & 0xFFu,
                                (decorationColor >> 8u) & 0xFFu,
                                decorationColor & 0xFFu,
                                (decorationColor >> 24u) & 0xFFu,
                                liveStyleMarkPrefix,
                                decorationOrdinal++);
                        decorationStart = decorationEnd;
                    }
                }

                if (!usesProvidedPage) {
                    applied = FPDFPage_GenerateContent(page) != 0;
                }
            }
        }

        if (!applied) {
            LOGE("PDF_EDIT_NATIVE text edit rejected page=%d object=%d", pageIndex, objectIndex);
            allApplied = false;
        }
        if (expectedText) env->DeleteLocalRef(expectedText);
        if (replacementText) env->DeleteLocalRef(replacementText);
        if (replacementFontPathChars) {
            env->ReleaseStringUTFChars(replacementFontPath, replacementFontPathChars);
        }
        if (encodedStyleRunChars) {
            env->ReleaseStringUTFChars(encodedStyleRuns, encodedStyleRunChars);
        }
        if (encodedFontRunChars) {
            env->ReleaseStringUTFChars(encodedFontRuns, encodedFontRunChars);
        }
        if (encodedAdvanceChars) {
            env->ReleaseStringUTFChars(encodedAdvances, encodedAdvanceChars);
        }
        if (encodedVisualLineEndChars) {
            env->ReleaseStringUTFChars(encodedVisualLineEnds, encodedVisualLineEndChars);
        }
        if (encodedVisualLineOffsetChars) {
            env->ReleaseStringUTFChars(encodedVisualLineOffsets, encodedVisualLineOffsetChars);
        }
        if (encodedVisualLineOffsets) env->DeleteLocalRef(encodedVisualLineOffsets);
        if (encodedVisualLineYOffsetChars) env->ReleaseStringUTFChars(encodedVisualLineYOffsets, encodedVisualLineYOffsetChars);
        if (encodedVisualLineYOffsets) env->DeleteLocalRef(encodedVisualLineYOffsets);
        if (encodedStyleRuns) env->DeleteLocalRef(encodedStyleRuns);
        if (encodedFontRuns) env->DeleteLocalRef(encodedFontRuns);
        if (encodedAdvances) env->DeleteLocalRef(encodedAdvances);
        if (encodedVisualLineEnds) env->DeleteLocalRef(encodedVisualLineEnds);
        if (replacementFontPath) env->DeleteLocalRef(replacementFontPath);
        if (!usesProvidedPage) FPDF_ClosePage(page);
        env->DeleteLocalRef(edit);
    }

    for (const auto& removal : pendingMarkedTextRemovals) {
        FPDF_PAGE page = FPDF_LoadPage(doc, removal.first);
        if (!page) {
            if (page) FPDF_ClosePage(page);
            allApplied = false;
            continue;
        }
        FPDF_PAGEOBJECT textObject = nullptr;
        const int objectCount = FPDFPage_CountObjects(page);
        for (int objectIndex = 0; objectIndex < objectCount && !textObject; ++objectIndex) {
            FPDF_PAGEOBJECT candidate = FPDFPage_GetObject(page, objectIndex);
            if (!candidate || FPDFPageObj_GetType(candidate) != FPDF_PAGEOBJ_TEXT) continue;
            const int markCount = FPDFPageObj_CountMarks(candidate);
            for (int markIndex = 0; markIndex < markCount; ++markIndex) {
                if (GetPageObjectMarkName(FPDFPageObj_GetMark(
                        candidate,
                        static_cast<unsigned long>(markIndex))) == removal.second) {
                    textObject = candidate;
                    break;
                }
            }
        }
        const bool removed = textObject && FPDFPage_RemoveObject(page, textObject);
        if (removed) {
            FPDFPageObj_Destroy(textObject);
            if (!FPDFPage_GenerateContent(page)) allApplied = false;
        } else {
            allApplied = false;
        }
        FPDF_ClosePage(page);
    }
    env->DeleteLocalRef(editClass);
    return allApplied;
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSavePdfEditObjects(
        JNIEnv* env,
        jobject thiz,
        jstring inputPath_,
        jstring outputPath_,
        jobjectArray editObjectsArray,
        jobjectArray savedContentUpdatesArray,
        jobjectArray textContentUpdatesArray) {
    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);
    LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects start input=%s output=%s array=%p", inputPath, outputPath, editObjectsArray);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) {
        LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects failed: FPDF_LoadDocument input=%s", inputPath);
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    jclass editObjectClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfAnnotationNative");
    jfieldID typeField = env->GetFieldID(editObjectClass, "type", "I");
    jfieldID pageField = env->GetFieldID(editObjectClass, "pageIndex", "I");
    jfieldID leftField = env->GetFieldID(editObjectClass, "left", "F");
    jfieldID topField = env->GetFieldID(editObjectClass, "top", "F");
    jfieldID rightField = env->GetFieldID(editObjectClass, "right", "F");
    jfieldID bottomField = env->GetFieldID(editObjectClass, "bottom", "F");
    jfieldID rField = env->GetFieldID(editObjectClass, "r", "I");
    jfieldID gField = env->GetFieldID(editObjectClass, "g", "I");
    jfieldID bField = env->GetFieldID(editObjectClass, "b", "I");
    jfieldID alphaField = env->GetFieldID(editObjectClass, "alpha", "I");
    jfieldID urlField = env->GetFieldID(editObjectClass, "linkUrl", "Ljava/lang/String;");
    jfieldID markupRectsField = env->GetFieldID(editObjectClass, "markupRectsJson", "Ljava/lang/String;");
    jfieldID dataPropsField = env->GetFieldID(editObjectClass, "dataProperties", "Ljava/lang/String;");
    jfieldID nativeSourceIdField = env->GetFieldID(editObjectClass, "nativeSourceId", "I");
    jfieldID nativeEditActionField = env->GetFieldID(editObjectClass, "nativeEditAction", "I");

    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jclass jsonArrayClass = env->FindClass("org/json/JSONArray");
    jmethodID jsonArrayInit = env->GetMethodID(jsonArrayClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayLength = env->GetMethodID(jsonArrayClass, "length", "()I");
    jmethodID jsonArrayGetObject = env->GetMethodID(jsonArrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jmethodID jsonGetDouble = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID jsonOptDouble = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");

    const int editObjectCount = editObjectsArray ? env->GetArrayLength(editObjectsArray) : 0;
    LOGE(
            "PDF_EDIT_NATIVE nativeSavePdfEditObjects loaded doc pageCount=%d editObjectCount=%d",
            FPDF_GetPageCount(doc),
            editObjectCount
    );
    if (!ApplyNativeTextContentEdits(env, doc, textContentUpdatesArray, nullptr, -1, true)) {
        FPDF_CloseDocument(doc);
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }
    ApplyNativeAnnotationEditActions(env, doc, savedContentUpdatesArray, nullptr, -1, false);
    LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects after ApplyNativeAnnotationEditActions(processNewObjects=false)");

    FPDF_PAGE currentPage = nullptr;
    int lastPageIndex = -1;

    for (int i = 0; i < editObjectCount; i++) {
        jobject obj = env->GetObjectArrayElement(editObjectsArray, i);
        if (!obj) continue;

        int typeInt = env->GetIntField(obj, typeField);
        int pageIndex = env->GetIntField(obj, pageField);
        int nativeSourceId = env->GetIntField(obj, nativeSourceIdField);
        int nativeEditAction = env->GetIntField(obj, nativeEditActionField);
        LOGE(
                "PDF_EDIT_NATIVE nativeSavePdfEditObjects object index=%d type=%d page=%d sourceId=%d action=%d",
                i,
                typeInt,
                pageIndex,
                nativeSourceId,
                nativeEditAction
        );

        if (nativeEditAction == 1 || nativeEditAction == 2 || nativeEditAction == 5 || nativeSourceId >= 0) {
            LOGE(
                    "PDF_EDIT_NATIVE nativeSavePdfEditObjects skip object index=%d type=%d page=%d sourceId=%d action=%d",
                    i,
                    typeInt,
                    pageIndex,
                    nativeSourceId,
                    nativeEditAction
            );
            env->DeleteLocalRef(obj);
            continue;
        }

        if (pageIndex != lastPageIndex) {
            if (currentPage) {
                LOGE(
                        "PDF_EDIT_NATIVE nativeSavePdfEditObjects generate+close previousPage=%d beforeGenerate objectCount=%d annotCount=%d",
                        lastPageIndex,
                        FPDFPage_CountObjects(currentPage),
                        FPDFPage_GetAnnotCount(currentPage)
                );
                FPDFPage_GenerateContent(currentPage);
                LOGE(
                        "PDF_EDIT_NATIVE nativeSavePdfEditObjects previousPage=%d afterGenerate objectCount=%d annotCount=%d",
                        lastPageIndex,
                        FPDFPage_CountObjects(currentPage),
                        FPDFPage_GetAnnotCount(currentPage)
                );
                FPDF_ClosePage(currentPage);
            }
            currentPage = FPDF_LoadPage(doc, pageIndex);
            lastPageIndex = pageIndex;
            LOGE(
                    "PDF_EDIT_NATIVE nativeSavePdfEditObjects load page=%d page=%p objectCount=%d annotCount=%d",
                    pageIndex,
                    currentPage,
                    currentPage ? FPDFPage_CountObjects(currentPage) : -1,
                    currentPage ? FPDFPage_GetAnnotCount(currentPage) : -1
            );
        }

        if (!currentPage) {
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects skip object index=%d because page load failed page=%d", i, pageIndex);
            env->DeleteLocalRef(obj);
            continue;
        }

        float left = env->GetFloatField(obj, leftField);
        float top = env->GetFloatField(obj, topField);
        float right = env->GetFloatField(obj, rightField);
        float bottom = env->GetFloatField(obj, bottomField);
        int r = env->GetIntField(obj, rField);
        int g = env->GetIntField(obj, gField);
        int b = env->GetIntField(obj, bField);
        int alpha = env->GetIntField(obj, alphaField);

        FS_RECTF rect;
        rect.left = fmin(left, right);
        rect.right = fmax(left, right);
        rect.bottom = fmin(top, bottom);
        rect.top = fmax(top, bottom);

        bool isEditTextSignature = false;
        std::string textPropsUtf8;
        if (typeInt == 5) {
            textPropsUtf8 = GetBridgeDataPropertyUtf8(
                    env, obj, dataPropsField, jsonClass, jsonInit, "textProperties");
            isEditTextSignature =
                    textPropsUtf8.find("\"signatureSubType\"") != std::string::npos &&
                    textPropsUtf8.find("Sign_text") != std::string::npos;
        }
        const bool isSimplePdfStampEdit = isSimplePdfStampBridgeAnnotation(env, obj, dataPropsField, jsonClass, jsonInit);
        const std::string freehandPropsUtf8 = typeInt == 6
                ? GetBridgeDataPropertyUtf8(
                        env, obj, dataPropsField, jsonClass, jsonInit, "fhDrawingProperties")
                : std::string();
        const std::string imagePropsUtf8 = typeInt == 9
                ? GetBridgeDataPropertyUtf8(
                        env, obj, dataPropsField, jsonClass, jsonInit, "imageProperties")
                : std::string();
        const bool isSvgPathStampEdit =
                typeInt == 9 &&
                (
                    imagePropsUtf8.find("\"stampKind\":\"shape_element_svg\"") != std::string::npos ||
                    imagePropsUtf8.find("\"assetFormat\":\"svg-path\"") != std::string::npos
                );
        std::string contentMetadataKind;
        std::string contentMetadataSubtype;
        if (typeInt == 4) {
            contentMetadataKind = "redaction";
        } else if (isSimplePdfStampEdit) {
            contentMetadataKind = "preset_stamp";
            contentMetadataSubtype = "pdf";
        } else if (isEditTextSignature) {
            contentMetadataKind = "signature";
            contentMetadataSubtype = "Sign_text";
        } else if (typeInt == 6 && freehandPropsUtf8.find("Sign_draw") != std::string::npos) {
            contentMetadataKind = "signature";
            contentMetadataSubtype = "Sign_draw";
        } else if (typeInt == 6) {
            contentMetadataKind = "app_created";
        } else if (IsPdfShapeNativeType(typeInt)) {
            contentMetadataKind = "app_created";
            contentMetadataSubtype = "pdf_shape";
        } else if (typeInt == 9 && imagePropsUtf8.find("Sign_Image") != std::string::npos) {
            contentMetadataKind = "signature";
            contentMetadataSubtype = "Sign_Image";
        } else if (
                typeInt == 9 &&
                (
                    imagePropsUtf8.find("\"presetStamp\":true") != std::string::npos ||
                    imagePropsUtf8.find("\"stampKind\":\"preset_stamp\"") != std::string::npos ||
                    imagePropsUtf8.find("\"stampKind\":\"preset stamp\"") != std::string::npos
                )
        ) {
            contentMetadataKind = "preset_stamp";
            contentMetadataSubtype = "image";
        } else if (isSvgPathStampEdit) {
            contentMetadataKind = "app_created";
            contentMetadataSubtype = "shape_element_svg";
        }

        const int pageObjectCountBeforeSave = FPDFPage_CountObjects(currentPage);

        if (typeInt == 3) {
            // special case for text with link annot
            FPDF_ANNOTATION linkAnnotation = FPDFPage_CreateAnnot(currentPage, FPDF_ANNOT_LINK);
            if (linkAnnotation) {
                FPDFAnnot_SetRect(linkAnnotation, &rect);
                processLink(env, obj, currentPage, linkAnnotation, rect, urlField);
                FPDFPage_CloseAnnot(linkAnnotation);
                LOGE(
                        "PDF_EDIT_NATIVE nativeSavePdfEditObjects processLink index=%d page=%d",
                        i,
                        pageIndex
                );
            }
        } else if (typeInt == 4) {
            const bool savedRedactionContent = processRedactionContent(
                    currentPage,
                    rect,
                    r,
                    g,
                    b,
                    alpha
            );
            LOGE(
                    "PDF_EDIT_NATIVE nativeSavePdfEditObjects processRedactionContent index=%d page=%d success=%d afterObjects=%d",
                    i,
                    pageIndex,
                    savedRedactionContent ? 1 : 0,
                    FPDFPage_CountObjects(currentPage)
            );
        } else if (typeInt == 6) {
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects processFreeHand index=%d page=%d beforeObjects=%d", i, pageIndex, FPDFPage_CountObjects(currentPage));
            processFreeHand(env, obj, currentPage, dataPropsField, r, g, b, jsonClass, jsonInit, true);
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects processFreeHand done index=%d page=%d afterObjects=%d", i, pageIndex, FPDFPage_CountObjects(currentPage));
        } else if (isSimplePdfStampEdit) {
            const bool savedSimplePdfStampContent = processSimplePdfStamp(env, obj, doc, currentPage, nullptr, rect, typeInt, dataPropsField, r, g, b, alpha, jsonClass, jsonInit, true);
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects processSimplePdfStamp pageContent index=%d page=%d success=%d afterObjects=%d",
                 i, pageIndex, savedSimplePdfStampContent ? 1 : 0, FPDFPage_CountObjects(currentPage));
        } else if (typeInt == 11 || isEditTextSignature) {
            if (isEditTextSignature) {
                LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects route Sign_text type=%d through processFreeText index=%d page=%d", typeInt, i, pageIndex);
            }
            processFreeText(env, obj, doc, currentPage, rect, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
        } else if (typeInt == 9) {
            const bool savedImageContent = processImageOrPresetStamp(env, obj, doc, currentPage, nullptr, rect, dataPropsField, jsonClass, jsonInit, true);
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects processImageOrPresetStamp pageContent index=%d page=%d success=%d afterObjects=%d",
                 i, pageIndex, savedImageContent ? 1 : 0, FPDFPage_CountObjects(currentPage));
        } else if (IsPdfShapeNativeType(typeInt)) {
            const bool savedShapeContent = processPdfShapeContent(env, obj, currentPage, rect, typeInt, dataPropsField, r, g, b, alpha, jsonClass, jsonInit);
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects processPdfShapeContent index=%d page=%d type=%d success=%d afterObjects=%d",
                 i, pageIndex, typeInt, savedShapeContent ? 1 : 0, FPDFPage_CountObjects(currentPage));
        } else {
            LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects skip unsupported edit content type index=%d type=%d page=%d", i, typeInt, pageIndex);
        }

        if (!contentMetadataKind.empty()) {
            const int pageObjectCountAfterSave = FPDFPage_CountObjects(currentPage);
            const std::string groupId = contentMetadataKind == "app_created"
                                        ? std::string()
                                        : "p" + std::to_string(pageIndex) + "_o" + std::to_string(i);
            for (int objectIndex = pageObjectCountBeforeSave;
                 objectIndex < pageObjectCountAfterSave;
                 ++objectIndex) {
                AddLufickContentObjectMetadata(
                        doc,
                        FPDFPage_GetObject(currentPage, objectIndex),
                        contentMetadataKind,
                        contentMetadataSubtype,
                        groupId
                );
            }
        }

        env->DeleteLocalRef(obj);
    }

    if (currentPage) {
        LOGE(
                "PDF_EDIT_NATIVE nativeSavePdfEditObjects generate+close finalPage=%d beforeGenerate objectCount=%d annotCount=%d",
                lastPageIndex,
                FPDFPage_CountObjects(currentPage),
                FPDFPage_GetAnnotCount(currentPage)
        );
        FPDFPage_GenerateContent(currentPage);
        LOGE(
                "PDF_EDIT_NATIVE nativeSavePdfEditObjects finalPage=%d afterGenerate objectCount=%d annotCount=%d",
                lastPageIndex,
                FPDFPage_CountObjects(currentPage),
                FPDFPage_GetAnnotCount(currentPage)
        );
        FPDF_ClosePage(currentPage);
    }

    FILE* file = fopen(outputPath, "wb");
    LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects open output file success=%d output=%s", file ? 1 : 0, outputPath);
    PdfFileWriter writer{ {1, WriteBlock}, file };
    int success = (file) ? FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL) : JNI_FALSE;
    if (file) fclose(file);
    LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects FPDF_SaveAsCopy success=%d", success);
    if (success && !PatchSavedPdfShapeNativeDictionaries(outputPath)) {
        LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects PatchSavedPdfShapeNativeDictionaries failed");
        success = JNI_FALSE;
    }
    if (success && !AppendPdfEditContentMarker(outputPath)) {
        LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects AppendPdfEditContentMarker failed");
        success = JNI_FALSE;
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
    LOGE("PDF_EDIT_NATIVE nativeSavePdfEditObjects finish success=%d", success ? 1 : 0);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveScannedPdfObjects(
        JNIEnv* env,
        jobject thiz,
        jstring inputPath_,
        jstring outputPath_,
        jobjectArray objectsArray) {
    if (!objectsArray) return JNI_FALSE;

    jclass objectClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfAnnotationNative");
    if (!objectClass) return JNI_FALSE;
    jfieldID typeField = env->GetFieldID(objectClass, "type", "I");
    if (!typeField) {
        env->DeleteLocalRef(objectClass);
        return JNI_FALSE;
    }
    const auto isScannedContentObject = [](int type) {
        return type == 4 ||
               type == 5 ||
               type == 6 ||
               type == 9 ||
               type == 11 ||
               IsPdfShapeNativeType(type);
    };

    const int objectCount = env->GetArrayLength(objectsArray);
    int contentCount = 0;
    int annotationCount = 0;
    for (int i = 0; i < objectCount; ++i) {
        jobject object = env->GetObjectArrayElement(objectsArray, i);
        if (!object) continue;
        const int type = env->GetIntField(object, typeField);
        if (isScannedContentObject(type)) contentCount++; else annotationCount++;
        env->DeleteLocalRef(object);
    }

    jobjectArray annotationObjects = env->NewObjectArray(annotationCount, objectClass, nullptr);
    jobjectArray contentObjects = env->NewObjectArray(contentCount, objectClass, nullptr);
    int annotationIndex = 0;
    int contentIndex = 0;
    for (int i = 0; i < objectCount; ++i) {
        jobject object = env->GetObjectArrayElement(objectsArray, i);
        if (!object) continue;
        const int type = env->GetIntField(object, typeField);
        if (isScannedContentObject(type)) {
            env->SetObjectArrayElement(contentObjects, contentIndex++, object);
        } else {
            env->SetObjectArrayElement(annotationObjects, annotationIndex++, object);
        }
        env->DeleteLocalRef(object);
    }

    jboolean success = JNI_FALSE;
    if (objectCount == 0) {
        success = Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveAnnotations(
                env, thiz, inputPath_, outputPath_, annotationObjects);
    } else if (annotationCount == 0) {
        success = Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSavePdfEditObjects(
                env, thiz, inputPath_, outputPath_, contentObjects, nullptr, nullptr);
    } else if (contentCount == 0) {
        success = Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveAnnotations(
                env, thiz, inputPath_, outputPath_, annotationObjects);
    } else {
        const char* outputPath = env->GetStringUTFChars(outputPath_, nullptr);
        const std::string contentStagePath = std::string(outputPath) + ".content_stage.pdf";
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        jstring contentStagePath_ = env->NewStringUTF(contentStagePath.c_str());

        success = Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSavePdfEditObjects(
                env, thiz, inputPath_, contentStagePath_, contentObjects, nullptr, nullptr);
        if (success == JNI_TRUE) {
            success = Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveAnnotations(
                    env, thiz, contentStagePath_, outputPath_, annotationObjects);
        }
        remove(contentStagePath.c_str());
        env->DeleteLocalRef(contentStagePath_);
    }

    env->DeleteLocalRef(annotationObjects);
    env->DeleteLocalRef(contentObjects);
    env->DeleteLocalRef(objectClass);
    return success;
}

static float ClampCropPercent(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 95.0f) return 95.0f;
    return value;
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSavePdfCrop(
        JNIEnv* env,
        jobject thiz,
        jstring inputPath_,
        jstring outputPath_,
        jobjectArray cropsArray
) {
    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) {
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    const int cropCount = cropsArray ? env->GetArrayLength(cropsArray) : 0;
    if (cropCount <= 0) {
        FPDF_CloseDocument(doc);
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    jclass cropClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfCropNative");
    if (!cropClass) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        FPDF_CloseDocument(doc);
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    jfieldID pageIndexField = env->GetFieldID(cropClass, "pageIndex", "I");
    jfieldID leftPercentField = env->GetFieldID(cropClass, "leftPercent", "F");
    jfieldID topPercentField = env->GetFieldID(cropClass, "topPercent", "F");
    jfieldID rightPercentField = env->GetFieldID(cropClass, "rightPercent", "F");
    jfieldID bottomPercentField = env->GetFieldID(cropClass, "bottomPercent", "F");
    if (!pageIndexField || !leftPercentField || !topPercentField || !rightPercentField ||
        !bottomPercentField) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        FPDF_CloseDocument(doc);
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    const int pageCount = FPDF_GetPageCount(doc);
    bool appliedAnyCrop = false;
    bool failed = false;

    for (int i = 0; i < cropCount; i++) {
        jobject cropObj = env->GetObjectArrayElement(cropsArray, i);
        if (!cropObj) continue;

        const int pageIndex = env->GetIntField(cropObj, pageIndexField);
        const float leftPercent = ClampCropPercent(env->GetFloatField(cropObj, leftPercentField));
        const float topPercent = ClampCropPercent(env->GetFloatField(cropObj, topPercentField));
        const float rightPercent = ClampCropPercent(env->GetFloatField(cropObj, rightPercentField));
        const float bottomPercent = ClampCropPercent(env->GetFloatField(cropObj, bottomPercentField));
        env->DeleteLocalRef(cropObj);

        if (pageIndex < 0 || pageIndex >= pageCount) {
            failed = true;
            break;
        }

        FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
        if (!page) {
            failed = true;
            break;
        }

        float baseLeft = 0.0f;
        float baseBottom = 0.0f;
        float baseRight = 0.0f;
        float baseTop = 0.0f;
        if (!FPDFPage_GetCropBox(page, &baseLeft, &baseBottom, &baseRight, &baseTop)) {
            if (!FPDFPage_GetMediaBox(page, &baseLeft, &baseBottom, &baseRight, &baseTop)) {
                baseRight = static_cast<float>(FPDF_GetPageWidth(page));
                baseTop = static_cast<float>(FPDF_GetPageHeight(page));
            }
        }

        float originalLeftPercent = leftPercent;
        float originalTopPercent = topPercent;
        float originalRightPercent = rightPercent;
        float originalBottomPercent = bottomPercent;
        switch (FPDFPage_GetRotation(page)) {
            case 1: // 90 degrees clockwise.
                originalLeftPercent = topPercent;
                originalTopPercent = rightPercent;
                originalRightPercent = bottomPercent;
                originalBottomPercent = leftPercent;
                break;
            case 2: // 180 degrees clockwise.
                originalLeftPercent = rightPercent;
                originalTopPercent = bottomPercent;
                originalRightPercent = leftPercent;
                originalBottomPercent = topPercent;
                break;
            case 3: // 270 degrees clockwise.
                originalLeftPercent = bottomPercent;
                originalTopPercent = leftPercent;
                originalRightPercent = topPercent;
                originalBottomPercent = rightPercent;
                break;
            default:
                break;
        }

        const float pageWidth = baseRight - baseLeft;
        const float pageHeight = baseTop - baseBottom;
        const float cropLeft = baseLeft + pageWidth * (originalLeftPercent / 100.0f);
        const float cropRight = baseRight - pageWidth * (originalRightPercent / 100.0f);
        const float cropBottom = baseBottom + pageHeight * (originalBottomPercent / 100.0f);
        const float cropTop = baseTop - pageHeight * (originalTopPercent / 100.0f);

        if (cropLeft >= cropRight || cropBottom >= cropTop) {
            FPDF_ClosePage(page);
            failed = true;
            break;
        }

        FPDFPage_SetCropBox(page, cropLeft, cropBottom, cropRight, cropTop);
        FPDF_ClosePage(page);
        appliedAnyCrop = true;
    }

    int success = JNI_FALSE;
    if (!failed && appliedAnyCrop) {
        FILE* file = fopen(outputPath, "wb");
        PdfFileWriter writer{ {1, WriteBlock}, file };
        success = (file) ? FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL) : JNI_FALSE;
        if (file) fclose(file);
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
    return success ? JNI_TRUE : JNI_FALSE;
}

static bool NativePageIndexRequested(const std::set<int>& requestedPages, int pageIndex) {
    return requestedPages.empty() || requestedPages.find(pageIndex) != requestedPages.end();
}

static bool NativeWrapPageContentsWithMatrix(
        const std::string& originalBody,
        const std::vector<PdfObjectInfo>& objects,
        std::vector<PdfObjectReplacement>* replacements,
        int prefixObjectNumber,
        int suffixObjectNumber,
        std::string* outBody
) {
    if (!replacements || !outBody) return false;
    std::string body = originalBody;
    size_t dictStart = 0;
    size_t dictEnd = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindTopLevelPdfDictionary(body, &dictStart, &dictEnd) ||
        !FindPdfDictionaryRawValueSegment(
                body, dictStart, dictEnd, "Contents", &valueStart, &valueEnd)) {
        return false;
    }

    const std::string prefixRef = std::to_string(prefixObjectNumber) + " 0 R";
    const std::string suffixRef = std::to_string(suffixObjectNumber) + " 0 R";
    if (body[valueStart] == '[') {
        std::string contentsArray = body.substr(valueStart, valueEnd - valueStart);
        const size_t closeBracket = contentsArray.rfind(']');
        if (closeBracket == std::string::npos) return false;
        contentsArray.insert(closeBracket, " " + suffixRef);
        contentsArray.insert(1, " " + prefixRef);
        body.replace(valueStart, valueEnd - valueStart, contentsArray);
        *outBody = std::move(body);
        return true;
    }

    int contentsObjectNumber = 0;
    int contentsGeneration = 0;
    size_t refEnd = valueStart;
    if (!ParseIndirectReferenceAt(
            body,
            valueStart,
            valueEnd,
            &contentsObjectNumber,
            &contentsGeneration,
            &refEnd)) {
        return false;
    }

    const PdfObjectInfo* contentsObject =
            FindPdfObjectInfoByRef(objects, contentsObjectNumber, contentsGeneration);
    if (contentsObject) {
        std::string contentsBody = GetCurrentPdfObjectBody(*contentsObject, replacements);
        size_t arrayStart = 0;
        while (arrayStart < contentsBody.size() &&
               std::isspace(static_cast<unsigned char>(contentsBody[arrayStart]))) {
            arrayStart++;
        }
        if (arrayStart < contentsBody.size() && contentsBody[arrayStart] == '[') {
            const size_t arrayEnd = contentsBody.find(']', arrayStart + 1);
            if (arrayEnd == std::string::npos) return false;
            contentsBody.insert(arrayEnd, " " + suffixRef);
            contentsBody.insert(arrayStart + 1, " " + prefixRef);
            if (!UpsertPdfObjectReplacement(
                    replacements,
                    contentsObjectNumber,
                    contentsGeneration,
                    contentsBody)) {
                return false;
            }
            *outBody = std::move(body);
            return true;
        }
    }

    body.replace(
            valueStart,
            refEnd - valueStart,
            "[ " + prefixRef + " " +
            std::to_string(contentsObjectNumber) + " " +
            std::to_string(contentsGeneration) + " R " + suffixRef + " ]"
    );
    *outBody = std::move(body);
    return true;
}

static void NativeCollectIndirectRefsInRange(
        const std::string& body,
        size_t start,
        size_t end,
        std::set<std::pair<int, int>>* refs
) {
    if (!refs || start >= end || end > body.size()) return;
    for (size_t cursor = start; cursor < end; cursor++) {
        if (!std::isdigit(static_cast<unsigned char>(body[cursor]))) continue;
        int objectNumber = 0;
        int generation = 0;
        size_t refEnd = cursor;
        if (ParseIndirectReferenceAt(
                body, cursor, end, &objectNumber, &generation, &refEnd)) {
            refs->insert({objectNumber, generation});
            cursor = refEnd - 1;
        }
    }
}

static bool NativeCollectPatternRefsFromResources(
        const std::string& resourcesBody,
        const std::vector<PdfObjectInfo>& objects,
        std::set<std::pair<int, int>>* refs
) {
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!refs || !FindTopLevelPdfDictionary(resourcesBody, &dictStart, &dictEnd)) return false;

    size_t patternStart = 0;
    size_t patternEnd = 0;
    if (FindDirectDictionaryValue(
            resourcesBody, dictStart, dictEnd, "Pattern", &patternStart, &patternEnd)) {
        NativeCollectIndirectRefsInRange(resourcesBody, patternStart, patternEnd, refs);
        return true;
    }

    int patternObjectNumber = 0;
    int patternGeneration = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (!FindIndirectReferenceValue(
            resourcesBody,
            dictStart,
            dictEnd,
            "Pattern",
            &patternObjectNumber,
            &patternGeneration,
            &valueStart,
            &valueEnd)) {
        return true;
    }
    const PdfObjectInfo* patternDictionary =
            FindPdfObjectInfoByRef(objects, patternObjectNumber, patternGeneration);
    if (!patternDictionary) return false;
    size_t patternDictStart = 0;
    size_t patternDictEnd = 0;
    if (!FindTopLevelPdfDictionary(
            patternDictionary->body, &patternDictStart, &patternDictEnd)) {
        return false;
    }
    NativeCollectIndirectRefsInRange(
            patternDictionary->body, patternDictStart, patternDictEnd, refs);
    return true;
}

static bool NativeCollectPagePatternRefs(
        const std::string& pageBody,
        const std::vector<PdfObjectInfo>& objects,
        std::set<std::pair<int, int>>* refs,
        int depth = 0
) {
    if (!refs || depth > 32) return false;
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!FindTopLevelPdfDictionary(pageBody, &dictStart, &dictEnd)) return false;

    size_t resourcesStart = 0;
    size_t resourcesEnd = 0;
    if (FindDirectDictionaryValue(
            pageBody, dictStart, dictEnd, "Resources", &resourcesStart, &resourcesEnd)) {
        const std::string resourcesBody =
                pageBody.substr(resourcesStart, resourcesEnd - resourcesStart);
        return NativeCollectPatternRefsFromResources(resourcesBody, objects, refs);
    }

    int resourcesObjectNumber = 0;
    int resourcesGeneration = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (FindIndirectReferenceValue(
            pageBody,
            dictStart,
            dictEnd,
            "Resources",
            &resourcesObjectNumber,
            &resourcesGeneration,
            &valueStart,
            &valueEnd)) {
        const PdfObjectInfo* resourcesObject =
                FindPdfObjectInfoByRef(objects, resourcesObjectNumber, resourcesGeneration);
        return resourcesObject &&
               NativeCollectPatternRefsFromResources(resourcesObject->body, objects, refs);
    }

    int parentObjectNumber = 0;
    int parentGeneration = 0;
    if (!FindIndirectReferenceValue(
            pageBody,
            dictStart,
            dictEnd,
            "Parent",
            &parentObjectNumber,
            &parentGeneration,
            &valueStart,
            &valueEnd)) {
        return true;
    }
    const PdfObjectInfo* parentObject =
            FindPdfObjectInfoByRef(objects, parentObjectNumber, parentGeneration);
    return parentObject &&
           NativeCollectPagePatternRefs(parentObject->body, objects, refs, depth + 1);
}

static bool NativeTransformPatternMatrix(
        const std::string& originalBody,
        const FS_MATRIX& pageMatrix,
        std::string* outBody
) {
    if (!outBody ||
        !ContainsPdfNameValue(originalBody, "Type", "Pattern")) {
        return false;
    }
    size_t dictStart = 0;
    size_t dictEnd = 0;
    if (!FindTopLevelPdfDictionary(originalBody, &dictStart, &dictEnd)) return false;

    FS_MATRIX patternMatrix = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    size_t matrixStart = 0;
    size_t matrixEnd = 0;
    const bool hasMatrix = FindPdfDictionaryRawValueSegment(
            originalBody, dictStart, dictEnd, "Matrix", &matrixStart, &matrixEnd);
    if (hasMatrix) {
        std::string rawMatrix = originalBody.substr(matrixStart, matrixEnd - matrixStart);
        std::replace(rawMatrix.begin(), rawMatrix.end(), '[', ' ');
        std::replace(rawMatrix.begin(), rawMatrix.end(), ']', ' ');
        std::istringstream values(rawMatrix);
        if (!(values >> patternMatrix.a >> patternMatrix.b >>
              patternMatrix.c >> patternMatrix.d >>
              patternMatrix.e >> patternMatrix.f)) {
            return false;
        }
    }

    FS_MATRIX transformed;
    transformed.a = pageMatrix.a * patternMatrix.a + pageMatrix.c * patternMatrix.b;
    transformed.b = pageMatrix.b * patternMatrix.a + pageMatrix.d * patternMatrix.b;
    transformed.c = pageMatrix.a * patternMatrix.c + pageMatrix.c * patternMatrix.d;
    transformed.d = pageMatrix.b * patternMatrix.c + pageMatrix.d * patternMatrix.d;
    transformed.e = pageMatrix.a * patternMatrix.e +
                    pageMatrix.c * patternMatrix.f + pageMatrix.e;
    transformed.f = pageMatrix.b * patternMatrix.e +
                    pageMatrix.d * patternMatrix.f + pageMatrix.f;
    const std::string matrixValue =
            "[" + FormatPdfFloat(transformed.a) + " " +
            FormatPdfFloat(transformed.b) + " " +
            FormatPdfFloat(transformed.c) + " " +
            FormatPdfFloat(transformed.d) + " " +
            FormatPdfFloat(transformed.e) + " " +
            FormatPdfFloat(transformed.f) + "]";

    std::string body = originalBody;
    if (hasMatrix) {
        body.replace(matrixStart, matrixEnd - matrixStart, matrixValue);
    } else {
        body.insert(dictEnd - 2, "/Matrix " + matrixValue + " ");
    }
    *outBody = std::move(body);
    return true;
}

static bool NativePatchPageSizeContentMatrices(
        const char* outputPath,
        const std::map<int, FS_MATRIX>& pageMatrices
) {
    if (!outputPath || pageMatrices.empty()) return false;
    std::string data;
    if (!ReadFileToString(outputPath, &data)) return false;

    const std::vector<PdfObjectInfo> objects = ScanPdfObjects(data);
    const std::vector<PdfObjectInfo> latestObjects = BuildLatestPdfObjectsByRef(objects);
    std::vector<PdfObjectInfo> pages = BuildPdfPageObjectsFromCatalog(data, latestObjects);
    if (pages.empty()) {
        pages = BuildLatestPdfPageObjects(objects, latestObjects);
    }
    if (pages.empty()) return false;

    int nextObjectNumber = GetMaxPdfObjectNumber(objects) + 1;
    std::vector<PdfObjectReplacement> replacements;
    std::map<std::pair<int, int>, FS_MATRIX> patternMatrices;
    int patchedPages = 0;
    for (const auto& pageMatrix : pageMatrices) {
        const int pageIndex = pageMatrix.first;
        if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pages.size()) return false;

        const int prefixObjectNumber = nextObjectNumber++;
        const int suffixObjectNumber = nextObjectNumber++;
        std::string pageBody = GetCurrentPdfObjectBody(pages[pageIndex], &replacements);
        if (!NativeWrapPageContentsWithMatrix(
                pageBody,
                latestObjects,
                &replacements,
                prefixObjectNumber,
                suffixObjectNumber,
                &pageBody)) {
            return false;
        }

        const FS_MATRIX& matrix = pageMatrix.second;
        const std::string matrixStream =
                "q\n" +
                FormatPdfFloat(matrix.a) + " " +
                FormatPdfFloat(matrix.b) + " " +
                FormatPdfFloat(matrix.c) + " " +
                FormatPdfFloat(matrix.d) + " " +
                FormatPdfFloat(matrix.e) + " " +
                FormatPdfFloat(matrix.f) + " cm\n";
        replacements.push_back({
                prefixObjectNumber,
                0,
                "<< /Length " + std::to_string(matrixStream.size()) +
                " >>\nstream\n" + matrixStream + "endstream"
        });
        replacements.push_back({
                suffixObjectNumber,
                0,
                "<< /Length 2 >>\nstream\nQ\nendstream"
        });
        if (!UpsertPdfObjectReplacement(
                &replacements,
                pages[pageIndex].objectNumber,
                pages[pageIndex].generation,
                pageBody)) {
            return false;
        }
        std::set<std::pair<int, int>> patternRefs;
        if (!NativeCollectPagePatternRefs(
                pages[pageIndex].body, latestObjects, &patternRefs)) {
            return false;
        }
        for (const auto& patternRef : patternRefs) {
            patternMatrices.emplace(patternRef, matrix);
        }
        patchedPages++;
    }

    for (const auto& patternMatrix : patternMatrices) {
        const PdfObjectInfo* patternObject = FindPdfObjectInfoByRef(
                latestObjects, patternMatrix.first.first, patternMatrix.first.second);
        if (!patternObject) return false;
        std::string transformedPatternBody;
        if (!NativeTransformPatternMatrix(
                patternObject->body, patternMatrix.second, &transformedPatternBody)) {
            continue;
        }
        if (!UpsertPdfObjectReplacement(
                &replacements,
                patternObject->objectNumber,
                patternObject->generation,
                transformedPatternBody)) {
            return false;
        }
    }

    if (patchedPages != static_cast<int>(pageMatrices.size()) ||
        !AppendIncrementalPdfObjectUpdates(&data, &replacements)) {
        return false;
    }
    return WriteStringToFile(outputPath, data);
}

static bool NativeResolvePageSize(FPDF_PAGE page, float* left, float* bottom, float* width, float* height) {
    if (!page || !left || !bottom || !width || !height) return false;

    float mediaLeft = 0.0f;
    float mediaBottom = 0.0f;
    float mediaRight = 0.0f;
    float mediaTop = 0.0f;
    if (FPDFPage_GetMediaBox(page, &mediaLeft, &mediaBottom, &mediaRight, &mediaTop) &&
        mediaRight > mediaLeft &&
        mediaTop > mediaBottom) {
        *left = mediaLeft;
        *bottom = mediaBottom;
        *width = mediaRight - mediaLeft;
        *height = mediaTop - mediaBottom;
        return true;
    }

    *left = 0.0f;
    *bottom = 0.0f;
    *width = static_cast<float>(FPDF_GetPageWidth(page));
    *height = static_cast<float>(FPDF_GetPageHeight(page));
    return *width > 0.0f && *height > 0.0f;
}

struct NativeTextMarkupGeometry {
    int annotIndex = -1;
    int subtype = -1;
    std::vector<FS_QUADPOINTSF> attachmentPoints;
};

static FS_POINTF NativeTransformPoint(const FS_MATRIX& matrix, float x, float y) {
    FS_POINTF point;
    point.x = (matrix.a * x) + (matrix.c * y) + matrix.e;
    point.y = (matrix.b * x) + (matrix.d * y) + matrix.f;
    return point;
}

static FS_RECTF NativeNormalizeRect(
        const FS_POINTF& first,
        const FS_POINTF& second,
        const FS_POINTF& third,
        const FS_POINTF& fourth
) {
    FS_RECTF rect;
    rect.left = std::min({first.x, second.x, third.x, fourth.x});
    rect.right = std::max({first.x, second.x, third.x, fourth.x});
    rect.bottom = std::min({first.y, second.y, third.y, fourth.y});
    rect.top = std::max({first.y, second.y, third.y, fourth.y});
    return rect;
}

static FS_QUADPOINTSF NativeTransformQuad(const FS_MATRIX& matrix, const FS_QUADPOINTSF& quad) {
    const FS_POINTF p1 = NativeTransformPoint(matrix, quad.x1, quad.y1);
    const FS_POINTF p2 = NativeTransformPoint(matrix, quad.x2, quad.y2);
    const FS_POINTF p3 = NativeTransformPoint(matrix, quad.x3, quad.y3);
    const FS_POINTF p4 = NativeTransformPoint(matrix, quad.x4, quad.y4);

    FS_QUADPOINTSF transformed;
    transformed.x1 = p1.x;
    transformed.y1 = p1.y;
    transformed.x2 = p2.x;
    transformed.y2 = p2.y;
    transformed.x3 = p3.x;
    transformed.y3 = p3.y;
    transformed.x4 = p4.x;
    transformed.y4 = p4.y;
    return transformed;
}

static bool NativeIsTextMarkupSubtype(int subtype) {
    return subtype == FPDF_ANNOT_HIGHLIGHT ||
           subtype == FPDF_ANNOT_UNDERLINE ||
           subtype == FPDF_ANNOT_STRIKEOUT ||
           subtype == FPDF_ANNOT_SQUIGGLY;
}

static int NativeTextMarkupSubtypeToTypeInt(int subtype) {
    switch (subtype) {
        case FPDF_ANNOT_UNDERLINE:
            return 1;
        case FPDF_ANNOT_STRIKEOUT:
            return 2;
        case FPDF_ANNOT_SQUIGGLY:
            return 8;
        case FPDF_ANNOT_HIGHLIGHT:
        default:
            return 0;
    }
}

static FS_RECTF NativeQuadBounds(const FS_QUADPOINTSF& quad) {
    const FS_POINTF p1 = {quad.x1, quad.y1};
    const FS_POINTF p2 = {quad.x2, quad.y2};
    const FS_POINTF p3 = {quad.x3, quad.y3};
    const FS_POINTF p4 = {quad.x4, quad.y4};
    return NativeNormalizeRect(p1, p2, p3, p4);
}

static void NativeExpandRect(FS_RECTF* base, const FS_RECTF& add) {
    if (!base) return;
    base->left = std::min(base->left, add.left);
    base->right = std::max(base->right, add.right);
    base->bottom = std::min(base->bottom, add.bottom);
    base->top = std::max(base->top, add.top);
}

static std::vector<NativeTextMarkupGeometry> NativeCollectTextMarkupGeometry(FPDF_PAGE page) {
    std::vector<NativeTextMarkupGeometry> geometries;
    if (!page) return geometries;

    const int annotCount = FPDFPage_GetAnnotCount(page);
    for (int annotIndex = 0; annotIndex < annotCount; annotIndex++) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotIndex);
        if (annot) {
            const int subtype = FPDFAnnot_GetSubtype(annot);
            if (NativeIsTextMarkupSubtype(subtype)) {
                NativeTextMarkupGeometry geometry;
                geometry.annotIndex = annotIndex;
                geometry.subtype = subtype;
                const int quadCount = FPDFAnnot_CountAttachmentPoints(annot);
                geometry.attachmentPoints.reserve(std::max(quadCount, 0));
                for (int quadIndex = 0; quadIndex < quadCount; quadIndex++) {
                    FS_QUADPOINTSF quad;
                    if (FPDFAnnot_GetAttachmentPoints(annot, quadIndex, &quad)) {
                        geometry.attachmentPoints.push_back(quad);
                    }
                }
                if (!geometry.attachmentPoints.empty()) {
                    geometries.push_back(std::move(geometry));
                }
            }
            FPDFPage_CloseAnnot(annot);
        }
    }
    return geometries;
}

static void NativeFixTextMarkupAfterPageSizeScale(
        FPDF_PAGE page,
        const FS_MATRIX& matrix,
        const std::vector<NativeTextMarkupGeometry>& geometries
) {
    if (!page || geometries.empty()) return;

    const int annotCount = FPDFPage_GetAnnotCount(page);
    for (const NativeTextMarkupGeometry& geometry : geometries) {
        if (geometry.annotIndex < 0 || geometry.annotIndex >= annotCount) continue;
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, geometry.annotIndex);
        if (!annot) continue;
        if (FPDFAnnot_GetSubtype(annot) != geometry.subtype) {
            FPDFPage_CloseAnnot(annot);
            continue;
        }

        bool hasBounds = false;
        FS_RECTF bounds = {0.0f, 0.0f, 0.0f, 0.0f};
        std::vector<FS_RECTF> transformedQuadBounds;
        const int currentQuadCount = FPDFAnnot_CountAttachmentPoints(annot);
        const int quadCount = static_cast<int>(geometry.attachmentPoints.size());
        transformedQuadBounds.reserve(std::max(quadCount, 0));
        for (int quadIndex = 0; quadIndex < quadCount; quadIndex++) {
            FS_QUADPOINTSF transformedQuad = NativeTransformQuad(
                    matrix,
                    geometry.attachmentPoints[quadIndex]
            );
            if (quadIndex < currentQuadCount) {
                FPDFAnnot_SetAttachmentPoints(annot, quadIndex, &transformedQuad);
            }
            const FS_RECTF quadBounds = NativeQuadBounds(transformedQuad);
            transformedQuadBounds.push_back(quadBounds);
            if (!hasBounds) {
                bounds = quadBounds;
                hasBounds = true;
            } else {
                NativeExpandRect(&bounds, quadBounds);
            }
        }

        if (hasBounds) {
            FPDFAnnot_SetRect(annot, &bounds);
            FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr);

            unsigned int r = 255;
            unsigned int g = 255;
            unsigned int b = 0;
            unsigned int a = 125;
            if (!FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &r, &g, &b, &a)) {
                FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, &r, &g, &b, &a);
            }
            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, a);
            if (geometry.subtype == FPDF_ANNOT_HIGHLIGHT) {
                FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, a);
                const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
                FPDFAnnot_SetStringValue(annot, "BM", (FPDF_WIDESTRING)blendMode);
            } else {
                const int typeInt = NativeTextMarkupSubtypeToTypeInt(geometry.subtype);
                for (const FS_RECTF& quadBounds : transformedQuadBounds) {
                    AppendFallbackTextMarkupAppearance(
                            annot,
                            typeInt,
                            quadBounds,
                            static_cast<int>(r),
                            static_cast<int>(g),
                            static_cast<int>(b),
                            static_cast<int>(a)
                    );
                }
            }
        }

        FPDFPage_CloseAnnot(annot);
    }
}

static bool NativeApplyPageSizeScale(
        FPDF_PAGE page,
        float targetWidth,
        float targetHeight,
        int scaleMode,
        float scaleFactor,
        float* outScaleX = nullptr,
        float* outScaleY = nullptr,
        FS_MATRIX* outMatrix = nullptr
) {
    float sourceLeft = 0.0f;
    float sourceBottom = 0.0f;
    float sourceWidth = 0.0f;
    float sourceHeight = 0.0f;
    if (!NativeResolvePageSize(page, &sourceLeft, &sourceBottom, &sourceWidth, &sourceHeight)) {
        return false;
    }

    const float finalWidth = targetWidth > 0.0f ? targetWidth : sourceWidth;
    const float finalHeight = targetHeight > 0.0f ? targetHeight : sourceHeight;
    const float factor = std::isfinite(scaleFactor) && scaleFactor > 0.0f ? scaleFactor : 1.0f;
    if (finalWidth <= 0.0f || finalHeight <= 0.0f) {
        return false;
    }

    float scaleX = factor;
    float scaleY = factor;
    const float fitScale = std::min(finalWidth / sourceWidth, finalHeight / sourceHeight);
    const float fillScale = std::max(finalWidth / sourceWidth, finalHeight / sourceHeight);
    switch (scaleMode) {
        case 1:
            scaleX = fitScale * factor;
            scaleY = fitScale * factor;
            break;
        case 2:
            scaleX = fillScale * factor;
            scaleY = fillScale * factor;
            break;
        case 3:
            scaleX = (finalWidth / sourceWidth) * factor;
            scaleY = (finalHeight / sourceHeight) * factor;
            break;
        case 0:
        case 4:
        default:
            scaleX = factor;
            scaleY = factor;
            break;
    }
    if (outScaleX) *outScaleX = scaleX;
    if (outScaleY) *outScaleY = scaleY;

    const float offsetX = (finalWidth - (sourceWidth * scaleX)) * 0.5f;
    const float offsetY = (finalHeight - (sourceHeight * scaleY)) * 0.5f;
    FS_MATRIX matrix;
    matrix.a = scaleX;
    matrix.b = 0.0f;
    matrix.c = 0.0f;
    matrix.d = scaleY;
    matrix.e = offsetX - (sourceLeft * scaleX);
    matrix.f = offsetY - (sourceBottom * scaleY);
    if (outMatrix) *outMatrix = matrix;

    const std::vector<NativeTextMarkupGeometry> textMarkupGeometry =
            NativeCollectTextMarkupGeometry(page);
    FPDFPage_TransformAnnots(page, matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f);
    NativeFixTextMarkupAfterPageSizeScale(page, matrix, textMarkupGeometry);
    FPDFPage_SetMediaBox(page, 0.0f, 0.0f, finalWidth, finalHeight);
    FPDFPage_SetCropBox(page, 0.0f, 0.0f, finalWidth, finalHeight);
    return true;
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSavePdfPageSizeScale(
        JNIEnv* env,
        jobject thiz,
        jstring inputPath_,
        jstring outputPath_,
        jfloat targetWidth,
        jfloat targetHeight,
        jint scaleMode,
        jfloat scaleFactor,
        jintArray pageIndices_
) {
    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);

    std::set<int> requestedPages;
    if (pageIndices_) {
        const int indexCount = env->GetArrayLength(pageIndices_);
        std::vector<jint> indices(indexCount);
        if (indexCount > 0) {
            env->GetIntArrayRegion(pageIndices_, 0, indexCount, indices.data());
            for (int i = 0; i < indexCount; i++) {
                if (indices[i] >= 0) {
                    requestedPages.insert(indices[i]);
                }
            }
        }
    }

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) {
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    const int pageCount = FPDF_GetPageCount(doc);
    bool appliedAnyPage = false;
    bool failed = false;
    std::map<int, FS_MATRIX> pageMatrices;
    for (int pageIndex = 0; pageIndex < pageCount; pageIndex++) {
        if (!NativePageIndexRequested(requestedPages, pageIndex)) continue;

        FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
        if (!page) {
            failed = true;
            break;
        }

        FS_MATRIX pageMatrix;
        const bool applied = NativeApplyPageSizeScale(
                page,
                targetWidth,
                targetHeight,
                scaleMode,
                scaleFactor,
                nullptr,
                nullptr,
                &pageMatrix
        );
        FPDF_ClosePage(page);
        if (!applied) {
            failed = true;
            break;
        }
        pageMatrices[pageIndex] = pageMatrix;
        appliedAnyPage = true;
    }

    int success = JNI_FALSE;
    if (!failed && appliedAnyPage) {
        FILE* file = fopen(outputPath, "wb");
        PdfFileWriter writer{ {1, WriteBlock}, file };
        success = (file) ? FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL) : JNI_FALSE;
        if (file) fclose(file);
        if (success && !NativePatchPageSizeContentMatrices(outputPath, pageMatrices)) {
            success = JNI_FALSE;
        }
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveEdit(
        JNIEnv* env,
    jobject thiz,
    jstring inputPath_,
    jstring outputPath_,
    jobjectArray watermarksArray
) {
    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);

    jclass watermarkClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfWatermarkNative");
    jfieldID dataPropsField = env->GetFieldID(watermarkClass, "dataProperties", "Ljava/lang/String;");
    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");

    const int watermarkCount = watermarksArray ? env->GetArrayLength(watermarksArray) : 0;
    std::vector<RawPdfWatermarkSpec> rawWatermarkSpecs;
    rawWatermarkSpecs.reserve(std::max(0, watermarkCount));

    for (int i = 0; i < watermarkCount; i++) {
        jobject obj = env->GetObjectArrayElement(watermarksArray, i);
        if (!obj) continue;

        RawPdfWatermarkSpec spec;
        if (CollectPageLevelTextWatermarkPatternSpec(env, obj, dataPropsField, jsonClass, jsonInit, &spec)) {
            rawWatermarkSpecs.push_back(spec);
        }
        env->DeleteLocalRef(obj);
    }

    const bool editContentInput = PdfFileHasEditContentMarker(inputPath);
    int success = JNI_FALSE;
    if (editContentInput) {
        success = CopyFileBinary(inputPath, outputPath) ? JNI_TRUE : JNI_FALSE;
    }
    if (!success && !editContentInput) {
        FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
        if (doc) {
            FILE* file = fopen(outputPath, "wb");
            PdfFileWriter writer{ {1, WriteBlock}, file };
            success = (file) ? FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL) : JNI_FALSE;
            if (file) fclose(file);
            FPDF_CloseDocument(doc);
        }
    }
    if (!success) {
        success = CopyFileBinary(inputPath, outputPath) ? JNI_TRUE : JNI_FALSE;
    }

    if (success && watermarkCount > 0 && rawWatermarkSpecs.empty()) {
        success = JNI_FALSE;
    }
    if (success && !rawWatermarkSpecs.empty()) {
        const bool watermarkPatched = PatchRawPdfWatermarkPatterns(env, outputPath, rawWatermarkSpecs);
        if (!watermarkPatched) {
            success = JNI_FALSE;
        }
    }
    if (success && watermarkCount == 0 && !PatchRawPdfWatermarkPatterns(env, outputPath, rawWatermarkSpecs)) {
        success = JNI_FALSE;
    }
    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);
    return success ? JNI_TRUE : JNI_FALSE;
}

// dummy native save testing
JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeDummySave(
        JNIEnv* env,
        jobject thiz,
        jstring inputPath_,
        jstring outputPath_) {

    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);

    LOGD("DUMMY SAVE - Opening PDF: %s", inputPath);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) {
        LOGE("Failed to load PDF");
        return JNI_FALSE;
    }

    // Load first page
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    if (!page) {
        LOGE("Failed to load page 0");
        FPDF_CloseDocument(doc);
        return JNI_FALSE;
    }

    float pageWidth = FPDF_GetPageWidth(page);
    float pageHeight = FPDF_GetPageHeight(page);

    LOGD("Page size W=%f H=%f", pageWidth, pageHeight);

    // Create annotation (TRY SQUARE FIRST - 100% visible)
    FPDF_ANNOTATION annot =
            FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE);

    if (!annot) {
        LOGE("Failed to create annotation");
        FPDF_ClosePage(page);
        FPDF_CloseDocument(doc);
        return JNI_FALSE;
    }

    // Big center rectangle
    float left = pageWidth * 0.25f;
    float right = pageWidth * 0.75f;
    float bottom = pageHeight * 0.25f;
    float top = pageHeight * 0.75f;

    LOGD("DUMMY RECT L=%f R=%f T=%f B=%f", left, right, top, bottom);

    FS_RECTF rect;
    rect.left = left;
    rect.right = right;
    rect.top = top;
    rect.bottom = bottom;

    FPDFAnnot_SetRect(annot, &rect);

    // Bright red color
    FPDFAnnot_SetColor(
            annot,
            FPDFANNOT_COLORTYPE_Color,
            255, 0, 0,
            255
    );

    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);

    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);

    // Save
    FILE* file = fopen(outputPath, "wb");
    if (!file) {
        LOGE("Failed to open output file");
        FPDF_CloseDocument(doc);
        return JNI_FALSE;
    }

    PdfFileWriter writer;
    memset(&writer, 0, sizeof(writer));
    writer.base.version = 1;
    writer.base.WriteBlock = WriteBlock;
    writer.file = file;

    int success = FPDF_SaveAsCopy(
            doc,
            (FPDF_FILEWRITE*)&writer,
            FPDF_NO_INCREMENTAL
    );

    fclose(file);
    FPDF_CloseDocument(doc);

    LOGD("DUMMY SAVE result: %d", success);

    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);

    return success ? JNI_TRUE : JNI_FALSE;
}

// get annotation part for kotlin
#include <vector>
#include <algorithm>
#include <map>
#include <functional>

static bool GetNativeFormPaintedBounds(
        FPDF_PAGE page,
        FPDF_PAGEOBJECT targetForm,
        int requestedWidth,
        int requestedHeight,
        float* outLeft,
        float* outBottom,
        float* outRight,
        float* outTop) {
    if (!page || !targetForm || !outLeft || !outBottom || !outRight || !outTop) {
        return false;
    }

    const int pageObjectCount = FPDFPage_CountObjects(page);
    if (pageObjectCount <= 0) return false;

    const int sourceWidth = std::max(1, requestedWidth);
    const int sourceHeight = std::max(1, requestedHeight);
    constexpr float kMaximumRenderSide = 1536.0f;
    const float renderScale = std::min(
            1.0f,
            kMaximumRenderSide / static_cast<float>(std::max(sourceWidth, sourceHeight))
    );
    const int renderWidth =
            std::max(1, static_cast<int>(ceilf(sourceWidth * renderScale)));
    const int renderHeight =
            std::max(1, static_cast<int>(ceilf(sourceHeight * renderScale)));
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
            renderWidth,
            renderHeight,
            FPDFBitmap_BGRA,
            nullptr,
            0
    );
    if (!bitmap) return false;

    struct NativePageObjectActiveState {
        FPDF_PAGEOBJECT object;
        FPDF_BOOL active;
    };
    std::vector<NativePageObjectActiveState> activeStates;
    activeStates.reserve(pageObjectCount);
    for (int objectIndex = 0; objectIndex < pageObjectCount; ++objectIndex) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectIndex);
        if (!object) continue;
        FPDF_BOOL active = true;
        FPDFPageObj_GetIsActive(object, &active);
        activeStates.push_back({object, active});
        FPDFPageObj_SetIsActive(object, object == targetForm ? true : false);
    }

    FPDFBitmap_FillRect(bitmap, 0, 0, renderWidth, renderHeight, 0x00000000);
    FPDF_RenderPageBitmap(
            bitmap,
            page,
            0,
            0,
            renderWidth,
            renderHeight,
            0,
            0
    );
    for (const auto& state : activeStates) {
        FPDFPageObj_SetIsActive(state.object, state.active);
    }

    auto* pixels = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    const int stride = FPDFBitmap_GetStride(bitmap);
    if (!pixels || stride <= 0) {
        FPDFBitmap_Destroy(bitmap);
        return false;
    }

    int paintedLeft = renderWidth;
    int paintedTop = renderHeight;
    int paintedRight = -1;
    int paintedBottom = -1;
    constexpr uint8_t kPaintedAlphaThreshold = 8;
    for (int y = 0; y < renderHeight; ++y) {
        const uint8_t* row = pixels + (y * stride);
        for (int x = 0; x < renderWidth; ++x) {
            if (row[(x * 4) + 3] <= kPaintedAlphaThreshold) continue;
            paintedLeft = std::min(paintedLeft, x);
            paintedTop = std::min(paintedTop, y);
            paintedRight = std::max(paintedRight, x);
            paintedBottom = std::max(paintedBottom, y);
        }
    }
    if (paintedRight < paintedLeft || paintedBottom < paintedTop) {
        FPDFBitmap_Destroy(bitmap);
        return false;
    }

    paintedLeft = std::max(0, paintedLeft - 1);
    paintedTop = std::max(0, paintedTop - 1);
    paintedRight = std::min(renderWidth, paintedRight + 2);
    paintedBottom = std::min(renderHeight, paintedBottom + 2);

    double firstX = 0.0;
    double firstY = 0.0;
    double secondX = 0.0;
    double secondY = 0.0;
    const bool convertedFirstPoint = FPDF_DeviceToPage(
            page,
            0,
            0,
            renderWidth,
            renderHeight,
            0,
            paintedLeft,
            paintedTop,
            &firstX,
            &firstY
    );
    const bool convertedSecondPoint = FPDF_DeviceToPage(
            page,
            0,
            0,
            renderWidth,
            renderHeight,
            0,
            paintedRight,
            paintedBottom,
            &secondX,
            &secondY
    );
    FPDFBitmap_Destroy(bitmap);
    if (!convertedFirstPoint || !convertedSecondPoint) return false;

    *outLeft = static_cast<float>(std::min(firstX, secondX));
    *outRight = static_cast<float>(std::max(firstX, secondX));
    *outBottom = static_cast<float>(std::min(firstY, secondY));
    *outTop = static_cast<float>(std::max(firstY, secondY));
    return std::isfinite(*outLeft) &&
           std::isfinite(*outBottom) &&
           std::isfinite(*outRight) &&
           std::isfinite(*outTop) &&
           *outRight - *outLeft >= 0.0001f &&
           *outTop - *outBottom >= 0.0001f;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeReadFileAttachment(
        JNIEnv* env,
        jobject,
        jlong docPtr,
        jint pageIndex,
        jint annotationIndex) {
    DocumentFile* docFile = reinterpret_cast<DocumentFile*>(docPtr);
    if (!docFile || !docFile->pdfDocument || pageIndex < 0 || annotationIndex < 0) {
        return nullptr;
    }
    FPDF_DOCUMENT doc = docFile->pdfDocument;

    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return nullptr;
    jbyteArray result = nullptr;
    if (annotationIndex < FPDFPage_GetAnnotCount(page)) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotationIndex);
        if (annot && FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_FILEATTACHMENT) {
            FPDF_ATTACHMENT attachment = FPDFAnnot_GetFileAttachment(annot);
            unsigned long fileSize = 0;
            if (attachment &&
                FPDFAttachment_GetFile(attachment, nullptr, 0, &fileSize) &&
                fileSize <= 2147483647UL) {
                std::vector<unsigned char> bytes(fileSize);
                unsigned long actualSize = 0;
                if (FPDFAttachment_GetFile(
                        attachment,
                        bytes.empty() ? nullptr : bytes.data(),
                        fileSize,
                        &actualSize) &&
                    actualSize <= fileSize) {
                    result = env->NewByteArray(static_cast<jsize>(actualSize));
                    if (result && actualSize > 0) {
                        env->SetByteArrayRegion(
                                result,
                                0,
                                static_cast<jsize>(actualSize),
                                reinterpret_cast<const jbyte*>(bytes.data()));
                    }
                }
            }
        }
        if (annot) FPDFPage_CloseAnnot(annot);
    }
    FPDF_ClosePage(page);
    return result;
}

JNIEXPORT jobjectArray JNICALL //todo main get annotation method
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeGetAnnotationsForPage(
        JNIEnv* env,
        jobject thiz,
        jlong docPtr,
        jlong pagePtr,
        jint pageIndex,
        jint viewWidth,
        jint viewHeight,
        jboolean includeEditContent,
        jboolean includeImageBitmaps) {

    FPDF_DOCUMENT doc = (FPDF_DOCUMENT) docPtr;
    FPDF_PAGE page = (FPDF_PAGE) pagePtr;
    if (!doc || !page) return nullptr;

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    int annotCount = FPDFPage_GetAnnotCount(page);
    int charCount = FPDFText_CountChars(textPage);

    jclass annotClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfAnnotationNative");
    jmethodID constructor = env->GetMethodID(
            annotClass,
            "<init>",
            "(IIFFFFIIIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/graphics/Bitmap;II)V"
    );
    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonPut = env->GetMethodID(jsonClass, "put", "(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;");
    jmethodID jsonToString = env->GetMethodID(jsonClass, "toString", "()Ljava/lang/String;");

    // Use a vector to prevent ArrayIndexOutOfBoundsException
    std::vector<jobject> tempCollector;
    struct LoadedNativeAnnotBounds {
        float left;
        float top;
        float right;
        float bottom;
        unsigned int r;
        unsigned int g;
        unsigned int b;
        unsigned int a;
    };
    std::vector<LoadedNativeAnnotBounds> loadedNativeAnnotBounds;

    for (int i = 0; !includeEditContent && i < annotCount; i++) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
        if (!annot) continue;

        int subtype = FPDFAnnot_GetSubtype(annot);
        int type = -1;
        bool usesRectOnly = false;

        switch (subtype) {
            case FPDF_ANNOT_HIGHLIGHT: type = 0; break;
            case FPDF_ANNOT_UNDERLINE: type = 1; break;
            case FPDF_ANNOT_STRIKEOUT: type = 2; break;
            case FPDF_ANNOT_SQUIGGLY:  type = 8; break;
            case FPDF_ANNOT_LINK:
                type = 3;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_FREETEXT:
                type = 11;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_TEXT:
                type = 10;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_FILEATTACHMENT:
                type = 21;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_INK:
                type = 6;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_STAMP:
                type = 5;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_LINE:
                type = 17;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_CIRCLE:
                type = 14;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_POLYGON:
                type = 15;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_POLYLINE:
                type = 16;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_SQUARE:
                type = 4;
                usesRectOnly = true;
                break;
        }

        if (type == -1) {
            FPDFPage_CloseAnnot(annot);
            continue;
        }

        unsigned int r=0, g=0, b=0, a=255;
        bool hasStrokeColor = FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &r, &g, &b, &a);
        unsigned int interiorR = r, interiorG = g, interiorB = b, interiorA = a;
        bool hasInteriorColor = FPDFAnnot_GetColor(
                annot,
                FPDFANNOT_COLORTYPE_InteriorColor,
                &interiorR,
                &interiorG,
                &interiorB,
                &interiorA
        );
        ResolveAnnotAppearanceColors(
                annot,
                &hasStrokeColor,
                &r,
                &g,
                &b,
                &a,
                &hasInteriorColor,
                &interiorR,
                &interiorG,
                &interiorB,
                &interiorA
        );
        if (type == 0) {
            if (hasInteriorColor) {
                r = interiorR;
                g = interiorG;
                b = interiorB;
                a = interiorA;
            }
        } else if (subtype == FPDF_ANNOT_SQUARE && !IsPdfShapeNativeType(type)) {
            const std::u16string redactionMarker = ReadAnnotStringValueUtf16(annot, "LufickPdfRedaction");
            const std::u16string areaMarkupMarker = ReadAnnotStringValueUtf16(annot, "LufickAreaMarkup");
            const bool isAreaMarkup =
                    !areaMarkupMarker.empty() &&
                    areaMarkupMarker != u"0" &&
                    areaMarkupMarker != u"false" &&
                    areaMarkupMarker != u"FALSE";
            const bool looksLikeRedaction =
                    !redactionMarker.empty() ||
                    (!isAreaMarkup &&
                     (hasStrokeColor && r == 0 && g == 0 && b == 0 && a == 255) &&
                     (!hasInteriorColor || (interiorR == 0 && interiorG == 0 && interiorB == 0 && interiorA == 255)));
            const float visibleStrokeWidth = ResolveAnnotVisibleStrokeWidth(annot);
            type = looksLikeRedaction
                   ? 4
                   : (isAreaMarkup
                      ? 7
                      : (visibleStrokeWidth > 0.0f ? GetPdfBoxShapeTypeFromAnnotBounds(annot) : 7));
            const bool interiorIsMeaningful =
                    hasInteriorColor &&
                    (interiorA < 255 || interiorR != 0 || interiorG != 0 || interiorB != 0);
            const bool shouldUseInteriorColor =
                    type == 7 &&
                    interiorIsMeaningful &&
                    (!hasStrokeColor ||
                     interiorR != r || interiorG != g || interiorB != b || interiorA != a);
            if (!IsPdfShapeNativeType(type) &&
                (shouldUseInteriorColor || (!hasStrokeColor && hasInteriorColor))) {
                r = interiorR;
                g = interiorG;
                b = interiorB;
                a = interiorA;
            }
            if (type == 7) {
                const std::u16string areaAlphaMarker =
                        ReadAnnotStringValueUtf16(annot, "LufickAreaMarkupAlpha");
                int storedAlpha = 0;
                bool hasStoredAlpha = !areaAlphaMarker.empty();
                for (char16_t ch : areaAlphaMarker) {
                    if (ch < u'0' || ch > u'9') {
                        hasStoredAlpha = false;
                        break;
                    }
                    storedAlpha = (storedAlpha * 10) + static_cast<int>(ch - u'0');
                }
                if (hasStoredAlpha) {
                    a = static_cast<unsigned int>(std::max(0, std::min(storedAlpha, 255)));
                }
            }
        }
        jstring jLinkUrl = nullptr;
        if (type == 3) {
            const std::string linkTarget = ReadLinkAnnotationTarget(doc, annot);
            if (!linkTarget.empty()) {
                jLinkUrl = env->NewStringUTF(linkTarget.c_str());
            }
        }
        jstring jTextProps = nullptr;
        jstring jImageProps = nullptr;
        jstring jShapeProps = nullptr;
        jstring jSimplePdfStampProps = nullptr;
        jstring jAttachmentProps = nullptr;
        jstring jStoredMarkupRects = nullptr;
        std::ostringstream markupRectsStream;
        bool hasMarkupRects = false;
        const std::u16string shapeMeta = ReadAnnotStringValueUtf16(annot, "LufickPdfShapeMeta");
        if (type == 21) {
            FPDF_ATTACHMENT attachment = FPDFAnnot_GetFileAttachment(annot);
            const std::u16string fileName = ReadAttachmentNameUtf16(attachment);
            std::u16string mimeType = ReadAttachmentSubtypeUtf16(attachment);
            std::u16string iconName = ReadAnnotStringValueUtf16(annot, "Name");
            if (iconName.empty()) iconName = u"Paperclip";
            if (mimeType.empty()) {
                mimeType = ReadAnnotStringValueUtf16(annot, "LufickAttachmentMime");
            }
            unsigned long fileSize = 0;
            if (attachment) {
                FPDFAttachment_GetFile(attachment, nullptr, 0, &fileSize);
            }
            jAttachmentProps = BuildAttachmentPropertiesJString(
                    env,
                    jsonClass,
                    jsonInit,
                    jsonPut,
                    jsonToString,
                    fileName,
                    mimeType,
                    fileSize,
                    iconName);
        }
        if (!shapeMeta.empty()) {
            const std::string shapeMetaJson = Utf16ToSimpleUtf8(shapeMeta);
            type = GetPdfShapeTypeFromMeta(shapeMetaJson, type);
            usesRectOnly = true;
            if (!IsDirectNativePdfBoxShape(type)) {
                jShapeProps = env->NewStringUTF(shapeMetaJson.c_str());
            }
        }
        if (!jShapeProps && IsPdfShapeNativeType(type)) {
            FS_RECTF shapeRect = {0, 0, 0, 0};
            FPDFAnnot_GetRect(annot, &shapeRect);

            float borderHorizontalRadius = 0.0f;
            float borderVerticalRadius = 0.0f;
            float borderWidth = 1.0f;
            if (!FPDFAnnot_GetBorder(annot, &borderHorizontalRadius, &borderVerticalRadius, &borderWidth) ||
                borderWidth <= 0.0f) {
                borderWidth = 1.0f;
            }
            borderWidth = ResolveAnnotAppearanceStrokeWidth(annot, borderWidth);

            const int strokeRForProps = hasStrokeColor ? (int)r : 0;
            const int strokeGForProps = hasStrokeColor ? (int)g : 0;
            const int strokeBForProps = hasStrokeColor ? (int)b : 0;
            const int strokeAlphaForProps = hasStrokeColor ? (int)a : 0;
            const int fillAlphaForProps = hasInteriorColor ? (int)interiorA : 0;
            std::ostringstream shapeProps;
            shapeProps << "{"
                       << "\"shapeType\":\"" << GetPdfShapeName(type) << "\","
                       << "\"baseLeft\":" << shapeRect.left << ","
                       << "\"baseTop\":" << shapeRect.top << ","
                       << "\"baseRight\":" << shapeRect.right << ","
                       << "\"baseBottom\":" << shapeRect.bottom << ","
                       << "\"pageLeft\":" << shapeRect.left << ","
                       << "\"pageTop\":" << shapeRect.top << ","
                       << "\"pageRight\":" << shapeRect.right << ","
                       << "\"pageBottom\":" << shapeRect.bottom << ","
                       << "\"width\":" << fabs(shapeRect.right - shapeRect.left) << ","
                       << "\"height\":" << fabs(shapeRect.top - shapeRect.bottom) << ","
                       << "\"rotation\":0,"
                       << "\"strokeWidth\":" << borderWidth << ","
                       << "\"strokeR\":" << strokeRForProps << ","
                       << "\"strokeG\":" << strokeGForProps << ","
                       << "\"strokeB\":" << strokeBForProps << ","
                       << "\"strokeA\":" << strokeAlphaForProps << ","
                       << "\"fillR\":" << (hasInteriorColor ? (int)interiorR : 0) << ","
                       << "\"fillG\":" << (hasInteriorColor ? (int)interiorG : 0) << ","
                       << "\"fillB\":" << (hasInteriorColor ? (int)interiorB : 0) << ","
                       << "\"fillA\":" << fillAlphaForProps
                       << "}";
            jShapeProps = env->NewStringUTF(shapeProps.str().c_str());
        }
        const std::u16string signatureSubtype = ReadAnnotStringValueUtf16(annot, "LufickSignatureSubtype");
        const std::u16string signImageMarker = ReadAnnotStringValueUtf16(annot, "LufickSignImage");
        const bool isSignImage =
                !signImageMarker.empty() &&
                signImageMarker != u"0" &&
                signImageMarker != u"false" &&
                signImageMarker != u"FALSE";

        if (type == 5) {
            const std::u16string stampKind = ReadAnnotStringValueUtf16(annot, "LufickStampKind");
            const std::u16string simplePdfStampMarker = ReadAnnotStringValueUtf16(annot, "LufickSimplePdfStamp");
            if (stampKind == u"simple_pdf_stamp" ||
                simplePdfStampMarker == u"1" ||
                simplePdfStampMarker == u"true" ||
                simplePdfStampMarker == u"TRUE") {
                jSimplePdfStampProps = ReadAnnotStringValueJString(env, annot, "LufickSimplePdfStampMeta");
            } else if (stampKind == u"image" ||
                stampKind == u"signature" ||
                stampKind == u"sticker" ||
                stampKind == u"preset_stamp" ||
                stampKind == u"shape_element_svg") {
                type = 9;
                r = 0;
                g = 0;
                b = 0;
                a = 0;
                jImageProps = ReadAnnotStringValueJString(env, annot, "LufickImageMeta");
                jImageProps = AppendSignatureSubtypeToPropsJson(env, jImageProps, signatureSubtype);
                if (isSignImage) {
                    if (signatureSubtype.empty()) {
                        jImageProps = AppendSignatureSubtypeToPropsJson(env, jImageProps, u"Sign_Image");
                    }
                    jImageProps = AppendSignImageFlagToPropsJson(env, jImageProps);
                }
            }
        }

        if (type == 10) {
            jTextProps = BuildStickyNoteCommentMetaJString(env, annot);
            ReadAppStickyNoteAppearanceColor(annot, &r, &g, &b, &a);
        }

        if (type == 11) {
            jTextProps = ReadAnnotStringValueJString(env, annot, "LufickFreeTextMeta");
            if (!jTextProps) {
                FS_RECTF freeTextRect = {0, 0, 0, 0};
                FPDFAnnot_GetRect(annot, &freeTextRect);
                const std::u16string contents = ReadAnnotStringValueUtf16(annot, "Contents");
                const std::string text = EscapeJsonString(Utf16ToSimpleUtf8(contents));
                const std::string fontName = EscapeJsonString(ResolveFreeTextFontName(annot));
                const float fontSize = ResolveFreeTextFontSize(annot);
                std::ostringstream props;
                props << "{"
                      << "\"text\":\"" << text << "\","
                      << "\"font\":\"" << fontName << "\","
                      << "\"size\":" << fontSize << ","
                      << "\"width\":" << fabs(freeTextRect.right - freeTextRect.left) << ","
                      << "\"height\":" << fabs(freeTextRect.top - freeTextRect.bottom) << ","
                      << "\"textColorR\":" << r << ","
                      << "\"textColorG\":" << g << ","
                      << "\"textColorB\":" << b << ","
                      << "\"textColorA\":255"
                      << "}";
                jTextProps = env->NewStringUTF(props.str().c_str());
            }
            a = 255;
        }

        if (type == 6) {
            std::string freehandProps;
            if (!BuildFreehandPropsFromInkAnnot(annot, &freehandProps, &r, &g, &b, &a)) {
                FPDFPage_CloseAnnot(annot);
                continue;
            }

            FS_RECTF rect;
            const bool hasInkBounds = GetInkAnnotPageBounds(
                    annot,
                    &rect.left,
                    &rect.bottom,
                    &rect.right,
                    &rect.top
            );
            if (!hasInkBounds && !FPDFAnnot_GetRect(annot, &rect)) {
                FPDFPage_CloseAnnot(annot);
                continue;
            }

            int dLeft, dTop, dRight, dBottom;
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.left, rect.top, &dLeft, &dTop);
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.right, rect.bottom, &dRight, &dBottom);
            const float deviceLeft = static_cast<float>(std::min(dLeft, dRight));
            const float deviceRight = static_cast<float>(std::max(dLeft, dRight));
            const float deviceTop = static_cast<float>(std::min(dTop, dBottom));
            const float deviceBottom = static_cast<float>(std::max(dTop, dBottom));
            jstring jFhProps = env->NewStringUTF(freehandProps.c_str());
            jFhProps = AppendSignatureSubtypeToPropsJson(env, jFhProps, signatureSubtype);

            jstring jDataProps = BuildBridgeDataPropertiesJString(
                    env,
                    jsonClass,
                    jsonInit,
                    jsonPut,
                    jsonToString,
                    nullptr,
                    jFhProps,
                    nullptr,
                    nullptr
            );
            jobject annotObj = env->NewObject(
                    annotClass,
                    constructor,
                    type,
                    pageIndex,
                    deviceLeft,
                    deviceTop,
                    deviceRight,
                    deviceBottom,
                    (int)r,
                    (int)g,
                    (int)b,
                    (int)a,
                    nullptr,
                    nullptr,
                    jDataProps,
                    nullptr,
                    i,
                    0
            );

            if (annotObj) tempCollector.push_back(annotObj);
            loadedNativeAnnotBounds.push_back({
                    deviceLeft,
                    deviceTop,
                    deviceRight,
                    deviceBottom,
                    static_cast<unsigned int>(r),
                    static_cast<unsigned int>(g),
                    static_cast<unsigned int>(b),
                    static_cast<unsigned int>(a)
            });
            if (jDataProps) env->DeleteLocalRef(jDataProps);
            if (jFhProps) env->DeleteLocalRef(jFhProps);
            FPDFPage_CloseAnnot(annot);
            continue;
        }

        if (type == 5) {
            jTextProps = ReadAnnotStringValueJString(env, annot, "LufickTextStampMeta");

            auto escapeJson = [](const std::string& input) {
                std::string output;
                output.reserve(input.size() + 8);
                for (char ch : input) {
                    switch (ch) {
                        case '\\': output += "\\\\"; break;
                        case '"': output += "\\\""; break;
                        case '\n': output += "\\n"; break;
                        case '\r': break;
                        case '\t': output += "\\t"; break;
                        default: output += ch; break;
                    }
                }
                return output;
            };

            const int annotObjectCount = FPDFAnnot_GetObjectCount(annot);
            FS_RECTF stampRect = {0, 0, 0, 0};
            FPDFAnnot_GetRect(annot, &stampRect);
            if (!jTextProps) {
                const unsigned long contentsLength = FPDFAnnot_GetStringValue(annot, "Contents", nullptr, 0);
                if (contentsLength > sizeof(FPDF_WCHAR)) {
                    std::vector<FPDF_WCHAR> contentsBuffer(contentsLength / sizeof(FPDF_WCHAR));
                    FPDFAnnot_GetStringValue(annot, "Contents", contentsBuffer.data(), contentsLength);
                    const int contentCharCount = static_cast<int>(contentsBuffer.size()) - 1;
                    if (contentCharCount > 0) {
                        std::u16string contentValue(
                                reinterpret_cast<const char16_t*>(contentsBuffer.data()),
                                contentCharCount
                        );
                        std::string utf8Content;
                        utf8Content.reserve(contentValue.size());
                        for (char16_t ch : contentValue) {
                            utf8Content.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
                        }
                        if (!utf8Content.empty()) {
                            jTextProps = env->NewStringUTF(utf8Content.c_str());
                        }
                    }
                }
            }

            bool textColorResolved = false;
            for (int objectIndex = 0; objectIndex < annotObjectCount; objectIndex++) {
                FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
                if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_TEXT) continue;

                unsigned int objR = 0, objG = 0, objB = 0, objA = 0;
                if ((FPDFPageObj_GetFillColor(pageObject, &objR, &objG, &objB, &objA) && objA > 0) ||
                    (FPDFPageObj_GetStrokeColor(pageObject, &objR, &objG, &objB, &objA) && objA > 0)) {
                    r = objR;
                    g = objG;
                    b = objB;
                    a = objA;
                    textColorResolved = true;
                }

                if (jTextProps) {
                    if (textColorResolved) break;
                    continue;
                }

                unsigned long length = FPDFTextObj_GetText(pageObject, textPage, nullptr, 0);
                if (length == 0) continue;

                std::vector<FPDF_WCHAR> buffer(length);
                FPDFTextObj_GetText(pageObject, textPage, buffer.data(), length);
                int actualCharCount = (length > 0) ? (length - 1) : 0;
                std::u16string textValue(reinterpret_cast<const char16_t*>(buffer.data()), actualCharCount);
                std::string utf8Text;
                utf8Text.reserve(textValue.size());
                for (char16_t ch : textValue) {
                    utf8Text.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
                }

                float fontSize = 0.0f;
                FPDFTextObj_GetFontSize(pageObject, &fontSize);
                std::ostringstream props;
                props << "{"
                      << "\"text\":\"" << escapeJson(utf8Text) << "\","
                      << "\"font\":\"currentFont\","
                      << "\"size\":" << fontSize << ","
                      << "\"width\":" << fabs(stampRect.right - stampRect.left) << ","
                      << "\"height\":" << fabs(stampRect.top - stampRect.bottom) << ","
                      << "\"underline\":false,"
                      << "\"strikeout\":false,"
                      << "\"italic\":false,"
                      << "\"bold\":false,"
                      << "\"rotation\":0,"
                      << "\"lineSpacing\":1.0,"
                      << "\"letterSpacing\":0.0,"
                      << "\"alignment\":\"center\","
                      << "\"textColorR\":" << r << ","
                      << "\"textColorG\":" << g << ","
                      << "\"textColorB\":" << b << ","
                      << "\"textColorA\":" << a << ","
                      << "\"hasBackground\":false,"
                      << "\"bgColorR\":255,"
                      << "\"bgColorG\":255,"
                      << "\"bgColorB\":255,"
                      << "\"bgOpacity\":0.0,"
                      << "\"bgRoundness\":0.0"
                      << "}";
                jTextProps = env->NewStringUTF(props.str().c_str());
                break;
            }
            jTextProps = AppendSignatureSubtypeToPropsJson(env, jTextProps, signatureSubtype);
        }

        jStoredMarkupRects = ReadAnnotStringValueJString(env, annot, "LufickMarkupMeta");

        if (usesRectOnly) {
            // Logic for Redaction (Square) - Use the Bounding Box
            FS_RECTF rect;
            if (FPDFAnnot_GetRect(annot, &rect)) {
                int dLeft, dTop, dRight, dBottom;
                FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.left, rect.top, &dLeft, &dTop);
                FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.right, rect.bottom, &dRight, &dBottom);

                jstring jDataProps = BuildBridgeDataPropertiesJString(
                        env,
                        jsonClass,
                        jsonInit,
                        jsonPut,
                        jsonToString,
                        jTextProps,
                        nullptr,
                        jImageProps,
                        jShapeProps,
                        jSimplePdfStampProps,
                        jAttachmentProps
                );
                jobject annotObj = env->NewObject(annotClass, constructor,
                                                   type, pageIndex, (float)dLeft, (float)dTop, (float)dRight, (float)dBottom,
                                                   (int)r, (int)g, (int)b, (int)a, jLinkUrl, nullptr, jDataProps, nullptr, i, 0);

                if (annotObj) tempCollector.push_back(annotObj);
                loadedNativeAnnotBounds.push_back({
                        static_cast<float>(std::min(dLeft, dRight)),
                        static_cast<float>(std::min(dTop, dBottom)),
                        static_cast<float>(std::max(dLeft, dRight)),
                        static_cast<float>(std::max(dTop, dBottom)),
                        static_cast<unsigned int>(r),
                        static_cast<unsigned int>(g),
                        static_cast<unsigned int>(b),
                        static_cast<unsigned int>(a)
                });
                if (jDataProps) env->DeleteLocalRef(jDataProps);
                if (jAttachmentProps) env->DeleteLocalRef(jAttachmentProps);
            }
        } else {
            // Logic for Highlights/Underlines - Map to characters
            int quadCount = FPDFAnnot_CountAttachmentPoints(annot);
            for (int q = 0; q < quadCount; q++) {
                FS_QUADPOINTSF quad;
                if (!FPDFAnnot_GetAttachmentPoints(annot, q, &quad)) continue;

                float qL = std::min({quad.x1, quad.x2, quad.x3, quad.x4});
                float qR = std::max({quad.x1, quad.x2, quad.x3, quad.x4});
                float qB = std::min({quad.y1, quad.y2, quad.y3, quad.y4});
                float qT = std::max({quad.y1, quad.y2, quad.y3, quad.y4});

                if (hasMarkupRects) markupRectsStream << ",";
                markupRectsStream
                        << "{\"left\":" << qL
                        << ",\"top\":" << qT
                        << ",\"right\":" << qR
                        << ",\"bottom\":" << qB
                        << "}";
                hasMarkupRects = true;
            }

            jstring jMarkupRects = jStoredMarkupRects
                    ? jStoredMarkupRects
                    : (hasMarkupRects
                        ? env->NewStringUTF((std::string("[") + markupRectsStream.str() + "]").c_str())
                        : nullptr);

            for (int q = 0; q < quadCount; q++) {
                FS_QUADPOINTSF quad;
                if (!FPDFAnnot_GetAttachmentPoints(annot, q, &quad)) continue;

                float qL = std::min({quad.x1, quad.x2, quad.x3, quad.x4});
                float qR = std::max({quad.x1, quad.x2, quad.x3, quad.x4});
                float qB = std::min({quad.y1, quad.y2, quad.y3, quad.y4});
                float qT = std::max({quad.y1, quad.y2, quad.y3, quad.y4});

                for (int c = 0; c < charCount; c++) {
                    double cl, cr, cb, ct;
                    if (!FPDFText_GetCharBox(textPage, c, &cl, &cr, &cb, &ct)) continue;
                    if (cr < qL || cl > qR || ct < qB || cb > qT) continue;

                    int dLeft, dTop, dRight, dBottom;
                    FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, cl, ct, &dLeft, &dTop);
                    FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, cr, cb, &dRight, &dBottom);

                    jstring jDataProps = BuildBridgeDataPropertiesJString(
                            env,
                            jsonClass,
                            jsonInit,
                            jsonPut,
                            jsonToString,
                            jTextProps,
                            nullptr,
                            jImageProps,
                            nullptr
                    );
                    jobject annotObj = env->NewObject(annotClass, constructor,
                                                       type, pageIndex, (float)dLeft, (float)dTop, (float)dRight, (float)dBottom,
                                                       (int)r, (int)g, (int)b, (int)a, jLinkUrl, jMarkupRects, jDataProps, nullptr, i, 0);

                    if (annotObj) tempCollector.push_back(annotObj);
                    if (jDataProps) env->DeleteLocalRef(jDataProps);
                }
            }
            if (jMarkupRects) env->DeleteLocalRef(jMarkupRects);
        }
        if (jLinkUrl) env->DeleteLocalRef(jLinkUrl);
        FPDFPage_CloseAnnot(annot);
    }

    const float pageWidth = FPDF_GetPageWidthF(page);
    const float pageHeight = FPDF_GetPageHeightF(page);
    int objectCount = FPDFPage_CountObjects(page);
    int formOrdinal = 0;
    int pathOrdinal = 0;
    for (int i = 0; includeEditContent && i < objectCount; i++) {
        FPDF_PAGEOBJECT pageObj = FPDFPage_GetObject(page, i);
        if (!pageObj) continue;
        const int pageObjectType = FPDFPageObj_GetType(pageObj);
        const LufickContentObjectMetadata contentMetadata =
                ReadLufickContentObjectMetadata(pageObj);
        if (
                contentMetadata.kind == "redaction" ||
                (
                    contentMetadata.kind == "signature" &&
                    contentMetadata.subtype == "Sign_draw"
                )
        ) {
            continue;
        }

        if (
                pageObjectType == FPDF_PAGEOBJ_TEXT &&
                (
//                    (contentMetadata.kind == "signature" && contentMetadata.subtype == "Sign_text") ||
                    (contentMetadata.kind == "preset_stamp" && contentMetadata.subtype == "pdf")
                )
        ) {
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
            if (!FPDFPageObj_GetBounds(pageObj, &left, &bottom, &right, &top)) continue;
            int dLeft, dTop, dRight, dBottom;
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);

            unsigned int r = 0, g = 0, b = 0, a = 255;
            FPDFPageObj_GetFillColor(pageObj, &r, &g, &b, &a);
            unsigned long textLength = FPDFTextObj_GetText(pageObj, textPage, nullptr, 0);
            std::string textValue;
            if (textLength > 0) {
                std::vector<FPDF_WCHAR> textBuffer(textLength);
                FPDFTextObj_GetText(pageObj, textPage, textBuffer.data(), textLength);
                const int characterCount = static_cast<int>(textLength) - 1;
                textValue = Utf16ToSimpleUtf8(std::u16string(
                        reinterpret_cast<const char16_t*>(textBuffer.data()),
                        std::max(characterCount, 0)
                ));
            }
            float fontSize = 0.0f;
            FPDFTextObj_GetFontSize(pageObj, &fontSize);
            std::ostringstream textProps;
            textProps << "{\"text\":\"" << EscapeJsonString(textValue) << "\","
                      << "\"size\":" << fontSize << ","
                      << "\"contentKind\":\"" << EscapeJsonString(contentMetadata.kind) << "\","
                      << "\"contentSubtype\":\"" << EscapeJsonString(contentMetadata.subtype) << "\","
                      << "\"contentGroupId\":\"" << EscapeJsonString(contentMetadata.groupId) << "\"";
            if (contentMetadata.kind == "signature") {
                textProps << ",\"signatureSubType\":\"Sign_text\"";
            }
            textProps << "}";
            jstring jTextProps = env->NewStringUTF(textProps.str().c_str());
            jstring jSimplePdfStampProps = nullptr;
            if (contentMetadata.kind == "preset_stamp") {
                const std::string simpleProps =
                        "{\"stampKind\":\"simple_pdf_stamp\",\"contentGroupId\":\"" +
                        EscapeJsonString(contentMetadata.groupId) + "\"}";
                jSimplePdfStampProps = env->NewStringUTF(simpleProps.c_str());
            }
            jstring jDataProps = BuildBridgeDataPropertiesJString(
                    env, jsonClass, jsonInit, jsonPut, jsonToString,
                    jTextProps, nullptr, nullptr, nullptr, jSimplePdfStampProps
            );
            jobject textObject = env->NewObject(
                    annotClass,
                    constructor,
                    5,
                    pageIndex,
                    static_cast<float>(std::min(dLeft, dRight)),
                    static_cast<float>(std::min(dTop, dBottom)),
                    static_cast<float>(std::max(dLeft, dRight)),
                    static_cast<float>(std::max(dTop, dBottom)),
                    static_cast<int>(r),
                    static_cast<int>(g),
                    static_cast<int>(b),
                    static_cast<int>(a),
                    nullptr,
                    nullptr,
                    jDataProps,
                    nullptr,
                    i,
                    4
            );
            if (textObject) tempCollector.push_back(textObject);
            if (jDataProps) env->DeleteLocalRef(jDataProps);
            if (jSimplePdfStampProps) env->DeleteLocalRef(jSimplePdfStampProps);
            env->DeleteLocalRef(jTextProps);
            continue;
        }

        if (pageObjectType == FPDF_PAGEOBJ_IMAGE) {
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
            if (!FPDFPageObj_GetBounds(pageObj, &left, &bottom, &right, &top)) continue;

            int dLeft, dTop, dRight, dBottom;
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);
            const float deviceLeft = static_cast<float>(std::min(dLeft, dRight));
            const float deviceRight = static_cast<float>(std::max(dLeft, dRight));
            const float deviceTop = static_cast<float>(std::min(dTop, dBottom));
            const float deviceBottom = static_cast<float>(std::max(dTop, dBottom));
            if (deviceRight - deviceLeft < 1.0f || deviceBottom - deviceTop < 1.0f) continue;

            if (contentMetadata.kind == "shading_proxy") {
                jobject shadingObject = env->NewObject(
                        annotClass,
                        constructor,
                        19,
                        pageIndex,
                        deviceLeft,
                        deviceTop,
                        deviceRight,
                        deviceBottom,
                        0,
                        0,
                        0,
                        0,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        i,
                        4
                );
                if (shadingObject) tempCollector.push_back(shadingObject);
                continue;
            }

            FPDF_IMAGEOBJ_METADATA imageMetadata{};
            const bool hasImageMetadata = FPDFImageObj_GetImageMetadata(pageObj, page, &imageMetadata);
            const bool canExtractBitmap = !hasImageMetadata ||
                    static_cast<uint64_t>(imageMetadata.width) * static_cast<uint64_t>(imageMetadata.height) <= 16000000ULL;
            FPDF_BITMAP imageBitmap = includeImageBitmaps && canExtractBitmap
                                       ? FPDFImageObj_GetRenderedBitmap(doc, page, pageObj)
                                       : nullptr;
            if (!imageBitmap && includeImageBitmaps && canExtractBitmap) {
                imageBitmap = FPDFImageObj_GetBitmap(pageObj);
            }
            jobject androidBitmap = ConvertFPDFBitmapToAndroidBitmap(env, imageBitmap);
            if (imageBitmap) FPDFBitmap_Destroy(imageBitmap);

            double imageRotation = 0.0;
            double imageBaseWidth = deviceRight - deviceLeft;
            double imageBaseHeight = deviceBottom - deviceTop;
            unsigned int imageR = 255, imageG = 255, imageB = 255, imageAlpha = 255;
            FPDFPageObj_GetFillColor(pageObj, &imageR, &imageG, &imageB, &imageAlpha);
            FS_MATRIX imageMatrix{};
            if (FPDFPageObj_GetMatrix(pageObj, &imageMatrix)) {
                imageRotation = atan2(
                        static_cast<double>(imageMatrix.b),
                        static_cast<double>(imageMatrix.a)
                ) * 180.0 / M_PI;
                int originX = 0, originY = 0;
                int widthX = 0, widthY = 0;
                int heightX = 0, heightY = 0;
                FPDF_PageToDevice(
                        page, 0, 0, viewWidth, viewHeight, 0,
                        imageMatrix.e, imageMatrix.f, &originX, &originY);
                FPDF_PageToDevice(
                        page, 0, 0, viewWidth, viewHeight, 0,
                        imageMatrix.e + imageMatrix.a,
                        imageMatrix.f + imageMatrix.b,
                        &widthX, &widthY);
                FPDF_PageToDevice(
                        page, 0, 0, viewWidth, viewHeight, 0,
                        imageMatrix.e + imageMatrix.c,
                        imageMatrix.f + imageMatrix.d,
                        &heightX, &heightY);
                imageBaseWidth = std::max(
                        hypot(static_cast<double>(widthX - originX),
                              static_cast<double>(widthY - originY)),
                        1.0);
                imageBaseHeight = std::max(
                        hypot(static_cast<double>(heightX - originX),
                              static_cast<double>(heightY - originY)),
                        1.0);
            }

            std::ostringstream imageProps;
            imageProps << "{\"stampKind\":\"";
            if (contentMetadata.kind == "preset_stamp" && contentMetadata.subtype == "image") {
                imageProps << "preset_stamp\",\"presetStamp\":true,";
            } else if (contentMetadata.kind == "signature") {
                imageProps << "signature\",\"signatureSubType\":\""
                           << EscapeJsonString(contentMetadata.subtype) << "\",";
            } else {
                imageProps << "content_image\",";
            }
            if (contentMetadata.isValid()) {
                imageProps << "\"contentKind\":\"" << EscapeJsonString(contentMetadata.kind) << "\","
                           << "\"contentSubtype\":\"" << EscapeJsonString(contentMetadata.subtype) << "\","
                           << "\"contentGroupId\":\"" << EscapeJsonString(contentMetadata.groupId) << "\",";
            }
            imageProps << "\"stretchToBounds\":false,"
                       << "\"opacity\":" << (static_cast<double>(imageAlpha) / 255.0) << ","
                       << "\"baseWidth\":" << imageBaseWidth << ","
                       << "\"baseHeight\":" << imageBaseHeight << ","
                       << "\"rotation\":" << imageRotation << "}";
            jstring jImageProps = env->NewStringUTF(imageProps.str().c_str());
            jstring jSimplePdfStampProps = nullptr;
            if (contentMetadata.kind == "preset_stamp" && contentMetadata.subtype == "pdf") {
                const std::string simpleProps =
                        "{\"stampKind\":\"simple_pdf_stamp\",\"contentGroupId\":\"" +
                        EscapeJsonString(contentMetadata.groupId) + "\"}";
                jSimplePdfStampProps = env->NewStringUTF(simpleProps.c_str());
            }
            jstring jDataProps = BuildBridgeDataPropertiesJString(
                    env,
                    jsonClass,
                    jsonInit,
                    jsonPut,
                    jsonToString,
                    nullptr,
                    nullptr,
                    jImageProps,
                    nullptr,
                    jSimplePdfStampProps
            );
            jobject imageObject = env->NewObject(
                    annotClass,
                    constructor,
                    9,
                    pageIndex,
                    deviceLeft,
                    deviceTop,
                    deviceRight,
                    deviceBottom,
                    255,
                    255,
                    255,
                    static_cast<int>(imageAlpha),
                    nullptr,
                    nullptr,
                    jDataProps,
                    androidBitmap,
                    i,
                    4
            );
            if (imageObject) tempCollector.push_back(imageObject);
            if (jDataProps) env->DeleteLocalRef(jDataProps);
            if (jSimplePdfStampProps) env->DeleteLocalRef(jSimplePdfStampProps);
            env->DeleteLocalRef(jImageProps);
            if (androidBitmap) env->DeleteLocalRef(androidBitmap);
            continue;
        }

        if (pageObjectType == FPDF_PAGEOBJ_SHADING) {
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
            if (!FPDFPageObj_GetBounds(pageObj, &left, &bottom, &right, &top)) continue;
            if (fabs(right - left) < 0.0001f || fabs(top - bottom) < 0.0001f) continue;

            int dLeft, dTop, dRight, dBottom;
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);

            jobject shadingObject = env->NewObject(
                    annotClass,
                    constructor,
                    19,
                    pageIndex,
                    static_cast<float>(std::min(dLeft, dRight)),
                    static_cast<float>(std::min(dTop, dBottom)),
                    static_cast<float>(std::max(dLeft, dRight)),
                    static_cast<float>(std::max(dTop, dBottom)),
                    0,
                    0,
                    0,
                    0,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    i,
                    4
            );
            if (shadingObject) tempCollector.push_back(shadingObject);
            continue;
        }

        if (pageObjectType == FPDF_PAGEOBJ_FORM) {
            const int currentFormOrdinal = formOrdinal++;
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
            const bool hasOuterBounds =
                    FPDFPageObj_GetBounds(pageObj, &left, &bottom, &right, &top);
            FS_MATRIX formMatrix{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
            FPDFPageObj_GetMatrix(pageObj, &formMatrix);
            const bool hasPaintedBounds = GetNativeFormPaintedBounds(
                    page,
                    pageObj,
                    viewWidth,
                    viewHeight,
                    &left,
                    &bottom,
                    &right,
                    &top
            );
            if ((!hasOuterBounds && !hasPaintedBounds) ||
                fabs(right - left) < 0.0001f ||
                fabs(top - bottom) < 0.0001f) {
                continue;
            }

            int dLeft, dTop, dRight, dBottom;
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);
            std::ostringstream formProps;
            formProps << "{\"formOrdinal\":" << currentFormOrdinal
                      << ",\"sourceLeft\":" << std::min(left, right)
                      << ",\"sourceTop\":" << std::max(top, bottom)
                      << ",\"sourceRight\":" << std::max(left, right)
                      << ",\"sourceBottom\":" << std::min(top, bottom)
                      << ",\"matrixA\":" << formMatrix.a
                      << ",\"matrixB\":" << formMatrix.b
                      << ",\"matrixC\":" << formMatrix.c
                      << ",\"matrixD\":" << formMatrix.d
                      << ",\"matrixE\":" << formMatrix.e
                      << ",\"matrixF\":" << formMatrix.f
                      << "}";
            jstring jFormProps = env->NewStringUTF(formProps.str().c_str());
            jstring jDataProps = BuildBridgeDataPropertiesJString(
                    env,
                    jsonClass,
                    jsonInit,
                    jsonPut,
                    jsonToString,
                    nullptr,
                    nullptr,
                    nullptr,
                    jFormProps
            );

            jobject formObject = env->NewObject(
                    annotClass,
                    constructor,
                    20,
                    pageIndex,
                    static_cast<float>(std::min(dLeft, dRight)),
                    static_cast<float>(std::min(dTop, dBottom)),
                    static_cast<float>(std::max(dLeft, dRight)),
                    static_cast<float>(std::max(dTop, dBottom)),
                    0,
                    0,
                    0,
                    0,
                    nullptr,
                    nullptr,
                    jDataProps,
                    nullptr,
                    i,
                    4
            );
            if (formObject) tempCollector.push_back(formObject);
            if (jDataProps) env->DeleteLocalRef(jDataProps);
            env->DeleteLocalRef(jFormProps);
            continue;
        }

        // Unknown page objects are intentionally unsupported.
        if (pageObjectType != FPDF_PAGEOBJ_PATH) continue;
        const int currentPathOrdinal = pathOrdinal++;

        FPDF_CLIPPATH clipPath = FPDFPageObj_GetClipPath(pageObj);
        const bool hasClipPath =
                clipPath != nullptr && FPDFClipPath_CountPaths(clipPath) > 0;

        float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
        if (!FPDFPageObj_GetBounds(pageObj, &left, &bottom, &right, &top)) continue;

        if (contentMetadata.kind == "redaction") {
            int dLeft, dTop, dRight, dBottom;
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
            FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);
            unsigned int r = 0, g = 0, b = 0, a = 255;
            FPDFPageObj_GetFillColor(pageObj, &r, &g, &b, &a);
            const std::string metadataJson =
                    "{\"contentKind\":\"redaction\",\"contentGroupId\":\"" +
                    EscapeJsonString(contentMetadata.groupId) + "\"}";
            jstring jMetadataProps = env->NewStringUTF(metadataJson.c_str());
            jstring jDataProps = BuildBridgeDataPropertiesJString(
                    env, jsonClass, jsonInit, jsonPut, jsonToString,
                    nullptr, nullptr, nullptr, jMetadataProps
            );
            jobject redactionObject = env->NewObject(
                    annotClass,
                    constructor,
                    4,
                    pageIndex,
                    static_cast<float>(std::min(dLeft, dRight)),
                    static_cast<float>(std::min(dTop, dBottom)),
                    static_cast<float>(std::max(dLeft, dRight)),
                    static_cast<float>(std::max(dTop, dBottom)),
                    static_cast<int>(r),
                    static_cast<int>(g),
                    static_cast<int>(b),
                    static_cast<int>(a),
                    nullptr,
                    nullptr,
                    jDataProps,
                    nullptr,
                    i,
                    4
            );
            if (redactionObject) tempCollector.push_back(redactionObject);
            if (jDataProps) env->DeleteLocalRef(jDataProps);
            env->DeleteLocalRef(jMetadataProps);
            continue;
        }

        const float boundsWidth = fabs(right - left);
        const float boundsHeight = fabs(top - bottom);
        if (boundsWidth < 1.0f && boundsHeight < 1.0f) continue;

        const float widthRatio = pageWidth > 0.0f ? (boundsWidth / pageWidth) : 0.0f;
        const float heightRatio = pageHeight > 0.0f ? (boundsHeight / pageHeight) : 0.0f;
        const float areaRatio = (pageWidth > 0.0f && pageHeight > 0.0f)
                ? ((boundsWidth * boundsHeight) / (pageWidth * pageHeight))
                : 0.0f;
        if (!contentMetadata.isValid() &&
            ((widthRatio > 0.80f && heightRatio > 0.80f) || areaRatio > 0.55f)) {
            continue;
        }

        unsigned int r = 0, g = 170, b = 90, a = 255;
        const bool isMarkedPdfStampPath =
                contentMetadata.kind == "preset_stamp" && contentMetadata.subtype == "pdf";
        FS_MATRIX pathMatrix{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        FPDFPageObj_GetMatrix(pageObj, &pathMatrix);
        const int stablePathId = GetLufickNativePathStableId(pageObj);
        std::string freehandProps;
        if (!BuildFreehandPropsFromPathObject(
                pageObj,
                &freehandProps,
                &r,
                &g,
                &b,
                &a
        )) {
            continue;
        }
        if (!freehandProps.empty() && freehandProps.back() == '}') {
            freehandProps.pop_back();
            std::ostringstream pathMetadata;
            pathMetadata << ",\"pathOrdinal\":" << currentPathOrdinal
                         << ",\"stablePathId\":" << stablePathId
                         << ",\"sourceLeft\":" << std::min(left, right)
                         << ",\"sourceTop\":" << std::max(top, bottom)
                         << ",\"sourceRight\":" << std::max(left, right)
                         << ",\"sourceBottom\":" << std::min(top, bottom)
                         << ",\"matrixA\":" << pathMatrix.a
                         << ",\"matrixB\":" << pathMatrix.b
                         << ",\"matrixC\":" << pathMatrix.c
                         << ",\"matrixD\":" << pathMatrix.d
                         << ",\"matrixE\":" << pathMatrix.e
                         << ",\"matrixF\":" << pathMatrix.f
                         << ",\"hasClipPath\":" << (hasClipPath ? "true" : "false")
                         << "}";
            freehandProps += pathMetadata.str();
        }
        int dLeft, dTop, dRight, dBottom;
        FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
        FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);
        const float deviceLeft = static_cast<float>(std::min(dLeft, dRight));
        const float deviceRight = static_cast<float>(std::max(dLeft, dRight));
        const float deviceTop = static_cast<float>(std::min(dTop, dBottom));
        const float deviceBottom = static_cast<float>(std::max(dTop, dBottom));

        const float pathWidth = fmax(deviceRight - deviceLeft, 0.0f);
        const float pathHeight = fmax(deviceBottom - deviceTop, 0.0f);
        const float pathArea = pathWidth * pathHeight;
        bool duplicatesLoadedNativeAnnot = false;
        for (const auto& loadedBounds : loadedNativeAnnotBounds) {
            const float overlapLeft = fmax(deviceLeft, loadedBounds.left);
            const float overlapTop = fmax(deviceTop, loadedBounds.top);
            const float overlapRight = fmin(deviceRight, loadedBounds.right);
            const float overlapBottom = fmin(deviceBottom, loadedBounds.bottom);
            if (overlapRight <= overlapLeft || overlapBottom <= overlapTop) continue;

            const float overlapArea = (overlapRight - overlapLeft) * (overlapBottom - overlapTop);
            const float loadedArea = fmax(loadedBounds.right - loadedBounds.left, 0.0f) *
                                     fmax(loadedBounds.bottom - loadedBounds.top, 0.0f);
            const float areaBase = fmax(fmin(pathArea, loadedArea), 1.0f);
            const bool colorMatches =
                    loadedBounds.r == r &&
                    loadedBounds.g == g &&
                    loadedBounds.b == b;
            if (colorMatches && (overlapArea / areaBase) >= 0.85f) {
                duplicatesLoadedNativeAnnot = true;
                break;
            }
        }
        if (duplicatesLoadedNativeAnnot && !contentMetadata.isValid()) continue;

        const bool isAppCreatedPath =
                contentMetadata.kind == "app_created" ||
                isMarkedPdfStampPath;
        if (isAppCreatedPath &&
            !freehandProps.empty() &&
            freehandProps.back() == '}') {
            freehandProps.pop_back();
            freehandProps += ",\"appCreated\":true}";
        }
        jstring jFhProps = env->NewStringUTF(freehandProps.c_str());
        if (contentMetadata.kind == "signature") {
            std::u16string signatureSubtype(
                    contentMetadata.subtype.begin(),
                    contentMetadata.subtype.end()
            );
            jstring taggedProps = AppendSignatureSubtypeToPropsJson(
                    env,
                    jFhProps,
                    signatureSubtype
            );
            env->DeleteLocalRef(jFhProps);
            jFhProps = taggedProps;
        }
        jstring jSimplePdfStampProps = nullptr;
        if (isMarkedPdfStampPath) {
            const std::string simpleProps =
                    "{\"stampKind\":\"simple_pdf_stamp\",\"contentGroupId\":\"" +
                    EscapeJsonString(contentMetadata.groupId) + "\"}";
            jSimplePdfStampProps = env->NewStringUTF(simpleProps.c_str());
        }

        jstring jDataProps = BuildBridgeDataPropertiesJString(
                env,
                jsonClass,
                jsonInit,
                jsonPut,
                jsonToString,
                nullptr,
                jFhProps,
                nullptr,
                nullptr,
                jSimplePdfStampProps
        );
        jobject annotObj = env->NewObject(
                annotClass,
                constructor,
                6,
                pageIndex,
                deviceLeft,
                deviceTop,
                deviceRight,
                deviceBottom,
                (int)r,
                (int)g,
                (int)b,
                (int)a,
                nullptr,
                nullptr,
                jDataProps,
                nullptr,
                i,
                4
        );

        if (annotObj) tempCollector.push_back(annotObj);
        if (jDataProps) env->DeleteLocalRef(jDataProps);
        if (jSimplePdfStampProps) env->DeleteLocalRef(jSimplePdfStampProps);
        if (jFhProps) env->DeleteLocalRef(jFhProps);
    }

    // Convert vector back to Java Array
    jobjectArray resultArray = env->NewObjectArray((jsize)tempCollector.size(), annotClass, nullptr);
    for (size_t i = 0; i < tempCollector.size(); i++) {
        env->SetObjectArrayElement(resultArray, (jsize)i, tempCollector[i]);
        env->DeleteLocalRef(tempCollector[i]);
    }

    FPDFText_ClosePage(textPage);
    return resultArray;
}


// update annotation part
JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeApplyAnnotationEdits(
        JNIEnv* env,
        jobject thiz,
        jlong docPtr,
        jlong pagePtr,
        jint pageIndex,
        jobjectArray highlightsArray) {

    DocumentFile* docFile = reinterpret_cast<DocumentFile*>(docPtr);
    FPDF_PAGE page = (FPDF_PAGE)pagePtr;
    if (!docFile || !docFile->pdfDocument || !page || !highlightsArray) return JNI_FALSE;

    return ApplyNativeAnnotationEditActions(env, docFile->pdfDocument, highlightsArray, page, pageIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeApplyTextEdits(
        JNIEnv* env,
        jobject thiz,
        jlong docPtr,
        jlong pagePtr,
        jint pageIndex,
        jobjectArray textEditsArray) {
    DocumentFile* docFile = reinterpret_cast<DocumentFile*>(docPtr);
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    if (!docFile || !docFile->pdfDocument || !page || !textEditsArray) return JNI_FALSE;
    // Preview edits target the already-open page used by rendering. Repeated keystrokes replace
    // that same native object with the latest complete text, so validation against the original
    // string is reserved for the final file save path.
    return ApplyNativeTextContentEdits(
            env,
            docFile->pdfDocument,
            textEditsArray,
            page,
            pageIndex,
            false,
            true)
           ? JNI_TRUE
           : JNI_FALSE;
}


// text editing part (not final or tested - code from old branch)
#define LOG_TAG "PDF_EDIT_NATIVE"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

typedef struct {
    FPDF_FILEWRITE dest;
    FILE* file;
} MyFileWrite;

int TestBlockWrite(FPDF_FILEWRITE* pFileWrite, const void* pData, unsigned long size) {
    MyFileWrite* pMyWrite = (MyFileWrite*)pFileWrite;
    size_t written = fwrite(pData, 1, size, pMyWrite->file);
    return (written == size) ? 1 : 0;
}


// Helper to update text or replace object while preserving metadata
void UpdateTextObject(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_PAGEOBJECT oldObj, jstring jtext, JNIEnv *env) {
    const jchar* textPtr = env->GetStringChars(jtext, NULL);
    int textLen = env->GetStringLength(jtext);
    std::vector<unsigned short> utf16_str(textLen + 1);
    for (int j = 0; j < textLen; j++) utf16_str[j] = (unsigned short)textPtr[j];
    utf16_str[textLen] = 0;

    // 1. Get original state to preserve metadata and appearance
    float fontSize = 12.0f;
    FPDFTextObj_GetFontSize(oldObj, &fontSize);
    FPDF_FONT originalFont = FPDFTextObj_GetFont(oldObj);

    float L, B, R, T;
    FPDFPageObj_GetBounds(oldObj, &L, &B, &R, &T);
    float originalWidth = R - L;

    // 2. Attempt to update text on the existing object
    // This is the best way to keep selection and metadata intact.
    if (FPDFText_SetText(oldObj, (FPDF_WIDESTRING)utf16_str.data())) {
        // Horizontal Scaling: Adjust to fit original width if text changed significantly
        float nL, nB, nR, nT;
        FPDFPageObj_GetBounds(oldObj, &nL, &nB, &nR, &nT);
        float newWidth = nR - nL;
        if (newWidth > 0.1f) {
            float scaleX = originalWidth / newWidth;
            FPDFPageObj_Transform(oldObj, scaleX, 0, 0, 1, 0, 0);
        }
    } else {
        // 3. Fallback: Create a new object with standard font if original fails
        // We load a standard font but apply the original's size and position
        FPDF_FONT fallbackFont = FPDFText_LoadStandardFont(doc, "Helvetica");
        FPDF_PAGEOBJECT newObj = FPDFPageObj_CreateTextObj(doc, fallbackFont, fontSize);

        FPDFText_SetText(newObj, (FPDF_WIDESTRING)utf16_str.data());

        // Position at exactly the same place as the old one
        FPDFPageObj_Transform(newObj, 1, 0, 0, 1, L, B);

        // Adjust width to match
        float nL, nB, nR, nT;
        FPDFPageObj_GetBounds(newObj, &nL, &nB, &nR, &nT);
        float newWidth = nR - nL;
        if (newWidth > 0.1f) {
            float scaleX = originalWidth / newWidth;
            FPDFPageObj_Transform(newObj, scaleX, 0, 0, 1, 0, 0);
        }

        // Replace the old object in the page tree
        FPDFPage_InsertObject(page, newObj);
        if (FPDFPage_RemoveObject(page, oldObj)) {
            FPDFPageObj_Destroy(oldObj);
        }
    }

    env->ReleaseStringChars(jtext, textPtr);
}

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfHighlightSaver_nativeSaveTextEdits1(
        JNIEnv *env, jobject thiz,
        jstring input_path, jstring output_path,
        jobjectArray text_edits) {

    const char *in_path = env->GetStringUTFChars(input_path, NULL);
    const char *out_path = env->GetStringUTFChars(output_path, NULL);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(in_path, NULL);
    if (!doc) {
        env->ReleaseStringUTFChars(input_path, in_path);
        env->ReleaseStringUTFChars(output_path, out_path);
        return JNI_FALSE;
    }

    jclass editClazz = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfTextEditNative");
    jfieldID fPageIdx = env->GetFieldID(editClazz, "pageIndex", "I");
    jfieldID fLeft    = env->GetFieldID(editClazz, "left", "F");
    jfieldID fBottom  = env->GetFieldID(editClazz, "bottom", "F");
    jfieldID fRight   = env->GetFieldID(editClazz, "right", "F");
    jfieldID fTop     = env->GetFieldID(editClazz, "top", "F");
    jfieldID fNewText = env->GetFieldID(editClazz, "newText", "Ljava/lang/String;");

    int editCount = env->GetArrayLength(text_edits);

    for (int e = 0; e < editCount; e++) {
        jobject editObj = env->GetObjectArrayElement(text_edits, e);
        int pageIdx = env->GetIntField(editObj, fPageIdx);
        float selL = env->GetFloatField(editObj, fLeft);
        float selB = env->GetFloatField(editObj, fBottom);
        float selR = env->GetFloatField(editObj, fRight);
        float selT = env->GetFloatField(editObj, fTop);
        jstring jtext = (jstring) env->GetObjectField(editObj, fNewText);

        FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
        if (!page) { env->DeleteLocalRef(editObj); continue; }

        int objCount = FPDFPage_CountObjects(page);
        for (int i = 0; i < objCount; i++) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
            if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;

            float left, bottom, right, top;
            FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top);

            // Bounding box intersection check
            if (!(right < selL || left > selR || top < selB || bottom > selT)) {
                UpdateTextObject(doc, page, obj, jtext, env);
                break;
            }
        }

        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        env->DeleteLocalRef(jtext);
        env->DeleteLocalRef(editObj);
    }

    // Standard saving block
    FILE* file = fopen(out_path, "wb");
    jboolean success = JNI_FALSE;
    if (file) {
        MyFileWrite writer;
        writer.dest.version = 1;
        writer.dest.WriteBlock = TestBlockWrite;
        writer.file = file;
        success = FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL);
        fclose(file);
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(input_path, in_path);
    env->ReleaseStringUTFChars(output_path, out_path);
    return success;
}


struct FoundObject {
    FPDF_PAGEOBJECT obj;
    float left;
};

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveTextEdits(
        JNIEnv *env, jobject thiz,
        jstring input_path, jstring output_path,
        jobjectArray text_edits) {

    const char *in_path = env->GetStringUTFChars(input_path, NULL);
    const char *out_path = env->GetStringUTFChars(output_path, NULL);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(in_path, NULL);
    if (!doc) {
        env->ReleaseStringUTFChars(input_path, in_path);
        env->ReleaseStringUTFChars(output_path, out_path);
        return JNI_FALSE;
    }

    jclass editClazz = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfTextEditNative");
    jfieldID fPageIdx = env->GetFieldID(editClazz, "pageIndex", "I");
    jfieldID fLeft    = env->GetFieldID(editClazz, "left", "F");
    jfieldID fBottom  = env->GetFieldID(editClazz, "bottom", "F");
    jfieldID fRight   = env->GetFieldID(editClazz, "right", "F");
    jfieldID fTop     = env->GetFieldID(editClazz, "top", "F");
    jfieldID fNewText = env->GetFieldID(editClazz, "newText", "Ljava/lang/String;");

    int editCount = env->GetArrayLength(text_edits);

    for (int e = 0; e < editCount; e++) {
        jobject editObj = env->GetObjectArrayElement(text_edits, e);
        int pageIdx = env->GetIntField(editObj, fPageIdx);
        float selL = env->GetFloatField(editObj, fLeft);
        float selB = env->GetFloatField(editObj, fBottom);
        float selR = env->GetFloatField(editObj, fRight);
        float selT = env->GetFloatField(editObj, fTop);
        jstring jtext = (jstring) env->GetObjectField(editObj, fNewText);

        FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
        if (!page) { env->DeleteLocalRef(editObj); continue; }

        // --- FIX: Grouping Logic ---
        std::vector<FoundObject> foundObjects;
        int objCount = FPDFPage_CountObjects(page);

        for (int i = 0; i < objCount; i++) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
            if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;

            float L, B, R, T;
            if (FPDFPageObj_GetBounds(obj, &L, &B, &R, &T)) {
                // Check if object is inside selection
                if (!(R < selL || L > selR || T < selB || B > selT)) {
                    foundObjects.push_back({obj, L});
                }
            }
        }

        if (!foundObjects.empty()) {
            // 1. Sort objects left-to-right to find the "beginning" of the phrase
            std::sort(foundObjects.begin(), foundObjects.end(), [](const FoundObject& a, const FoundObject& b) {
                return a.left < b.left;
            });

            // 2. The first object will be our "Anchor" for the new text
            FPDF_PAGEOBJECT anchorObj = foundObjects[0].obj;

            // Convert Java String to UTF-16
            const jchar* textPtr = env->GetStringChars(jtext, NULL);
            int textLen = env->GetStringLength(jtext);
            std::vector<unsigned short> utf16_str(textLen + 1);
            for (int j = 0; j < textLen; j++) utf16_str[j] = (unsigned short)textPtr[j];
            utf16_str[textLen] = 0;

            // Try to set text on anchor. If font subset is missing chars,
            // you might need the Fallback/Recreate logic from previous steps here.
            FPDFText_SetText(anchorObj, (FPDF_WIDESTRING)utf16_str.data());
            env->ReleaseStringChars(jtext, textPtr);

            // 3. Delete all other objects that were part of this selection
            // We iterate from 1 to end (skipping the anchor)
            for (size_t k = 1; k < foundObjects.size(); k++) {
                if (FPDFPage_RemoveObject(page, foundObjects[k].obj)) {
                    FPDFPageObj_Destroy(foundObjects[k].obj);
                }
            }
        }

        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        env->DeleteLocalRef(jtext);
        env->DeleteLocalRef(editObj);
    }

    // Save Logic (Standard WriteBlock)
    FILE* file = fopen(out_path, "wb");
    jboolean success = JNI_FALSE;
    if (file) {
        MyFileWrite writer;
        writer.dest.version = 1;
        writer.dest.WriteBlock = TestBlockWrite;
        writer.file = file;
        success = FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL);
        fclose(file);
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(input_path, in_path);
    env->ReleaseStringUTFChars(output_path, out_path);
    return success;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfHighlightSaver_nativeSaveTextEdits2(
        JNIEnv *env, jobject thiz,
        jstring input_path, jstring output_path,
        jobjectArray text_edits) {

    const char *in_path = env->GetStringUTFChars(input_path, NULL);
    const char *out_path = env->GetStringUTFChars(output_path, NULL);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(in_path, NULL);
    if (!doc) {
        env->ReleaseStringUTFChars(input_path, in_path);
        env->ReleaseStringUTFChars(output_path, out_path);
        return JNI_FALSE;
    }

    jclass editClazz = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfTextEditNative");
    jfieldID fPageIdx = env->GetFieldID(editClazz, "pageIndex", "I");
    jfieldID fLeft    = env->GetFieldID(editClazz, "left", "F");
    jfieldID fBottom  = env->GetFieldID(editClazz, "bottom", "F");
    jfieldID fRight   = env->GetFieldID(editClazz, "right", "F");
    jfieldID fTop     = env->GetFieldID(editClazz, "top", "F");
    jfieldID fNewText = env->GetFieldID(editClazz, "newText", "Ljava/lang/String;");

    int editCount = env->GetArrayLength(text_edits);

    for (int e = 0; e < editCount; e++) {
        jobject editObj = env->GetObjectArrayElement(text_edits, e);
        int pageIdx = env->GetIntField(editObj, fPageIdx);
        float selL = env->GetFloatField(editObj, fLeft);
        float selB = env->GetFloatField(editObj, fBottom);
        float selR = env->GetFloatField(editObj, fRight);
        float selT = env->GetFloatField(editObj, fTop);
        jstring jtext = (jstring) env->GetObjectField(editObj, fNewText);

        FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
        if (!page) { env->DeleteLocalRef(editObj); continue; }

        std::vector<FPDF_PAGEOBJECT> objectsToRemove;
        float firstL = 1000000.0f, firstB = 1000000.0f;
        float fontSize = 12.0f;

        // 1. Identify ALL objects in the selection area
        int objCount = FPDFPage_CountObjects(page);
        for (int i = 0; i < objCount; i++) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
            if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;

            float L, B, R, T;
            if (FPDFPageObj_GetBounds(obj, &L, &B, &R, &T)) {
                if (!(R < selL || L > selR || T < selB || B > selT)) {
                    objectsToRemove.push_back(obj);
                    // Track the very first position (Anchor)
                    if (L < firstL) { firstL = L; firstB = B; }
                    FPDFTextObj_GetFontSize(obj, &fontSize);
                }
            }
        }

        // 2. REMOVE all selected objects to clear the "Old Data"
        for (FPDF_PAGEOBJECT obj : objectsToRemove) {
            if (FPDFPage_RemoveObject(page, obj)) {
                FPDFPageObj_Destroy(obj);
            }
        }

        // 3. INSERT New Object with New Text (No overlap)
        if (!objectsToRemove.empty()) {
            FPDF_FONT font = FPDFText_LoadStandardFont(doc, "Helvetica");
            FPDF_PAGEOBJECT newTextObj = FPDFPageObj_CreateTextObj(doc, font, fontSize);

            const jchar* textPtr = env->GetStringChars(jtext, NULL);
            int textLen = env->GetStringLength(jtext);
            std::vector<unsigned short> utf16_str(textLen + 1);
            for (int j = 0; j < textLen; j++) utf16_str[j] = (unsigned short)textPtr[j];
            utf16_str[textLen] = 0;

            FPDFText_SetText(newTextObj, (FPDF_WIDESTRING)utf16_str.data());
            env->ReleaseStringChars(jtext, textPtr);

            FPDFPageObj_SetFillColor(newTextObj, 0, 0, 0, 255);
            // Place at the original coordinates
            FPDFPageObj_Transform(newTextObj, 1, 0, 0, 1, firstL, firstB);

            FPDFPage_InsertObject(page, newTextObj);
        }

        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        env->DeleteLocalRef(jtext);
        env->DeleteLocalRef(editObj);
    }

    // Save with FPDF_NO_INCREMENTAL (0) to force clean rewrite
    FILE* file = fopen(out_path, "wb");
    jboolean success = JNI_FALSE;
    if (file) {
        MyFileWrite writer;
        writer.dest.version = 1;
        writer.dest.WriteBlock = TestBlockWrite;
        writer.file = file;
        success = FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, 0);
        fclose(file);
    }

    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(input_path, in_path);
    env->ReleaseStringUTFChars(output_path, out_path);
    return success;
}


// for draw part
JNIEXPORT jlong JNICALL
Java_com_shockwave_pdfium_PdfiumCore_nativeOpenTextPage(JNIEnv *env, jobject thiz, jlong pagePtr) {
    return (jlong)FPDFText_LoadPage((FPDF_PAGE)pagePtr);
}

JNIEXPORT jobject JNICALL
Java_com_shockwave_pdfium_PdfiumCore_nativeGetTextRect(JNIEnv *env, jobject thiz, jlong textPtr, jint index) {
    double l, b, r, t;
    if (!FPDFText_GetCharBox((FPDF_TEXTPAGE)textPtr, index, &l, &r, &b, &t)) return NULL;

    jclass rectClazz = env->FindClass("android/graphics/RectF");
    jmethodID constructor = env->GetMethodID(rectClazz, "<init>", "(FFFF)V");
    // PDFium uses L, R, B, T coordinates
    return env->NewObject(rectClazz, constructor, (float)l, (float)t, (float)r, (float)b);
}

JNIEXPORT jstring JNICALL
Java_com_shockwave_pdfium_PdfiumCore_nativeGetTextUnicode(JNIEnv *env, jobject thiz, jlong textPtr, jint start, jint count) {
    unsigned short buffer[count + 1];
    int actual = FPDFText_GetText((FPDF_TEXTPAGE)textPtr, start, count, buffer);
    return env->NewString((const jchar*)buffer, actual);
}


extern "C"
JNIEXPORT jobjectArray JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeGetTextObjects(
        JNIEnv* env,
        jobject thiz,
        jlong pagePtr,
        jint pageIndex,
        jstring fontCacheDir_) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    if (!page) return nullptr;
    const char* fontCacheDirChars = fontCacheDir_
                                    ? env->GetStringUTFChars(fontCacheDir_, nullptr)
                                    : nullptr;
    const std::string fontCacheDir = fontCacheDirChars ? fontCacheDirChars : "";
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        if (fontCacheDirChars) env->ReleaseStringUTFChars(fontCacheDir_, fontCacheDirChars);
        return nullptr;
    }
    jclass stringClass = env->FindClass("java/lang/String");
    std::vector<jstring> values;
    auto appendValue = [&](const std::string& value) {
        values.push_back(env->NewStringUTF(value.c_str()));
    };
    std::map<FPDF_PAGEOBJECT, int> objectIndices;
    std::map<FPDF_PAGEOBJECT, int> textObjectOrdinals;
    const std::vector<FPDF_PAGEOBJECT> editableTextObjects = CollectEditableTextObjects(page);
    for (size_t ordinal = 0; ordinal < editableTextObjects.size(); ++ordinal) {
        FPDF_PAGEOBJECT object = editableTextObjects[ordinal];
        objectIndices[object] = static_cast<int>(ordinal);
        textObjectOrdinals[object] = static_cast<int>(ordinal);
    }
    std::map<FPDF_PAGEOBJECT, int> ownerOffsets;
    struct TextDecorationPathBounds {
        float left;
        float bottom;
        float right;
        float top;
        unsigned int r;
        unsigned int g;
        unsigned int b;
        unsigned int a;
    };
    std::vector<TextDecorationPathBounds> decorationPaths;
    const int pageObjectCount = FPDFPage_CountObjects(page);
    for (int pageObjectIndex = 0; pageObjectIndex < pageObjectCount; ++pageObjectIndex) {
        FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, pageObjectIndex);
        if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_PATH) continue;
        float left = 0, bottom = 0, right = 0, top = 0;
        if (!FPDFPageObj_GetBounds(pageObject, &left, &bottom, &right, &top)) continue;
        unsigned int pathR = 0, pathG = 0, pathB = 0, pathA = 255;
        FPDFPageObj_GetStrokeColor(pageObject, &pathR, &pathG, &pathB, &pathA);
        decorationPaths.push_back({left, bottom, right, top, pathR, pathG, pathB, pathA});
    }
    FPDF_PAGEOBJECT segmentOwner = nullptr;
    int segmentStart = 0;
    int segmentEnd = 0;
    double segmentLeft = 0, segmentBottom = 0, segmentRight = 0, segmentTop = 0;
    double previousRight = 0, previousCenterY = 0, previousHeight = 0;
    std::vector<jchar> segmentText;
    std::vector<double> segmentCursorAdvances;

    auto appendSegment = [&]() {
        if (!segmentOwner || segmentText.empty()) return;
        const int objectIndex = objectIndices[segmentOwner];
        unsigned long ownerTextBytes = FPDFTextObj_GetText(segmentOwner, textPage, nullptr, 0);
        std::vector<FPDF_WCHAR> ownerTextBuffer(ownerTextBytes / sizeof(FPDF_WCHAR));
        int ownerCharCount = 0;
        if (ownerTextBytes >= sizeof(FPDF_WCHAR)) {
            FPDFTextObj_GetText(segmentOwner, textPage, ownerTextBuffer.data(), ownerTextBytes);
            ownerCharCount = static_cast<int>(ownerTextBuffer.size()) - 1;
        }
        std::string fontName;
        FPDF_FONT font = FPDFTextObj_GetFont(segmentOwner);
        int fontWeight = 400;
        int italicAngle = 0;
        std::string sourceFontPath;
        if (font) {
            const size_t nameLength = FPDFFont_GetBaseFontName(font, nullptr, 0);
            if (nameLength > 0) {
                std::vector<char> nameBuffer(nameLength);
                if (FPDFFont_GetBaseFontName(font, nameBuffer.data(), nameLength) > 0) {
                    fontName.assign(nameBuffer.data());
                }
            }
            const int extractedWeight = FPDFFont_GetWeight(font);
            if (extractedWeight > 0) fontWeight = extractedWeight;
            FPDFFont_GetItalicAngle(font, &italicAngle);
            size_t fontDataSize = 0;
            if (!fontCacheDir.empty() &&
                FPDFFont_GetFontData(font, nullptr, 0, &fontDataSize) &&
                fontDataSize > 0) {
                std::vector<uint8_t> fontData(fontDataSize);
                size_t writtenSize = 0;
                if (FPDFFont_GetFontData(
                        font,
                        fontData.data(),
                        fontData.size(),
                        &writtenSize) &&
                    writtenSize == fontDataSize) {
                    uint64_t fontHash = 1469598103934665603ULL;
                    for (uint8_t byte : fontData) {
                        fontHash ^= byte;
                        fontHash *= 1099511628211ULL;
                    }
                    const bool isOpenType = fontData.size() >= 4 &&
                            fontData[0] == 'O' && fontData[1] == 'T' &&
                            fontData[2] == 'T' && fontData[3] == 'O';
                    const bool isType1 = (fontData.size() >= 2 &&
                            fontData[0] == 0x80 && fontData[1] == 0x01) ||
                            (fontData.size() >= 2 &&
                             fontData[0] == '%' && fontData[1] == '!');
                    const char* fontExtension = isOpenType
                                                ? ".otf"
                                                : (isType1 ? ".pfb" : ".ttf");
                    sourceFontPath = fontCacheDir + "/source_" +
                            std::to_string(fontHash) + fontExtension;
                    std::ifstream existingFont(sourceFontPath, std::ios::binary | std::ios::ate);
                    const bool hasCachedFont = existingFont &&
                            static_cast<size_t>(existingFont.tellg()) == fontDataSize;
                    existingFont.close();
                    if (!hasCachedFont) {
                        std::ofstream fontOutput(sourceFontPath, std::ios::binary | std::ios::trunc);
                        if (fontOutput) {
                            fontOutput.write(
                                    reinterpret_cast<const char*>(fontData.data()),
                                    static_cast<std::streamsize>(fontData.size()));
                        }
                        if (!fontOutput.good()) sourceFontPath.clear();
                    }
                } else {
                    sourceFontPath.clear();
                }
            }
        }
        float fontSize = 0.0f;
        FPDFTextObj_GetFontSize(segmentOwner, &fontSize);
        unsigned int r = 0, g = 0, b = 0, a = 255;
        FPDFPageObj_GetFillColor(segmentOwner, &r, &g, &b, &a);
        bool isUnderline = false;
        bool isStrikeout = false;
        const float segmentWidth = static_cast<float>(fabs(segmentRight - segmentLeft));
        const float segmentHeight = static_cast<float>(fabs(segmentTop - segmentBottom));
        if (segmentWidth > 0.01f && segmentHeight > 0.01f) {
            for (const TextDecorationPathBounds& path : decorationPaths) {
                const float pathHeight = fabsf(path.top - path.bottom);
                if (pathHeight > segmentHeight * 0.22f) continue;
                const float overlap = fmaxf(
                        0.0f,
                        fminf(static_cast<float>(segmentRight), path.right) -
                        fmaxf(static_cast<float>(segmentLeft), path.left));
                const float pathWidth = fabsf(path.right - path.left);
                if (overlap < fminf(segmentWidth, pathWidth) * 0.72f) continue;
                if (abs(static_cast<int>(path.r) - static_cast<int>(r)) > 12 ||
                    abs(static_cast<int>(path.g) - static_cast<int>(g)) > 12 ||
                    abs(static_cast<int>(path.b) - static_cast<int>(b)) > 12) continue;
                const float pathY = (path.bottom + path.top) * 0.5f;
                const float underlineY = static_cast<float>(segmentBottom) + segmentHeight * 0.08f;
                const float strikeoutY = static_cast<float>(segmentBottom) + segmentHeight * 0.52f;
                const float tolerance = fmaxf(segmentHeight * 0.18f, 0.75f);
                if (fabsf(pathY - underlineY) <= tolerance) isUnderline = true;
                if (fabsf(pathY - strikeoutY) <= tolerance) isStrikeout = true;
            }
        }
        FS_MATRIX matrix = {1, 0, 0, 1, 0, 0};
        FPDFPageObj_GetMatrix(segmentOwner, &matrix);

        appendValue(std::to_string(objectIndex));
        values.push_back(env->NewString(segmentText.data(), static_cast<jsize>(segmentText.size())));
        appendValue(std::to_string(segmentLeft));
        appendValue(std::to_string(segmentBottom));
        appendValue(std::to_string(segmentRight));
        appendValue(std::to_string(segmentTop));
        appendValue(fontName);
        appendValue(std::to_string(fontSize));
        appendValue(std::to_string(r));
        appendValue(std::to_string(g));
        appendValue(std::to_string(b));
        appendValue(std::to_string(a));
        appendValue(std::to_string(matrix.a));
        appendValue(std::to_string(matrix.b));
        appendValue(std::to_string(matrix.c));
        appendValue(std::to_string(matrix.d));
        appendValue(std::to_string(matrix.e));
        appendValue(std::to_string(matrix.f));
        values.push_back(ownerCharCount > 0
                         ? env->NewString(reinterpret_cast<const jchar*>(ownerTextBuffer.data()), ownerCharCount)
                         : env->NewStringUTF(""));
        appendValue(std::to_string(segmentStart));
        appendValue(std::to_string(segmentEnd));
        appendValue(std::to_string(textObjectOrdinals[segmentOwner]));
        appendValue(std::to_string(fontWeight));
        appendValue(std::to_string(italicAngle));
        appendValue(sourceFontPath);
        std::ostringstream cursorAdvanceStream;
        cursorAdvanceStream << std::setprecision(12);
        for (size_t index = 0; index < segmentCursorAdvances.size(); ++index) {
            if (index > 0) cursorAdvanceStream << ',';
            cursorAdvanceStream << segmentCursorAdvances[index];
        }
        appendValue(cursorAdvanceStream.str());
        appendValue(isUnderline ? "1" : "0");
        appendValue(isStrikeout ? "1" : "0");
        segmentText.clear();
        segmentCursorAdvances.clear();
    };

    const int charCount = FPDFText_CountChars(textPage);
    for (int charIndex = 0; charIndex < charCount; ++charIndex) {
        FPDF_PAGEOBJECT owner = FPDFText_GetTextObject(textPage, charIndex);
        if (!owner || objectIndices.find(owner) == objectIndices.end()) continue;
        const unsigned int unicode = FPDFText_GetUnicode(textPage, charIndex);
        const int utf16Units = unicode > 0xFFFF ? 2 : (unicode == 0 ? 0 : 1);
        const int ownerOffset = ownerOffsets[owner];
        ownerOffsets[owner] += utf16Units;
        double left, right, bottom, top;
        if (unicode == 0 || unicode == '\r' || unicode == '\n' ||
            !FPDFText_GetCharBox(textPage, charIndex, &left, &right, &bottom, &top)) {
            appendSegment();
            segmentOwner = nullptr;
            continue;
        }
        const double centerY = (top + bottom) * 0.5;
        const double height = std::fabs(top - bottom);
        const bool newVisualLine = segmentOwner &&
                (owner != segmentOwner ||
                 std::fabs(centerY - previousCenterY) > std::max(height, previousHeight) * 0.55 ||
                 left < previousRight - std::max(height, previousHeight) * 0.25 ||
                 left - previousRight > std::max(height, previousHeight) * 2.0);
        if (newVisualLine) appendSegment();
        FS_MATRIX ownerMatrix = {1, 0, 0, 1, 0, 0};
        FPDFPageObj_GetMatrix(owner, &ownerMatrix);
        const double baselineLengthSquared =
                static_cast<double>(ownerMatrix.a) * ownerMatrix.a +
                static_cast<double>(ownerMatrix.b) * ownerMatrix.b;
        double originX = left;
        double originY = bottom;
        FPDFText_GetCharOrigin(textPage, charIndex, &originX, &originY);
        auto projectToTextAdvance = [&](double x, double y) {
            if (baselineLengthSquared <= 1e-12) return x;
            return ((x - ownerMatrix.e) * ownerMatrix.a +
                    (y - ownerMatrix.f) * ownerMatrix.b) / baselineLengthSquared;
        };
        const double characterStartAdvance = projectToTextAdvance(originX, originY);
        double characterEndAdvance = characterStartAdvance;
        const double cornerAdvances[] = {
                projectToTextAdvance(left, bottom),
                projectToTextAdvance(left, top),
                projectToTextAdvance(right, bottom),
                projectToTextAdvance(right, top)
        };
        for (double cornerAdvance : cornerAdvances) {
            characterEndAdvance = std::max(characterEndAdvance, cornerAdvance);
        }
        if (segmentText.empty()) {
            segmentOwner = owner;
            segmentStart = ownerOffset;
            segmentEnd = ownerOffset;
            segmentLeft = left;
            segmentBottom = bottom;
            segmentRight = right;
            segmentTop = top;
        } else {
            segmentLeft = std::min(segmentLeft, left);
            segmentBottom = std::min(segmentBottom, bottom);
            segmentRight = std::max(segmentRight, right);
            segmentTop = std::max(segmentTop, top);
            if (!segmentCursorAdvances.empty()) {
                // The next glyph origin is the exact PDF advance of the preceding glyph,
                // including TJ/word/character spacing used by justified text.
                segmentCursorAdvances.back() = characterStartAdvance;
            }
        }
        if (unicode <= 0xFFFF) {
            segmentText.push_back(static_cast<jchar>(unicode));
        } else {
            const unsigned int codePoint = unicode - 0x10000;
            segmentText.push_back(static_cast<jchar>(0xD800 + (codePoint >> 10)));
            segmentText.push_back(static_cast<jchar>(0xDC00 + (codePoint & 0x3FF)));
        }
        if (segmentCursorAdvances.empty()) {
            segmentCursorAdvances.push_back(characterStartAdvance);
        }
        if (utf16Units == 2) segmentCursorAdvances.push_back(characterStartAdvance);
        segmentCursorAdvances.push_back(characterEndAdvance);
        segmentEnd = ownerOffset + utf16Units;
        previousRight = right;
        previousCenterY = centerY;
        previousHeight = height;
    }
    appendSegment();
    FPDFText_ClosePage(textPage);
    jobjectArray result = env->NewObjectArray((jsize)values.size(), stringClass, nullptr);
    for (jsize i = 0; i < (jsize)values.size(); ++i) {
        env->SetObjectArrayElement(result, i, values[i]);
        env->DeleteLocalRef(values[i]);
    }
    env->DeleteLocalRef(stringClass);
    if (fontCacheDirChars) env->ReleaseStringUTFChars(fontCacheDir_, fontCacheDirChars);
    return result;
}


extern "C" JNIEXPORT jstring JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeGetTextForObject(JNIEnv *env, jobject thiz, jlong pagePtr, jint objectIndex) {
    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    if (!page) return env->NewStringUTF("");

    // 1. Load the text page (This maps to the charIndex/objectIndex you have)
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) return env->NewStringUTF("");

    // 2. Prepare a buffer for 1 UTF-16 character + null terminator
    unsigned short buffer[2];

    // 🔥 FIX: Use the 'objectIndex' passed from Kotlin as the character index
    // FPDFText_GetText(textPage, start_index, count, buffer)
    int count = FPDFText_GetText(textPage, (int)objectIndex, 1, buffer);

    // 3. Close the text page to prevent memory leaks
    FPDFText_ClosePage(textPage);

    // 4. If we found a character, convert it to a Java String
    if (count > 0) {
        // We only want 1 character, so we pass 1 as the length
        return env->NewString((const jchar*)buffer, 1);
    }

    return env->NewStringUTF("");
}



}//extern C
