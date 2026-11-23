#pragma once

#include <QString>
#include <cstdint>
#include <vector>

#include "sync/InstallerFiles.h"

class QWidget;

namespace frondInstallerRunner {

bool available();
QString conduitHostPath();
struct RunResult {
    std::vector<frondSync::InstallerFile> files;
    QString error;
    bool conduitsChanged = false;
    bool palmDesktopSaved = false;
};

RunResult run(QWidget* parent, const QString& installer, const QString& user, bool windowsMobile,
              const QString& syncRoot, const QString& profileFolder = {}, const QString& conduitUserKey = {},
              uint32_t conduitUserId = 0);
void removeDuplicatePayloads(std::vector<frondSync::InstallerFile>& files);
bool unpack(QWidget* parent, const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error);

}
