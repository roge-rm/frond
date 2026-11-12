#ifndef _DB_BACKUP_H_
#define _DB_BACKUP_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "db_list.h"

struct SyscallDispatch;
struct zip_t;

class DbBackup {
   public:
    static constexpr int BACKUP_TYPE_RAM = 0;
    static constexpr int BACKUP_TYPE_RAM_ROM = 1;
    static constexpr int BACKUP_TYPE_EVERYTHING = 2;

    static constexpr int STATE_CREATED = 0;
    static constexpr int STATE_IN_PROGRESS = 1;
    static constexpr int STATE_DONE = 2;
    static constexpr int STATE_ERROR = -1;

   public:
    DbBackup(SyscallDispatch* syscallDispatch, int backupType, std::string databaseName = {});
    ~DbBackup();

    bool Init();
    int GetState() const;
    bool Continue();
    bool HasLastProcessedDb() const;
    const char* GetLastProcessedDb() const;

    const void* GetArchiveData();
    size_t GetArchiveSize();

   private:
    bool Include(DbMetadata& db);
    void CopyArchiveData();

   private:
   private:
    SyscallDispatch* sd{nullptr};

    std::string databaseName;
    int backupType{BACKUP_TYPE_RAM};
    int state{STATE_CREATED};

    zip_t* zip{nullptr};
    void* archiveData{nullptr};
    ssize_t archiveSize{0};

    DbMetadataList dbList;
    DbMetadataList::iterator dbIterator;

    std::unique_ptr<uint8_t[]> chunkBuffer;
    char lastProcessedDb[32];

   private:
    DbBackup(const DbBackup&) = delete;
    DbBackup(DbBackup&&) = delete;
    DbBackup& operator=(const DbBackup&) = delete;
    DbBackup& operator=(DbBackup&&) = delete;
};

#endif  // _DB_BACKUP_H_