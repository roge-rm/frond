#pragma once
#include <QDialog>
#include "SdExchange.h"
class QLineEdit;
class QPlainTextEdit;
class SdExchangeDialog : public QDialog {
 public:
    explicit SdExchangeDialog(SdExchange& exchange, const QString& recovery, QWidget* parent = nullptr);
 private:
    void showFiles();
    void report(bool ok, const QString& error);
    SdExchange& exchange;
    QLineEdit* path;
    QPlainTextEdit* files;
};
