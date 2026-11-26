#include "mainwindow.h"
#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Kadris Studio");
    QCoreApplication::setApplicationName("Kadris Studio");
    QCoreApplication::setApplicationVersion("0.1.0");
    app.setStyle("Fusion");
    app.setWindowIcon(QIcon(":/assets/kadris-studio.svg"));

    QFile theme(":/resources/kadris.qss");
    if (theme.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&theme);
        app.setStyleSheet(stream.readAll());
    }

    MainWindow window;
    window.show();
    return app.exec();
}
