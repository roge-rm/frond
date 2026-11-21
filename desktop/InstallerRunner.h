#pragma once

#include <QString>
#include <vector>

#include "sync/InstallerFiles.h"

class QWidget;

namespace frondInstallerRunner {

bool available();
struct RunResult {
    std::vector<frondSync::InstallerFile> files;
    QString error;
};

RunResult run(QWidget* parent, const QString& installer, const QString& user, bool windowsMobile, const QString& syncRoot);
void removeDuplicatePayloads(std::vector<frondSync::InstallerFile>& files);
bool unpack(QWidget* parent, const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error);

}
