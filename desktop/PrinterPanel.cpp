#include "PrinterPanel.h"
#include <algorithm>
#include <QComboBox>
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
    auto* label = new QLabel(tr("USB port"), this);
    portBox = new QComboBox(this);
    portBox->setObjectName("usbPortChoice");
    portBox->addItem(tr("Off"), "off");
    readout = new QLabel(this);
    readout->setObjectName("printerReadout");
    row->addWidget(label);
    row->addWidget(portBox);
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
    connect(portBox, &QComboBox::activated, this, [this] { emit portChosen(port()); });
    connect(pdf, &QPushButton::clicked, this, &PrinterPanel::pdfRequested);
    connect(print, &QPushButton::clicked, this, &PrinterPanel::printRequested);
    connect(raw, &QPushButton::clicked, this, &PrinterPanel::rawRequested);
    connect(discard, &QPushButton::clicked, this, &PrinterPanel::discardRequested);
}
void PrinterPanel::setSupported(bool printer, bool wifi) {
    const QSignalBlocker blocked(portBox);
    const auto current = port();
    portBox->clear();
    portBox->addItem(tr("Off"), "off");
    if (printer) portBox->addItem(tr("Printer"), "printer");
    if (wifi) portBox->addItem(tr("Wi-Fi adapter"), "wifi");
    setPort(current);
    setVisible(printer || wifi);
}
QString PrinterPanel::port() const { return portBox->currentData().toString(); }
void PrinterPanel::setPort(const QString& port) {
    const QSignalBlocker blocked(portBox);
    portBox->setCurrentIndex(std::max(0, portBox->findData(port)));
    readout->clear();
}
void PrinterPanel::setLinkReadout(const QString& text) { readout->setText(text); }
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
