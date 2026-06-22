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

    parser.addOption(trayOption);
    parser.addOption(socketOption);
    parser.process(app);

    bool tray = parser.isSet(trayOption);
    QString configPath = parser.positionalArguments().value(0);
    QString socketPath = parser.value(socketOption);

    // If socket path provided, set it in environment for idk_client
    if (!socketPath.isEmpty()) {
        qputenv("IDK_SOCKET_PATH", socketPath.toUtf8().data());
    }

    Manager manager(configPath, tray);
    return app.exec();
}
