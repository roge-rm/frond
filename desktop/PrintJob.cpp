#include "PrintJob.h"
#include <algorithm>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QObject>
#include <QPrinter>
#include <QRegularExpression>

namespace frondPrint {

Status parseStatus(const QString& message) {
    static const QRegularExpression pattern(R"((attached|off) (\S+) bytes=(\d+)(?: receiving=(\d+))? last=(\S+) idle=(-?\d+)\s*$)");
    Status s;
    const auto m = pattern.match(message);
    if (!m.hasMatch()) return s;
    s.valid = true;
    s.attached = m.captured(1) == "attached";
    s.state = m.captured(2);
    s.bytes = m.captured(3).toLongLong();
    s.receiving = m.captured(4).toLongLong();
    s.last = m.captured(5);
    s.idleMs = m.captured(6).toLongLong();
    s.error = message.left(m.capturedStart()).trimmed();
    return s;
}

UsbPort parseUsbPort(const QString& message) {
    UsbPort s;
    const auto words = message.split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) return s;
    s.port = words[0];
    if (s.port != "off" && s.port != "printer" && s.port != "wifi") return s;
    s.valid = true;
    if (s.port != "wifi") return s;
    s.powered = words.size() > 1 && words[1] == "powered";
    for (const auto& word : words) {
        if (word == "link=up") s.linkUp = true;
        else if (word.startsWith("ip=")) s.ip = word.mid(3);
    }
    return s;
}

QString wifiReadout(const UsbPort& state) {
    if (!state.valid || state.port != "wifi") return {};
    if (state.linkUp) return QObject::tr("Connected") + (state.ip.isEmpty() ? QString() : ", " + state.ip);
    return state.powered ? QObject::tr("Not connected") : QObject::tr("Waiting for the Dana");
}

QString moduleReadout(const QString& state, bool radioOn) {
    if (!radioOn) return QObject::tr("Off");
    if (state.trimmed() == "off") return QObject::tr("Waiting for the Dana");
    return wifiReadout(parseUsbPort("wifi " + state));
}

int drawPages(QPrinter* printer, const frond::PrintText& text) {
    QPainter painter;
    if (!painter.begin(printer)) return 0;
    const auto area = printer->pageLayout().paintRectPixels(printer->resolution());
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(100);
    const QFontMetricsF probe(font, printer);
    const double columnsWidth = probe.horizontalAdvance(QString(frond::kPrintColumns, QLatin1Char('0')));
    const double linesHeight = probe.lineSpacing() * frond::kPrintLinesPerPage;
    const double scale = std::min(area.width() / std::max(1.0, columnsWidth), area.height() / std::max(1.0, linesHeight));
    font.setPixelSize(std::max(1, int(100 * scale)));
    painter.setFont(font);
    const QFontMetricsF metrics(font, printer);
    int drawn = 0;
    for (const auto& page : text.pages) {
        if (drawn > 0 && !printer->newPage()) break;
        double y = area.top() + metrics.ascent();
        for (const auto& line : page) {
            painter.drawText(QPointF(area.left(), y), QString::fromUtf8(line.data(), int(line.size())));
            y += metrics.lineSpacing();
        }
        drawn++;
    }
    painter.end();
    return drawn;
}

}  // namespace frondPrint
