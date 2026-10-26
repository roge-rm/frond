#include "SdExchangeDialog.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

SdExchangeDialog::SdExchangeDialog(SdExchange& exchange, const QString& recovery, QWidget* parent)
    : QDialog(parent), exchange(exchange) {
    setObjectName("sdExchangeDialog");
    setWindowTitle(tr("Exchange SD Files — Card Ejected")); resize(680, 520);
    auto* layout = new QVBoxLayout(this);
    auto* warning = new QLabel(tr("Edits reach the Palm only after Apply and Reinsert. Cancel keeps the original card ejected."), this);
    warning->setWordWrap(true); layout->addWidget(warning);
    auto* backup = new QLabel(tr("Recovery session: %1").arg(recovery), this);
    backup->setTextFormat(Qt::PlainText); backup->setWordWrap(true); backup->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(backup);
    auto* form = new QFormLayout;
    path = new QLineEdit("/", this); path->setObjectName("cardPath");
    form->addRow(tr("Card folder:"), path); layout->addLayout(form);
    auto* browse = new QPushButton(tr("List Folder"), this); layout->addWidget(browse);
    files = new QPlainTextEdit(this); files->setReadOnly(true); layout->addWidget(files);
    auto* operations = new QDialogButtonBox(this);
    auto* put = operations->addButton(tr("Copy Host File to Card…"), QDialogButtonBox::ActionRole);
    auto* get = operations->addButton(tr("Copy Card File to Host…"), QDialogButtonBox::ActionRole);
    auto* mkdir = operations->addButton(tr("New Card Folder…"), QDialogButtonBox::ActionRole);
    layout->addWidget(operations);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* apply = buttons->addButton(tr("Apply and Reinsert"), QDialogButtonBox::AcceptRole);
    apply->setObjectName("applyExchange");
    layout->addWidget(buttons);
    for (auto* button : findChildren<QPushButton*>()) button->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, &SdExchangeDialog::showFiles);
    connect(path, &QLineEdit::returnPressed, this, &SdExchangeDialog::showFiles);
    const auto prefix = [this] { return path->text() == "/" ? QString("/") : path->text() + '/'; };
    connect(put, &QPushButton::clicked, this, [this, prefix] {
        const auto source = QFileDialog::getOpenFileName(this, tr("Copy Host File to Card"));
        if (source.isEmpty()) return;
        bool ok;
        const auto destination = QInputDialog::getText(this, tr("New Card Filename"), tr("Complete card path (existing names are refused):"),
            QLineEdit::Normal, prefix() + QFileInfo(source).fileName(), &ok);
        if (!ok) return;
        QString error; const bool copied = this->exchange.put(source, destination, error); report(copied, error);
    });
    connect(get, &QPushButton::clicked, this, [this, prefix] {
        bool ok;
        const auto source = QInputDialog::getText(this, tr("Copy Card File to Host"), tr("Complete card filename:"),
            QLineEdit::Normal, prefix(), &ok);
        if (!ok) return;
        QFileDialog dialog(this, tr("New Host Filename"), QFileInfo(source).fileName());
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setOption(QFileDialog::DontConfirmOverwrite);
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
        QString error; const bool copied = this->exchange.get(source, dialog.selectedFiles().first(), error); report(copied, error);
    });
    connect(mkdir, &QPushButton::clicked, this, [this, prefix] {
        bool ok;
        const auto destination = QInputDialog::getText(this, tr("New Card Folder"), tr("Complete new folder path:"),
            QLineEdit::Normal, prefix(), &ok);
        if (!ok) return;
        QString error; const bool created = this->exchange.mkdir(destination, error); report(created, error);
    });
    showFiles();
}
void SdExchangeDialog::showFiles() {
    QString listing, error;
    if (exchange.list(path->text(), listing, error)) files->setPlainText(listing);
    else files->setPlainText(error);
}
void SdExchangeDialog::report(bool ok, const QString& error) {
    if (!ok) QMessageBox::warning(this, tr("SD File Exchange"), error);
    showFiles();
}
