// The desktop USB printer row: status parsing, the row by capability, and the PDF layout.
#include <QtTest>
#include <QPrinter>
#include <QPushButton>
#include <QLabel>
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
    void rowByCapability() {
        QWidget parent;
        PrinterPanel panel(&parent);
        parent.show();
        int shown = 0;
        for (const auto& p : deviceProfiles5) {
            const auto caps = frondFirmware::capabilities(static_cast<frondFirmware::Device>(p.deviceType));
            panel.setSupported(caps.usbPrinter);
            QCOMPARE(panel.isVisible(), caps.usbPrinter);
            QCOMPARE(panel.isVisible(), p.deviceType == deviceTypeDana);
            shown += panel.isVisible();
        }
        QCOMPARE(shown, 1);
    }
    void panelChoices() {
        PrinterPanel panel;
        panel.setSupported(true);
        panel.show();
        QSignalSpy plugged(&panel, &PrinterPanel::pluggedToggled);
        panel.setPlugged(true);
        QCOMPARE(plugged.count(), 0);
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
