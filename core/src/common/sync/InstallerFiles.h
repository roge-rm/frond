#ifndef FROND_SYNC_INSTALLER_FILES_H
#define FROND_SYNC_INSTALLER_FILES_H

#include "PalmDb.h"

namespace frondSync {

struct InstallerFile {
    std::string name;
    Bytes bytes;
};

bool IsInstallPayload(const InstallerFile& file);
bool IsInstallFileName(const std::string& name);
bool IsInstallerFileName(const std::string& name);
bool UnpackInstaller(const std::string& name, const Bytes& bytes, std::vector<InstallerFile>& files, std::string& error);

}  // namespace frondSync
#endif
