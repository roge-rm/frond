#include "SdExchange.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

SdExchange::SdExchange(const QString& scratchTemplate)
    : directory(scratchTemplate.isEmpty() ? QDir::tempPath() + "/frond-exchange-XXXXXX" : scratchTemplate) {}

bool SdExchange::available() {
    for (const auto* name : {"mdir", "mcopy", "mmd"})
        if (QStandardPaths::findExecutable(name).isEmpty()) return false;
    return true;
}

bool SdExchange::validPath(const QString& path, bool root) {
    if (root && path == "/") return true;
    if (!path.startsWith('/') || path.size() > 240) return false;
    // Literal FAT paths only: mtools also accepts wildcards and drive syntax.
    static const QRegularExpression forbidden(R"([\x00-\x1f\x7f*?\[\]<>|":\\])");
    for (const auto& part : path.mid(1).split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
            part.endsWith(' ') || part.startsWith(' ') || forbidden.match(part).hasMatch()) return false;
    }
    return true;
}

bool SdExchange::run(const QString& tool, const QString& image, const QStringList& args,
                     QString& output, QString& error) {
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto& key : env.keys()) if (key.startsWith("MTOOLS")) env.remove(key);
    env.insert("MTOOLSRC", "/dev/null");
    env.insert("LC_ALL", "C.UTF-8");
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStandardPaths::findExecutable(tool),
                  QStringList{"-i", image + "@@" + QString::number(offset)} + args);
    process.closeWriteChannel();
    if (!process.waitForStarted(3000)) {
        error = "Install mtools to exchange files (Debian: sudo apt install mtools)."; return false;
    }
    if (!process.waitForFinished(15000)) {
        process.kill(); process.waitForFinished();
        error = "SD file operation timed out. The working image has not changed."; return false;
    }
    output = QString::fromUtf8(process.readAll());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = tool + ": " + output.trimmed(); return false;
    }
    return true;
}

bool SdExchange::open(const QString& source, QString& error) {
    if (!current.isEmpty()) { error = "An exchange is already open."; return false; }
    if (!directory.isValid()) { error = "Could not create private exchange storage."; return false; }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    auto boot = file.read(512);
    if (boot.size() != 512 || file.size() < (32ll << 20) || file.size() > (128ll << 20) || file.size() % 512) {
        error = "Use a 32–128 MiB FAT16 card image."; return false;
    }
    auto word = [](const QByteArray& b, int at) { return qFromLittleEndian<quint16>(b.constData() + at); };
    auto dword = [](const QByteArray& b, int at) { return qFromLittleEndian<quint32>(b.constData() + at); };
    offset = 0;
    if (boot.mid(54, 8) != "FAT16   ") {
        // Frond cards have a single primary FAT16 partition. Never guess offsets.
        const auto type = quint8(boot[450]);
        const auto begin = dword(boot, 454), sectors = dword(boot, 458);
        if (word(boot, 510) != 0xaa55 || (type != 4 && type != 6 && type != 14) || !begin ||
            !sectors || (quint64(begin) + sectors) * 512 > quint64(file.size())) {
            error = "Exchange supports FAT16 superfloppies or a primary FAT16 partition."; return false;
        }
        offset = qint64(begin) * 512;
        if (!file.seek(offset)) { error = file.errorString(); return false; }
        boot = file.read(512);
    }
    if (boot.size() != 512 || boot.mid(54, 8) != "FAT16   " || word(boot, 11) != 512 || word(boot, 510) != 0xaa55) {
        error = "The card does not contain a supported FAT16 filesystem."; return false;
    }
    const auto path = directory.path() + "/card.img";
    if (!QFile::copy(source, path)) { error = "Could not copy the card image to private storage."; return false; }
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
        QFile::remove(path); error = "Could not make the private image writable."; return false;
    }
    QString listing;
    if (!run("mdir", path, {"::/"}, listing, error)) { QFile::remove(path); return false; }
    current = path;
    return true;
}

bool SdExchange::list(const QString& path, QString& listing, QString& error) {
    if (current.isEmpty() || !validPath(path, true)) { error = "Enter a literal absolute card path, such as /PALM."; return false; }
    return run("mdir", current, {"::" + path}, listing, error);
}

QString SdExchange::candidate(QString& error) {
    const auto path = directory.path() + "/edit-" + QString::number(++serial) + ".img";
    if (current.isEmpty() || !QFile::copy(current, path)) { error = "Could not copy the working card image."; return {}; }
    return path;
}
void SdExchange::adopt(const QString& path) {
    const auto previous = current; current = path; QFile::remove(previous);
}

static QByteArray digest(const QString& path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result();
}

bool SdExchange::put(const QString& source, const QString& destination, QString& error) {
    if (current.isEmpty() || !validPath(destination)) { error = "Enter a complete card filename, such as /PALM/song.sunvox."; return false; }
    QString output, ignored;
    if (run("mdir", current, {"::" + destination}, output, ignored)) {
        error = "That card name already exists. Choose another name; files are never overwritten."; return false;
    }
    QTemporaryDir transfer(directory.path() + "/transfer-XXXXXX");
    const auto input = transfer.path() + "/input", verified = transfer.path() + "/verified";
    if (!transfer.isValid() || !QFileInfo(source).isFile() || !QFile::copy(source, input)) {
        error = "Could not read the host file."; return false;
    }
    const auto next = candidate(error);
    if (next.isEmpty()) return false;
    // Skip any unexpected collision, then verify the actual stored bytes. Only
    // a complete, verified candidate can replace the private working revision.
    bool ok = run("mcopy", next, {"-D", "s", input, "::" + destination}, output, error) &&
              run("mcopy", next, {"::" + destination, verified}, output, error);
    if (ok && (digest(input).isEmpty() || digest(input) != digest(verified))) {
        error = "The card copy did not match the host file. The working image has not changed."; ok = false;
    }
    if (ok) adopt(next); else QFile::remove(next);
    return ok;
}

bool SdExchange::get(const QString& source, const QString& destination, QString& error) {
    if (current.isEmpty() || !validPath(source)) { error = "Enter a complete card filename."; return false; }
    QTemporaryDir transfer(directory.path() + "/transfer-XXXXXX");
    QString output;
    const auto staged = transfer.path() + "/output";
    if (!transfer.isValid()) { error = "Could not create transfer storage."; return false; }
    if (!run("mcopy", current, {"::" + source, staged}, output, error)) return false;
    // Qt's copy refuses existing files, including symlinks. No overwrite prompt
    // can grant permission to overwrite a host image, session or unrelated file.
    if (!QFile::copy(staged, destination)) { error = "Could not create the host file. Choose a new filename in a writable folder."; return false; }
    return true;
}

bool SdExchange::mkdir(const QString& path, QString& error) {
    if (current.isEmpty() || !validPath(path)) { error = "Enter a new absolute card folder path."; return false; }
    const auto next = candidate(error);
    if (next.isEmpty()) return false;
    QString output;
    if (!run("mmd", next, {"-D", "s", "::" + path}, output, error) ||
        !run("mdir", next, {"::" + path}, output, error)) { QFile::remove(next); return false; }
    adopt(next); return true;
}
