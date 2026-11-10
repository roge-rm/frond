#include "PrinterPanel.h"
#include <algorithm>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

PrinterPanel::PrinterPanel(QWidget* parent, bool bannerOnly) : QWidget(parent) {
    setObjectName("printerPanel");
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout;
    auto* label = new QLabel(tr("USB port"), this);
    portBox = new QComboBox(this);
    portBox->setObjectName("usbPortChoice");
    // The choices change with the device, so the box sizes to them each time and the readout
    // wraps instead of squeezing it.
    portBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    portBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    portBox->addItem(tr("Off"), "off");
    readout = new QLabel(this);
    readout->setObjectName("printerReadout");
    readout->setWordWrap(true);
    row->addWidget(label);
    row->addWidget(portBox);
    row->addWidget(readout, 1);
    column->addLayout(row);
    moduleRow = new QWidget(this);
    moduleRow->setObjectName("wifiModuleRow");
    auto* moduleLayout = new QHBoxLayout(moduleRow);
    moduleLayout->setContentsMargins(0, 0, 0, 0);
    moduleBox = new QComboBox(moduleRow);
    moduleBox->setObjectName("wifiModuleChoice");
    moduleBox->addItem(tr("On"), true);
    moduleBox->addItem(tr("Off"), false);
    moduleReadout = new QLabel(moduleRow);
    moduleReadout->setObjectName("wifiModuleReadout");
    moduleReadout->setWordWrap(true);
    moduleLayout->addWidget(new QLabel(tr("Wi-Fi"), moduleRow));
    moduleLayout->addWidget(moduleBox);
    moduleLayout->addWidget(moduleReadout, 1);
    column->addWidget(moduleRow);
    moduleRow->hide();
    banner = new QWidget(this);
    banner->setObjectName("printerBanner");
    auto* inner = new QVBoxLayout(banner);
    inner->setContentsMargins(0, 0, 0, 0);
    received = new QLabel(banner);
    received->setObjectName("printerReceived");
    received->setWordWrap(true);
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
    // Two rows of two, so the buttons fit the side panel's width.
    auto* buttons = new QGridLayout;
    buttons->addWidget(pdf, 0, 0); buttons->addWidget(print, 0, 1);
    buttons->addWidget(raw, 1, 0); buttons->addWidget(discard, 1, 1);
    inner->addLayout(buttons);
    column->addWidget(banner);
    banner->hide();
    if (bannerOnly) {
        setObjectName("irPrinterPanel");
        for (QWidget* w : {static_cast<QWidget*>(label), static_cast<QWidget*>(portBox), static_cast<QWidget*>(readout)}) w->hide();
        banner->setObjectName("irPrinterBanner");
        received->setObjectName("irPrinterReceived");
        pdf->setObjectName("irPrinterPdf"); print->setObjectName("irPrinterPrint");
        raw->setObjectName("irPrinterRaw"); discard->setObjectName("irPrinterDiscard");
    }
    connect(portBox, &QComboBox::activated, this, [this] { emit portChosen(port()); });
    connect(moduleBox, &QComboBox::activated, this, [this] { emit moduleChosen(moduleOn()); });
    connect(pdf, &QPushButton::clicked, this, &PrinterPanel::pdfRequested);
    connect(print, &QPushButton::clicked, this, &PrinterPanel::printRequested);
    connect(raw, &QPushButton::clicked, this, &PrinterPanel::rawRequested);
    connect(discard, &QPushButton::clicked, this, &PrinterPanel::discardRequested);
}
void PrinterPanel::setSupported(bool printer, bool wifi) {
    const QSignalBlocker blocked(portBox);
    const auto current = port();
    const auto text = readout->text();
    portBox->clear();
    portBox->addItem(tr("Off"), "off");
    if (printer) portBox->addItem(tr("Printer"), "printer");
    if (wifi) portBox->addItem(tr("Wi-Fi adapter"), "wifi");
    portBox->setCurrentIndex(std::max(0, portBox->findData(current)));
    readout->setText(port() == current ? text : QString());
    setVisible(printer || wifi);
}
void PrinterPanel::setModuleSupported(bool supported) {
    moduleRow->setVisible(supported);
    if (!supported) moduleReadout->clear();
    if (supported) setVisible(true);
}
bool PrinterPanel::moduleOn() const { return moduleBox->currentData().toBool(); }
void PrinterPanel::setModuleOn(bool on) {
    const QSignalBlocker blocked(moduleBox);
    moduleBox->setCurrentIndex(moduleBox->findData(on));
}
void PrinterPanel::setModuleReadout(const QString& text) { moduleReadout->setText(text); }
QString PrinterPanel::port() const { return portBox->currentData().toString(); }
// The readout belongs to the port, so it only clears when the port changes.
void PrinterPanel::setPort(const QString& port) {
    const QSignalBlocker blocked(portBox);
    const auto before = this->port();
    portBox->setCurrentIndex(std::max(0, portBox->findData(port)));
    if (this->port() != before) readout->clear();
}
void PrinterPanel::setLinkReadout(const QString& text) { readout->setText(text); }
void PrinterPanel::setReadout(bool attached, qint64 bytes) {
    readout->setText(attached ? tr("%n byte(s) received", nullptr, int(bytes)) : QString());
}
void PrinterPanel::showReceived(qint64 bytes, bool renderable) {
    receivedBytes = bytes;
    received->setText(source.isEmpty() ? tr("Print received (%n byte(s))", nullptr, int(bytes))
                                       : tr("Print received from the %1 (%n byte(s))", nullptr, int(bytes)).arg(source));
    pdf->setEnabled(renderable);
    print->setEnabled(renderable);
    note->setVisible(!renderable);
    banner->show();
}
void PrinterPanel::hideReceived() { banner->hide(); }
bool PrinterPanel::receivedShown() const { return !banner->isHidden(); }
void PrinterPanel::setSource(const QString& name) {
    if (name == source) return;
    source = name;
    if (!banner->isHidden()) showReceived(receivedBytes, pdf->isEnabled());
}
