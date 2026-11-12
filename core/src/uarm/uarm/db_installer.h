
#ifndef _DB_INSTALLER_H_
#define _DB_INSTALLER_H_

#include <cstdint>

#include "SoC.h"

#define DB_INSTALL_RESULT_NEEDS_RESET 1
#define DB_INSTALL_RESULT_OK 0
#define DB_INSTALL_RESULT_ERR_CORRUPT -2
#define DB_INSTALL_RESULT_ERR_OOM -3
#define DB_INSTALL_RESULT_ERR_IS_OPEN -4
#define DB_INSTALL_RESULT_ERR_COULD_NOT_OVERWRITE -5
#define DB_INSTALL_RESULT_ERR_NOT_SUPPORTED -6
#define DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE -7
#define DB_INSTALL_RESULT_ERR_UNKNOWN -8

struct SyscallDispatch;

int32_t dbInstallerInstall(struct SyscallDispatch* sd, size_t len, void* data);

// What Palm OS gets after a HotSync install, for the database just installed from data: the app
// with its creator gets sysAppLaunchCmdSyncNotify unless it's the app in front, and an app install
// while the launcher is in front relaunches the launcher so it lists the app. Any other app in front
// stays. Runs at the same EvtGetEvent safe point as the install, in its own guest transaction.
void dbInstallerFinish(struct SyscallDispatch* sd, size_t len, const void* data);

#endif  // _DB_INSTALLER_H_
