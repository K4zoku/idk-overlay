#include "manager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>

int main(int argc, char *argv[])
{
    qputenv("QT_XKB_INPUT_METHOD", "none");
    qputenv("QT_XKB_SCREEN", "default");

    QApplication app(argc, argv);
    app.setApplicationName("idk-webview");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("config", "Config file path");

    QCommandLineOption trayOption({ "t", "tray" },
                                  "Start minimized in system tray.");

    QCommandLineOption socketOption({ "s", "socket" },
                                    "Socket path (overrides config).",
                                    "path");

    QCommandLineOption noDmaBufOption("no-dmabuf",
                                      "Force SHM mode (disable DMABUF zero-copy).");

    parser.addOption(trayOption);
    parser.addOption(socketOption);
    parser.addOption(noDmaBufOption);
    parser.process(app);

    bool tray = parser.isSet(trayOption);
    QString configPath = parser.positionalArguments().value(0);
    QString socketPath = parser.value(socketOption);
    bool noDmaBuf = parser.isSet(noDmaBufOption);

    Manager manager(configPath, socketPath, tray, noDmaBuf);
    return app.exec();
}
