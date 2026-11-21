#pragma once

#include <QString>
#include <vector>

#include "sync/PimRecord.h"

namespace frondSyncFiles {

QString filters(frondSync::PimKind kind);
QString extension(frondSync::PimKind kind);
bool read(const QString& path, frondSync::PimKind kind, std::vector<frondSync::PimRecord>& records, QString& error);
bool write(const QString& path, frondSync::PimKind kind, const std::vector<frondSync::PimRecord>& records, QString& error);
bool readDatabase(const QString& path, frondSync::Bytes& bytes, QString& error);
bool writeDatabase(const QString& path, const frondSync::Bytes& bytes, QString& error);

}
