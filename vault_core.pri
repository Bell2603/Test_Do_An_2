# Lõi kho video mã hoá đầu-cuối (không có UI) - dùng chung cho ứng dụng và bộ test.
QT += core sql

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

# OpenSSL 3 (AES-256-GCM, scrypt, HKDF, X25519) và libssh2 (SFTP) cài qua Homebrew.
# Đổi prefix khi cần: qmake BREW_PREFIX=/usr/local
isEmpty(BREW_PREFIX): BREW_PREFIX = /opt/homebrew
OPENSSL_PREFIX = $$BREW_PREFIX/opt/openssl@3
LIBSSH2_PREFIX = $$BREW_PREFIX/opt/libssh2

!exists($$OPENSSL_PREFIX/include/openssl/evp.h): error("Thiếu OpenSSL 3: brew install openssl@3")
!exists($$LIBSSH2_PREFIX/include/libssh2_sftp.h): error("Thiếu libssh2: brew install libssh2")

INCLUDEPATH += $$OPENSSL_PREFIX/include $$LIBSSH2_PREFIX/include
LIBS += -L$$OPENSSL_PREFIX/lib -lcrypto \
        -L$$LIBSSH2_PREFIX/lib -lssh2

HEADERS += \
    $$PWD/bucketcache.h \
    $$PWD/cryptoservice.h \
    $$PWD/decryptingdevice.h \
    $$PWD/encryptedbtree.h \
    $$PWD/keymanager.h \
    $$PWD/objectstore.h \
    $$PWD/remotestore.h \
    $$PWD/vaultservice.h \
    $$PWD/vaulttypes.h

SOURCES += \
    $$PWD/bucketcache.cpp \
    $$PWD/cryptoservice.cpp \
    $$PWD/decryptingdevice.cpp \
    $$PWD/encryptedbtree.cpp \
    $$PWD/keymanager.cpp \
    $$PWD/objectstore.cpp \
    $$PWD/remotestore.cpp \
    $$PWD/vaultservice.cpp
