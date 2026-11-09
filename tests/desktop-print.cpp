// The desktop USB printer row: status parsing, the row by capability, and the PDF layout.
#include <QtTest>
#include <QPrinter>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QTemporaryDir>
#include "FirmwareInput.h"
#include "PrintJob.h"
#include "PrinterPanel.h"

class DesktopPrint : public QObject {
    Q_OBJECT
 private slots:
    void statusParsing() {
        auto s = frondPrint::parseStatus("attached collecting bytes=289 last=none idle=3500");
        QVERIFY(s.valid && s.attached);
        QCOMPARE(s.state, QString("collecting"));
        QCOMPARE(s.bytes, qint64(289));
        QCOMPARE(s.last, QString("none"));
        QCOMPARE(s.idleMs, qint64(3500));
        QVERIFY(s.error.isEmpty());
        QVERIFY(frondPrint::received(s));
        s = frondPrint::parseStatus("attached collecting bytes=289 last=none idle=2999");
        QVERIFY(!frondPrint::received(s));
        s = frondPrint::parseStatus("attached collecting bytes=0 last=published idle=-1");
        QCOMPARE(s.idleMs, qint64(-1));
        QVERIFY(!frondPrint::received(s));
        s = frondPrint::parseStatus("off idle bytes=0 last=none idle=-1");
        QVERIFY(s.valid && !s.attached);
        QVERIFY(!frondPrint::received(s));
        // A failed save puts the error text in front of the status.
        s = frondPrint::parseStatus("Could not write the file. attached collecting bytes=0 last=rejected idle=-1");
        QVERIFY(s.valid);
        QCOMPARE(s.error, QString("Could not write the file."));
        QCOMPARE(s.last, QString("rejected"));
        QVERIFY(!frondPrint::parseStatus("Use attach on|off.").valid);
        QVERIFY(!frondPrint::parseStatus("").valid);
    }
    void usbPortParsing() {
        auto s = frondPrint::parseUsbPort("wifi powered link=up ip=10.0.2.15 mac=02:00:00:00:00:01 tx=3 rx=4 dropped=0");
        QVERIFY(s.valid && s.powered && s.linkUp);
        QCOMPARE(s.ip, QString("10.0.2.15"));
        QCOMPARE(frondPrint::wifiReadout(s), QString("Connected, 10.0.2.15"));
        s = frondPrint::parseUsbPort("wifi powered link=down ip= mac=02:00:00:00:00:01 tx=0 rx=0 dropped=0");
        QVERIFY(s.valid && !s.linkUp);
        QCOMPARE(frondPrint::wifiReadout(s), QString("Not connected"));
        s = frondPrint::parseUsbPort("wifi unpowered link=down ip= mac=02:00:00:00:00:01 tx=0 rx=0 dropped=0");
        QCOMPARE(frondPrint::wifiReadout(s), QString("Waiting for the Dana"));
        s = frondPrint::parseUsbPort("printer attached collecting bytes=0 last=none idle=-1");
        QVERIFY(s.valid);
        QCOMPARE(s.port, QString("printer"));
        QVERIFY(frondPrint::wifiReadout(s).isEmpty());
        QVERIFY(frondPrint::parseUsbPort("off").valid);
        QVERIFY(!frondPrint::parseUsbPort("").valid);
        QVERIFY(!frondPrint::parseUsbPort("Use usb-port off|printer|wifi.").valid);
    }
    void rowByCapability() {
        QWidget parent;
        PrinterPanel panel(&parent);
        parent.show();
        int shown = 0;
        for (const auto& p : deviceProfiles5) {
            const auto caps = frondFirmware::capabilities(static_cast<frondFirmware::Device>(p.deviceType));
            panel.setSupported(caps.usbPrinter, caps.usbWifi);
            QCOMPARE(panel.isVisible(), caps.usbPrinter || caps.usbWifi);
            QCOMPARE(panel.isVisible(), p.deviceType == deviceTypeDana);
            shown += panel.isVisible();
        }
        QCOMPARE(shown, 1);
        // The Dana lists Off, Printer and Wi-Fi adapter; a device with only a printer has no Wi-Fi choice.
        auto* box = panel.findChild<QComboBox*>("usbPortChoice");
        panel.setSupported(true, true);
        QStringList choices;
        for (int i = 0; i < box->count(); i++) choices << box->itemText(i);
        QCOMPARE(choices, (QStringList{"Off", "Printer", "Wi-Fi adapter"}));
        panel.setSupported(true, false);
        QCOMPARE(box->count(), 2);
    }
    void portChoice() {
        PrinterPanel panel;
        panel.setSupported(true, true);
        panel.show();
        QSignalSpy chosen(&panel, &PrinterPanel::portChosen);
        QCOMPARE(panel.port(), QString("off"));
        panel.setPort("wifi");
        QCOMPARE(chosen.count(), 0);
        QCOMPARE(panel.port(), QString("wifi"));
        QVERIFY(!panel.plugged());
        panel.setLinkReadout("Connected, 10.0.2.15");
        QCOMPARE(panel.findChild<QLabel*>("printerReadout")->text(), QString("Connected, 10.0.2.15"));
        // Refreshing the choices or setting the same port keeps the readout.
        panel.setSupported(true, true);
        panel.setPort("wifi");
        QCOMPARE(panel.findChild<QLabel*>("printerReadout")->text(), QString("Connected, 10.0.2.15"));
        // Printer keeps the Print received row working.
        panel.setPort("printer");
        QVERIFY(panel.plugged());
        QVERIFY(panel.findChild<QLabel*>("printerReadout")->text().isEmpty());
        panel.setReadout(true, 12);
        QVERIFY(panel.findChild<QLabel*>("printerReadout")->text().contains("12"));
        panel.showReceived(12, true);
        QVERIFY(panel.receivedShown());
        // A choice by the user emits the port.
        auto* box = panel.findChild<QComboBox*>("usbPortChoice");
        box->setCurrentIndex(box->findData("wifi"));
        emit box->activated(box->currentIndex());
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.at(0).at(0).toString(), QString("wifi"));
        // A port the device lacks falls back to Off.
        panel.setSupported(true, false);
        QCOMPARE(panel.port(), QString("off"));
    }
    void panelChoices() {
        PrinterPanel panel;
        panel.setSupported(true, true);
        panel.show();
        QSignalSpy chosen(&panel, &PrinterPanel::portChosen);
        panel.setPort("printer");
        QCOMPARE(chosen.count(), 0);
        QVERIFY(!panel.receivedShown());
        panel.showReceived(289, true);
        QVERIFY(panel.receivedShown());
        QVERIFY(panel.findChild<QPushButton*>("printerPdf")->isEnabled());
        QVERIFY(panel.findChild<QLabel*>("printerNote")->isHidden());
        panel.showReceived(5, false);
        QVERIFY(!panel.findChild<QPushButton*>("printerPdf")->isEnabled());
        QVERIFY(!panel.findChild<QPushButton*>("printerPrint")->isEnabled());
        QVERIFY(panel.findChild<QPushButton*>("printerRaw")->isEnabled());
        QVERIFY(panel.findChild<QLabel*>("printerNote")->text().contains("Only the Generic printer profile can be shown as pages"));
        panel.hideReceived();
        QVERIFY(!panel.receivedShown());
    }
    void pdfPages() {
        // 70 lines, a form feed, one more line: pages of 66 and 4 lines, then 1.
        std::string job;
        for (int i = 0; i < 70; i++) job += "line " + std::to_string(i) + "\r\n";
        job += "\f";
        job += "last\r\n";
        const auto text = frond::renderPrintText(reinterpret_cast<const uint8_t*>(job.data()), job.size());
        QVERIFY(text.renderable);
        QCOMPARE(int(text.pages.size()), 3);
        QTemporaryDir dir;
        const auto path = dir.filePath("job.pdf");
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(path);
        QCOMPARE(frondPrint::drawPages(&printer, text), 3);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
        QCOMPARE(bytes.count("/Type /Page\n"), 3);
    }
};
QTEST_MAIN(DesktopPrint)
#include "desktop-print.moc"
