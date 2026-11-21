#include "InstallerRunner.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QLabel>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSet>
#include <QVBoxLayout>
#include <QtEndian>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>

#include "SyncFiles.h"

namespace frondInstallerRunner {
namespace {

QString shims() { return QCoreApplication::applicationDirPath() + "/installer-shims"; }

QString runtime(const QString& folder) {
#ifdef Q_OS_WIN
    return folder + "/RunInstaller.exe";
#elif defined(Q_OS_LINUX)
    Q_UNUSED(folder);
    return QStandardPaths::findExecutable("wine");
#else
    Q_UNUSED(folder);
    return {};
#endif
}

QString helperFolder(const QString& installer) {
    const QDir helpers(shims());
    if (!helpers.exists("x86/RunInstaller.exe")) return helpers.path();
    if (QFileInfo(installer).suffix().compare("msi", Qt::CaseInsensitive) == 0) return helpers.filePath("x86");
    QFile exe(installer);
    if (!exe.open(QIODevice::ReadOnly)) return helpers.path();
    const auto header = exe.read(64);
    if (header.size() < 64 || !header.startsWith("MZ")) return helpers.path();
    const auto offset = qFromLittleEndian<quint32>(header.constData() + 60);
    if (!exe.seek(offset)) return helpers.path();
    const auto pe = exe.read(6);
    if (pe.size() == 6 && pe.startsWith(QByteArray("PE\0\0", 4)) && qFromLittleEndian<quint16>(pe.constData() + 4) == 0x14c)
        return helpers.filePath("x86");
    return helpers.path();
}

class InstallerWait : public QDialog {
   public:
    using QDialog::QDialog;
    void reject() override {}
};

QString windowsPath(const QString& path) {
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(path);
#else
    auto converted = path;
    return "Z:" + converted.replace('/', '\\');
#endif
}

constexpr qint64 maxFileSize = 64 * 1024 * 1024;
constexpr qint64 maxTotalSize = 128 * 1024 * 1024;
constexpr size_t maxFiles = 64;

bool commandOutput(const QString& program, const QStringList& args, qsizetype limit, QByteArray& output) {
    QProcess process;
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, args);
    if (!process.waitForStarted(3000)) return false;
    constexpr auto timeout = 15000;
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        process.waitForReadyRead(100);
        output += process.readAllStandardOutput();
        if (output.size() > limit || elapsed.elapsed() > timeout) {
            process.kill();
            process.waitForFinished();
            return false;
        }
    } while (process.state() != QProcess::NotRunning);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

struct ArchiveMember {
    QString path;
    QString leaf;
    qint64 size;
};

bool archiveMembers(const QString& program, const QString& archive, std::vector<ArchiveMember>& members) {
    QByteArray listing;
    if (!commandOutput(program, {"l", "-slt", "-sccUTF-8", "--", archive}, 1024 * 1024, listing)) return false;
    const auto contents = QString::fromUtf8(listing).section("----------", 1);
    QString path;
    for (auto line : contents.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.startsWith("Path = ")) path = line.mid(7);
        if (!line.startsWith("Size = ") || path.isEmpty()) continue;
        bool valid = false;
        const auto size = line.mid(7).toLongLong(&valid);
        auto normalised = path;
        normalised.replace('\\', '/');
        const auto leaf = normalised.section('/', -1);
        if (valid && size > 0 && size <= maxFileSize && !path.startsWith('@') && !path.contains(QChar::Null))
            members.push_back({path, leaf, size});
        path.clear();
    }
    return true;
}

bool readMember(const QString& program, const QString& archive, const ArchiveMember& member, QByteArray& data) {
    return commandOutput(program, {"x", "-so", "-spd", "--", archive, member.path}, member.size, data) &&
           data.size() == member.size;
}

bool writeMember(const QString& program, const QString& archive, const ArchiveMember& member, const QString& path) {
    QByteArray data;
    if (!readMember(program, archive, member, data)) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size() && file.flush();
}

bool collectDirect(const QString& program, const QString& archive, const std::vector<ArchiveMember>& members,
                   std::vector<frondSync::InstallerFile>& files, qint64& total) {
    QSet<QString> seen;
    for (const auto& member : members) {
        const auto name = member.leaf.toStdString();
        if (name.size() > 255 || !frondSync::IsInstallFileName(name) || seen.contains(member.path)) continue;
        seen.insert(member.path);
        if (size_t(seen.size()) > maxFiles || files.size() >= maxFiles || member.size > maxTotalSize - total) return false;
        QByteArray data;
        if (!readMember(program, archive, member, data)) continue;
        const auto* first = reinterpret_cast<const uint8_t*>(data.constData());
        frondSync::InstallerFile payload{name, frondSync::Bytes(first, first + data.size())};
        if (!frondSync::IsInstallPayload(payload)) continue;
        files.push_back(std::move(payload));
        total += member.size;
    }
    return true;
}

#ifdef Q_OS_UNIX
bool extractCabinet(const QString& program, const QString& cabinet, const QString& destination,
                    const QString& group, const QStringList& names) {
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setChildProcessModifier([] {
        const rlimit fileLimit{maxFileSize, maxFileSize};
        if (setrlimit(RLIMIT_FSIZE, &fileLimit) != 0) _exit(1);
    });
    process.start(program, QStringList{"-j", "-g", group, "-d", destination, "x", cabinet} + names);
    if (!process.waitForStarted(3000)) return false;
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool unpackInstallShield(const QString& program, const QString& archive, const std::vector<ArchiveMember>& members,
                         const QString& unshield, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files,
                         qint64& total) {
    // Stage a matching header and cabinet under fixed names. unshield locates the header beside the cabinet.
    const auto header = std::find_if(members.begin(), members.end(), [](const ArchiveMember& member) {
        return member.leaf.compare("data1.hdr", Qt::CaseInsensitive) == 0;
    });
    if (header == members.end()) return true;
    const auto cabinetPath = header->path.chopped(3) + "cab";
    const auto cabinet = std::find_if(members.begin(), members.end(), [&](const ArchiveMember& member) {
        return member.path.compare(cabinetPath, Qt::CaseInsensitive) == 0;
    });
    if (cabinet == members.end()) return true;
    QTemporaryDir stage(QDir(syncRoot).filePath("unpack-cabinet-XXXXXX"));
    if (!stage.isValid()) return true;
    const auto stagedCabinet = stage.filePath("data1.cab");
    if (!writeMember(program, archive, *header, stage.filePath("data1.hdr")) ||
        !writeMember(program, archive, *cabinet, stagedCabinet)) return true;

    QByteArray listing;
    if (!commandOutput(unshield, {"-j", "l", stagedCabinet}, 1024 * 1024, listing)) return true;
    // File arguments are patterns in unshield. Accept only plain basenames without pattern characters.
    static const QRegularExpression listed(R"(^\s*(\d+)\s+(.+)$)");
    static const QRegularExpression basename(R"(^[A-Za-z0-9_][A-Za-z0-9_ .()+,-]{0,239}$)");
    QMap<QString, std::vector<ArchiveMember>> groups;
    QSet<QString> seen;
    qint64 listedTotal = total;
    for (const auto& line : QString::fromUtf8(listing).split('\n')) {
        const auto match = listed.match(line);
        if (!match.hasMatch()) continue;
        auto path = match.captured(2);
        path.replace('\\', '/');
        const auto group = path.section('/', 0, 0);
        const auto leaf = path.section('/', -1);
        if (group == leaf || !basename.match(group).hasMatch() || !basename.match(leaf).hasMatch() ||
            !frondSync::IsInstallFileName(leaf.toStdString())) continue;
        const auto key = group.toCaseFolded() + '/' + leaf.toCaseFolded();
        if (seen.contains(key)) return true;
        bool valid = false;
        const auto size = match.captured(1).toLongLong(&valid);
        if (!valid || size <= 0 || size > maxFileSize || size > maxTotalSize - listedTotal ||
            files.size() + size_t(seen.size()) >= maxFiles) return false;
        seen.insert(key);
        groups[group].push_back({{}, leaf, size});
        listedTotal += size;
    }
    // Even with -j, unshield writes the group directory with spaces changed to underscores.
    for (auto group = groups.begin(); group != groups.end(); ++group) {
        QTemporaryDir output(stage.filePath("files-XXXXXX"));
        QStringList names;
        for (const auto& member : group.value()) names << member.leaf;
        if (!output.isValid() || !extractCabinet(unshield, stagedCabinet, output.path(), group.key(), names)) continue;
        auto groupFolder = group.key();
        groupFolder.replace(' ', '_');
        const QDir extracted(output.filePath(groupFolder));
        for (const auto& member : group.value()) {
            const QFileInfo info(extracted.filePath(member.leaf));
            if (!info.isFile() || info.isSymLink() || info.size() != member.size) continue;
            frondSync::InstallerFile payload{member.leaf.toStdString(), {}};
            QString error;
            if (!frondSyncFiles::readDatabase(info.filePath(), payload.bytes, error) ||
                qint64(payload.bytes.size()) != member.size || !frondSync::IsInstallPayload(payload)) continue;
            files.push_back(std::move(payload));
            total += member.size;
        }
    }
    return true;
}
#endif

bool unpackWith7z(const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files) {
    const QString program = QStandardPaths::findExecutable("7z");
    if (program.isEmpty()) return false;
    std::vector<ArchiveMember> members;
    if (!archiveMembers(program, installer, members)) return false;
    qint64 total = 0;
    if (!collectDirect(program, installer, members, files, total)) return false;
#ifdef Q_OS_UNIX
    const QString unshield = QStandardPaths::findExecutable("unshield");
    if (unshield.isEmpty()) return !files.empty();
    if (!unpackInstallShield(program, installer, members, unshield, syncRoot, files, total)) return false;

    if (QFileInfo(installer).suffix().compare("zip", Qt::CaseInsensitive) == 0) {
        constexpr int maxNested = 4;
        qint64 nestedTotal = 0;
        int nestedCount = 0;
        for (const auto& member : members) {
            if (QFileInfo(member.leaf).suffix().compare("exe", Qt::CaseInsensitive) != 0) continue;
            if (nestedCount == maxNested || member.size > maxTotalSize - nestedTotal) break;
            ++nestedCount;
            nestedTotal += member.size;
            QTemporaryDir nested(QDir(syncRoot).filePath("unpack-installer-XXXXXX"));
            if (!nested.isValid()) break;
            const auto inner = nested.filePath("installer.exe");
            std::vector<ArchiveMember> innerMembers;
            if (!writeMember(program, installer, member, inner) || !archiveMembers(program, inner, innerMembers)) continue;
            if (!collectDirect(program, inner, innerMembers, files, total) ||
                !unpackInstallShield(program, inner, innerMembers, unshield, syncRoot, files, total)) return false;
        }
    }
#else
    Q_UNUSED(syncRoot);
#endif
    return !files.empty();
}

}  // namespace

bool available() {
    const QDir helpers(shims());
    return !runtime(helpers.path()).isEmpty() && helpers.exists("RunInstaller.exe") && helpers.exists("InstAide.dll") &&
           helpers.exists("Instapp.exe") && helpers.exists("CEAppMgr.exe");
}

bool run(QWidget* parent, const QString& installer, const QString& user, bool windowsMobile,
         const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error) {
    QTemporaryDir stage(QDir(syncRoot).filePath("installer-XXXXXX"));
    if (!stage.isValid() || !QDir(stage.path()).mkpath("User/Install") || !QDir(stage.path()).mkpath("User/Received")) {
        error = QObject::tr("Couldn't create the installer folder.");
        return false;
    }

    const auto folder = helperFolder(installer);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("FROND_INSTALL_STAGE", windowsPath(stage.path()));
    env.insert("FROND_INSTALL_SHIMS", windowsPath(folder));
    env.insert("FROND_INSTALL_USER", user);
    env.insert("FROND_INSTALL_KIND", windowsMobile ? "winmo" : "palm");
    QStringList args{windowsPath(installer)};
#ifdef Q_OS_LINUX
    env.insert("WINEPREFIX", QDir(syncRoot).filePath("wine-installers"));
    env.insert("FROND_INSTALL_WINE", "1");
    args.prepend(folder + "/RunInstaller.exe");
#else
    env.remove("FROND_INSTALL_WINE");
#endif

    InstallerWait waiting(parent);
    waiting.setWindowTitle(QObject::tr("Windows installer"));
    waiting.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    auto* layout = new QVBoxLayout(&waiting);
    layout->addWidget(new QLabel(QObject::tr("Finish the installer to choose its files."), &waiting));

    QProcess process;
    process.setProcessEnvironment(env);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &waiting, &QDialog::accept);
    process.start(runtime(folder), args);
    if (!process.waitForStarted()) {
        error = process.errorString();
        return false;
    }
    if (process.state() != QProcess::NotRunning) waiting.exec();
    process.waitForFinished(-1);
    const auto exitCode = process.exitCode();

    size_t total = 0;
    for (const auto& folder : {QStringLiteral("User/Install"), QStringLiteral("User/Received")}) {
        const QDir received(QDir(stage.path()).filePath(folder));
        for (const auto& file : received.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
            const auto name = file.fileName().toStdString();
            if (!frondSync::IsInstallFileName(name)) continue;
            const bool cab = file.suffix().compare("cab", Qt::CaseInsensitive) == 0;
            if (cab != windowsMobile) continue;
            if (file.size() <= 0 || file.size() > 64 * 1024 * 1024 || files.size() >= 64 ||
                size_t(file.size()) > 128 * 1024 * 1024 - total) {
                error = QObject::tr("The installer has too many files or exceeds the unpacked size limit.");
                files.clear();
                return false;
            }
            frondSync::InstallerFile payload{name, {}};
            if (!frondSyncFiles::readDatabase(file.filePath(), payload.bytes, error)) {
                files.clear();
                return false;
            }
            total += payload.bytes.size();
            files.push_back(std::move(payload));
        }
    }
    if (!files.empty()) return true;
    error = exitCode ? QObject::tr("The installer exited with code %1.").arg(exitCode)
                     : QObject::tr("The installer supplied no files for this device.");
    return false;
}

bool unpack(const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error) {
    frondSync::Bytes bytes;
    if (!frondSyncFiles::readDatabase(installer, bytes, error)) return false;
    const QFileInfo info(installer);
    const bool zip = info.suffix().compare("zip", Qt::CaseInsensitive) == 0;
    const auto name = info.fileName().toStdString();
    std::string problem;
    if (zip && frondSync::UnpackInstaller(name, bytes, files, problem)) return true;
    std::vector<frondSync::InstallerFile> unpacked;
    if (unpackWith7z(installer, syncRoot, unpacked)) {
        files = std::move(unpacked);
        return true;
    }
    if (!zip && frondSync::UnpackInstaller(name, bytes, files, problem)) return true;
    error = QString::fromStdString(problem);
    return false;
}

}  // namespace frondInstallerRunner
