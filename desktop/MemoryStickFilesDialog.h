#pragma once
#include <QDialog>
#include <QList>
#include <functional>
#include "DesktopClient.h"
class QLabel;
class QLineEdit;
class QTreeWidget;
class QPushButton;

class MemoryStickFilesDialog : public QDialog {
 public:
    MemoryStickFilesDialog(DesktopClient& client, std::function<bool(const QString&, const QString&)> send,
                           bool writeProtected, QWidget* parent = nullptr);
 private:
    void list();
    void request(const QString& command, const QString& argument);
    void result(const QString& command, bool ok, const QString& code, const QString& message);
    void refresh();
    QString selectedPath();
    QString joined(const QString& name) const;
    DesktopClient& client;
    std::function<bool(const QString&, const QString&)> send;
    QLineEdit* path;
    QTreeWidget* files;
    QLabel* status;
    QList<QPushButton*> buttons;
    QList<QPushButton*> writes;
    QString pending, listedPath, listingPath;
    bool writeProtected;
};
