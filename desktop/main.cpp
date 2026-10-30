#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QStandardPaths>
#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Frond");
    app.setApplicationName("Frond");
    app.setApplicationVersion("0.5.0");
    app.setDesktopFileName("org.frond.Frond");
    app.setWindowIcon(QIcon::fromTheme("frond"));
    QCommandLineParser parser;
    parser.setApplicationDescription("Frond Palm TX emulator");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument("session", "Open a saved Frond session.", "[session]");
    parser.addOption({"core", "Use this emulator executable.", "path"});
    parser.process(app);
    if (parser.positionalArguments().size() > 1) parser.showHelp(1);
    QString core = parser.value("core");
    if (core.isEmpty()) {
        const auto directory = QCoreApplication::applicationDirPath();
        const QStringList candidates{directory + "/frond", directory + "/../lib/frond/frond-core"};
        for (const auto& candidate : candidates) if (QFileInfo(candidate).isExecutable()) { core = candidate; break; }
    }
    if (!core.isEmpty()) core = QFileInfo(core).absoluteFilePath();
    MainWindow window(core);
    window.show();
    if (!parser.positionalArguments().isEmpty())
        window.openSession(parser.positionalArguments().first(), true);
    return app.exec();
}
