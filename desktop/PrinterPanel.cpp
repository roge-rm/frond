#include "PrinterPanel.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

PrinterPanel::PrinterPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("printerPanel");
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout;
    switchBox = new QCheckBox(tr("USB printer"), this);
    switchBox->setObjectName("printerPlugged");
    readout = new QLabel(this);
    readout->setObjectName("printerReadout");
    row->addWidget(switchBox);
    row->addWidget(readout, 1);
    column->addLayout(row);
    banner = new QWidget(this);
    banner->setObjectName("printerBanner");
    auto* inner = new QVBoxLayout(banner);
    inner->setContentsMargins(0, 0, 0, 0);
    received = new QLabel(banner);
    received->setObjectName("printerReceived");
    note = new QLabel(tr("Only the Generic printer profile can be shown as pages"), banner);
    note->setObjectName("printerNote");
    note->setWordWrap(true);
    pdf = new QPushButton(tr("Save as PDF"), banner);
    print = new QPushButton(tr("Print…"), banner);
    raw = new QPushButton(tr("Save raw file"), banner);
    discard = new QPushButton(tr("Discard"), banner);
    pdf->setObjectName("printerPdf"); print->setObjectName("printerPrint");
    raw->setObjectName("printerRaw"); discard->setObjectName("printerDiscard");
    inner->addWidget(received);
    inner->addWidget(note);
    auto* buttons = new QHBoxLayout;
    for (auto* b : {pdf, print, raw, discard}) buttons->addWidget(b);
    buttons->addStretch();
    inner->addLayout(buttons);
    column->addWidget(banner);
    banner->hide();
    connect(switchBox, &QCheckBox::toggled, this, &PrinterPanel::pluggedToggled);
    connect(pdf, &QPushButton::clicked, this, &PrinterPanel::pdfRequested);
    connect(print, &QPushButton::clicked, this, &PrinterPanel::printRequested);
    connect(raw, &QPushButton::clicked, this, &PrinterPanel::rawRequested);
    connect(discard, &QPushButton::clicked, this, &PrinterPanel::discardRequested);
}
bool PrinterPanel::plugged() const { return switchBox->isChecked(); }
void PrinterPanel::setPlugged(bool plugged) {
    const QSignalBlocker blocked(switchBox);
    switchBox->setChecked(plugged);
    if (!plugged) readout->clear();
}
void PrinterPanel::setReadout(bool attached, qint64 bytes) {
    readout->setText(attached ? tr("%n byte(s) received", nullptr, int(bytes)) : QString());
}
void PrinterPanel::showReceived(qint64 bytes, bool renderable) {
    received->setText(tr("Print received (%n byte(s))", nullptr, int(bytes)));
    pdf->setEnabled(renderable);
    print->setEnabled(renderable);
    note->setVisible(!renderable);
    banner->show();
}
void PrinterPanel::hideReceived() { banner->hide(); }
bool PrinterPanel::receivedShown() const { return !banner->isHidden(); }
