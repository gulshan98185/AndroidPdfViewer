#include "util.hpp"
#include "fpdf_flatten.h"
#include "fpdf_formfill.h"

#define HAVE_PTHREADS true;
extern "C" {
#include <unistd.h>
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
#include <fpdfview.h>
#include <fpdf_doc.h>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>
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
static void processTextStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, jclass jsonClass, jmethodID jsonInit);
static void processFreeHand(JNIEnv* env, jobject obj, FPDF_PAGE page, jfieldID fhDrawingProperties, int r, int g, int b, jclass jsonClass, jmethodID jsonInit);

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

// --- HELPER 2: TEXT STAMP LOGIC ---
static void processTextStamp(JNIEnv* env, jobject obj, FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_ANNOTATION annot, FS_RECTF rect, jfieldID textPropsField, int r, int g, int b, jclass jsonClass, jmethodID jsonInit) {
    jstring jJsonStr = (jstring)env->GetObjectField(obj, textPropsField);
    if (!jJsonStr) return;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    jmethodID getS = env->GetMethodID(jsonClass, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID getD = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID getB = env->GetMethodID(jsonClass, "getBoolean", "(Ljava/lang/String;)Z");
    jmethodID getI = env->GetMethodID(jsonClass, "getInt", "(Ljava/lang/String;)I");

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

    const char16_t* textContent = (const char16_t*)env->GetStringChars(jText, nullptr);
    const char* fontName = jFont ? env->GetStringUTFChars(jFont, nullptr) : nullptr;
    const char* alignStr = env->GetStringUTFChars(jAlign, nullptr);
    const char* fontPath = jFontPath ? env->GetStringUTFChars(jFontPath, nullptr) : nullptr;

    // Use the exported PDF rect as the single source of truth for placement/box size.
    // Mixing this with jsonWidth/jsonHeight causes the saved text to shrink/shift.
    float initialWidth = fabs(rect.right - rect.left);
    float initialHeight = fabs(rect.top - rect.bottom);
    double angleRad = rotation * M_PI / 180.0;

    // Expanded Bounding Box Math
    float expandedWidth = (float)(initialWidth * fabs(cos(angleRad)) + initialHeight * fabs(sin(angleRad)));
    float expandedHeight = (float)(initialHeight * fabs(cos(angleRad)) + initialWidth * fabs(sin(angleRad)));
    float origCenterX = (rect.left + rect.right) / 2.0f;
    float origCenterY = (rect.bottom + rect.top) / 2.0f;

    FS_RECTF drawRect = {origCenterX - (expandedWidth / 2.0f), origCenterY + (expandedHeight / 2.0f),
                         origCenterX + (expandedWidth / 2.0f), origCenterY - (expandedHeight / 2.0f)};
    FPDFAnnot_SetRect(annot, &drawRect);

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
        FPDFPageObj_SetFillColor(textObj, r, g, b, 255);
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
            FPDFPageObj_SetStrokeColor(textObj, r, g, b, 255);
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
            FPDFPageObj_SetStrokeColor(line, r, g, b, 255); FPDFPageObj_SetStrokeWidth(line, textH * 0.05f);
            FPDFPath_SetDrawMode(line, 0, JNI_TRUE); FPDFAnnot_AppendObject(annot, line);
        };
        if (hasUnderline) drawLine(-0.15f);
        if (hasStrikeout) drawLine(0.30f);
    }
    const jchar* rawJsonContent = env->GetStringChars(jJsonStr, nullptr);
    FPDFAnnot_SetStringValue(annot, "Contents", (FPDF_WIDESTRING)rawJsonContent);
    env->ReleaseStringChars(jJsonStr, rawJsonContent);
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_READONLY);
    env->ReleaseStringChars(jText, (const jchar*)textContent);
    if (fontName) env->ReleaseStringUTFChars(jFont, fontName);
    env->ReleaseStringUTFChars(jAlign, alignStr);
    if (fontPath) env->ReleaseStringUTFChars(jFontPath, fontPath);
    env->DeleteLocalRef(json);
}

// --- HELPER 3: FREEHAND LOGIC ---
static void processFreeHand(JNIEnv* env, jobject obj, FPDF_PAGE page, jfieldID fhField, int r, int g, int b, jclass jsonClass, jmethodID jsonInit) {
    jstring jJsonStr = (jstring)env->GetObjectField(obj, fhField);
    if (!jJsonStr) return;

    jobject json = env->NewObject(jsonClass, jsonInit, jJsonStr);
    jmethodID getS = env->GetMethodID(jsonClass, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID getD = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
    jmethodID getI = env->GetMethodID(jsonClass, "getInt", "(Ljava/lang/String;)I");
    jmethodID getA = env->GetMethodID(jsonClass, "getJSONArray", "(Ljava/lang/String;)Lorg/json/JSONArray;");

    jstring jMode = (jstring)env->CallObjectMethod(json, getS, env->NewStringUTF("mode"));
    const char* modeStr = env->GetStringUTFChars(jMode, nullptr);
    jobject pointsArray = env->CallObjectMethod(json, getA, env->NewStringUTF("points"));

    jclass arrayClass = env->FindClass("org/json/JSONArray");
    jmethodID lenM = env->GetMethodID(arrayClass, "length", "()I");
    jmethodID getObjM = env->GetMethodID(arrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    int pointsCount = env->CallIntMethod(pointsArray, lenM);

    if (pointsCount >= 2) {
        jobject ptStart = env->CallObjectMethod(pointsArray, getObjM, 0);
        float startX = (float)env->CallDoubleMethod(ptStart, getD, env->NewStringUTF("x"));
        float startY = (float)env->CallDoubleMethod(ptStart, getD, env->NewStringUTF("y"));
        FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(startX, startY);
        float segmentStartX = startX;
        float segmentStartY = startY;
        float prevX = startX;
        float prevY = startY;

        for (int i = 1; i < pointsCount; i++) {
            jobject pt = env->CallObjectMethod(pointsArray, getObjM, i);
            float curX = (float)env->CallDoubleMethod(pt, getD, env->NewStringUTF("x"));
            float curY = (float)env->CallDoubleMethod(pt, getD, env->NewStringUTF("y"));

            if (i == pointsCount - 1) {
                FPDFPath_LineTo(path, curX, curY);
            } else {
                float midX = (prevX + curX) * 0.5f;
                float midY = (prevY + curY) * 0.5f;

                float cp1X = segmentStartX + (2.0f / 3.0f) * (prevX - segmentStartX);
                float cp1Y = segmentStartY + (2.0f / 3.0f) * (prevY - segmentStartY);
                float cp2X = midX + (2.0f / 3.0f) * (prevX - midX);
                float cp2Y = midY + (2.0f / 3.0f) * (prevY - midY);

                FPDFPath_BezierTo(path, cp1X, cp1Y, cp2X, cp2Y, midX, midY);
                segmentStartX = midX;
                segmentStartY = midY;
            }

            prevX = curX;
            prevY = curY;
            env->DeleteLocalRef(pt);
        }
        FPDFPageObj_SetStrokeWidth(path, (float)env->CallDoubleMethod(json, getD, env->NewStringUTF("strokeWidth")));
        FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND);
        FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND);

        if (strcmp(modeStr, "HIGHLIGHTER") == 0) {
            FPDFPageObj_SetStrokeColor(path, r, g, b, 125);
            FPDFPageObj_SetBlendMode(path, "Multiply");
        } else {
            FPDFPageObj_SetStrokeColor(path, r, g, b, env->CallIntMethod(json, getI, env->NewStringUTF("alpha")));
        }
        FPDFPath_SetDrawMode(path, 0, JNI_TRUE);
        FPDFPage_InsertObject(page, path);
        env->DeleteLocalRef(ptStart);
    }
    env->ReleaseStringUTFChars(jMode, modeStr);
    env->DeleteLocalRef(pointsArray);
    env->DeleteLocalRef(json);
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
        int b
) {
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
    int alpha = 90;
    FPDFAnnot_SetColor(annot,FPDFANNOT_COLORTYPE_InteriorColor,r, g, b, alpha);
    const unsigned short blendMode[] = {'M','u','l','t','i','p','l','y',0};
    FPDFAnnot_SetStringValue(annot,"BM",(FPDF_WIDESTRING)blendMode);
    FPDFAnnot_SetBorder(annot, 0, 0, 0);
    FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
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

        unsigned int objR = 0, objG = 0, objB = 0, objA = 0;
        if (!*hasFillColor &&
            FPDFPageObj_GetFillColor(pageObject, &objR, &objG, &objB, &objA) &&
            objA > 0) {
            *hasFillColor = true;
            *fillR = objR;
            *fillG = objG;
            *fillB = objB;
            *fillA = objA;
        }

        objR = objG = objB = objA = 0;
        if (!*hasStrokeColor &&
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
    jfieldID nativeSourceIdField = env->GetFieldID(highlightClass, "nativeSourceId", "I");
    jfieldID nativeEditActionField = env->GetFieldID(highlightClass, "nativeEditAction", "I");

    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jclass jsonArrayClass = env->FindClass("org/json/JSONArray");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayInit = env->GetMethodID(jsonArrayClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayLength = env->GetMethodID(jsonArrayClass, "length", "()I");
    jmethodID jsonArrayGetObject = env->GetMethodID(jsonArrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jmethodID jsonGetDouble = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");

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
    };
    std::map<int, std::vector<AnnotColorUpdate>> colorUpdateMap;

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
            colorUpdateMap[pageIndex].push_back({
                                                        nativeSourceId,
                                                        env->GetIntField(obj, typeField),
                                                        env->GetIntField(obj, rField),
                                                        env->GetIntField(obj, gField),
                                                        env->GetIntField(obj, bField),
                                                        env->GetIntField(obj, alphaField)
                                                });
        }
        env->DeleteLocalRef(obj);
    }

    std::map<int, bool> touchedPages;
    for (const auto& entry : removalMap) touchedPages[entry.first] = true;
    for (const auto& entry : objectRemovalMap) touchedPages[entry.first] = true;
    for (const auto& entry : colorUpdateMap) touchedPages[entry.first] = true;

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
                ApplyExistingAnnotationColor(annot, update.typeInt, update.r, update.g, update.b, update.alpha);
                FPDFPage_CloseAnnot(annot);
            }
        }

        auto removalsIt = removalMap.find(pageIndex);
        if (removalsIt != removalMap.end()) {
            auto ids = removalsIt->second;
            std::sort(ids.begin(), ids.end(), std::greater<int>());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            for (int annotIndex : ids) {
                if (annotIndex >= 0 && annotIndex < FPDFPage_GetAnnotCount(page)) {
                    FPDFPage_RemoveAnnot(page, annotIndex);
                }
            }
        }

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
                bool removed = false;
                if (target.objectIndex >= 0 && target.objectIndex < FPDFPage_CountObjects(page)) {
                    FPDF_PAGEOBJECT pageObject = FPDFPage_GetObject(page, target.objectIndex);
                    if (pageObject && FPDFPageObj_GetType(pageObject) == FPDF_PAGEOBJ_PATH &&
                        FPDFPage_RemoveObject(page, pageObject)) {
                        FPDFPageObj_Destroy(pageObject);
                        removed = true;
                    }
                }
                if (!removed) {
                    const float targetLeft = fmin(target.left, target.right);
                    const float targetRight = fmax(target.left, target.right);
                    const float targetBottom = fmin(target.top, target.bottom);
                    const float targetTop = fmax(target.top, target.bottom);
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
                int pdfType = (typeInt == 1) ? FPDF_ANNOT_UNDERLINE : (typeInt == 2) ? FPDF_ANNOT_STRIKEOUT :
                              (typeInt == 3) ? FPDF_ANNOT_LINK : (typeInt == 4 || typeInt == 7) ? FPDF_ANNOT_SQUARE : FPDF_ANNOT_HIGHLIGHT;
                FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, (typeInt == 5) ? FPDF_ANNOT_STAMP : pdfType);
                if (annot) {
                    FPDFAnnot_SetRect(annot, &rect);
                    if (typeInt == 3) processLink(env, obj, page, annot, rect, urlField);
                    else if (typeInt == 4) {
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 0, 0, 0, 255);
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, 0, 0, 0, 255);
                    }
                    else if (typeInt == 5) processTextStamp(env, obj, doc, page, annot, rect, textPropsField, r, g, b, jsonClass, jsonInit);
                    else if (typeInt == 7) {
                        processRegionHighlight(env, obj, page, annot, rect, r, g, b);
                    }
                    else {
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, alpha);

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

                                    FS_QUADPOINTSF qp = {
                                            fmin(quadLeft, quadRight), fmax(quadTop, quadBottom),
                                            fmax(quadLeft, quadRight), fmax(quadTop, quadBottom),
                                            fmin(quadLeft, quadRight), fmin(quadTop, quadBottom),
                                            fmax(quadLeft, quadRight), fmin(quadTop, quadBottom)
                                    };
                                    FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                                    appended = true;
                                    env->DeleteLocalRef(rectObj);
                                }
                                env->DeleteLocalRef(rectsArray);
                            }
                        }
                        if (!appended) {
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
    jfieldID nativeSourceIdField = env->GetFieldID(highlightClass, "nativeSourceId", "I");
    jfieldID nativeEditActionField = env->GetFieldID(highlightClass, "nativeEditAction", "I");

    jclass jsonClass = env->FindClass("org/json/JSONObject");
    jmethodID jsonInit = env->GetMethodID(jsonClass, "<init>", "(Ljava/lang/String;)V");
    jclass jsonArrayClass = env->FindClass("org/json/JSONArray");
    jmethodID jsonArrayInit = env->GetMethodID(jsonArrayClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID jsonArrayLength = env->GetMethodID(jsonArrayClass, "length", "()I");
    jmethodID jsonArrayGetObject = env->GetMethodID(jsonArrayClass, "getJSONObject", "(I)Lorg/json/JSONObject;");
    jmethodID jsonGetDouble = env->GetMethodID(jsonClass, "getDouble", "(Ljava/lang/String;)D");
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

            FS_RECTF rect;
            rect.left = fmin(left, right); rect.right = fmax(left, right);
            rect.bottom = fmin(top, bottom); rect.top = fmax(top, bottom);

            if (typeInt == 6) { // Freehand is not a standard Annotation type in your logic
                processFreeHand(env, obj, currentPage, fhDrawingProperties, r, g, b, jsonClass, jsonInit);
            } else {
                int pdfType = (typeInt == 1) ? FPDF_ANNOT_UNDERLINE : (typeInt == 2) ? FPDF_ANNOT_STRIKEOUT :
                                                                      (typeInt == 3) ? FPDF_ANNOT_LINK : (typeInt == 4 || typeInt == 7) ? FPDF_ANNOT_SQUARE : FPDF_ANNOT_HIGHLIGHT;

                FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(currentPage, (typeInt == 5) ? FPDF_ANNOT_STAMP : pdfType);
                if (annot) {
                    FPDFAnnot_SetRect(annot, &rect);
                    if (typeInt == 3) processLink(env, obj, currentPage, annot, rect, urlField);
                    else if (typeInt == 4) { // Redaction
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 0, 0, 0, 255);
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, 0, 0, 0, 255);
                    }
                    else if (typeInt == 5) processTextStamp(env, obj, doc, currentPage, annot, rect, textPropsField, r, g, b, jsonClass, jsonInit);
                    else if (typeInt == 7) {
                        processRegionHighlight(env, obj, currentPage, annot, rect, r, g, b);
                    }
                    else { // Highlight / Underline / Strikeout
                        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
                        if (typeInt == 0) {
                            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_InteriorColor, r, g, b, 255);
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

                                    FS_QUADPOINTSF qp = {
                                            fmin(quadLeft, quadRight), fmax(quadTop, quadBottom),
                                            fmax(quadLeft, quadRight), fmax(quadTop, quadBottom),
                                            fmin(quadLeft, quadRight), fmin(quadTop, quadBottom),
                                            fmax(quadLeft, quadRight), fmin(quadTop, quadBottom)
                                    };
                                    FPDFAnnot_AppendAttachmentPoints(annot, &qp);
                                    appended = true;
                                    env->DeleteLocalRef(rectObj);
                                }
                                env->DeleteLocalRef(rectsArray);
                            }
                        }
                        if (!appended) {
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
            "(IIFFFFIIIILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/graphics/Bitmap;II)V"
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
            case FPDF_ANNOT_LINK:      type = 3; break;
            case FPDF_ANNOT_STAMP:
                type = 5;
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
        } else if (subtype == FPDF_ANNOT_SQUARE) {
            const bool looksLikeRedaction =
                    (hasStrokeColor && r == 0 && g == 0 && b == 0 && a == 255) &&
                    (!hasInteriorColor || (interiorR == 0 && interiorG == 0 && interiorB == 0 && interiorA == 255));
            type = looksLikeRedaction ? 4 : 7;
            const bool interiorIsMeaningful =
                    hasInteriorColor &&
                    (interiorA < 255 || interiorR != 0 || interiorG != 0 || interiorB != 0);
            const bool shouldUseInteriorColor =
                    type == 7 &&
                    interiorIsMeaningful &&
                    (!hasStrokeColor ||
                     interiorR != r || interiorG != g || interiorB != b || interiorA != a);
            if (shouldUseInteriorColor || (!hasStrokeColor && hasInteriorColor)) {
                r = interiorR;
                g = interiorG;
                b = interiorB;
                a = interiorA;
            }
        }
        jstring jLinkUrl = nullptr;
        jstring jTextProps = nullptr;
        std::ostringstream markupRectsStream;
        bool hasMarkupRects = false;

        if (type == 5) {
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

            if (!jTextProps) {
                const int annotObjectCount = FPDFAnnot_GetObjectCount(annot);
                FS_RECTF stampRect = {0, 0, 0, 0};
                FPDFAnnot_GetRect(annot, &stampRect);
                for (int objectIndex = 0; objectIndex < annotObjectCount; objectIndex++) {
                    FPDF_PAGEOBJECT pageObject = FPDFAnnot_GetObject(annot, objectIndex);
                    if (!pageObject || FPDFPageObj_GetType(pageObject) != FPDF_PAGEOBJ_TEXT) continue;

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
            }
        }

        if (usesRectOnly) {
            // Logic for Redaction (Square) - Use the Bounding Box
            FS_RECTF rect;
            if (FPDFAnnot_GetRect(annot, &rect)) {
                int dLeft, dTop, dRight, dBottom;
                FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.left, rect.top, &dLeft, &dTop);
                FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, rect.right, rect.bottom, &dRight, &dBottom);

                jobject annotObj = env->NewObject(annotClass, constructor,
                                                  type, pageIndex, (float)dLeft, (float)dTop, (float)dRight, (float)dBottom,
                                                  (int)r, (int)g, (int)b, (int)a, jLinkUrl, nullptr, jTextProps, nullptr, nullptr, i, 0);

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

            jstring jMarkupRects = hasMarkupRects
                    ? env->NewStringUTF((std::string("[") + markupRectsStream.str() + "]").c_str())
                    : nullptr;

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
                                                      (int)r, (int)g, (int)b, (int)a, jLinkUrl, jMarkupRects, jTextProps, nullptr, nullptr, i, 0);

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

        int dLeft, dTop, dRight, dBottom;
        FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, left, top, &dLeft, &dTop);
        FPDF_PageToDevice(page, 0, 0, viewWidth, viewHeight, 0, right, bottom, &dRight, &dBottom);

        jobject annotObj = env->NewObject(
                annotClass,
                constructor,
                6,
                pageIndex,
                (float)dLeft,
                (float)dTop,
                (float)dRight,
                (float)dBottom,
                (int)r,
                (int)g,
                (int)b,
                (int)a,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                i,
                0
        );

        if (annotObj) tempCollector.push_back(annotObj);
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

}//extern C
