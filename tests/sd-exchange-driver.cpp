// Firmware-free adapter also used by the optional real TX VFS acceptance run.
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include "SdExchange.h"
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 2 || !((args[1] == "put" && args.size() == 6) || (args[1] == "get" && args.size() == 5))) {
        QTextStream(stderr) << "Usage: sd-exchange-driver put IMAGE HOST_FILE CARD_PATH NEW_IMAGE\n       sd-exchange-driver get IMAGE CARD_PATH HOST_FILE\n";
        return 2;
    }
    SdExchange exchange;
    QString error;
    bool ok = exchange.open(args[2], error);
    if (ok && args[1] == "put") {
        ok = exchange.put(args[3], args[4], error);
        if (ok && !QFile::copy(exchange.image(), args[5])) { error = "Choose a new writable image destination."; ok = false; }
    }
    else if (ok) ok = exchange.get(args[3], args[4], error);
    if (!ok) QTextStream(stderr) << "SD exchange failed: " << error << '\n';
    return ok ? 0 : 1;
}
