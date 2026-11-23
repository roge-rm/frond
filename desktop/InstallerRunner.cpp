#include "InstallerRunner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QSet>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QtEndian>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <msiquery.h>
#endif

#include <algorithm>
#include <atomic>
#include <memory>
#include <iterator>
#include <utility>

#include "SyncFiles.h"
#ifdef Q_OS_WIN
#include "sync/PalmConduitStorage.h"
#include "sync/SyncStore.h"
#endif

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
    explicit InstallerWait(QWidget* parent, bool canReject = false) : QDialog(parent), canReject(canReject) {}
    void reject() override {
        if (canReject) QDialog::reject();
    }

   private:
    bool canReject;
};

template <typename Work>
void waitForWorker(InstallerWait& progress, Work&& work) {
    std::unique_ptr<QThread> worker(QThread::create(std::forward<Work>(work)));
    QObject::connect(worker.get(), &QThread::finished, &progress, &QDialog::accept);
    worker->start();
    progress.exec();
    worker->wait();
}

template <typename Work>
void finishOnWorker(QWidget* parent, Work&& work,
                    const QString& message = QObject::tr("Checking installer files and cleaning up…")) {
    InstallerWait progress(parent);
    progress.setWindowTitle(QObject::tr("Windows installer"));
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    auto* layout = new QVBoxLayout(&progress);
    layout->addWidget(new QLabel(message, &progress));
    waitForWorker(progress, std::forward<Work>(work));
}

#ifdef Q_OS_LINUX
bool waitForExit(QProcess& process, int timeout) {
    if (process.state() == QProcess::NotRunning) return true;
    QEventLoop loop;
    QObject::connect(&process, &QProcess::stateChanged, &loop, [&](QProcess::ProcessState state) {
        if (state == QProcess::NotRunning) loop.quit();
    });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeout);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return process.state() == QProcess::NotRunning;
}

QString wineServer() { return QStandardPaths::findExecutable("wineserver"); }

bool wineServerCommand(const QProcessEnvironment& env, const QString& command) {
    QProcess server;
    server.setProcessEnvironment(env);
    server.setStandardOutputFile(QProcess::nullDevice());
    server.setStandardErrorFile(QProcess::nullDevice());
    server.start(wineServer(), {command});
    if (!waitForExit(server, 5000)) {
        server.kill();
        waitForExit(server, 1000);
        return false;
    }
    return server.error() != QProcess::FailedToStart && server.exitStatus() == QProcess::NormalExit && server.exitCode() == 0;
}
#endif

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

struct ExtractionAttempt {
    std::atomic<bool> cancelled{false};
    QDeadlineTimer deadline{15000, Qt::PreciseTimer};

    bool stopped() const { return cancelled.load() || deadline.hasExpired(); }

    bool command(const QString& program, const QStringList& args, qsizetype limit,
                 QByteArray* output = nullptr) const {
        if (stopped()) return false;
#ifdef Q_OS_UNIX
        int channel[] = {-1, -1};
        const auto closeChannel = qScopeGuard([&] {
            for (const int fd : channel)
                if (fd >= 0) close(fd);
        });
        if (output) {
#ifdef Q_OS_LINUX
            if (pipe2(channel, O_CLOEXEC) != 0) return false;
#else
            if (pipe(channel) != 0) return false;
            for (const int fd : channel)
                if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
#endif
            if (fcntl(channel[0], F_SETFL, O_NONBLOCK) < 0) return false;
        }
        const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0) return false;
        const auto closeNull = qScopeGuard([&] { close(null); });

        std::vector<QByteArray> encoded{QFile::encodeName(program)};
        for (const auto& arg : args) encoded.push_back(QFile::encodeName(arg));
        std::vector<char*> argv;
        for (auto& arg : encoded) argv.push_back(arg.data());
        argv.push_back(nullptr);
        const auto executable = argv.data();
        const bool capture = output != nullptr;
        const pid_t leader = fork();
        if (leader < 0) return false;
        if (leader == 0) {
            const rlimit fileLimit{maxFileSize, maxFileSize};
            if (setpgid(0, 0) != 0 || (!capture && setrlimit(RLIMIT_FSIZE, &fileLimit) != 0) ||
                dup2(null, STDIN_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0 ||
                dup2(capture ? channel[1] : null, STDOUT_FILENO) < 0) _exit(126);
            execv(executable[0], executable);
            _exit(127);
        }
        // Create the group from either side of fork so cancellation can signal it immediately.
        // EACCES means the child has already run execv after setting its group.
        const bool groupCreated = setpgid(leader, leader) == 0 || errno == EACCES;
        if (capture) {
            close(channel[1]);
            channel[1] = -1;
        }
        const auto stopGroup = [&] {
            // The unreaped leader pins the group ID until after this signal.
            if (groupCreated || getpgid(leader) == leader) kill(-leader, SIGKILL);
        };
        bool exited = false;
        bool failed = false;
        while (!stopped()) {
            siginfo_t status{};
            if (!exited) {
                if (waitid(P_PID, leader, &status, WEXITED | WNOHANG | WNOWAIT) < 0) {
                    if (errno == EINTR) continue;
                    if (errno == ECHILD) return false;
                    failed = true;
                    break;
                }
                exited = status.si_pid == leader;
                if (exited) stopGroup();
            }
            if (capture) {
                char buffer[16384];
                const auto count = read(channel[0], buffer, sizeof(buffer));
                if (count > 0) {
                    if (count > limit - output->size()) {
                        failed = true;
                        break;
                    }
                    output->append(buffer, count);
                    continue;
                }
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    failed = true;
                    break;
                }
            }
            if (exited) break;
            QThread::msleep(10);
        }
        if (!exited) stopGroup();
        int status = 0;
        pid_t reaped;
        do {
            reaped = waitpid(leader, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        return reaped == leader && !failed && !stopped() && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
        QProcess process;
        process.setStandardErrorFile(QProcess::nullDevice());
        if (!output) process.setStandardOutputFile(QProcess::nullDevice());
        process.start(program, args);
        process.closeWriteChannel();
        bool exceeded = false;
        do {
            if (process.state() == QProcess::Starting) process.waitForStarted(50);
            else if (output) process.waitForReadyRead(50);
            else process.waitForFinished(50);
            if (output) {
                const auto chunk = process.readAllStandardOutput();
                if (chunk.size() > limit - output->size()) exceeded = true;
                if (!exceeded && !stopped()) *output += chunk;
            }
            if (stopped() || exceeded) {
                if (process.state() != QProcess::NotRunning) {
                    process.kill();
                    process.waitForFinished(1000);
                }
                return false;
            }
        } while (process.state() != QProcess::NotRunning);
        return !stopped() && !exceeded && process.error() != QProcess::FailedToStart &&
               process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
#endif
    }
};

struct ArchiveMember {
    QString path;
    QString leaf;
    qint64 size;
};

bool archiveMembers(const ExtractionAttempt& attempt, const QString& program, const QString& archive, std::vector<ArchiveMember>& members) {
    QByteArray listing;
    if (!attempt.command(program, {"l", "-slt", "-sccUTF-8", "--", archive}, 1024 * 1024, &listing)) return false;
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

bool readMember(const ExtractionAttempt& attempt, const QString& program, const QString& archive, const ArchiveMember& member, QByteArray& data) {
    return attempt.command(program, {"x", "-so", "-spd", "--", archive, member.path}, member.size, &data) &&
           data.size() == member.size;
}

bool writeMember(const ExtractionAttempt& attempt, const QString& program, const QString& archive, const ArchiveMember& member, const QString& path) {
    QByteArray data;
    if (!readMember(attempt, program, archive, member, data)) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size() && file.flush();
}

bool collectDirect(const ExtractionAttempt& attempt, const QString& program, const QString& archive, const std::vector<ArchiveMember>& members,
                   std::vector<frondSync::InstallerFile>& files, qint64& total) {
    QSet<QString> seen;
    for (const auto& member : members) {
        if (attempt.stopped()) return false;
        const auto name = member.leaf.toStdString();
        if (name.size() > 255 || !frondSync::IsInstallFileName(name) || seen.contains(member.path)) continue;
        seen.insert(member.path);
        if (size_t(seen.size()) > maxFiles || files.size() >= maxFiles || member.size > maxTotalSize - total) return false;
        QByteArray data;
        if (!readMember(attempt, program, archive, member, data)) continue;
        const auto* first = reinterpret_cast<const uint8_t*>(data.constData());
        frondSync::InstallerFile payload{name, frondSync::Bytes(first, first + data.size())};
        if (!frondSync::IsInstallPayload(payload)) continue;
        files.push_back(std::move(payload));
        total += member.size;
    }
    return true;
}

#ifdef Q_OS_UNIX
bool unpackInstallShield(const ExtractionAttempt& attempt, const QString& program, const QString& archive, const std::vector<ArchiveMember>& members,
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
    if (!writeMember(attempt, program, archive, *header, stage.filePath("data1.hdr")) ||
        !writeMember(attempt, program, archive, *cabinet, stagedCabinet)) return true;

    QByteArray listing;
    if (!attempt.command(unshield, {"-j", "l", stagedCabinet}, 1024 * 1024, &listing)) return true;
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
        if (attempt.stopped()) return false;
        QTemporaryDir output(stage.filePath("files-XXXXXX"));
        QStringList names;
        for (const auto& member : group.value()) names << member.leaf;
        if (!output.isValid() || !attempt.command(unshield, QStringList{"-j", "-g", group.key(), "-d", output.path(), "x", stagedCabinet} + names, 0)) continue;
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

struct MsiTables {
    QMap<QString, QString> names;
    QSet<QString> embeddedCabinets;
    QSet<QString> externalCabinets;

    void addFile(const QString& key, const QString& rawName) {
        const auto name = rawName.section('|', -1);
        if (!name.contains('/') && !name.contains('\\') && !name.contains(QChar::Null))
            names.insert(key, name);
    }

    void addCabinet(const QString& name) {
        if (name.startsWith('#')) {
            embeddedCabinets.insert(name.mid(1));
            return;
        }
        static const QRegularExpression basename(R"(^[A-Za-z0-9_][A-Za-z0-9_ .()+,-]{0,239}$)");
        if (basename.match(name).hasMatch()) externalCabinets.insert(name);
    }
};

#ifdef Q_OS_WIN
QString msiString(MSIHANDLE row, UINT column) {
    wchar_t value[256];
    DWORD length = std::size(value);
    return MsiRecordGetStringW(row, column, value, &length) == ERROR_SUCCESS
        ? QString::fromWCharArray(value, length)
        : QString{};
}

template <class ReadRow>
void readMsiTable(const ExtractionAttempt& attempt, MSIHANDLE database, const wchar_t* query, ReadRow readRow) {
    MSIHANDLE view{};
    if (MsiDatabaseOpenViewW(database, query, &view) != ERROR_SUCCESS) return;
    const auto closeView = qScopeGuard([&] { MsiCloseHandle(view); });
    if (MsiViewExecute(view, 0) != ERROR_SUCCESS) return;
    for (unsigned i = 0; i < 32767 && !attempt.stopped(); ++i) {
        MSIHANDLE row{};
        if (MsiViewFetch(view, &row) != ERROR_SUCCESS) return;
        const auto closeRow = qScopeGuard([&] { MsiCloseHandle(row); });
        readRow(row);
    }
}
#endif

MsiTables msiTables(const ExtractionAttempt& attempt, const QString& installer) {
    MsiTables tables;
#ifdef Q_OS_WIN
    const auto path = QDir::toNativeSeparators(QFileInfo(installer).absoluteFilePath()).toStdWString();
    MSIHANDLE database{};
    if (MsiOpenDatabaseW(path.c_str(), nullptr, &database) != ERROR_SUCCESS) return tables;
    const auto close = qScopeGuard([&] { MsiCloseHandle(database); });
    readMsiTable(attempt, database, L"SELECT `File`, `FileName` FROM `File`",
                 [&](MSIHANDLE row) { tables.addFile(msiString(row, 1), msiString(row, 2)); });
    readMsiTable(attempt, database, L"SELECT `Cabinet` FROM `Media`",
                 [&](MSIHANDLE row) { tables.addCabinet(msiString(row, 1)); });
#else
    const auto program = QStandardPaths::findExecutable("msiinfo");
    if (program.isEmpty()) return tables;
    QByteArray files;
    if (attempt.command(program, {"export", installer, "File"}, 1024 * 1024, &files)) {
        for (const auto& row : QString::fromUtf8(files).split('\n').mid(3)) {
            const auto columns = row.split('\t');
            if (columns.size() >= 3) tables.addFile(columns[0], columns[2]);
        }
    }
    QByteArray media;
    if (attempt.command(program, {"export", installer, "Media"}, 1024 * 1024, &media)) {
        for (const auto& row : QString::fromUtf8(media).split('\n').mid(3)) {
            const auto columns = row.split('\t');
            if (columns.size() >= 4) tables.addCabinet(columns[3]);
        }
    }
#endif
    return tables;
}

bool mobileCabinet(const ExtractionAttempt& attempt, const QString& program, const QString& archive,
                   const std::vector<ArchiveMember>& members) {
    constexpr qint64 maxMetadata = 1024 * 1024;
    const auto setup = std::find_if(members.begin(), members.end(), [](const ArchiveMember& member) {
        return member.path == member.leaf && member.leaf.compare("_setup.xml", Qt::CaseInsensitive) == 0;
    });
    QByteArray data;
    if (setup != members.end() && setup->size <= maxMetadata && readMember(attempt, program, archive, *setup, data)) {
        QXmlStreamReader xml(data);
        if (xml.readNextStartElement() && xml.name() == QLatin1String("wap-provisioningdoc")) return true;
    }

    // The first member of a CAB Wizard package is the .000 installation header.
    if (members.empty() || members.front().size < 100 || members.front().size > maxMetadata ||
        !members.front().leaf.endsWith(".000", Qt::CaseInsensitive)) return false;
    data.clear();
    return readMember(attempt, program, archive, members.front(), data) && data.startsWith(QByteArray("MSCE\0\0\0\0", 8));
}

bool unpackInno(const ExtractionAttempt& attempt, const QString& installer, const QString& syncRoot,
                std::vector<frondSync::InstallerFile>& files);

struct ArchivePeer {
    const QString& archive;
    const std::vector<ArchiveMember>& members;
    const ArchiveMember& installer;
    QSet<QString>& media;
};

struct ArchiveScan {
    const ExtractionAttempt& attempt;
    const QString program;
    const QString syncRoot;
    const QString unshield;
    std::vector<frondSync::InstallerFile>& files;
    qint64 total = 0;
    qint64 nestedBytes = 0;
    size_t nestedCount = 0;

    bool reserveNested(qint64 bytes) {
        if (nestedCount >= 8 || bytes > maxTotalSize - nestedBytes) return false;
        ++nestedCount;
        nestedBytes += bytes;
        return true;
    }

    bool unpack(const QString& installer, unsigned depth, bool& inspectedMsiMedia, const ArchivePeer* peer = nullptr) {
        const auto suffix = QFileInfo(installer).suffix().toLower();
        const bool msi = suffix == "msi";
        const auto tables = msi ? msiTables(attempt, installer) : MsiTables{};
        const auto before = files.size();
        std::vector<ArchiveMember> members;
        if (archiveMembers(attempt, program, installer, members)) {
            QSet<QString> media;
            for (const auto& member : members) {
                if (attempt.stopped()) return false;
                const bool cabinet = msi && (tables.embeddedCabinets.contains(member.path) ||
                                              member.leaf.endsWith(".cab", Qt::CaseInsensitive));
                if (cabinet) {
                    media.insert(member.path);
                    inspectedMsiMedia = true;
                }
                if (!cabinet && (depth >= 2 || !frondSync::IsInstallerFileName(member.leaf.toStdString()))) continue;
                if (!reserveNested(member.size)) continue;
                QTemporaryDir stage(QDir(syncRoot).filePath("unpack-installer-XXXXXX"));
                if (!stage.isValid()) continue;
                const auto inner = stage.filePath("archive." + (cabinet ? QStringLiteral("cab") : QFileInfo(member.leaf).suffix().toLower()));
                if (!writeMember(attempt, program, installer, member, inner)) continue;
                if (cabinet) {
                    if (!unpackMsiCabinet(inner, member.leaf, tables)) return false;
                    continue;
                }
                const auto beforeInner = files.size();
                const ArchivePeer innerPeer{installer, members, member, media};
                bool innerMsiMedia = false;
                const bool opened = unpack(inner, depth + 1, innerMsiMedia, &innerPeer);
                inspectedMsiMedia |= innerMsiMedia;
                if (!opened) return false;
                if (files.size() == beforeInner && !innerMsiMedia && !unpackEmbedded(inner, member.leaf)) return false;
            }

            std::vector<ArchiveMember> direct;
            for (const auto& member : members)
                if (!media.contains(member.path)) direct.push_back(member);

            if (!collectDirect(attempt, program, installer, direct, files, total)) return false;
#ifdef Q_OS_UNIX
            if (!unshield.isEmpty() && !unpackInstallShield(attempt, program, installer, members, unshield, syncRoot, files, total))
                return false;
#endif
        }

        inspectedMsiMedia |= !tables.externalCabinets.isEmpty() || !tables.embeddedCabinets.isEmpty();
        if (msi && !unpackExternalMsi(installer, tables, peer)) return false;
        if (suffix == "exe" && files.size() == before) {
            std::vector<frondSync::InstallerFile> extracted;
            if (unpackInno(attempt, installer, syncRoot, extracted) && !append(extracted)) return false;
        }
        return !attempt.stopped();
    }

    bool unpackExternalMsi(const QString& installer, const MsiTables& tables, const ArchivePeer* peer) {
        const auto parent = QFileInfo(installer).absoluteDir();
        auto cabinets = tables.externalCabinets.values();
        std::sort(cabinets.begin(), cabinets.end());
        for (const auto& name : cabinets) {
            if (attempt.stopped()) return false;
            if (peer) {
                auto installerPath = peer->installer.path;
                installerPath.replace('\\', '/');
                const auto path = installerPath.left(installerPath.lastIndexOf('/') + 1) + name;
                const ArchiveMember* media = nullptr;
                bool ambiguous = false;
                for (const auto& candidate : peer->members) {
                    auto candidatePath = candidate.path;
                    candidatePath.replace('\\', '/');
                    if (candidatePath.compare(path, Qt::CaseInsensitive) != 0) continue;
                    peer->media.insert(candidate.path);
                    ambiguous |= media != nullptr;
                    media = &candidate;
                }
                if (!media || ambiguous || !reserveNested(media->size)) continue;

                QTemporaryDir stage(QDir(syncRoot).filePath("unpack-msi-media-XXXXXX"));
                if (!stage.isValid()) continue;
                const auto cabinet = stage.filePath("media.cab");
                if (writeMember(attempt, program, peer->archive, *media, cabinet) &&
                    !unpackMsiCabinet(cabinet, name, tables)) return false;
            } else {
                const auto candidates = parent.entryInfoList({name}, QDir::Files | QDir::NoSymLinks);
                if (candidates.size() != 1) continue;
                const auto& source = candidates.front();
                if (source.size() <= 0 || source.size() > maxFileSize || !reserveNested(source.size())) continue;
                if (!unpackMsiCabinet(source.absoluteFilePath(), name, tables)) return false;
            }
        }
        return true;
    }

    bool unpackMsiCabinet(const QString& path, QString name, const MsiTables& tables) {
        std::vector<ArchiveMember> members;
        if (!archiveMembers(attempt, program, path, members)) return !attempt.stopped();
        if (mobileCabinet(attempt, program, path, members)) {
            if (!name.endsWith(".cab", Qt::CaseInsensitive)) name += ".cab";
            frondSync::InstallerFile file{name.toStdString(), {}};
            QString error;
            if (file.name.size() > 255 || !frondSyncFiles::readDatabase(path, file.bytes, error) ||
                !frondSync::IsInstallPayload(file)) return true;
            if (files.size() >= maxFiles || qint64(file.bytes.size()) > maxTotalSize - total) return false;
            total += file.bytes.size();
            files.push_back(std::move(file));
            return true;
        }
        if (attempt.stopped()) return false;
        for (auto& member : members)
            member.leaf = tables.names.value(member.path, member.leaf);
        return collectDirect(attempt, program, path, members, files, total);
    }

    bool unpackEmbedded(const QString& path, const QString& name) {
        frondSync::Bytes bytes;
        QString readError;
        if (!frondSyncFiles::readDatabase(path, bytes, readError)) return true;
        std::vector<frondSync::InstallerFile> embedded;
        std::string error;
        if (!frondSync::UnpackInstaller(name.toStdString(), bytes, embedded, error, false,
                                       [&] { return attempt.stopped(); })) return !attempt.stopped();
        return append(embedded);
    }

    bool append(std::vector<frondSync::InstallerFile>& extracted) {
        for (auto& payload : extracted) {
            if (files.size() >= maxFiles || qint64(payload.bytes.size()) > maxTotalSize - total) return false;
            total += payload.bytes.size();
            files.push_back(std::move(payload));
        }
        return true;
    }
};

bool unpackInno(const ExtractionAttempt& attempt, const QString& installer, const QString& syncRoot,
                std::vector<frondSync::InstallerFile>& files) {
    const auto program = QStandardPaths::findExecutable("innoextract");
    if (program.isEmpty()) return false;

    const QStringList options{"--silent", "--color=0", "--progress=0", "--no-gog-galaxy", "--no-extract-unknown"};
    const auto archive = QFileInfo(installer).absoluteFilePath();
    QByteArray listing;
    if (!attempt.command(program, options + QStringList{"--list-sizes", "--", archive}, 1024 * 1024, &listing))
        return false;

    static const QRegularExpression component(R"(^[A-Za-z0-9_][A-Za-z0-9_ .()+,-]{0,239}$)");
    QStringList includes;
    std::vector<ArchiveMember> members;
    QSet<QString> selectedEntries;
    qint64 total = 0;
    for (auto line : QString::fromUtf8(listing).split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        const auto offset = line.indexOf(' ');
        if (offset <= 0) continue;
        auto path = line.mid(offset + 1);
        path.replace('\\', '/');
        const auto leaf = path.section('/', -1);
        if (!frondSync::IsInstallFileName(leaf.toStdString())) continue;
        const auto parts = path.split('/');
        if (!std::all_of(parts.begin(), parts.end(), [&](const QString& part) { return component.match(part).hasMatch(); }))
            continue;
        bool valid = false;
        const auto size = line.left(offset).toLongLong(&valid);
        if (!valid || size <= 0 || size > maxFileSize || size > maxTotalSize - total || members.size() >= maxFiles)
            return false;
        if (selectedEntries.contains(line)) return false;
        selectedEntries.insert(line);
        includes << "--include" << QDir::toNativeSeparators('/' + path);
        members.push_back({path, leaf, size});
        total += size;
    }
    if (members.empty()) return false;

    // Include expressions also match directories. Check what they select before writing anything.
    QByteArray selected;
    if (!attempt.command(program, options + includes + QStringList{"--list-sizes", "--", archive}, 1024 * 1024, &selected))
        return false;
    for (auto line : QString::fromUtf8(selected).split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty() && !selectedEntries.remove(line)) return false;
    }
    if (!selectedEntries.isEmpty()) return false;
    QTemporaryDir stage(QDir(syncRoot).filePath("unpack-inno-XXXXXX"));
    if (!stage.isValid() ||
        !attempt.command(program, options + includes + QStringList{"--extract", "--test", "--timestamps=none",
                                                                   "--output-dir", stage.path(), "--", archive}, 0))
        return false;
    for (const auto& member : members) {
        const QFileInfo info(stage.filePath(member.path));
        if (!info.isFile() || info.isSymLink() || info.size() != member.size ||
            !info.canonicalFilePath().startsWith(QDir(stage.path()).canonicalPath() + '/')) continue;
        frondSync::InstallerFile payload{member.leaf.toStdString(), {}};
        QString error;
        if (frondSyncFiles::readDatabase(info.filePath(), payload.bytes, error) &&
            qint64(payload.bytes.size()) == member.size && frondSync::IsInstallPayload(payload))
            files.push_back(std::move(payload));
    }
    return !files.empty() && !attempt.stopped();
}

QString sevenZip() {
    const auto program = QStandardPaths::findExecutable("7z");
    return program.isEmpty() ? QStandardPaths::findExecutable("7zz") : program;
}

#ifdef Q_OS_LINUX
bool legacyWinZipInstaller(const QString& installer) {
    if (QFileInfo(installer).suffix().compare("exe", Qt::CaseInsensitive) != 0) return false;
    QFile file(installer);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto header = file.read(64 * 1024);
    return header.startsWith("MZ") && header.contains("WinZip(R) Self-Extractor Copyright");
}

QString stagePalmInstallerRuntime(const QString& installer, const QString& root) {
    // Some Palm installers ship MFC beside the app but need it before installing any files.
    const auto program = sevenZip();
    if (program.isEmpty()) return {};
    const ExtractionAttempt attempt;
    std::vector<ArchiveMember> members;
    if (!archiveMembers(attempt, program, installer, members)) return {};
    const ArchiveMember* selected = nullptr;
    for (const auto& member : members) {
        if (member.leaf.compare("mfc42.dll", Qt::CaseInsensitive) != 0 || member.size > 2 * 1024 * 1024) continue;
        if (selected) return {};
        selected = &member;
    }
    if (!selected) return {};
    QByteArray dll;
    if (!readMember(attempt, program, installer, *selected, dll) || dll.size() < 64 || !dll.startsWith("MZ")) return {};
    const auto offset = qFromLittleEndian<quint32>(dll.constData() + 60);
    if (offset > quint32(dll.size() - 24) || dll.mid(offset, 4) != QByteArray("PE\0\0", 4) ||
        qFromLittleEndian<quint16>(dll.constData() + offset + 4) != 0x14c ||
        !(qFromLittleEndian<quint16>(dll.constData() + offset + 22) & 0x2000)) return {};
    QFile copy(QDir(root).filePath("mfc42.dll"));
    return copy.open(QIODevice::WriteOnly | QIODevice::NewOnly) && copy.write(dll) == dll.size() && copy.flush()
        ? copy.fileName() : QString();
}

QString stageLegacyWinZipSetup(const QString& installer, const QString& root) {
    const auto program = sevenZip();
    if (program.isEmpty()) return {};
    const ExtractionAttempt attempt;
    const auto archive = QFileInfo(installer).absoluteFilePath();
    std::vector<ArchiveMember> members;
    if (!archiveMembers(attempt, program, archive, members) || members.size() > maxFiles) return {};

    static const QRegularExpression basename(R"(^[A-Za-z0-9_][A-Za-z0-9_ .()+,-]{0,239}$)");
    QSet<QString> names;
    QString setup;
    qint64 total = 0;
    for (const auto& member : members) {
        const auto key = member.leaf.toCaseFolded();
        if (member.leaf != member.path || !basename.match(member.leaf).hasMatch() ||
            names.contains(key) || member.size > maxTotalSize - total) return {};
        names.insert(key);
        total += member.size;
        if (key == "setup.exe") setup = member.leaf;
    }
    // The old WinZip wrapper can exit without starting its bundled InstallShield setup under Wine.
    if (setup.isEmpty() || !names.contains("setup.ins") || !names.contains("data1.hdr") || !names.contains("data1.cab"))
        return {};

    QTemporaryDir stage(QDir(root).filePath("setup-XXXXXX"));
    if (!stage.isValid()) return {};
    for (const auto& member : members) {
        if (!writeMember(attempt, program, archive, member, stage.filePath(member.leaf))) return {};
    }
    stage.setAutoRemove(false);
    return stage.filePath(setup);
}
#endif

bool unpackWith7z(const ExtractionAttempt& attempt, const QString& program, const QString& installer, const QString& syncRoot,
                 std::vector<frondSync::InstallerFile>& files, bool& inspectedMsiMedia) {
    ArchiveScan scan{attempt, program, syncRoot, QStandardPaths::findExecutable("unshield"), files};
    return scan.unpack(installer, 0, inspectedMsiMedia) && !files.empty();
}

}  // namespace

namespace {

struct PalmUserFiles {
    unsigned entries{};
    qint64 totalBytes{};

    bool copy(const QDir& source, const QDir& destination, unsigned depth = 0) {
        constexpr qint64 maxUserBytes = 1024ll * 1024 * 1024;
        const QFileInfo root(source.path());
        if (!root.exists() && !root.isSymLink()) return true;
        if (!root.isDir() || root.isSymLink() || depth > 24) return false;
        QDirIterator files(source.path(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (files.hasNext()) {
            files.next();
            const auto file = files.fileInfo();
            const auto name = file.fileName();
            if (!depth && (!name.compare("Install", Qt::CaseInsensitive) || !name.compare("Received", Qt::CaseInsensitive)))
                continue;
            if (++entries > 8192 || file.isSymLink()) return false;
            if (file.isDir()) {
                if (!destination.exists(name) && !QDir().mkdir(destination.filePath(name))) return false;
                if (!copy(QDir(file.filePath()), QDir(destination.filePath(name)), depth + 1)) return false;
            } else {
                if (!file.isFile() || file.size() < 0 || file.size() > maxUserBytes - totalBytes ||
                    !QFile::copy(file.filePath(), destination.filePath(name))) return false;
                totalBytes += file.size();
            }
        }
        return true;
    }
};

bool restorePrevious(const QString& current) {
    const QFileInfo original(current), previous(current + ".previous");
    if (original.isSymLink() || previous.isSymLink() || (original.exists() && !original.isDir()) ||
        (previous.exists() && !previous.isDir())) return false;
    if (!previous.exists()) return true;
    return original.exists() ? QDir(previous.filePath()).removeRecursively()
                             : QDir().rename(previous.filePath(), current);
}

bool replaceDirectory(const QString& current, const QString& replacement) {
    const bool exists = QFileInfo::exists(current);
    const auto previous = current + ".previous";
    QDir files;
    if (exists && !files.rename(current, previous)) return false;
    if (!files.rename(replacement, current)) {
        if (exists) files.rename(previous, current);
        return false;
    }
    return !exists || QDir(previous).removeRecursively();
}

QByteArray installerDigest(const QString& installer) {
    QFile executable(installer);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!executable.open(QIODevice::ReadOnly) || !hash.addData(&executable)) return {};
    return hash.result().toHex();
}

bool installMarker(const QString& marker, QByteArray value, const QString& profile = {}) {
    if (value.isEmpty()) return false;
    if (!profile.isEmpty()) {
        value += '\n';
        value += QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex();
    }
    const QFileInfo previous(marker);
    if (previous.isSymLink()) return false;
    if (previous.exists()) {
        QFile current(marker);
        return previous.isFile() && previous.size() == value.size() && current.open(QIODevice::ReadOnly) &&
               current.readAll() == value;
    }
    QSaveFile pending(marker);
    return pending.open(QIODevice::WriteOnly) && pending.write(value) == value.size() && pending.commit();
}

bool registrationFiles(const QDir& source, const QDir& destination) {
    static const QRegularExpression leaf(QStringLiteral("^[0-9a-f]{8}\\.conf$"));
    for (const auto& file : source.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
        if (!leaf.match(file.fileName()).hasMatch()) continue;
        if (file.size() <= 0 || file.size() > 65536 || !QFile::copy(file.filePath(), destination.filePath(file.fileName())))
            return false;
    }
    return true;
}

bool prepareRegistrations(const QDir& root, const QDir& staged) {
    const auto registered = root.filePath("registered");
    return restorePrevious(registered) && QDir().mkdir(staged.path()) && registrationFiles(QDir(registered), staged);
}

bool publishRegistrations(const QDir& root, const QDir& staged, bool& changed) {
    QTemporaryDir checked(staged.absoluteFilePath("checked-XXXXXX"));
    if (!checked.isValid() || !registrationFiles(staged, QDir(checked.path()))) return false;
    const QDir current(root.filePath("registered"));
    const QDir verified(checked.path());
    const auto files = verified.entryList(QDir::Files);
    changed = current.entryList(QDir::Files) != files;
    if (!changed) {
        for (const auto& name : files) {
            QFile oldFile(current.filePath(name)), newFile(verified.filePath(name));
            if (!oldFile.open(QIODevice::ReadOnly) || !newFile.open(QIODevice::ReadOnly)) return false;
            if (oldFile.readAll() != newFile.readAll()) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return true;
    return replaceDirectory(current.path(), verified.path());
}

}  // namespace

QString conduitHostPath() {
    const QDir helpers(shims());
    for (const auto* name : {"x86/PalmConduitHost.exe", "PalmConduitHost.exe"}) {
        const QFileInfo host(helpers.filePath(QString::fromLatin1(name)));
        if (host.isFile()) return host.absoluteFilePath();
    }
    return {};
}

bool available() {
    const QDir helpers(shims());
#ifdef Q_OS_LINUX
    if (wineServer().isEmpty()) return false;
#endif
    return !runtime(helpers.path()).isEmpty() && helpers.exists("RunInstaller.exe") && helpers.exists("InstAide.dll") &&
           helpers.exists("Instapp.exe") && helpers.exists("CEAppMgr.exe");
}

RunResult run(QWidget* parent, const QString& installer, const QString& user, bool windowsMobile,
              const QString& syncRoot, const QString& profileFolder, const QString& conduitUserKey,
              uint32_t conduitUserId) {
    RunResult result;
    const auto report = [&](const QString& message) {
        if (!result.error.isEmpty()) result.error += '\n';
        result.error += message;
    };
    QTemporaryDir stage(QDir(syncRoot).filePath("installer-XXXXXX"));
    stage.setAutoRemove(false);
    const auto removeStage = [&] {
        if (!stage.isValid()) return;
        bool removed = stage.remove();
#ifdef Q_OS_WIN
        // Job termination can still be closing the installer's file handles.
        for (int retry = 0; !removed && retry < 20; ++retry) {
            QThread::msleep(50);
            removed = stage.remove();
        }
#endif
        if (!removed) report(QObject::tr("Couldn't remove the installer folder: %1.").arg(stage.path()));
    };
    if (!stage.isValid() || !QDir(stage.path()).mkpath("User/Install") || !QDir(stage.path()).mkpath("User/Received")) {
        report(QObject::tr("Couldn't create the installer folder."));
        if (stage.isValid()) finishOnWorker(parent, removeStage);
        return result;
    }

    auto launchPath = installer;
#ifdef Q_OS_LINUX
    if (legacyWinZipInstaller(installer)) {
        finishOnWorker(parent, [&] {
            const auto setup = stageLegacyWinZipSetup(installer, stage.path());
            if (!setup.isEmpty()) launchPath = setup;
        }, QObject::tr("Preparing the installer…"));
    }
#endif
    const auto folder = helperFolder(launchPath);
    const bool manageConduits = !windowsMobile && !profileFolder.isEmpty() &&
        QFileInfo::exists(QDir(folder).filePath("CondMgr.dll")) && QFileInfo::exists(QDir(folder).filePath("HotSync.exe")) &&
        (QFileInfo::exists(QDir(folder).filePath("PalmConduitHost.exe")) ||
         QFileInfo::exists(QDir(shims()).filePath("x86/PalmConduitHost.exe")));
    const QDir conduitFolder(QDir(profileFolder).filePath("conduits"));
    const QDir registrations(stage.filePath("conduits"));
    const QDir userStage(stage.filePath("User"));
    const auto marker = conduitFolder.filePath("installing");
    const auto seed = conduitFolder.filePath("seed");
    const auto data = conduitFolder.filePath("data");
    const auto userFolder = conduitUserKey.isEmpty() ? seed : QDir(data).filePath(conduitUserKey);
#ifdef Q_OS_WIN
    frondSync::ProfileSyncLock nativeInstallLock;
    const auto nativeMarker = QDir(syncRoot).filePath(QString::fromLatin1(frondSync::PalmConduitNativeInstallMarker));
    std::string nativeLockError;
    if (manageConduits && !nativeInstallLock.Acquire(syncRoot.toStdString(), frondSync::PalmConduitNativeLockId,
                                                       nativeLockError)) {
        report(QObject::tr("A Palm conduit is already running."));
        finishOnWorker(parent, removeStage);
        return result;
    }
#endif
    if (manageConduits) {
        bool ready = false;
        finishOnWorker(parent, [&] {
            static const QRegularExpression key(QStringLiteral("^[0-9a-f]{32}-[0-9a-f]{8}$"));
            if (QFileInfo(conduitFolder.path()).isSymLink() || !QDir().mkpath(conduitFolder.path()) ||
                (!conduitUserKey.isEmpty() && (!key.match(conduitUserKey).hasMatch() || QFileInfo(data).isSymLink() ||
                 !QDir().mkpath(data))) || !restorePrevious(seed) || !restorePrevious(userFolder) ||
                !prepareRegistrations(conduitFolder, registrations)) return;
            const auto needsSeed = !conduitUserKey.isEmpty() && !QFileInfo::exists(userFolder);
            if (!PalmUserFiles{}.copy(QDir(needsSeed ? seed : userFolder), userStage)) return;
            const auto checksum = installerDigest(installer);
#ifdef Q_OS_WIN
            const auto nativeProfile = QFileInfo(profileFolder).canonicalFilePath();
            if (nativeProfile.isEmpty() || !installMarker(nativeMarker, checksum, nativeProfile)) {
                report(QObject::tr("Retry the previous Palm installer in its original sync profile."));
                return;
            }
#endif
            if (!installMarker(marker, checksum)) {
                report(QObject::tr("Retry the previous Palm installer to finish updating this profile."));
                return;
            }
            ready = true;
        }, QObject::tr("Preparing the installer…"));
        if (!ready) {
            if (result.error.isEmpty()) report(QObject::tr("Couldn't prepare the Palm conduit folder."));
            finishOnWorker(parent, removeStage);
            return result;
        }
    }
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("FROND_INSTALL_STAGE", windowsPath(stage.path()));
    env.insert("FROND_INSTALL_SHIMS", windowsPath(folder));
    env.insert("FROND_INSTALL_USER", user);
    env.insert("FROND_INSTALL_KIND", windowsMobile ? "winmo" : "palm");
    if (manageConduits) env.insert("FROND_PALM_CONDUIT_STAGE", windowsPath(registrations.path()));
    else env.remove("FROND_PALM_CONDUIT_STAGE");
    if (manageConduits && conduitUserId) env.insert("FROND_INSTALL_USER_ID", QString::number(conduitUserId));
    else env.remove("FROND_INSTALL_USER_ID");
    QStringList args{windowsPath(launchPath)};
#ifdef Q_OS_LINUX
    // Palm conduits need their installed DLLs and desktop files on later syncs.
    const auto prefix = manageConduits ? conduitFolder.filePath("wine") : stage.filePath("wine");
    if (QFileInfo(prefix).isSymLink()) {
        report(QObject::tr("Couldn't use the installer's Wine folder."));
        finishOnWorker(parent, removeStage);
        return result;
    }
    const bool freshConduitPrefix = manageConduits && !QFileInfo::exists(prefix);
    env.remove("FROND_PALM_MFC42_STAGE");
    env.remove("FROND_MANAGED_PALM_APP");
    if (manageConduits) {
        const auto drive = QDir(QDir(prefix).filePath("drive_c")).canonicalPath();
        const auto program = QFileInfo(launchPath).canonicalFilePath();
        if (!drive.isEmpty() && program.startsWith(drive + '/')) {
            auto relative = program.mid(drive.size() + 1);
            args.front() = QStringLiteral("C:\\") + relative.replace('/', '\\');
            if (QFileInfo(launchPath).suffix().compare("exe", Qt::CaseInsensitive) == 0)
                env.insert("FROND_MANAGED_PALM_APP", "1");
        }
        QString mfc;
        finishOnWorker(parent, [&] { mfc = stagePalmInstallerRuntime(launchPath, stage.path()); },
                       QObject::tr("Preparing the installer…"));
        if (!mfc.isEmpty()) env.insert("FROND_PALM_MFC42_STAGE", windowsPath(mfc));
    }
    const auto overrides = env.value("WINEDLLOVERRIDES");
    env.insert("WINEDLLOVERRIDES", overrides.isEmpty() ? "winemenubuilder.exe=d" : overrides + ";winemenubuilder.exe=d");
    env.insert("WINEPREFIX", prefix);
    env.insert("WINESERVER", wineServer());
    env.insert("FROND_INSTALL_WINE", "1");
    args.prepend(folder + "/RunInstaller.exe");
#else
    env.remove("FROND_INSTALL_WINE");
    env.remove("FROND_PALM_MFC42_STAGE");
    env.remove("FROND_MANAGED_PALM_APP");
#endif
    const bool managedCompanion = env.value("FROND_MANAGED_PALM_APP") == "1";

#ifdef Q_OS_WIN
    InstallerWait waiting(parent);
#else
    InstallerWait waiting(parent, true);
#endif
    waiting.setWindowTitle(QObject::tr("Windows installer"));
    auto* layout = new QVBoxLayout(&waiting);
    auto* status = new QLabel(QObject::tr("Finish the installer to choose its files."), &waiting);
    layout->addWidget(status);
    if (manageConduits) layout->addWidget(new QLabel(QObject::tr("Installed Palm conduits can run during HotSync for this profile."), &waiting));
    auto* buttons = new QDialogButtonBox(&waiting);
    layout->addWidget(buttons);
#ifdef Q_OS_WIN
    waiting.setWindowFlag(Qt::WindowCloseButtonHint, false);
    buttons->hide();
#else
    buttons->addButton(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &waiting, &QDialog::reject);
#endif
    QTimer ready;
    if (manageConduits) {
        auto* finish = buttons->addButton(QObject::tr("Finish"), QDialogButtonBox::ActionRole);
        finish->setAutoDefault(false);
        finish->hide();
        QObject::connect(&ready, &QTimer::timeout, &waiting, [&, finish] {
            const QFileInfo offered(stage.filePath("finish-ready"));
            if (!offered.isFile() || offered.isSymLink()) return;
            ready.stop();
            status->setText(QObject::tr("Close the installer’s remaining windows before choosing Finish."));
            finish->show();
            buttons->show();
        });
        QObject::connect(finish, &QPushButton::clicked, &waiting, [&, finish] {
            QFile request(stage.filePath("finish"));
            if (!request.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                status->setText(QObject::tr("Couldn't finish the installer."));
                return;
            }
            finish->setEnabled(false);
        });
        ready.start(250);
    }

    QProcess process;
    process.setProcessEnvironment(env);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_LINUX
    process.setChildProcessModifier([] {
        if (setpgid(0, 0) != 0) _exit(1);
    });
#endif
    QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &waiting, &QDialog::accept);
    bool started = false;
    QObject::connect(&process, &QProcess::started, &waiting, [&] { started = true; });
    QObject::connect(&process, &QProcess::errorOccurred, &waiting, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) waiting.accept();
    });
    process.start(runtime(folder), args);
    const bool cancelled = process.state() != QProcess::NotRunning && waiting.exec() == QDialog::Rejected;
    if (cancelled) report(QObject::tr("The installer was cancelled."));
    else if (!started) report(process.errorString());
    else if (process.exitStatus() == QProcess::CrashExit) report(QObject::tr("The installer launcher crashed."));
    else if (process.exitCode()) report(QObject::tr("The installer exited with code %1.").arg(process.exitCode()));

#ifdef Q_OS_LINUX
    if (cancelled && process.state() != QProcess::NotRunning) {
        QFile stop(stage.filePath("stop"));
        if (stop.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            stop.close();
            waitForExit(process, 2000);
        }
    }
    if (process.state() != QProcess::NotRunning) {
        const auto pid = process.processId();
        if (pid > 0) kill(-pid, SIGKILL);
        process.kill();
        if (!waitForExit(process, 3000)) report(QObject::tr("Couldn't stop the installer launcher."));
    }
#endif

    const bool launcherStopped = process.state() == QProcess::NotRunning;
#ifdef Q_OS_WIN
    const bool crashed = started && process.exitStatus() == QProcess::CrashExit;
#endif
    finishOnWorker(parent, [&] {
        bool stopped = launcherStopped;
#ifdef Q_OS_LINUX
        const QFileInfo winePrefix(prefix);
        if (winePrefix.isSymLink()) {
            stopped = false;
            report(QObject::tr("The installer changed its Wine folder."));
        } else if (winePrefix.isDir()) {
            // A server may already have exited by the time -k runs. -w confirms it is gone.
            wineServerCommand(env, "-k");
            if (!wineServerCommand(env, "-w")) {
                stopped = false;
                report(QObject::tr("Couldn't stop the installer's Wine session."));
            }
        }
#endif
#ifdef Q_OS_WIN
        if (crashed) stopped = false;
#endif
        if (started && stopped) {
            size_t total = 0;
            for (const auto& folder : {QStringLiteral("User/Install"), QStringLiteral("User/Received")}) {
                const QDir received(QDir(stage.path()).filePath(folder));
                for (const auto& file : received.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
                    const auto name = file.fileName().toStdString();
                    if (!frondSync::IsInstallFileName(name)) continue;
                    const bool cab = file.suffix().compare("cab", Qt::CaseInsensitive) == 0;
                    if (cab != windowsMobile) continue;
                    if (file.size() <= 0) continue;
                    if (file.size() > maxFileSize || result.files.size() >= maxFiles || size_t(file.size()) > maxTotalSize - total) {
                        report(QObject::tr("The installer has too many files or exceeds the unpacked size limit."));
                        break;
                    }
                    frondSync::InstallerFile payload{name, {}};
                    QString error;
                    if (!frondSyncFiles::readDatabase(file.filePath(), payload.bytes, error)) {
                        report(error);
                        continue;
                    }
                    if (!frondSync::IsInstallPayload(payload)) {
                        report(QObject::tr("The installer supplied an invalid file: %1").arg(file.fileName()));
                        continue;
                    }
                    total += payload.bytes.size();
                    result.files.push_back(std::move(payload));
                }
            }
            removeDuplicatePayloads(result.files);
        }

        if (manageConduits && started && stopped && result.error.isEmpty()) {
            QTemporaryDir checked(stage.filePath("palm-user-XXXXXX"));
            PalmUserFiles desktopFiles;
            bool saved = checked.isValid() && QFileInfo(userStage.path()).isDir() &&
                         desktopFiles.copy(userStage, QDir(checked.path())) &&
                         replaceDirectory(userFolder, checked.path()) &&
                         publishRegistrations(conduitFolder, registrations, result.conduitsChanged) &&
                         QFile::remove(marker);
#ifdef Q_OS_WIN
            saved = saved && QFile::remove(nativeMarker);
#endif
            if (saved) result.palmDesktopSaved = managedCompanion && desktopFiles.totalBytes > 0;
            else report(QObject::tr("Couldn't save the Palm conduit registrations and user files."));
        }
        if (started && stopped && result.files.empty() && !result.conduitsChanged && !result.palmDesktopSaved &&
            result.error.isEmpty()) report(QObject::tr("The installer supplied no files for this device."));
#ifdef Q_OS_LINUX
        if (stopped && freshConduitPrefix && !QFileInfo(conduitFolder.filePath("registered")).isDir() &&
            !QDir(prefix).removeRecursively())
            report(QObject::tr("Couldn't remove the installer's Wine folder."));
#endif
        if (stopped) removeStage();
        else report(QObject::tr("The installer folder was kept at %1.").arg(stage.path()));
    });
    if (result.error.isEmpty() && result.files.empty() && result.conduitsChanged && !result.palmDesktopSaved) {
        std::vector<frondSync::InstallerFile> deferred;
        QString error;
        if (unpack(parent, installer, syncRoot, deferred, error)) {
            deferred.erase(std::remove_if(deferred.begin(), deferred.end(), [](const auto& file) {
                return QFileInfo(QString::fromStdString(file.name)).suffix().compare("cab", Qt::CaseInsensitive) == 0;
            }), deferred.end());
            result.files = std::move(deferred);
        }
    }
    return result;
}

void removeDuplicatePayloads(std::vector<frondSync::InstallerFile>& files) {
    for (auto file = files.begin(); file != files.end(); ++file) {
        files.erase(std::remove_if(std::next(file), files.end(), [&](const auto& other) {
            return file->bytes == other.bytes;
        }), files.end());
    }
}

static bool unpackFiles(const ExtractionAttempt& attempt, const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error) {
    frondSync::Bytes bytes;
    if (!frondSyncFiles::readDatabase(installer, bytes, error)) return false;
    const QFileInfo info(installer);
    const auto suffix = info.suffix().toLower();
    const auto program = sevenZip();
    if (program.isEmpty() && (suffix == "7z" || suffix == "rar")) {
        error = QObject::tr("Install 7-Zip to open this archive.");
        return false;
    }
    std::vector<frondSync::InstallerFile> unpacked;
    bool inspectedMsiMedia = false;
    if (!program.isEmpty() && unpackWith7z(attempt, program, installer, syncRoot, unpacked, inspectedMsiMedia)) {
        files = std::move(unpacked);
        return true;
    }
    if (attempt.stopped()) return false;
    if (suffix == "exe" && program.isEmpty()) {
        unpacked.clear();
        if (unpackInno(attempt, installer, syncRoot, unpacked)) {
            files = std::move(unpacked);
            return true;
        }
    }
    if (attempt.stopped()) return false;
    // A normal MSI media cabinet is itself MSCF, which the raw fallback cannot distinguish.
    std::string problem = "No Palm or Windows Mobile install files were found.";
    if (!inspectedMsiMedia && frondSync::UnpackInstaller(info.fileName().toStdString(), bytes, files, problem, true,
                                    [&] { return attempt.stopped(); })) return true;
    error = QString::fromStdString(problem);
    return false;
}

bool unpack(QWidget* parent, const QString& installer, const QString& syncRoot,
            std::vector<frondSync::InstallerFile>& files, QString& error) {
    ExtractionAttempt attempt;
    InstallerWait progress(parent);
    progress.setWindowTitle(QObject::tr("Extract install files"));
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    auto* layout = new QVBoxLayout(&progress);
    auto* label = new QLabel(QObject::tr("Extracting install files…"), &progress);
    layout->addWidget(label);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &progress);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &progress, [&] {
        attempt.cancelled = true;
        buttons->setEnabled(false);
        label->setText(QObject::tr("Stopping extraction…"));
    });
    std::vector<frondSync::InstallerFile> extracted;
    bool ok = false;
    waitForWorker(progress, [&] {
        ok = unpackFiles(attempt, installer, syncRoot, extracted, error);
    });
    if (attempt.cancelled) error = QObject::tr("Extraction was cancelled.");
    else if (attempt.deadline.hasExpired()) error = QObject::tr("Extraction timed out.");
    else if (ok) {
        files = std::move(extracted);
        return true;
    }
    return false;
}

}  // namespace frondInstallerRunner
