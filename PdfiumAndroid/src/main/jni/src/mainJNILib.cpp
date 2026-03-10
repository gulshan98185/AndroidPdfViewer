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
#include <vector>
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

    //if (renderAnnot) {
        flags |= FPDF_ANNOT;
    //}

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

    // 1) Page content (text, images, vectors, non-widget annots)
    int flagsBase = FPDF_REVERSE_BYTE_ORDER | FPDF_LCD_TEXT | FPDF_ANNOT; // keep FPDF_ANNOT for non-widget annots
    FPDF_RenderPageBitmap(pdfBitmap, page,
                          startX, startY,
                          (int)drawSizeHor, (int)drawSizeVer,
                          0, flagsBase);

    // 2) Form widgets (AcroForm fields, signature appearance)
    int flagsFFL = FPDF_REVERSE_BYTE_ORDER | FPDF_LCD_TEXT; // FFL draws widgets; FPDF_ANNOT here is optional
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
Java_com_cv_lufick_compose_1editor_helper_PdfCustomNativeSaver_nativeSaveAnnotations( // todo main annotation block
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
    jfieldID urlField = env->GetFieldID(highlightClass, "linkUrl", "Ljava/lang/String;");

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

//        if (currentPage) {
//            // Create a HIGHLIGHT annotation
////            FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(currentPage, FPDF_ANNOT_HIGHLIGHT);
//
//            int pdfAnnotType;
//            if (typeInt == 1) {
//                pdfAnnotType = FPDF_ANNOT_UNDERLINE;
//            } else if (typeInt == 2) {
//                pdfAnnotType = FPDF_ANNOT_STRIKEOUT;
//            } else if (typeInt == 3) {
//                pdfAnnotType = FPDF_ANNOT_LINK;
//            } else if (typeInt == 4) {
//                pdfAnnotType = FPDF_ANNOT_REDACT;
//            } else {
//                pdfAnnotType = FPDF_ANNOT_HIGHLIGHT;
//            }
//
//            LOGE("Creating Annot: Type %d at L:%.1f T:%.1f R:%.1f B:%.1f", typeInt, left, top, right, bottom);
//
//            FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(currentPage, pdfAnnotType);
//            if (!annot) {
//                LOGE("ANNOT CREATION FAILED for type %d", pdfAnnotType);
//            }
//            if (annot) {
//                // Define the Bounding Box (Rect)
//                FS_RECTF rect;
//                rect.left = fmin(left, right);
//                rect.right = fmax(left, right);
//                rect.bottom = fmin(top, bottom);
//                rect.top = fmax(top, bottom);
//                FPDFAnnot_SetRect(annot, &rect);
//
//                // Set QuadPoints
//                // Order: Top-Left, Top-Right, Bottom-Left, Bottom-Right
//                FS_QUADPOINTSF quadPoints;
//                quadPoints.x1 = rect.left;  quadPoints.y1 = rect.top;    // Top-Left
//                quadPoints.x2 = rect.right; quadPoints.y2 = rect.top;    // Top-Right
//                quadPoints.x3 = rect.left;  quadPoints.y3 = rect.bottom; // Bottom-Left
//                quadPoints.x4 = rect.right; quadPoints.y4 = rect.bottom; // Bottom-Right
//
//                // Use the function to attach these points to the annotation
//                FPDFAnnot_AppendAttachmentPoints(annot, &quadPoints);
//                FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);
//
//                FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
//                FPDFPage_CloseAnnot(annot);
//            }
//        }

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

                    // B. Turn Text Blue - We iterate objects to find text under this link
                    int objCount = FPDFPage_CountObjects(currentPage);
                    for (int j = 0; j < objCount; j++) {
                        FPDF_PAGEOBJECT pageObj = FPDFPage_GetObject(currentPage, j);
                        if (FPDFPageObj_GetType(pageObj) == FPDF_PAGEOBJ_TEXT) {
                            float l, b, r, t;
                            if (FPDFPageObj_GetBounds(pageObj, &l, &b, &r, &t)) {
                                // INCREASED TOLERANCE: Use 2.0f instead of 1.0f
                                // PDF text often floats slightly outside the selection rect
                                if (l >= (rect.left - 2.0f) && r <= (rect.right + 2.0f) &&
                                    b >= (rect.bottom - 2.0f) && t <= (rect.top + 2.0f)) {

                                    FPDFPageObj_SetFillColor(pageObj, 0, 0, 255, 255); // Blue
                                }
                            }
                        }
                    }
                    // C. Add QuadPoints for the Link (helps viewers show the clickable area)
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
                } else {
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

JNIEXPORT jboolean JNICALL
Java_com_cv_lufick_compose_1editor_helper_PdfHighlightSaver_nativeSaveHighlights2(
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

    // Cache Field IDs
    jfieldID pageField = env->GetFieldID(highlightClass, "pageIndex", "I");
    jfieldID leftField = env->GetFieldID(highlightClass, "left", "F");
    jfieldID topField = env->GetFieldID(highlightClass, "top", "F");
    jfieldID rightField = env->GetFieldID(highlightClass, "right", "F");
    jfieldID bottomField = env->GetFieldID(highlightClass, "bottom", "F");
    jfieldID rField = env->GetFieldID(highlightClass, "r", "I");
    jfieldID gField = env->GetFieldID(highlightClass, "g", "I");
    jfieldID bField = env->GetFieldID(highlightClass, "b", "I");

    FPDF_PAGE currentPage = nullptr;
    int lastPageIndex = -1;

    for (int i = 0; i < highlightCount; i++) {
        jobject obj = env->GetObjectArrayElement(highlightsArray, i);

        int pageIndex = env->GetIntField(obj, pageField);
        float pdfLeft = env->GetFloatField(obj, leftField);
        float pdfTop = env->GetFloatField(obj, topField);
        float pdfRight = env->GetFloatField(obj, rightField);
        float pdfBottom = env->GetFloatField(obj, bottomField);
        int r = env->GetIntField(obj, rField);
        int g = env->GetIntField(obj, gField);
        int b = env->GetIntField(obj, bField);

        if (pageIndex != lastPageIndex) {
            if (currentPage != nullptr) {
                FPDFPage_GenerateContent(currentPage);
                FPDF_ClosePage(currentPage);
            }
            currentPage = FPDF_LoadPage(doc, pageIndex);
            lastPageIndex = pageIndex;
        }

        if (currentPage) {
            // Normalize ordering
            if (pdfLeft > pdfRight) std::swap(pdfLeft, pdfRight);
            if (pdfBottom > pdfTop) std::swap(pdfBottom, pdfTop);

            FS_RECTF rect = {pdfLeft, pdfTop, pdfRight, pdfBottom};

            // ✅ Create specific HIGHLIGHT annotation
            FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(currentPage, FPDF_ANNOT_HIGHLIGHT);

            if (annot) {
                // Set the bounding box
                FPDFAnnot_SetRect(annot, &rect);

                // ✅ Define QuadPoints (8 coordinates)
                // Order: top-left, top-right, bottom-left, bottom-right
                FS_QUADPOINTSF quad;
                quad.x1 = rect.left;  quad.y1 = rect.top;    // Point 1
                quad.x2 = rect.right; quad.y2 = rect.top;    // Point 2
                quad.x3 = rect.left;  quad.y3 = rect.bottom; // Point 3
                quad.x4 = rect.right; quad.y4 = rect.bottom; // Point 4

                // Apply the points to the annotation
                FPDFAnnot_SetAttachmentPoints(annot, 0, &quad);

                // ✅ Set Color. For standard Highlights, viewers handle transparency.
                // We use 255 alpha here; if it's too dark, change to 120-150.
                FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, g, b, 255);

                FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
                FPDFPage_CloseAnnot(annot);
            }
        }
        env->DeleteLocalRef(obj);
    }

    if (currentPage != nullptr) {
        FPDFPage_GenerateContent(currentPage);
        FPDF_ClosePage(currentPage);
    }

    // Save logic
    FILE* file = fopen(outputPath, "wb");
    int success = 0;
    if (file) {
        PdfFileWriter writer{};
        writer.base.version = 1;
        writer.base.WriteBlock = WriteBlock;
        writer.file = file;
        success = FPDF_SaveAsCopy(doc, (FPDF_FILEWRITE*)&writer, FPDF_NO_INCREMENTAL);
        fclose(file);
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



}//extern C
