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

    QCommandLineOption socketOption({ "s", "socket" },
                                    "Socket path (overrides env/config).",
                                    "path");

    QCommandLineOption noDmaBufOption("no-dmabuf",
                                      "Force SHM mode (disable DMABUF zero-copy).");

    QCommandLineOption urlOption("url",
                                 "Override URL (bypasses config).",
                                 "url");

    QCommandLineOption widthOption("width",
                                   "Override initial width (bypasses config).",
                                   "pixels");

    QCommandLineOption heightOption("height",
                                    "Override initial height (bypasses config).",
                                    "pixels");

    QCommandLineOption matchOption("match",
                                   "Process name to match config sections against "
                                   "(regex). Used when forked by injected lib.",
                                   "regex");

    parser.addOption(socketOption);
    parser.addOption(noDmaBufOption);
    parser.addOption(urlOption);
    parser.addOption(widthOption);
    parser.addOption(heightOption);
    parser.addOption(matchOption);
    parser.process(app);

    QString configPath = parser.positionalArguments().value(0);
    QString socketPath = parser.value(socketOption);
    bool noDmaBuf = parser.isSet(noDmaBufOption);
    QString url = parser.value(urlOption);
    int width = parser.value(widthOption).toInt();
    int height = parser.value(heightOption).toInt();
    QString match = parser.value(matchOption);

    Manager manager(configPath, socketPath, noDmaBuf,
                    url, width, height, match);
    return app.exec();
}
