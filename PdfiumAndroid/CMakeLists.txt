# Sets the minimum version of CMake required to build your native library.
# This ensures that a certain set of CMake features is available to
# your build.

cmake_minimum_required(VERSION 3.4.1)

set(THREADS_PREFER_PTHREAD_FLAG ON)

# Specifies a library name, specifies whether the library is STATIC or
# SHARED, and provides relative paths to the source code. You can
# define multiple libraries by adding multiple add_library() commands,
# and CMake builds them for you. When you build your app, Gradle
# automatically packages shared libraries with your APK.


# Add other prebuilt libraries
#add_library(modft2
#        SHARED
#        IMPORTED)
#set_target_properties( # Specifies the target library.
#        modft2
#
#        # Specifies the parameter you want to define.
#        PROPERTIES IMPORTED_LOCATION
#
#        # Provides the path to the library you want to import.
#        ${CMAKE_SOURCE_DIR}/src/main/jni/lib/${ANDROID_ABI}/libmodft2.so)
#
#
## Add other prebuilt libraries
#add_library(modpng
#        SHARED
#        IMPORTED)
#set_target_properties( # Specifies the target library.
#        modpng
#
#        # Specifies the parameter you want to define.
#        PROPERTIES IMPORTED_LOCATION
#
#        # Provides the path to the library you want to import.
#        ${CMAKE_SOURCE_DIR}/src/main/jni/lib/${ANDROID_ABI}/libmodpng.so)


# Add other prebuilt libraries
add_library(pdfsdk
        SHARED
        IMPORTED)

set_target_properties( # Specifies the target library.
        pdfsdk

        # Specifies the parameter you want to define.
        PROPERTIES IMPORTED_LOCATION

        # Provides the path to the library you want to import.
        # ${CMAKE_SOURCE_DIR}/src/main/jni/lib/${ANDROID_ABI}/libpdfsdk.so
        ${CMAKE_SOURCE_DIR}/../../PDFiumNewLibsFixes/${ANDROID_ABI}/libpdfsdk.so
)

#find_package( Threads )
find_package(Threads REQUIRED)

include_directories(
        "${CMAKE_SOURCE_DIR}/src/main/jni/include"
        "${CMAKE_SOURCE_DIR}/src/main/jni/include/utils"
)

add_library( # Specifies the name of the library.
        jniPdfium

        # Sets the library as a shared library.
        SHARED

        # Provides a relative path to your source file(s).
        ${CMAKE_SOURCE_DIR}/src/main/jni/src/mainJNILib.cpp)


find_library( # Defines the name of the path variable that stores the
        # location of the NDK library.
        log-lib

        # Specifies the name of the NDK library that
        # CMake needs to locate.
        log)


# You need to link static libraries against your shared native library.
target_link_libraries(
        jniPdfium
        pdfsdk
        Threads::Threads
        -ljnigraphics
        android
        GLESv2
        ${log-lib}
)