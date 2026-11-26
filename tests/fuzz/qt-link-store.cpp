// The link hub's store (LinkStore.cpp): identity.json and connections.json from the input (split
// where the first byte says), loaded by Store::open and saved again. QtCore only.
#include <QDir>
#include <QFile>

#include "../../desktop/LinkStore.h"
#include "Scratch.h"

namespace {
void Put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static unsigned runs = 0;
    if (size < 1 || size > 65536) return 0;
    const auto folder = QString::fromStdString(FuzzScratch("qt-link-store", runs)) + "/" + QString::number(runs);
    QDir().mkpath(folder);
    const QByteArray rest(reinterpret_cast<const char*>(bytes + 1), int(size - 1));
    const int split = int(bytes[0] * rest.size() / 256);
    Put(folder + "/identity.json", rest.left(split));
    Put(folder + "/connections.json", rest.mid(split));
    {
        frondLink::Store store;
        if (store.open(folder).isEmpty()) {
            for (const auto& p : store.peers) store.find(p.id_());
            store.save();
        }
    }
    QDir(folder).removeRecursively();
    return 0;
}
