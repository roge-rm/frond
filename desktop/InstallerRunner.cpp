#include "InstallerRunner.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QSet>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QtEndian>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <memory>
#include <iterator>
#include <utility>

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
void finishOnWorker(QWidget* parent, Work&& work) {
    InstallerWait progress(parent);
    progress.setWindowTitle(QObject::tr("Windows installer"));
    progress.setWindowFlag(Qt::WindowCloseButtonHint, false);
    auto* layout = new QVBoxLayout(&progress);
    layout->addWidget(new QLabel(QObject::tr("Checking installer files and cleaning up…"), &progress));
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
        // Let the native helper finish. QProcess only owns its immediate process.
        do {
            if (process.state() == QProcess::Starting) process.waitForStarted(50);
            else if (output) process.waitForReadyRead(50);
            else process.waitForFinished(50);
            if (output) {
                const auto chunk = process.readAllStandardOutput();
                if (chunk.size() > limit - output->size()) exceeded = true;
                if (!exceeded && !stopped()) *output += chunk;
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

bool unpackWith7z(const ExtractionAttempt& attempt, const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files) {
    const QString program = QStandardPaths::findExecutable("7z");
    if (program.isEmpty()) return false;
    std::vector<ArchiveMember> members;
    if (!archiveMembers(attempt, program, installer, members)) return false;
    qint64 total = 0;
    if (!collectDirect(attempt, program, installer, members, files, total)) return false;
#ifdef Q_OS_UNIX
    const QString unshield = QStandardPaths::findExecutable("unshield");
    if (unshield.isEmpty()) return !files.empty();
    if (!unpackInstallShield(attempt, program, installer, members, unshield, syncRoot, files, total)) return false;

    if (QFileInfo(installer).suffix().compare("zip", Qt::CaseInsensitive) == 0) {
        constexpr int maxNested = 4;
        qint64 nestedTotal = 0;
        int nestedCount = 0;
        for (const auto& member : members) {
            if (attempt.stopped()) return false;
            if (QFileInfo(member.leaf).suffix().compare("exe", Qt::CaseInsensitive) != 0) continue;
            if (nestedCount == maxNested || member.size > maxTotalSize - nestedTotal) break;
            ++nestedCount;
            nestedTotal += member.size;
            QTemporaryDir nested(QDir(syncRoot).filePath("unpack-installer-XXXXXX"));
            if (!nested.isValid()) break;
            const auto inner = nested.filePath("installer.exe");
            std::vector<ArchiveMember> innerMembers;
            if (!writeMember(attempt, program, installer, member, inner) || !archiveMembers(attempt, program, inner, innerMembers)) continue;
            if (!collectDirect(attempt, program, inner, innerMembers, files, total) ||
                !unpackInstallShield(attempt, program, inner, innerMembers, unshield, syncRoot, files, total)) return false;
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
#ifdef Q_OS_LINUX
    if (wineServer().isEmpty()) return false;
#endif
    return !runtime(helpers.path()).isEmpty() && helpers.exists("RunInstaller.exe") && helpers.exists("InstAide.dll") &&
           helpers.exists("Instapp.exe") && helpers.exists("CEAppMgr.exe");
}

RunResult run(QWidget* parent, const QString& installer, const QString& user, bool windowsMobile, const QString& syncRoot) {
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

    const auto folder = helperFolder(installer);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("FROND_INSTALL_STAGE", windowsPath(stage.path()));
    env.insert("FROND_INSTALL_SHIMS", windowsPath(folder));
    env.insert("FROND_INSTALL_USER", user);
    env.insert("FROND_INSTALL_KIND", windowsMobile ? "winmo" : "palm");
    QStringList args{windowsPath(installer)};
#ifdef Q_OS_LINUX
    // A private prefix lets us stop this Wine session and its Windows processes together.
    const auto prefix = stage.filePath("wine");
    env.insert("WINEPREFIX", prefix);
    env.insert("WINESERVER", wineServer());
    env.insert("FROND_INSTALL_WINE", "1");
    args.prepend(folder + "/RunInstaller.exe");
#else
    env.remove("FROND_INSTALL_WINE");
#endif

#ifdef Q_OS_WIN
    InstallerWait waiting(parent);
#else
    InstallerWait waiting(parent, true);
#endif
    waiting.setWindowTitle(QObject::tr("Windows installer"));
    auto* layout = new QVBoxLayout(&waiting);
    layout->addWidget(new QLabel(QObject::tr("Finish the installer to choose its files."), &waiting));
#ifdef Q_OS_WIN
    waiting.setWindowFlag(Qt::WindowCloseButtonHint, false);
#else
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &waiting);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &waiting, &QDialog::reject);
#endif

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
            if (result.files.empty() && result.error.isEmpty())
                report(QObject::tr("The installer supplied no files for this device."));
        }

        if (stopped) removeStage();
        else report(QObject::tr("The installer folder was kept at %1.").arg(stage.path()));
    });
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
    const bool zip = info.suffix().compare("zip", Qt::CaseInsensitive) == 0;
    const auto name = info.fileName().toStdString();
    std::string problem;
    if (zip && frondSync::UnpackInstaller(name, bytes, files, problem)) return true;
    std::vector<frondSync::InstallerFile> unpacked;
    if (unpackWith7z(attempt, installer, syncRoot, unpacked)) {
        files = std::move(unpacked);
        return true;
    }
    if (attempt.stopped()) return false;
    if (!zip && frondSync::UnpackInstaller(name, bytes, files, problem)) return true;
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
#ifdef Q_OS_UNIX
        label->setText(QObject::tr("Stopping extraction…"));
#else
        label->setText(QObject::tr("Waiting for the extraction helper to exit…"));
#endif
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
