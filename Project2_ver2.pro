QT += widgets core multimedia multimediawidgets concurrent sql

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    appconfig.cpp \
    dialog.cpp \
    localfilecontroller.cpp \
    main.cpp \
    mainwindow.cpp \
    playercontroller.cpp \
    sftpcontroller.cpp \
    sftpfilemodel.cpp \
    sftpsession.cpp

HEADERS += \
    appconfig.h \
    dialog.h \
    localfilecontroller.h \
    mainwindow.h \
    playercontroller.h \
    sftpcontroller.h \
    sftpfilemodel.h \
    sftpsession.h

FORMS += \
    dialog.ui \
    mainwindow.ui

# Kho video mã hoá đầu-cuối (ZeroDB): cần OpenSSL 3 và libssh2 (brew install openssl@3 libssh2)
include(vault/vault.pri)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

DISTFILES += \
    config.ini
