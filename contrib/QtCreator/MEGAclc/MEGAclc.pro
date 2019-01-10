TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

TARGET = megaclc

debug_and_release {
    CONFIG -= debug_and_release
    CONFIG += debug_and_release
}
CONFIG(debug, debug|release) {
    CONFIG -= debug release
    CONFIG += debug
}
CONFIG(release, debug|release) {
    CONFIG -= debug release
    CONFIG += release
}

DEFINES += LOG_TO_LOGGER

CONFIG += USE_LIBUV
CONFIG += USE_MEGAAPI
CONFIG += USE_MEDIAINFO
CONFIG += ENABLE_CHAT
CONFIG += USE_WEBRTC
CONFIG += USE_AUTOCOMPLETE
CONFIG += USE_CONSOLE

include(../../../bindings/qt/megachat.pri)

DEPENDPATH += ../../../examples/megaclc
INCLUDEPATH += ../../../examples/megaclc

SOURCES +=  ../../../examples/megaclc/megaclc.cpp

win32 {
    QMAKE_LFLAGS += /LARGEADDRESSAWARE
    QMAKE_LFLAGS_WINDOWS += /SUBSYSTEM:WINDOWS,5.01
    QMAKE_LFLAGS_CONSOLE += /SUBSYSTEM:CONSOLE,5.01
    DEFINES += PSAPI_VERSION=1
}

macx {
    QMAKE_CXXFLAGS += -DCRYPTOPP_DISABLE_ASM -D_DARWIN_C_SOURCE
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.6
    QMAKE_CXXFLAGS -= -stdlib=libc++
    QMAKE_LFLAGS -= -stdlib=libc++
    CONFIG -= c++11
    QMAKE_CXXFLAGS += -fvisibility=hidden -fvisibility-inlines-hidden
    QMAKE_LFLAGS += -F /System/Library/Frameworks/Security.framework/
}

INCLUDEPATH += ../../../third-party/mega/sdk_build/install/include
QMAKE_LIBDIR += ../../../third-party/mega/sdk_build/install/lib

LIBS += -lstdc++fs -lreadline -ltermcap
