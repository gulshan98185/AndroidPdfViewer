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
#include <fpdf_edit.h>
#include <fpdfview.h>
#include <fpdf_doc.h>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <iomanip>
#include <fstream>
#include <cstdint>
#include <fpdf_text.h>

static Mutex sLibraryLock;

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
                                                   jobject arr, jlong textPtr, jint st, jint ed) {
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
    env->CallVoidMethod(arr, arrList_enssurecap, rectCount);
    double left, top, right, bottom;//width=1080,height=1527,left=365,top=621,right=686,bottom=440,deviceX=663,deviceY=400,ptr=543663849984
    int arraySize = env->CallIntMethod(arr, arrList_size);
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
            left = deviceX + offsetX;
            top = deviceY + offsetY;

          int  new_width =deviceRight - left;
          int   new_height =deviceBottom - top;

            right = left + new_width;
            bottom = top + new_height;
            if (i >= arraySize) {
                env->CallBooleanMethod(arr, arrList_add,
                                       env->NewObject(rectF, rectF_, (float) left, (float) top,
                                                      (float) right, (float) bottom));
            } else {
                jobject rI = env->CallObjectMethod(arr, arrList_get, i);
                env->CallVoidMethod(rI, rectF_set, (float) left, (float) top, (float) right,
                                    (float) bottom);
            }
        }
    }
    return rectCount;
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


JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveAnnotations1( // todo old main annotation block
        JNIEnv* env,
        jobject thiz,
        jstring inputPath_,
        jstring outputPath_,
        jobjectArray highlightsArray) {

    const char* inputPath = env->GetStringUTFChars(inputPath_, 0);
    const char* outputPath = env->GetStringUTFChars(outputPath_, 0);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(inputPath, nullptr);
    if (!doc) {
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    int highlightCount = env->GetArrayLength(highlightsArray);
    jclass highlightClass = env->FindClass("com/cv/lufick/compose_editor/data_class/PdfAnnotationNative");

    // Field IDs for coordinates and colors
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

    // add text annotaton
    jfieldID textPropsField = env->GetFieldID(highlightClass, "textProperties", "Ljava/lang/String;");

    // IDs for Java JSONObject parsing via JNI
    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonGetString = env->GetMethodID(jsonClass, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID jsonGetDouble = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID jsonGetBoolean = env->GetMethodID(jsonClass, "getBoolean", "(Ljava/lang/String;)Z");

    jmethodID jsonGetInt = env->GetMethodID(jsonClass, "getInt", "(Ljava/lang/String;)I");

    // free hand drawing
    // JSON Array Methods
    // Get the JSON classes
    jclass jsonArrayClass = env->FindClass("org/json/JSONArray");

// Method IDs for JSONObject
    jmethodID jsonGetArray = env->GetMethodID(jsonClass, "getJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");

// Method IDs for JSONArray
    jmethodID jsonArrayLength = env->GetMethodID(jsonArrayClass, "length", "()I");
    jmethodID jsonArrayGet = env->GetMethodID(jsonArrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");

    jfieldID fhDrawingProperties = env->GetFieldID(highlightClass, "fhDrawingProperties", "Ljava/lang/String;");
    FPDF_PAGE currentPage = nullptr;
    int lastPageIndex = -1;

    for (int i = 0; i < highlightCount; i++) {
        jobject obj = env->GetObjectArrayElement(highlightsArray, i);

        int typeInt = env->GetIntField(obj, typeField);
        int pageIndex = env->GetIntField(obj, pageField);
        float left = env->GetFloatField(obj, leftField);
        float top = env->GetFloatField(obj, topField);
        float right = env->GetFloatField(obj, rightField);
        float bottom = env->GetFloatField(obj, bottomField);
        int r = env->GetIntField(obj, rField);
        int g = env->GetIntField(obj, gField);
        int b = env->GetIntField(obj, bField);

        // Load page only when index changes
        if (pageIndex != lastPageIndex) {
            if (currentPage != nullptr) {
                FPDFPage_GenerateContent(currentPage);
                FPDF_ClosePage(currentPage);
            }
            currentPage = FPDF_LoadPage(doc, pageIndex);
            lastPageIndex = pageIndex;
        }

        if (currentPage) {
            int pdfAnnotType;

            // MAP JAVA TYPE TO PDFIUM TYPE
            if (typeInt == 1) {
                pdfAnnotType = FPDF_ANNOT_UNDERLINE;
            } else if (typeInt == 2) {
                pdfAnnotType = FPDF_ANNOT_STRIKEOUT;
            } else if (typeInt == 3) {
                pdfAnnotType = FPDF_ANNOT_LINK;
            } else if (typeInt == 4) {
                // FORCE SQUARE (5) INSTEAD OF REDACT (28)
                pdfAnnotType = FPDF_ANNOT_SQUARE;
            } else {
                pdfAnnotType = FPDF_ANNOT_HIGHLIGHT;
            }

            LOGE("Creating Annot: JavaType=%d -> PDFType=%d", typeInt, pdfAnnotType);

            FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(currentPage, pdfAnnotType);

            if (annot) {
                FS_RECTF rect;
                rect.left = fmin(left, right);
                rect.right = fmax(left, right);
                rect.bottom = fmin(top, bottom);
                rect.top = fmax(top, bottom);
                FPDFAnnot_SetRect(annot, &rect);

                if (typeInt == 3) {
                    jstring jUrl = (jstring)env->GetObjectField(obj, urlField);
                    if (jUrl) {
                        const char* url = env->GetStringUTFChars(jUrl, 0);
                        FPDFAnnot_SetURI(annot, url);
                        LOGE("Link URI set: %s", url);
                        env->ReleaseStringUTFChars(jUrl, url);
                    }

                    // B. Add QuadPoints for the Link (helps viewers show the clickable area)
                    FS_QUADPOINTSF qp;
                    qp.x1 = rect.left;  qp.y1 = rect.top;
                    qp.x2 = rect.right; qp.y2 = rect.top;
                    qp.x3 = rect.left;  qp.y3 = rect.bottom;
                    qp.x4 = rect.right; qp.y4 = rect.bottom;
                    FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                } else if (typeInt == 4) {
                    // REDACTION LOGIC (Using Square) - as we not have actual redaction active in this version
                    // Border color Black
                    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 0, 0, 0, 255);
                    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, 0, 0, 0, 255);

                    LOGE("Redaction created successfully using SQUARE type");
                }
                else if (typeInt == 5) {
                    jstring jJsonStr = (jstring)env->GetObjectField(obj, textPropsField);
                    if (!jJsonStr) continue;

                    // Use the base r, g, b passed into the function as the source of truth
                    int textRedColor = r;
                    int textGreenColor = g;
                    int textBlueColor = b;

                    jobject jsonObject = env->NewObject(jsonClass, jsonInit, jJsonStr);
                    jstring jText = (jstring)env->CallObjectMethod(jsonObject, jsonGetString, env->NewStringUTF("text"));
                    jstring jFont = (jstring)env->CallObjectMethod(jsonObject, jsonGetString, env->NewStringUTF("font"));
                    jstring jAlign = (jstring)env->CallObjectMethod(jsonObject, jsonGetString, env->NewStringUTF("alignment"));
                    jstring jFontPath = (jstring)env->CallObjectMethod(jsonObject, jsonGetString, env->NewStringUTF("fontPath"));

                    jboolean hasUnderline = env->CallBooleanMethod(jsonObject, jsonGetBoolean, env->NewStringUTF("underline"));
                    jboolean hasStrikeout = env->CallBooleanMethod(jsonObject, jsonGetBoolean, env->NewStringUTF("strikeout"));
                    jboolean isBold = env->CallBooleanMethod(jsonObject, jsonGetBoolean, env->NewStringUTF("bold"));
                    jboolean isItalic = env->CallBooleanMethod(jsonObject, jsonGetBoolean, env->NewStringUTF("italic"));
                    jboolean hasBg = env->CallBooleanMethod(jsonObject, jsonGetBoolean, env->NewStringUTF("hasBackground"));

                    // Extract New Size and Dimension Data
                    double jsonSize = 0, jsonWidth = 0, jsonHeight = 0, rotation = 0;
                    try {
                        jsonSize = env->CallDoubleMethod(jsonObject, jsonGetDouble, env->NewStringUTF("size"));
                        jsonWidth = env->CallDoubleMethod(jsonObject, jsonGetDouble, env->NewStringUTF("width"));
                        jsonHeight = env->CallDoubleMethod(jsonObject, jsonGetDouble, env->NewStringUTF("height"));
                        rotation = env->CallDoubleMethod(jsonObject, jsonGetDouble, env->NewStringUTF("rotation"));
                    } catch (...) {}

                    const char16_t* textContent = (const char16_t*)env->GetStringChars(jText, nullptr);
                    const char* fontName = env->GetStringUTFChars(jFont, nullptr);
                    const char* alignStr = env->GetStringUTFChars(jAlign, nullptr);
                    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;

                    FPDF_ANNOTATION drawAnnot = FPDFPage_CreateAnnot(currentPage, FPDF_ANNOT_STAMP);

                    if (drawAnnot) {
                        // 1. CALCULATE EXPANDED BOUNDING BOX
                        // This prevents the "cut off" issue when rotating long text
                        float initialWidth = (jsonWidth > 0) ? (float)jsonWidth : fabs(right - left);
                        float initialHeight = (jsonHeight > 0) ? (float)jsonHeight : fabs(top - bottom);

                        double angleRad = rotation * M_PI / 180.0;
                        double absCos = fabs(cos(angleRad));
                        double absSin = fabs(sin(angleRad));

                        // Calculate new dimensions to contain the rotated rectangle
                        float expandedWidth = (float)(initialWidth * absCos + initialHeight * absSin);
                        float expandedHeight = (float)(initialHeight * absCos + initialWidth * absSin);

                        float origCenterX = (left + right) / 2.0f;
                        float origCenterY = (top + bottom) / 2.0f;

                        FS_RECTF drawRect;
                        drawRect.left = origCenterX - (expandedWidth / 2.0f);
                        drawRect.right = origCenterX + (expandedWidth / 2.0f);
                        drawRect.bottom = origCenterY - (expandedHeight / 2.0f);
                        drawRect.top = origCenterY + (expandedHeight / 2.0f);

                        FPDFAnnot_SetRect(drawAnnot, &drawRect);

                        // Internal logic uses the center and initial sizes for drawing
                        float rectWidth = initialWidth;
                        float rectHeight = initialHeight;
                        float centerX = origCenterX;
//                        float centerY = origCenterY;
                        float upwardShift = rectHeight * 0.10f;
                        float centerY = origCenterY + upwardShift;

                        double cosA = cos(angleRad);
                        double sinA = sin(angleRad);

                        // -------- BACKGROUND ----------
                        if (hasBg) {
                            int bgR = env->CallIntMethod(jsonObject, jsonGetInt, env->NewStringUTF("bgColorR"));
                            int bgG = env->CallIntMethod(jsonObject, jsonGetInt, env->NewStringUTF("bgColorG"));
                            int bgB = env->CallIntMethod(jsonObject, jsonGetInt, env->NewStringUTF("bgColorB"));
                            double bgOpacity = env->CallDoubleMethod(jsonObject, jsonGetDouble, env->NewStringUTF("bgOpacity"));

                            FPDF_PAGEOBJECT bg = FPDFPageObj_CreateNewRect(-rectWidth/2.0f, -rectHeight/2.0f, rectWidth, rectHeight);
                            FPDFPageObj_SetFillColor(bg, bgR, bgG, bgB, (int)(bgOpacity * 255));
                            FPDFPath_SetDrawMode(bg, 1, JNI_FALSE);
                            FPDFPageObj_Transform(bg, cosA, sinA, -sinA, cosA, centerX, centerY);
                            FPDFAnnot_AppendObject(drawAnnot, bg);
                        }

                        // -------- FONT LOADING ----------
                        FPDF_FONT loadedFont = nullptr;
                        if (fontPath) {
                            FILE* fontFile = fopen(fontPath, "rb");
                            if (fontFile) {
                                fseek(fontFile, 0, SEEK_END);
                                long fontSize = ftell(fontFile);
                                rewind(fontFile);
                                std::vector<uint8_t> fontBuffer(fontSize);
                                fread(fontBuffer.data(), 1, fontSize, fontFile);
                                fclose(fontFile);
                                loadedFont = FPDFText_LoadFont(doc, fontBuffer.data(), fontSize, FPDF_FONT_TRUETYPE, true);
                            }
                        }
                        if (!loadedFont) loadedFont = FPDFText_LoadStandardFont(doc, "Helvetica");

                        // -------- TEXT OBJECT ----------
                        FPDF_PAGEOBJECT textObj = FPDFPageObj_CreateTextObj(doc, loadedFont, 1.0f);
                        if (textObj) {
                            FPDFText_SetText(textObj, (FPDF_WIDESTRING)textContent);
                            FPDFPageObj_SetFillColor(textObj, textRedColor, textGreenColor, textBlueColor, 255);

                            float tL, tB, tR, tT;
                            FPDFPageObj_GetBounds(textObj, &tL, &tB, &tR, &tT);
                            float baseWidth = tR - tL;
                            float baseHeight = tT - tB;

                            // Prioritize requested 'size' while ensuring it fits the box
                            float scale = (float)jsonSize;
                            float autoScaleX = rectWidth / baseWidth;
                            float autoScaleY = rectHeight / baseHeight;

                            if ((baseWidth * scale) > rectWidth || (baseHeight * scale) > rectHeight) {
                                scale = fmin(autoScaleX, autoScaleY) * 0.95f;
                            }

                            float textWidth = baseWidth * scale;
                            float textHeight = baseHeight * scale;

                            float localX = -textWidth / 2.0f;
                            if (strcmp(alignStr, "left") == 0) localX = -rectWidth / 2.0f;
                            else if (strcmp(alignStr, "right") == 0) localX = (rectWidth / 2.0f) - textWidth;

                            float localY = -textHeight / 2.0f;
                            float skewX = isItalic ? 0.25f : 0.0f;

                            double a = cosA * scale;
                            double b = sinA * scale;
                            double c = (-sinA + skewX) * scale;
                            double d = cosA * scale;
                            double e = centerX + (localX * cosA - localY * sinA);
                            double f = centerY + (localX * sinA + localY * cosA);

                            FPDFPageObj_Transform(textObj, a, b, c, d, e, f);

                            if (isBold) {
                                FPDFPageObj_SetStrokeColor(textObj, textRedColor, textGreenColor, textBlueColor, 255);
                                FPDFPageObj_SetStrokeWidth(textObj, textHeight * 0.05f);
                                FPDFPath_SetDrawMode(textObj, 2, JNI_TRUE);
                            }
                            FPDFAnnot_AppendObject(drawAnnot, textObj);

                            // -------- LINES (Underline & Strikeout) --------
                            auto drawLine = [&](float offsetPercent) {
                                float localLineY = localY + textHeight * offsetPercent;
                                FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(0, 0);
                                FPDFPath_LineTo(line, textWidth, 0);

                                double le = centerX + (localX * cosA - localLineY * sinA);
                                double lf = centerY + (localX * sinA + localLineY * cosA);
                                FPDFPageObj_Transform(line, cosA, sinA, -sinA, cosA, le, lf);

                                // Use captured colors to ensure exact match with text
                                FPDFPageObj_SetStrokeColor(line, textRedColor, textGreenColor, textBlueColor, 255);
                                FPDFPageObj_SetFillColor(line, textRedColor, textGreenColor, textBlueColor, 255);
                                FPDFPageObj_SetStrokeWidth(line, textHeight * 0.05f);
                                FPDFPath_SetDrawMode(line, 0, JNI_TRUE);

                                FPDFAnnot_AppendObject(drawAnnot, line);
                            };

                            if (hasUnderline) drawLine(-0.15f);
                            if (hasStrikeout) drawLine(0.35f);
                        }

                        FPDFAnnot_SetFlags(drawAnnot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
                        FPDFPage_CloseAnnot(drawAnnot);
                    }

                    // Cleanup
                    env->ReleaseStringChars(jText, (const jchar*)textContent);
                    env->ReleaseStringUTFChars(jFont, fontName);
                    env->ReleaseStringUTFChars(jAlign, alignStr);
                    if (jFontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
                    env->DeleteLocalRef(jsonObject);
                }
                else if (typeInt == 6) {
                    jstring jJsonStr = (jstring)env->GetObjectField(obj, fhDrawingProperties);
                    if (!jJsonStr) continue;

                    jobject jsonObject = env->NewObject(jsonClass, jsonInit, jJsonStr);
                    jstring jMode = (jstring)env->CallObjectMethod(jsonObject, jsonGetString, env->NewStringUTF("mode"));
                    const char* modeStr = env->GetStringUTFChars(jMode, nullptr);

                    jobject pointsArray = (jobject)env->CallObjectMethod(jsonObject, jsonGetArray, env->NewStringUTF("points"));
                    int pointsCount = env->CallIntMethod(pointsArray, jsonArrayLength);

                    double strokeWidth = 1.0;
                    int alphaValue = 255;
                    try {
                        strokeWidth = env->CallDoubleMethod(jsonObject, jsonGetDouble, env->NewStringUTF("strokeWidth"));
                        alphaValue = env->CallIntMethod(jsonObject, jsonGetInt, env->NewStringUTF("alpha"));
                    } catch (...) {}

                    if (pointsCount >= 2) {
                        jobject ptStart = (jobject)env->CallObjectMethod(pointsArray, jsonArrayGet, 0);
                        float startX = (float)env->CallDoubleMethod(ptStart, jsonGetDouble, env->NewStringUTF("x"));
                        float startY = (float)env->CallDoubleMethod(ptStart, jsonGetDouble, env->NewStringUTF("y"));

                        FPDF_PAGEOBJECT pathObj = FPDFPageObj_CreateNewPath(startX, startY);
                        env->DeleteLocalRef(ptStart);

                        for (int i = 1; i < pointsCount; i++) {
                            jobject pt = (jobject)env->CallObjectMethod(pointsArray, jsonArrayGet, i);
                            float px = (float)env->CallDoubleMethod(pt, jsonGetDouble, env->NewStringUTF("x"));
                            float py = (float)env->CallDoubleMethod(pt, jsonGetDouble, env->NewStringUTF("y"));
                            FPDFPath_LineTo(pathObj, px, py);
                            env->DeleteLocalRef(pt);
                        }

                        // --- ENHANCED VISUALS ---
                        FPDFPageObj_SetStrokeWidth(pathObj, (float)strokeWidth);

                        // Handle Line Join and Cap (Makes it look smoother/less "jagged")
                        FPDFPageObj_SetLineJoin(pathObj, FPDF_LINEJOIN_ROUND);
                        FPDFPageObj_SetLineCap(pathObj, FPDF_LINECAP_ROUND);

                        if (strcmp(modeStr, "HIGHLIGHTER") == 0) {
                            // Highlighter: Force lower alpha and semi-transparent mode
                            FPDFPageObj_SetStrokeColor(pathObj, r, g, b, 125);
                            // Optional: If your PDFium build supports it, you can set Blend Mode to Multiply
                             FPDFPageObj_SetBlendMode(pathObj, "Multiply");
                        } else {
                            FPDFPageObj_SetStrokeColor(pathObj, r, g, b, alphaValue);
                        }

                        FPDFPath_SetDrawMode(pathObj, 0, JNI_TRUE);
                        FPDFPage_InsertObject(currentPage, pathObj);
                    }

                    env->ReleaseStringUTFChars(jMode, modeStr);
                    env->DeleteLocalRef(pointsArray);
                    env->DeleteLocalRef(jsonObject);
                }
                else {
                    // HIGHLIGHT/UNDERLINE LOGIC
                    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);

                    FS_QUADPOINTSF quadPoints;
                    quadPoints.x1 = rect.left;  quadPoints.y1 = rect.top;
                    quadPoints.x2 = rect.right; quadPoints.y2 = rect.top;
                    quadPoints.x3 = rect.left;  quadPoints.y3 = rect.bottom;
                    quadPoints.x4 = rect.right; quadPoints.y4 = rect.bottom;
                    FPDFAnnot_AppendAttachmentPoints(annot, &quadPoints);
                }

                FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
                FPDFPage_CloseAnnot(annot);
            } else {
                // If this still fails for type 5, then there is a problem with the Page handle
                LOGE("ANNOT CREATION FAILED for type %d", pdfAnnotType);
            }
        }
        env->DeleteLocalRef(obj);
    }

    // Clean up last page and save
    if (currentPage != nullptr) {
        FPDFPage_GenerateContent(currentPage);
        FPDF_ClosePage(currentPage);
    }

    FILE* file = fopen(outputPath, "wb");
    if (!file) {
        FPDF_CloseDocument(doc);
        env->ReleaseStringUTFChars(inputPath_, inputPath);
        env->ReleaseStringUTFChars(outputPath_, outputPath);
        return JNI_FALSE;
    }

    PdfFileWriter writer{};
    writer.base.version = 1;
    writer.base.WriteBlock = WriteBlock; // Ensure your WriteBlock function is defined
    writer.file = file;

    int success = FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL);
    if (!success) LOGE("FPDF_SaveAsCopy failed!");

    fclose(file);
    FPDF_CloseDocument(doc);
    env->ReleaseStringUTFChars(inputPath_, inputPath);
    env->ReleaseStringUTFChars(outputPath_, outputPath);

    return success ? JNI_TRUE : JNI_FALSE;
}

// --- Helper Function Signatures --- Main Code for saving
static void processLink(JNIEnv* env, jobject obj, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID urlField);
static void processStickyNoteComment(JNIEnv* env, jobject obj, FPDF_ANNOTATION annot, jfieldID commentPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static void processTextStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static void processFreeText(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static bool processStickerStamp(JNIEnv* env, FPDF_DOCUMENT doc, FPDF_ANNOTATION annot, FS_RECTF rect, jobject json, jstring jJsonStr, jmethodID optS, jmethodID optD, jmethodID optI, jmethodID optB, int r, int g, int b, int alpha);
static bool processSvgPathStamp(JNIEnv* env, FPDF_ANNOTATION annot, FS_RECTF rect, jobject json, jstring jJsonStr, jmethodID optS, jmethodID optD, jmethodID optI, jmethodID optB, int r, int g, int b, int alpha);
static bool processImageOrPresetStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID imagePropsField, jclass jsonClass, jmethodID jsonInit);
static bool processPdfShape(JNIEnv* env, jobject obj, FPDF_ANNOTATION annot, FS_RECTF rect, int typeInt, jfieldID shapePropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit);
static void processFreeHand(JNIEnv* env, jobject obj, FPDF_PAGE page, jfieldID fhDrawingProperties, int r, int g, int b, jclass jsonClass, jmethodID jsonInit);

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
    return "right_pointer";
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
    return fmax(13.0f, fmin(fmax(lineLength * 0.16f, strokeWidth * 9.0f), 24.0f));
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
        float strokeWidth
) {
    if (!annot) return false;

    const float effectiveStrokeWidth = fmax(strokeWidth, 0.0f);
    const bool shouldStroke = strokeA > 0 && effectiveStrokeWidth > 0.0f;
    const bool shouldFill = fillA > 0 && typeInt != 16 && typeInt != 17 && typeInt != 18;
    if (!shouldStroke && !shouldFill) return false;

    FPDF_PAGEOBJECT path = nullptr;
    auto appendPoint = [&](PdfShapePoint point, bool first) {
        if (first) {
            FPDFPath_MoveTo(path, point.x, point.y);
        } else {
            FPDFPath_LineTo(path, point.x, point.y);
        }
    };

    if (typeInt == 14) {
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
    } else {
        const PdfShapePoint topLeft = PdfShapePointFromFraction(baseRect, 0.0f, 0.0f, rotationDegrees);
        path = FPDFPageObj_CreateNewPath(topLeft.x, topLeft.y);
        if (!path) return false;
        appendPoint(PdfShapePointFromFraction(baseRect, 1.0f, 0.0f, rotationDegrees), false);
        appendPoint(PdfShapePointFromFraction(baseRect, 1.0f, 1.0f, rotationDegrees), false);
        appendPoint(PdfShapePointFromFraction(baseRect, 0.0f, 1.0f, rotationDegrees), false);
        FPDFPath_Close(path);
    }

    FPDFPageObj_SetStrokeWidth(path, effectiveStrokeWidth);
    FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND);
    FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND);
    FPDFPageObj_SetStrokeColor(path, strokeR, strokeG, strokeB, strokeA);
    FPDFPageObj_SetFillColor(path, fillR, fillG, fillB, fillA);
    FPDFPath_SetDrawMode(path, shouldFill ? 1 : 0, shouldStroke ? 1 : 0);

    if (!FPDFAnnot_AppendObject(annot, path)) {
        FPDFPageObj_Destroy(path);
        return false;
    }
    FPDFAnnot_UpdateObject(annot, path);
    return true;
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
        if (typeInt == 18) {
            const float lineLength = hypotf(end.x - start.x, end.y - start.y);
            const float headLength = ResolvePdfArrowHeadLength(effectiveStrokeWidth, lineLength);
            const float headHalfHeight = fmax(headLength * 0.42f, effectiveStrokeWidth * 2.0f);
            emitMove(OffsetPdfShapePointFromLineEnd(start, end, headLength, headHalfHeight));
            emitLine(end);
            emitMove(OffsetPdfShapePointFromLineEnd(start, end, headLength, -headHalfHeight));
            emitLine(end);
        }
        stream << "S ";
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
        patch << "/L [";
        AppendPdfPoint(patch, points[0]);
        patch << ' ';
        AppendPdfPoint(patch, points[1]);
        patch << "] ";
        patch << "/LE [/None /OpenArrow] ";
        return patch.str();
    }

    patch << "/Vertices [";
    for (size_t index = 0; index < points.size(); index++) {
        if (index > 0) patch << ' ';
        AppendPdfPoint(patch, points[index]);
    }
    patch << "] ";
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

static bool ExtractLastTrailerDictionary(const std::string& data, std::string* outTrailer) {
    if (!outTrailer) return false;
    const size_t trailerPos = data.rfind("trailer");
    if (trailerPos == std::string::npos) return false;
    const size_t trailerStart = data.find("<<", trailerPos);
    if (trailerStart == std::string::npos) return false;

    std::vector<size_t> stack;
    for (size_t index = trailerStart; index + 1 < data.size(); index++) {
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

static bool AppendIncrementalPdfObjectUpdates(
        std::string* data,
        std::vector<PdfObjectReplacement>* replacements
) {
    if (!data || !replacements || replacements->empty()) return true;

    long long previousStartXref = 0;
    if (!ParseLastStartXref(*data, &previousStartXref)) return false;

    std::string trailer;
    if (!ExtractLastTrailerDictionary(*data, &trailer)) return false;

    size_t prevStart = 0;
    size_t prevEnd = 0;
    if (FindPdfDictionaryValueSegment(trailer, 0, trailer.size(), "Prev", &prevStart, &prevEnd)) {
        trailer.replace(prevStart, prevEnd - prevStart, prevEnd - prevStart, ' ');
    }
    const size_t trailerInsert = trailer.rfind(">>");
    if (trailerInsert == std::string::npos) return false;
    trailer.insert(trailerInsert, "/Prev " + std::to_string(previousStartXref) + " ");

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
        const float bubbleLeft = x(0.12f);
        const float bubbleRight = x(0.82f);
        const float bubbleTop = y(0.12f);
        const float bubbleBottom = y(0.76f);
        const float bubbleRadius = (bubbleTop - bubbleBottom) * 0.22f;
        AppendPdfRoundedRectPath(stream, bubbleLeft, bubbleBottom, bubbleRight, bubbleTop, bubbleRadius);
        stream << bubbleLeft + ((bubbleRight - bubbleLeft) * 0.26f) << ' ' << bubbleBottom << " m "
               << bubbleLeft + ((bubbleRight - bubbleLeft) * 0.38f) << ' ' << y(0.94f) << " l "
               << bubbleLeft + ((bubbleRight - bubbleLeft) * 0.46f) << ' ' << bubbleBottom << " l h f ";
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

    jstring jJsonStr = (jstring)env->GetObjectField(obj, commentPropsField);
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
    jstring iconKeyKey = env->NewStringUTF("iconKey");
    jstring createdAtRawKey = env->NewStringUTF("createdAtRaw");
    jstring jTitle = (jstring)env->CallObjectMethod(json, optString, titleKey);
    jstring jText = (jstring)env->CallObjectMethod(json, optString, textKey);
    jstring jIconKey = (jstring)env->CallObjectMethod(json, optString, iconKeyKey);
    jstring jCreatedAtRaw = (jstring)env->CallObjectMethod(json, optString, createdAtRawKey);

    if (jTitle && env->GetStringLength(jTitle) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "T", jTitle);
    }
    if (jText && env->GetStringLength(jText) > 0) {
        SetAnnotWideStringValueFromJString(env, annot, "Contents", jText);
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
    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jCreatedAtRaw);
    env->DeleteLocalRef(createdAtRawKey);
    env->DeleteLocalRef(iconKeyKey);
    env->DeleteLocalRef(textKey);
    env->DeleteLocalRef(titleKey);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(jJsonStr);
}
// --- HELPER 2: TEXT STAMP LOGIC ---
static void processTextStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit) {
    jstring jJsonStr = (jstring)env->GetObjectField(obj, textPropsField);
    if (!jJsonStr) return;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    jmethodID getS = env->GetMethodID(jsonClass, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID getD = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID getB = env->GetMethodID(jsonClass, "getBoolean", "(Ljava/lang/String;)Z");
    jmethodID getI = env->GetMethodID(jsonClass, "getInt", "(Ljava/lang/String;)I");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");
    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;)Ljava/lang/String;");

    jstring jText = (jstring)env->CallObjectMethod(json, getS, env->NewStringUTF("text"));
    jstring jFont = (jstring)env->CallObjectMethod(json, getS, env->NewStringUTF("font"));
    jstring jAlign = (jstring)env->CallObjectMethod(json, getS, env->NewStringUTF("alignment"));
    jstring jFontPath = (jstring)env->CallObjectMethod(json, getS, env->NewStringUTF("fontPath"));

    jboolean hasUnderline = env->CallBooleanMethod(json, getB, env->NewStringUTF("underline"));
    jboolean hasStrikeout = env->CallBooleanMethod(json, getB, env->NewStringUTF("strikeout"));
    jboolean isBold = env->CallBooleanMethod(json, getB, env->NewStringUTF("bold"));
    jboolean isItalic = env->CallBooleanMethod(json, getB, env->NewStringUTF("italic"));
    jboolean hasBg = env->CallBooleanMethod(json, getB, env->NewStringUTF("hasBackground"));

    double rotation = 0, jsonSize = 0, jsonWidth = 0, jsonHeight = 0;
    try {
        rotation = env->CallDoubleMethod(json, getD, env->NewStringUTF("rotation"));
        jsonSize = env->CallDoubleMethod(json, getD, env->NewStringUTF("size"));
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

    const char16_t* textContent = (const char16_t*)env->GetStringChars(jText, nullptr);
    const char* fontName = jFont ? env->GetStringUTFChars(jFont, nullptr) : nullptr;
    const char* alignStr = env->GetStringUTFChars(jAlign, nullptr);
    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;

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

    // Keep PDF text anchored to the exported object center. The extra vertical
    // offset made saved text appear slightly shifted compared to the canvas.
    float centerY = origCenterY;
    double cosA = cos(angleRad), sinA = sin(angleRad);

    if (hasBg) {
        FPDF_PAGEOBJECT bg = FPDFPageObj_CreateNewRect(-initialWidth/2.0f, -initialHeight/2.0f, initialWidth, initialHeight);
        FPDFPageObj_SetFillColor(bg, env->CallIntMethod(json, getI, env->NewStringUTF("bgColorR")),
                                 env->CallIntMethod(json, getI, env->NewStringUTF("bgColorG")),
                                 env->CallIntMethod(json, getI, env->NewStringUTF("bgColorB")),
                                 (int)(env->CallDoubleMethod(json, getD, env->NewStringUTF("bgOpacity")) * 255));
        FPDFPath_SetDrawMode(bg, 1, JNI_FALSE);
        FPDFPageObj_Transform(bg, cosA, sinA, -sinA, cosA, origCenterX, centerY);
        FPDFAnnot_AppendObject(annot, bg);
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

    // Build the text object at unit font size, then apply the editor text size
    // through the object transform. This matches the old canvas/export behavior
    // more closely than treating the editor size as a PDF point size directly.
    FPDF_PAGEOBJECT textObj = FPDFPageObj_CreateTextObj(doc, loadedFont, 1.0f);
    if (textObj) {
        FPDFText_SetText(textObj, (FPDF_WIDESTRING)textContent);
        FPDFPageObj_SetFillColor(textObj, textR, textG, textB, textA);
        float tL, tB, tR, tT; FPDFPageObj_GetBounds(textObj, &tL, &tB, &tR, &tT);
        float bW = tR - tL, bH = tT - tB;
        float scale = (jsonSize > 0.0) ? (float)jsonSize : 12.0f;
        if (bW > initialWidth || bH > initialHeight) {
            scale = fmin(initialWidth / fmax(bW, 0.0001f), initialHeight / fmax(bH, 0.0001f)) * 0.95f;
        } else if ((bW * scale) > initialWidth || (bH * scale) > initialHeight) {
            scale = fmin(initialWidth / fmax(bW, 0.0001f), initialHeight / fmax(bH, 0.0001f)) * 0.95f;
        }

        float scaledLeft = tL * scale, scaledBottom = tB * scale;
        float scaledRight = tR * scale, scaledTop = tT * scale;
        float textW = scaledRight - scaledLeft, textH = scaledTop - scaledBottom;

        float alignedLeft = -textW / 2.0f;
        if (strcmp(alignStr, "left") == 0) alignedLeft = -initialWidth / 2.0f;
        else if (strcmp(alignStr, "right") == 0) alignedLeft = (initialWidth / 2.0f) - textW;

        float alignedBottom = -textH / 2.0f;
        float lX = alignedLeft - scaledLeft;
        float lY = alignedBottom - scaledBottom;
        float skewX = isItalic ? 0.25f : 0.0f;
        FPDFPageObj_Transform(textObj, cosA * scale, sinA * scale, (-sinA + skewX) * scale, cosA * scale,
                              origCenterX + (lX * cosA - lY * sinA), centerY + (lX * sinA + lY * cosA));

        if (isBold) {
            FPDFPageObj_SetStrokeColor(textObj, textR, textG, textB, textA);
            FPDFPageObj_SetStrokeWidth(textObj, textH * 0.05f);
            FPDFTextObj_SetTextRenderMode(textObj, FPDF_TEXTRENDERMODE_FILL_STROKE);
        }
        FPDFAnnot_AppendObject(annot, textObj);

        auto drawLine = [&](float baselineOffset) {
            float lineStartX = alignedLeft;
            float lineY = lY + (baselineOffset * scale);
            FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(0, 0);
            FPDFPath_LineTo(line, textW, 0);
            FPDFPageObj_Transform(line, cosA, sinA, -sinA, cosA,
                                  origCenterX + (lineStartX * cosA - lineY * sinA),
                                  centerY + (lineStartX * sinA + lineY * cosA));
            FPDFPageObj_SetStrokeColor(line, textR, textG, textB, textA); FPDFPageObj_SetStrokeWidth(line, textH * 0.05f);
            FPDFPath_SetDrawMode(line, 0, JNI_TRUE); FPDFAnnot_AppendObject(annot, line);
        };
        if (hasUnderline) drawLine(-0.15f);
        if (hasStrikeout) drawLine(0.30f);
    }
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
    env->ReleaseStringChars(jText, (const jchar*)textContent);
    if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
    env->ReleaseStringUTFChars(jAlign, alignStr);
    if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
    env->DeleteLocalRef(json);
}

// --- HELPER 2B: REAL FREE TEXT ANNOTATION LOGIC ---
static void processFreeText(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, int alpha, jclass jsonClass, jmethodID jsonInit) {
    (void)doc;
    jstring jJsonStr = (jstring)env->GetObjectField(obj, textPropsField);
    if (!jJsonStr) return;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    if (!json) {
        env->DeleteLocalRef(jJsonStr);
        return;
    }

    jmethodID optS = env->GetMethodID(jsonClass, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");

    jstring textKey = env->NewStringUTF("text");
    jstring fallbackText = env->NewStringUTF("");
    jstring jText = (jstring)env->CallObjectMethod(json, optS, textKey, fallbackText);
    env->DeleteLocalRef(textKey);
    env->DeleteLocalRef(fallbackText);

    jstring fontKey = env->NewStringUTF("font");
    jstring fallbackFont = env->NewStringUTF("Helvetica");
    jstring jFont = (jstring)env->CallObjectMethod(json, optS, fontKey, fallbackFont);
    env->DeleteLocalRef(fontKey);
    env->DeleteLocalRef(fallbackFont);

    const char* appearanceFont = "Helv";

    auto optIntValue = [&](const char* key, int fallback) -> int {
        jstring jKey = env->NewStringUTF(key);
        const int value = env->CallIntMethod(json, optI, jKey, fallback);
        env->DeleteLocalRef(jKey);
        return value;
    };
    const int textR = optIntValue("textColorR", r);
    const int textG = optIntValue("textColorG", g);
    const int textB = optIntValue("textColorB", b);
    const int bgR = optIntValue("backgroundColorR", optIntValue("bgColorR", 255));
    const int bgG = optIntValue("backgroundColorG", optIntValue("bgColorG", 255));
    const int bgB = optIntValue("backgroundColorB", optIntValue("bgColorB", 255));
    const int bgA = optIntValue("backgroundColorA", optIntValue("bgColorA", 0));

    jstring sizeKey = env->NewStringUTF("size");
    const double fontSizeValue = env->CallDoubleMethod(json, optD, sizeKey, 12.0);
    env->DeleteLocalRef(sizeKey);
    const float fontSize = static_cast<float>(fontSizeValue > 0.0 ? fontSizeValue : 12.0);

    FPDFAnnot_SetRect(annot, &rect);
    std::ostringstream defaultAppearance;
    defaultAppearance << "/" << appearanceFont << " " << fontSize << " Tf "
                      << (textR / 255.0f) << " "
                      << (textG / 255.0f) << " "
                      << (textB / 255.0f) << " rg";
    SetAnnotAsciiStringValue(annot, "DA", defaultAppearance.str().c_str());

    char defaultStyle[128];
    snprintf(defaultStyle, sizeof(defaultStyle), "font: Helvetica %.2fpt; color:#%02X%02X%02X", fontSize, textR, textG, textB);
    SetAnnotAsciiStringValue(annot, "DS", defaultStyle);

    FPDFAnnot_SetBorder(annot, 0, 0, 0);
    if (jText) {
        SetAnnotWideStringValueFromJString(env, annot, "Contents", jText);
    }
    SetAnnotWideStringValueFromJString(env, annot, "LufickFreeTextMeta", jJsonStr);

    if (jText && env->GetStringLength(jText) > 0) {
        const std::u16string text = JStringToUtf16(env, jText);
        const std::u16string appearanceStream = BuildFreeTextAppearanceStream(
                rect,
                text,
                appearanceFont,
                fontSize,
                textR,
                textG,
                textB,
                bgR,
                bgG,
                bgB,
                bgA
        );
        if (!appearanceStream.empty()) {
            FPDFAnnot_SetAP(
                    annot,
                    FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                    reinterpret_cast<FPDF_WIDESTRING>(appearanceStream.c_str())
            );
        }
    }

    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
    if (jFont) env->DeleteLocalRef(jFont);
    if (jText) env->DeleteLocalRef(jText);
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
//------------------------------------------------------------------------------------------------------

static bool processSvgPathStamp(
        JNIEnv* env,
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
        int alpha
) {
    if (!env || !annot || !json || !jJsonStr || !optS || !optD || !optI || !optB) {
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
        int alpha
) {
    if (!env || !doc || !annot || !json || !jJsonStr || !optS || !optD || !optI || !optB) {
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

static bool processImageOrPresetStamp(
        JNIEnv* env,
        jobject obj,
        FPDF_DOCUMENT doc,
        FPDF_PAGE page,
        FPDF_ANNOTATION annot,
        FS_RECTF rect,
        jfieldID imagePropsField,
        jclass jsonClass,
        jmethodID jsonInit
) {
    jstring jJsonStr = (jstring)env->GetObjectField(obj, imagePropsField);
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
                255
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
                255
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
    const double rectWidth = fabs(rect.right - rect.left);
    const double rectHeight = fabs(rect.top - rect.bottom);
    const double rotation = env->CallDoubleMethod(json, optD, jRotationKey, 0.0);
    const double baseWidth = env->CallDoubleMethod(json, optD, jBaseWidthKey, rectWidth);
    const double baseHeight = env->CallDoubleMethod(json, optD, jBaseHeightKey, rectHeight);
    const bool stretchToBounds = env->CallBooleanMethod(json, optB, jStretchToBoundsKey, false);
    env->DeleteLocalRef(jRotationKey);
    env->DeleteLocalRef(jBaseWidthKey);
    env->DeleteLocalRef(jBaseHeightKey);
    env->DeleteLocalRef(jStretchToBoundsKey);

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

    FPDFImageObj_SetMatrix(imageObj, a, b, c, d, e, f);
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
            ? (jstring)env->GetObjectField(obj, shapePropsField)
            : nullptr;
    jobject json = jJsonStr ? env->NewObject(jsonClass, jsonInit, jJsonStr) : nullptr;

    jmethodID optD = env->GetMethodID(jsonClass, "optDouble", "(Ljava/lang/String;D)D");
    jmethodID optI = env->GetMethodID(jsonClass, "optInt", "(Ljava/lang/String;I)I");

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
    const int strokeR = optIntValue("strokeR", r);
    const int strokeG = optIntValue("strokeG", g);
    const int strokeB = optIntValue("strokeB", b);
    const int strokeA = std::max(0, std::min(optIntValue("strokeA", alpha), 255));
    const int fillR = optIntValue("fillR", 0);
    const int fillG = optIntValue("fillG", 0);
    const int fillB = optIntValue("fillB", 0);
    const int requestedFillA = std::max(0, std::min(optIntValue("fillA", 0), 255));
    const int fillA = requestedFillA > 0 ? strokeA : 0;
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
            strokeWidth
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

// --- HELPER 3: FREEHAND LOGIC ---
static void processFreeHand(JNIEnv* env, jobject obj, FPDF_PAGE page, jfieldID fhField, int r, int g, int b, jclass jsonClass, jmethodID jsonInit) {
    jstring jJsonStr = (jstring)env->GetObjectField(obj, fhField);
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
    if (jMode) {
        env->ReleaseStringUTFChars(jMode, modeStr);
        env->DeleteLocalRef(jMode);
    }
    env->DeleteLocalRef(jModeKey);
    env->DeleteLocalRef(jPointsKey);
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

    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);
    if (typeInt == 0) {
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, alpha);
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
    unsigned int r = outR ? *outR : 0;
    unsigned int g = outG ? *outG : 0;
    unsigned int b = outB ? *outB : 0;
    unsigned int a = outA ? *outA : 255;
    FPDFPageObj_GetStrokeColor(pathObj, &r, &g, &b, &a);
    const int lineJoin = FPDFPageObj_GetLineJoin(pathObj);
    const int lineCap = FPDFPageObj_GetLineCap(pathObj);
    freehandProps << "],"
                  << "\"strokeWidth\":" << strokeWidth << ","
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
        const unsigned long pointCount = FPDFAnnot_GetInkListPath(annot, 0, nullptr, 0);
        if (pointCount >= 2) {
            std::vector<FS_POINTF> points(pointCount);
            if (FPDFAnnot_GetInkListPath(annot, 0, points.data(), pointCount) == pointCount) {
                std::ostringstream freehandProps;
                freehandProps << "{\"points\":[";
                for (unsigned long pointIndex = 0; pointIndex < pointCount; pointIndex++) {
                    if (pointIndex > 0) freehandProps << ",";
                    freehandProps << "{"
                                  << "\"x\":" << points[pointIndex].x << ","
                                  << "\"y\":" << points[pointIndex].y
                                  << "}";
                }
                freehandProps << "],"
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

    const jchar* rawMarkupMeta = env->GetStringChars(jMarkupRects, nullptr);
    if (rawMarkupMeta) {
//        FPDFAnnot_SetStringValue(annot, "LufickMarkupMeta", (FPDF_WIDESTRING)rawMarkupMeta);
        env->ReleaseStringChars(jMarkupRects, rawMarkupMeta);
    }

    ClearAnnotationAppearanceObjects(annot);

    const int rectCount = env->CallIntMethod(rectsArray, jsonArrayLength);
    const size_t existingQuadCount = FPDFAnnot_CountAttachmentPoints(annot);
    bool appended = false;
    bool hasBounds = false;
    FS_RECTF bounds = {0.0f, 0.0f, 0.0f, 0.0f};

    for (int rectIndex = 0; rectIndex < rectCount; rectIndex++) {
        jobject rectObj = env->CallObjectMethod(rectsArray, jsonArrayGetObject, rectIndex);
        if (!rectObj) continue;

        const float quadLeft = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("left"));
        const float quadTop = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("top"));
        const float quadRight = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("right"));
        const float quadBottom = (float)env->CallDoubleMethod(rectObj, jsonGetDouble, env->NewStringUTF("bottom"));
        const float rectTop = fmax(quadTop, quadBottom);
        const float rectBottom = fmin(quadTop, quadBottom);
        const float rectHeight = fmax(rectTop - rectBottom, 0.5f);
        const float strokeRatio = GetClampedTextMarkupStrokeRatio(
                typeInt,
                env->CallDoubleMethod(
                        rectObj,
                        jsonOptDouble,
                        env->NewStringUTF("strokeWidthRatio"),
                        GetDefaultTextMarkupStrokeRatio(typeInt)
                )
        );
        const float thickness = (typeInt == 8)
                ? GetSquigglyRenderThickness(rectHeight, strokeRatio)
                : fmax(rectHeight * strokeRatio, 0.5f);

        float attachmentTop = rectTop;
        float attachmentBottom = rectBottom;
        if (typeInt == 1) {
            attachmentTop = rectBottom + thickness;
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
            attachmentTop = centerY + (thickness * 0.5f);
            attachmentBottom = centerY - (thickness * 0.5f);
            const float lineY = (attachmentTop + attachmentBottom) * 0.5f;
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
            attachmentTop = GetSquigglyAttachmentTop(rectBottom, rectTop, thickness);
            AppendSquigglyTextMarkupAppearance(
                    annot,
                    quadLeft,
                    quadRight,
                    rectBottom,
                    attachmentTop,
                    r,
                    g,
                    b,
                    alpha,
                    thickness
            );
        }

        FS_QUADPOINTSF qp = {
                fmin(quadLeft, quadRight), attachmentTop,
                fmax(quadLeft, quadRight), attachmentTop,
                fmin(quadLeft, quadRight), attachmentBottom,
                fmax(quadLeft, quadRight), attachmentBottom
        };
        if (static_cast<size_t>(rectIndex) < existingQuadCount) {
            FPDFAnnot_SetAttachmentPoints(annot, rectIndex, &qp);
        } else {
            FPDFAnnot_AppendAttachmentPoints(annot, &qp);
        }

        const FS_RECTF pieceBounds = {
                fmin(quadLeft, quadRight),
                typeInt == 8 ? attachmentBottom : rectBottom,
                fmax(quadLeft, quadRight),
                typeInt == 8 ? attachmentTop : rectTop
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

    env->DeleteLocalRef(rectsArray);
    env->DeleteLocalRef(jMarkupRects);
}

static bool ApplyNativeAnnotationEditActions(
        JNIEnv* env,
        FPDF_DOCUMENT doc,
        jobjectArray highlightsArray,
        FPDF_PAGE providedPage = nullptr,
        int providedPageIndex = -1
) {
    if (!doc || !highlightsArray) return false;

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
    jfieldID textPropsField = env->GetFieldID(highlightClass, "textProperties", "Ljava/lang/String;");
    jfieldID fhDrawingProperties = env->GetFieldID(highlightClass, "fhDrawingProperties", "Ljava/lang/String;");
    jfieldID imagePropsField = env->GetFieldID(highlightClass, "imageProperties", "Ljava/lang/String;");
    jfieldID shapePropsField = env->GetFieldID(highlightClass, "shapeProperties", "Ljava/lang/String;");
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

    std::map<int, std::vector<int>> removalMap;
    struct FreehandRemovalTarget {
        int objectIndex;
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

    int highlightCount = env->GetArrayLength(highlightsArray);
    for (int i = 0; i < highlightCount; i++) {
        jobject obj = env->GetObjectArrayElement(highlightsArray, i);
        if (!obj) continue;

        int pageIndex = env->GetIntField(obj, pageField);
        int nativeSourceId = env->GetIntField(obj, nativeSourceIdField);
        int nativeEditAction = env->GetIntField(obj, nativeEditActionField);
        if (nativeSourceId >= 0 && nativeEditAction == 1) {
            if (env->GetIntField(obj, typeField) == 6) {
                objectRemovalMap[pageIndex].push_back({
                    nativeSourceId,
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
        }
        env->DeleteLocalRef(obj);
    }

    std::map<int, bool> touchedPages;
    for (const auto& entry : removalMap) touchedPages[entry.first] = true;
    for (const auto& entry : objectRemovalMap) touchedPages[entry.first] = true;
    for (const auto& entry : colorUpdateMap) touchedPages[entry.first] = true;
    for (const auto& entry : rectUpdateMap) touchedPages[entry.first] = true;

    for (const auto& pageEntry : touchedPages) {
        int pageIndex = pageEntry.first;
        bool shouldClosePage = false;
        FPDF_PAGE page = (providedPage && pageIndex == providedPageIndex)
                         ? providedPage
                         : FPDF_LoadPage(doc, pageIndex);
        if (!page) continue;
        if (!(providedPage && pageIndex == providedPageIndex)) {
            shouldClosePage = true;
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
                if (target.objectIndex >= 0 && target.objectIndex < FPDFPage_GetAnnotCount(page)) {
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

                if (target.objectIndex >= 0 && target.objectIndex < FPDFPage_GetAnnotCount(page)) {
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
                    if (pageObject && FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_PATH &&
                        FPDFPage_RemoveObject(page, pageObject)) {
                        FPDFPageObj_Destroy(pageObject);
                        removed = true;
                    }
                }
                if (!removed) {
                    float bestScore = 0.0f;
                    int bestIndex = -1;

                    const int objectCount = FPDFPage_CountObjects(page);
                    for (int objectIndex = 0; objectIndex < objectCount; objectIndex++) {
                        FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, objectIndex);
                        if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_PATH) continue;

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

        if (providedPage != nullptr || providedPageIndex >= 0) {
            for (int i = 0; i < highlightCount; i++) {
                jobject obj = env->GetObjectArrayElement(highlightsArray, i);
                if (!obj) continue;
                int objPageIndex = env->GetIntField(obj, pageField);
                int nativeEditAction = env->GetIntField(obj, nativeEditActionField);
                int typeInt = env->GetIntField(obj, typeField);

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
                    processFreeHand(env, obj, page, fhDrawingProperties, r, g, b, jsonClass, jsonInit);
                } else {
                    int pdfType = (typeInt == 1) ? FPDF_ANNOT_UNDERLINE :
                                   (typeInt == 2) ? FPDF_ANNOT_STRIKEOUT :
                                   (typeInt == 8) ? FPDF_ANNOT_SQUIGGLY :
                                  (typeInt == 3) ? FPDF_ANNOT_LINK :
                                   (typeInt == 10) ? FPDF_ANNOT_TEXT :
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
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 0, 0, 0, 255);
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, 0, 0, 0, 255);
                        }
                        else if (typeInt == 10) processStickyNoteComment(env, obj, annot, textPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        else if (typeInt == 5) processTextStamp(env, obj, doc, page, annot, rect, textPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        else if (typeInt == 11) processFreeText(env, obj, doc, annot, rect, textPropsField, r, g, b, alpha, jsonClass, jsonInit);
                        else if (typeInt == 9) processImageOrPresetStamp(env, obj, doc, page, annot, rect, imagePropsField, jsonClass, jsonInit);
                        else if (typeInt == 7) {
                            processRegionHighlight(env, obj, page, annot, rect, r, g, b, alpha);
                        }
                        else if (IsPdfShapeNativeType(typeInt)) {
                            processPdfShape(env, obj, annot, rect, typeInt, shapePropsField, r, g, b, alpha, jsonClass, jsonInit);
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
                                                rectTop = rectBottom + thickness;
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
                                                    rectTop = centerY + (thickness * 0.5f);
                                                    rectBottom = centerY - (thickness * 0.5f);
                                                    float lineY = (rectTop + rectBottom) * 0.5f;
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
                                                    rectTop = GetSquigglyAttachmentTop(rectBottom, rectTop, thickness);
                                                    AppendSquigglyTextMarkupAppearance(
                                                            annot,
                                                            quadLeft,
                                                            quadRight,
                                                            rectBottom,
                                                            rectTop,
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
//                                FPDFAnnot_SetStringValue(annot, "LufickMarkupMeta", (FPDF_WIDESTRING)rawMarkupMeta);
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
        if (shouldClosePage) {
            FPDF_ClosePage(page);
        }
    }

    return true;
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
    jfieldID textPropsField = env->GetFieldID(highlightClass, "textProperties", "Ljava/lang/String;");
    jfieldID fhDrawingProperties = env->GetFieldID(highlightClass, "fhDrawingProperties", "Ljava/lang/String;");
    jfieldID imagePropsField = env->GetFieldID(highlightClass, "imageProperties", "Ljava/lang/String;");
    jfieldID shapePropsField = env->GetFieldID(highlightClass, "shapeProperties", "Ljava/lang/String;");
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

        if (nativeEditAction == 1 || nativeEditAction == 2 || nativeSourceId >= 0) {
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
                processFreeHand(env, obj, currentPage, fhDrawingProperties, r, g, b, jsonClass, jsonInit);
            } else {
                int pdfType = (typeInt == 1) ? FPDF_ANNOT_UNDERLINE :
                              (typeInt == 2) ? FPDF_ANNOT_STRIKEOUT :
                              (typeInt == 8) ? FPDF_ANNOT_SQUIGGLY :
                              (typeInt == 3) ? FPDF_ANNOT_LINK :
                              (typeInt == 10) ? FPDF_ANNOT_TEXT :
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
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 0, 0, 0, 255);
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, 0, 0, 0, 255);
                    }
                    else if (typeInt == 10) processStickyNoteComment(env, obj, annot, textPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    else if (typeInt == 5) processTextStamp(env, obj, doc, currentPage, annot, rect, textPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    else if (typeInt == 11) processFreeText(env, obj, doc, annot, rect, textPropsField, r, g, b, alpha, jsonClass, jsonInit);
                    else if (typeInt == 9) processImageOrPresetStamp(env, obj, doc, currentPage, annot, rect, imagePropsField, jsonClass, jsonInit);
                    else if (typeInt == 7) {
                        processRegionHighlight(env, obj, currentPage, annot, rect, r, g, b, alpha);
                    }
                    else if (IsPdfShapeNativeType(typeInt)) {
                        processPdfShape(env, obj, annot, rect, typeInt, shapePropsField, r, g, b, alpha, jsonClass, jsonInit);
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
                                            rectTop = rectBottom + thickness;
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
                                                rectTop = centerY + (thickness * 0.5f);
                                                rectBottom = centerY - (thickness * 0.5f);
                                                float lineY = (rectTop + rectBottom) * 0.5f;
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
                                            rectTop = GetSquigglyAttachmentTop(rectBottom, rectTop, thickness);
                                            AppendSquigglyTextMarkupAppearance(
                                                    annot,
                                                    quadLeft,
                                                    quadRight,
                                                    rectBottom,
                                                        rectTop,
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
//                            FPDFAnnot_SetStringValue(annot, "LufickMarkupMeta", (FPDF_WIDESTRING)rawMarkupMeta);
                            env->ReleaseStringChars(jMarkupRects, rawMarkupMeta);
                        }
                        if (!appended) {
                            AppendFallbackTextMarkupAppearance(annot, typeInt, rect, r, g, b, alpha);
                            FS_QUADPOINTSF qp = {rect.left, rect.top, rect.right, rect.top, rect.left, rect.bottom, rect.right, rect.bottom};
                            FPDFAnnot_AppendAttachmentPoints(annot, &qp);
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

JNIEXPORT jobjectArray JNICALL //todo main get annotation method
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeGetAnnotationsForPage(
        JNIEnv* env,
        jobject thiz,
        jlong docPtr,
        jlong pagePtr,
        jint pageIndex,
        jint viewWidth,
        jint viewHeight) {

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
            "(IIFFFFIIIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/graphics/Bitmap;II)V"
    );

    // Use a vector to prevent ArrayIndexOutOfBoundsException
    std::vector<jobject> tempCollector;

    for (int i = 0; i < annotCount; i++) {
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
            case FPDF_ANNOT_LINK:      type = 3; break;
            case FPDF_ANNOT_FREETEXT:
                type = 11;
                usesRectOnly = true;
                break;
            case FPDF_ANNOT_TEXT:
                type = 10;
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
            const bool looksLikeRedaction =
                    (hasStrokeColor && r == 0 && g == 0 && b == 0 && a == 255) &&
                    (!hasInteriorColor || (interiorR == 0 && interiorG == 0 && interiorB == 0 && interiorA == 255));
            const float visibleStrokeWidth = ResolveAnnotVisibleStrokeWidth(annot);
            type = looksLikeRedaction
                   ? 4
                   : (visibleStrokeWidth > 0.0f ? GetPdfBoxShapeTypeFromAnnotBounds(annot) : 7);
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
        }
        jstring jLinkUrl = nullptr;
        jstring jTextProps = nullptr;
        jstring jImageProps = nullptr;
        jstring jShapeProps = nullptr;
        jstring jStoredMarkupRects = nullptr;
        std::ostringstream markupRectsStream;
        bool hasMarkupRects = false;
        const std::u16string shapeMeta = ReadAnnotStringValueUtf16(annot, "LufickPdfShapeMeta");
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
            if (stampKind == u"image" ||
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
                    nullptr,
                    jFhProps,
                    nullptr,
                    nullptr,
                    nullptr,
                    i,
                    0
            );

            if (annotObj) tempCollector.push_back(annotObj);
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

//        const unsigned long markupMetaLength = FPDFAnnot_GetStringValue(annot, "LufickMarkupMeta", nullptr, 0);
//        if (markupMetaLength > sizeof(FPDF_WCHAR)) {
//            std::vector<FPDF_WCHAR> markupMetaBuffer(markupMetaLength / sizeof(FPDF_WCHAR));
//            FPDFAnnot_GetStringValue(annot, "LufickMarkupMeta", markupMetaBuffer.data(), markupMetaLength);
//            const int markupMetaCharCount = static_cast<int>(markupMetaBuffer.size()) - 1;
//            if (markupMetaCharCount > 0) {
//                std::u16string markupMetaValue(
//                        reinterpret_cast<const char16_t*>(markupMetaBuffer.data()),
//                        markupMetaCharCount
//                );
//                std::string markupMetaUtf8;
//                markupMetaUtf8.reserve(markupMetaValue.size());
//                for (char16_t ch : markupMetaValue) {
//                    markupMetaUtf8.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
//                }
//                if (!markupMetaUtf8.empty()) {
//                    jStoredMarkupRects = env->NewStringUTF(markupMetaUtf8.c_str());
//                }
//            }
//        }

        if (usesRectOnly) {
            // Logic for Redaction (Square) - Use the Bounding Box
            FS_RECTF rect;
            if (FPDFAnnot_GetRect(annot, &rect)) {
                int dLeft, dTop, dRight, dBottom;
                FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.left, rect.top, &dLeft, &dTop);
                FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.right, rect.bottom, &dRight, &dBottom);

                jobject annotObj = env->NewObject(annotClass, constructor,
                                                  type, pageIndex, (float)dLeft, (float)dTop, (float)dRight, (float)dBottom,
                                                  (int)r, (int)g, (int)b, (int)a, jLinkUrl, nullptr, jTextProps, nullptr, jImageProps, jShapeProps, nullptr, i, 0);

                if (annotObj) tempCollector.push_back(annotObj);
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

                    jobject annotObj = env->NewObject(annotClass, constructor,
                                                      type, pageIndex, (float)dLeft, (float)dTop, (float)dRight, (float)dBottom,
                                                      (int)r, (int)g, (int)b, (int)a, jLinkUrl, jMarkupRects, jTextProps, nullptr, jImageProps, nullptr, nullptr, i, 0);

                    if (annotObj) tempCollector.push_back(annotObj);
                }
            }
            if (jMarkupRects) env->DeleteLocalRef(jMarkupRects);
        }
        FPDFPage_CloseAnnot(annot);
    }

    const float pageWidth = FPDF_GetPageWidthF(page);
    const float pageHeight = FPDF_GetPageHeightF(page);
    int objectCount = FPDFPage_CountObjects(page);
    for (int i = 0; i < objectCount; i++) {
        FPDF_PAGEOBJECT pageObj = FPDFPage_GetObject(page, i);
        if (!pageObj) continue;
        if (FPDFPageObj_GetType(pageObj) != FPDF_PAGEOBJ_PATH) continue;

        float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
        if (!FPDFPageObj_GetBounds(pageObj, &left, &bottom, &right, &top)) continue;

        const float boundsWidth = fabs(right - left);
        const float boundsHeight = fabs(top - bottom);
        if (boundsWidth < 1.0f && boundsHeight < 1.0f) continue;

        // Skip page artwork/background vectors. Our freehand strokes should not
        // span most of the page in both dimensions.
        const float widthRatio = pageWidth > 0.0f ? (boundsWidth / pageWidth) : 0.0f;
        const float heightRatio = pageHeight > 0.0f ? (boundsHeight / pageHeight) : 0.0f;
        const float areaRatio = (pageWidth > 0.0f && pageHeight > 0.0f)
                ? ((boundsWidth * boundsHeight) / (pageWidth * pageHeight))
                : 0.0f;
        if ((widthRatio > 0.80f && heightRatio > 0.80f) || areaRatio > 0.55f) continue;

        unsigned int r = 0, g = 170, b = 90, a = 255;
        FPDFPageObj_GetStrokeColor(pageObj, &r, &g, &b, &a);
        if (a == 0) continue;
        std::string freehandProps;
        if (!BuildFreehandPropsFromPathObject(pageObj, &freehandProps, &r, &g, &b, &a)) continue;

        int dLeft, dTop, dRight, dBottom;
        FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
        FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);
        const float deviceLeft = static_cast<float>(std::min(dLeft, dRight));
        const float deviceRight = static_cast<float>(std::max(dLeft, dRight));
        const float deviceTop = static_cast<float>(std::min(dTop, dBottom));
        const float deviceBottom = static_cast<float>(std::max(dTop, dBottom));
        jstring jFhProps = env->NewStringUTF(freehandProps.c_str());

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
                nullptr,
                jFhProps,
                nullptr,
                nullptr,
                nullptr,
                i,
                0
        );

        if (annotObj) tempCollector.push_back(annotObj);
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
        jint pageIndex) {

    FPDF_PAGE page = reinterpret_cast<FPDF_PAGE>(pagePtr);
    if (!page) return nullptr;

    // 🔹 Create text page (REQUIRED in your version)
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) return nullptr;

    float pageHeight = FPDF_GetPageHeightF(page);
    int objectCount = FPDFPage_CountObjects(page);

    // Count text objects
    int textObjectCount = 0;
    for (int i = 0; i < objectCount; i++) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (obj && FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT)
            textObjectCount++;
    }

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result =
            env->NewObjectArray(textObjectCount * 6, stringClass, nullptr);

    int resultIndex = 0;

    for (int i = 0; i < objectCount; i++) {

        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj) continue;

        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT)
            continue;

        float left, bottom, right, top;
        if (!FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top))
            continue;

        float androidTop = pageHeight - top;
        float androidBottom = pageHeight - bottom;

        // 🔹 Get required buffer length
        unsigned long length =
                FPDFTextObj_GetText(obj, textPage, nullptr, 0);

        if (length == 0)
            continue;

        std::vector<FPDF_WCHAR> buffer(length);

        FPDFTextObj_GetText(obj, textPage, buffer.data(), length);

        // Convert UTF16 → jstring
//        jstring text = env->NewString(
//                reinterpret_cast<jchar*>(buffer.data()),
//                length - 1); // remove null

        int actualCharCount = (length > 0) ? length - 1 : 0;
        jstring text = (actualCharCount > 0)
                       ? env->NewString(reinterpret_cast<const jchar*>(buffer.data()), actualCharCount)
                       : env->NewStringUTF("");

        // Store values
        env->SetObjectArrayElement(result, resultIndex++,
                                   env->NewStringUTF(std::to_string(left).c_str()));
        env->SetObjectArrayElement(result, resultIndex++,
                                   env->NewStringUTF(std::to_string(androidTop).c_str()));
        env->SetObjectArrayElement(result, resultIndex++,
                                   env->NewStringUTF(std::to_string(right).c_str()));
        env->SetObjectArrayElement(result, resultIndex++,
                                   env->NewStringUTF(std::to_string(androidBottom).c_str()));
        env->SetObjectArrayElement(result, resultIndex++, text);
        env->SetObjectArrayElement(result, resultIndex++,
                                   env->NewStringUTF(std::to_string(i).c_str()));
    }

    // 🔹 Destroy text page (VERY IMPORTANT)
    FPDFText_ClosePage(textPage);

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
