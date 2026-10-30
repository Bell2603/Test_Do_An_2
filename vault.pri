# Kho video mã hoá đầu-cuối theo mô hình ZeroDB: lõi + controller + giao diện.
include($$PWD/vault_core.pri)

QT += concurrent multimedia widgets

HEADERS += \
    $$PWD/querycontroller.h \
    $$PWD/serverlogmodel.h \
    $$PWD/serverviewwidget.h \
    $$PWD/sharecontroller.h \
    $$PWD/sharedialog.h \
    $$PWD/vaultcontroller.h \
    $$PWD/vaultlogindialog.h \
    $$PWD/vaultmodel.h \
    $$PWD/vaultwidget.h \
    $$PWD/videoprobe.h

SOURCES += \
    $$PWD/querycontroller.cpp \
    $$PWD/serverlogmodel.cpp \
    $$PWD/serverviewwidget.cpp \
    $$PWD/sharecontroller.cpp \
    $$PWD/sharedialog.cpp \
    $$PWD/vaultcontroller.cpp \
    $$PWD/vaultlogindialog.cpp \
    $$PWD/vaultmodel.cpp \
    $$PWD/vaultwidget.cpp \
    $$PWD/videoprobe.cpp

FORMS += \
    $$PWD/serverviewwidget.ui \
    $$PWD/sharedialog.ui \
    $$PWD/vaultlogindialog.ui \
    $$PWD/vaultwidget.ui
