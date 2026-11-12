#ifndef _APP_LAUNCHER_H_
#define _APP_LAUNCHER_H_

struct SyscallDispatch;

bool launchAppByName(struct SyscallDispatch* sd, const char* name);

// Deletes a RAM database by name with DmDeleteDatabase. A ROM database and the running application
// are refused, and so is one Palm OS reports open.
enum class DeleteDbResult { deleted, missing, rom, running, open, failed };
// application, when given, tells whether the database was an application.
DeleteDbResult deleteDatabaseByName(struct SyscallDispatch* sd, const char* name, bool* application = nullptr);

// Relaunches the launcher when it is in front, unless an event is waiting, so it drops a deleted
// application's icon. False when the switch could not be made.
bool relaunchLauncher(struct SyscallDispatch* sd);

#endif  // _APP_LAUNCHER_H_
