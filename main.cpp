#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // Dùng cho QStandardPaths::AppDataLocation (cache bucket SQLite, known_hosts của vault)
    QApplication::setOrganizationName("SETP");
    QApplication::setApplicationName("SETP Ver 2");
    MainWindow w;
    w.show();
    return QApplication::exec();
}
