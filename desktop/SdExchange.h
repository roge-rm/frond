#pragma once
#include <QTemporaryDir>
#include <QString>

// Works only on private, ejected image copies. No emulator or host mounts.
class SdExchange {
 public:
    explicit SdExchange(const QString& scratchTemplate = {});
    static bool available();
    bool open(const QString& source, QString& error);
    QString image() const { return current; }
    bool list(const QString& path, QString& listing, QString& error);
    bool put(const QString& source, const QString& destination, QString& error);
    bool get(const QString& source, const QString& destination, QString& error);
    bool mkdir(const QString& path, QString& error);
    static bool validPath(const QString& path, bool root = false);
 private:
    bool run(const QString& tool, const QString& image, const QStringList& args,
             QString& output, QString& error);
    QString candidate(QString& error);
    void adopt(const QString& path);
    QTemporaryDir directory;
    QString current;
    qint64 offset{0};
    unsigned serial{0};
};
