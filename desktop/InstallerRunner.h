#pragma once

#include <QString>
#include <vector>

#include "sync/InstallerFiles.h"

class QWidget;

namespace frondInstallerRunner {

bool available();
bool run(QWidget* parent, const QString& installer, const QString& user, bool windowsMobile,
         const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error);
bool unpack(const QString& installer, const QString& syncRoot, std::vector<frondSync::InstallerFile>& files, QString& error);

}
