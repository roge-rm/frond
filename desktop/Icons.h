#pragma once
#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QHash>
#include <QList>
#include <QString>

// The bar's icons: Material path data (24 x 24 grid) drawn in the palette's text colours.
namespace frondIcons {

// Reads M, L, H, V, C, S and Z, upper and lower case, with repeated coordinates after a letter.
inline QPainterPath parsePath(const QString& d) {
    QPainterPath path;
    QList<double> n;
    QPointF cur, start, lastControl;
    char command = 0;
    int i = 0;
    auto number = [&](double* out) {
        while (i < d.size() && (d[i] == ' ' || d[i] == ',')) ++i;
        int j = i;
        if (j < d.size() && (d[j] == '-' || d[j] == '+')) ++j;
        bool dot = false;
        while (j < d.size() && (d[j].isDigit() || (d[j] == '.' && !dot))) { dot = dot || d[j] == '.'; ++j; }
        if (j == i) return false;
        *out = d.mid(i, j - i).toDouble();
        i = j;
        return true;
    };
    auto peekNumber = [&] {
        while (i < d.size() && (d[i] == ' ' || d[i] == ',')) ++i;
        return i < d.size() && (d[i].isDigit() || d[i] == '-' || d[i] == '.' || d[i] == '+');
    };
    while (i < d.size()) {
        while (i < d.size() && (d[i] == ' ' || d[i] == ',')) ++i;
        if (i >= d.size()) break;
        if (d[i].isLetter()) command = d[i++].toLatin1();
        const bool rel = command >= 'a';
        const char c = char(rel ? command - 32 : command);
        if (c == 'Z') { path.closeSubpath(); cur = start; continue; }
        do {
            double v[6] = {0, 0, 0, 0, 0, 0};
            const int count = c == 'H' || c == 'V' ? 1 : c == 'C' ? 6 : c == 'S' ? 4 : 2;
            for (int k = 0; k < count; ++k) if (!number(&v[k])) return path;
            const QPointF base = rel ? cur : QPointF();
            if (c == 'M') {
                cur = base + QPointF(v[0], v[1]); start = cur; path.moveTo(cur);
                command = rel ? 'l' : 'L';  // further pairs are lines
                lastControl = cur;
            } else if (c == 'L') { cur = base + QPointF(v[0], v[1]); path.lineTo(cur); lastControl = cur; }
            else if (c == 'H') { cur.setX(rel ? cur.x() + v[0] : v[0]); path.lineTo(cur); lastControl = cur; }
            else if (c == 'V') { cur.setY(rel ? cur.y() + v[0] : v[0]); path.lineTo(cur); lastControl = cur; }
            else if (c == 'C') {
                const QPointF a = base + QPointF(v[0], v[1]), b = base + QPointF(v[2], v[3]), e = base + QPointF(v[4], v[5]);
                path.cubicTo(a, b, e); lastControl = b; cur = e;
            } else {
                const QPointF a = cur * 2 - lastControl, b = base + QPointF(v[0], v[1]), e = base + QPointF(v[2], v[3]);
                path.cubicTo(a, b, e); lastControl = b; cur = e;
            }
        } while (peekNumber());
    }
    return path;
}

inline QString data(const QString& name) {
    static const QHash<QString, QString> paths = {
        {"back", "M20,11H7.83l5.59,-5.59L12,4l-8,8 8,8 1.41,-1.41L7.83,13H20v-2z"},
        {"apps", "M4,8h4V4H4v4zM10,20h4v-4h-4v4zM4,20h4v-4H4v4zM4,14h4v-4H4v4zM10,14h4v-4h-4v4zM16,4v4h4V4h-4zM10,8h4V4h-4v4zM16,14h4v-4h-4v4zM16,20h4v-4h-4v4z"},
        {"card-none", "M18,2h-8L4,8v12c0,1.1 0.9,2 2,2h12c1.1,0 2,-0.9 2,-2V4c0,-1.1 -0.9,-2 -2,-2zM18,20H6V8.83L10.83,4H18V20z|M3.5,2.1L2.1,3.5L20.5,21.9L21.9,20.5z"},
        {"card-out", "M18,2h-8L4,8v12c0,1.1 0.9,2 2,2h12c1.1,0 2,-0.9 2,-2V4c0,-1.1 -0.9,-2 -2,-2zM18,20H6V8.83L10.83,4H18V20z"},
        {"card-in", "M18,2h-8L4.02,8L4,20c0,1.1 0.9,2 2,2h12c1.1,0 2,-0.9 2,-2V4c0,-1.1 -0.9,-2 -2,-2zM12,8h-2V4h2v4zM15,8h-2V4h2v4zM18,8h-2V4h2v4z"},
        {"card-ejected", "M5,17h14v2H5zM12,5L5.33,15h13.34z"},
        {"sound-on", "M3,9v6h4l5,5V4L7,9H3zM16.5,12c0,-1.77 -1.02,-3.29 -2.5,-4.03v8.05c1.48,-0.73 2.5,-2.25 2.5,-4.02zM14,3.23v2.06c2.89,0.86 5,3.54 5,6.71s-2.11,5.85 -5,6.71v2.06c4.01,-0.91 7,-4.49 7,-8.77s-2.99,-7.86 -7,-8.77z"},
        {"sound-off", "M16.5,12c0,-1.77 -1.02,-3.29 -2.5,-4.03v2.21l2.45,2.45c0.03,-0.2 0.05,-0.41 0.05,-0.63zM19,12c0,0.94 -0.2,1.82 -0.54,2.64l1.51,1.51C20.63,14.91 21,13.5 21,12c0,-4.28 -2.99,-7.86 -7,-8.77v2.06c2.89,0.86 5,3.54 5,6.71zM4.27,3L3,4.27 7.73,9H3v6h4l5,5v-6.73l4.25,4.25c-0.67,0.52 -1.42,0.93 -2.25,1.18v2.06c1.38,-0.31 2.63,-0.95 3.69,-1.81L19.73,21 21,19.73l-9,-9L4.27,3zM12,4L9.91,6.09 12,8.18V4z"},
        {"power", "M13,3h-2v10h2V3zM17.83,5.17l-1.42,1.42C17.99,7.86 19,9.81 19,12c0,3.87 -3.13,7 -7,7s-7,-3.13 -7,-7c0,-2.19 1.01,-4.14 2.58,-5.42L6.17,5.17C4.23,6.82 3,9.26 3,12c0,4.97 4.03,9 9,9s9,-4.03 9,-9c0,-2.74 -1.23,-5.18 -3.17,-6.83z"},
        {"more", "M12,8c1.1,0 2,-0.9 2,-2s-0.9,-2 -2,-2 -2,0.9 -2,2 0.9,2 2,2zM12,10c-1.1,0 -2,0.9 -2,2s0.9,2 2,2 2,-0.9 2,-2 -0.9,-2 -2,-2zM12,16c-1.1,0 -2,0.9 -2,2s0.9,2 2,2 2,-0.9 2,-2 -0.9,-2 -2,-2z"},
        {"keyboard", "M20,5L4,5c-1.1,0 -1.99,0.9 -1.99,2L2,17c0,1.1 0.9,2 2,2h16c1.1,0 2,-0.9 2,-2L22,7c0,-1.1 -0.9,-2 -2,-2zM11,8h2v2h-2L11,8zM11,11h2v2h-2v-2zM8,8h2v2L8,10L8,8zM8,11h2v2L8,13v-2zM7,13L5,13v-2h2v2zM7,10L5,10L5,8h2v2zM16,17L8,17v-2h8v2zM16,13h-2v-2h2v2zM16,10h-2L14,8h2v2zM19,13h-2v-2h2v2zM19,10h-2L17,8h2v2z"},
        // Two tapered arrows chasing round a circle, after the Palm HotSync logo.
        {"hotsync", "M5.5,15.75L4.99,14.85 4.59,13.88 4.31,12.85 4.17,11.79 4.18,10.7 4.33,9.61 4.63,8.53 5.08,7.49 5.68,6.51 6.42,5.6 7.28,4.8 8.26,4.11 9.35,3.55 10.51,3.15 11.74,2.9 13,2.82 14.28,2.92 15.56,3.19 16.35,1.24 19.04,6.86 13.2,9.03 13.99,7.09 13.32,6.73 12.6,6.47 11.83,6.31 11.03,6.26 10.22,6.33 9.4,6.51 8.61,6.82 7.85,7.24 7.15,7.78 6.52,8.43 5.98,9.16 5.54,9.98 5.22,10.87 5.03,11.81 4.98,12.78 5.06,13.76 5.29,14.72 5.68,15.65ZM18.5,8.25L19.01,9.15 19.41,10.12 19.69,11.15 19.83,12.21 19.82,13.3 19.67,14.39 19.37,15.47 18.92,16.51 18.32,17.49 17.58,18.4 16.72,19.2 15.74,19.89 14.65,20.45 13.49,20.85 12.26,21.1 11,21.18 9.72,21.08 8.44,20.81 7.65,22.76 4.96,17.14 10.8,14.97 10.01,16.91 10.68,17.27 11.4,17.53 12.17,17.69 12.97,17.74 13.78,17.67 14.6,17.49 15.39,17.18 16.15,16.76 16.85,16.22 17.48,15.57 18.02,14.84 18.46,14.02 18.78,13.13 18.97,12.19 19.02,11.22 18.94,10.24 18.71,9.28 18.32,8.35Z"},
        // A disc with two circling arrows cut out, after the ActiveSync logo (even-odd fill).
        {"activesync", "M23.5,12L23.36,13.8 22.94,15.55 22.25,17.22 21.3,18.76 20.13,20.13 18.76,21.3 17.22,22.25 15.55,22.94 13.8,23.36 12,23.5 10.2,23.36 8.45,22.94 6.78,22.25 5.24,21.3 3.87,20.13 2.7,18.76 1.75,17.22 1.06,15.55 0.64,13.8 0.5,12 0.64,10.2 1.06,8.45 1.75,6.78 2.7,5.24 3.87,3.87 5.24,2.7 6.78,1.75 8.45,1.06 10.2,0.64 12,0.5 13.8,0.64 15.55,1.06 17.22,1.75 18.76,2.7 20.13,3.87 21.3,5.24 22.25,6.78 22.94,8.45 23.36,10.2ZM5.33,14.43L5.06,13.5 4.92,12.55 4.91,11.59 5.03,10.63 5.28,9.7 5.66,8.81 6.14,7.98 6.74,7.23 7.44,6.56 8.21,5.99 9.06,5.54 9.96,5.2 10.9,4.99 11.86,4.9 12.82,4.95 13.77,5.12 14.69,5.43 15.55,5.85 16.5,4.21 17.87,8.23 13.7,9.06 14.65,7.41 14,7.09 13.32,6.87 12.62,6.74 11.9,6.7 11.18,6.76 10.48,6.92 9.81,7.18 9.17,7.52 8.59,7.94 8.08,8.44 7.63,9 7.26,9.62 6.99,10.28 6.8,10.98 6.71,11.69 6.72,12.41 6.82,13.12 7.02,13.81ZM18.67,9.57L18.94,10.5 19.08,11.45 19.09,12.41 18.97,13.37 18.72,14.3 18.34,15.19 17.86,16.02 17.26,16.77 16.56,17.44 15.79,18.01 14.94,18.46 14.04,18.8 13.1,19.01 12.14,19.1 11.18,19.05 10.23,18.88 9.31,18.57 8.45,18.15 7.5,19.79 6.13,15.77 10.3,14.94 9.35,16.59 10,16.91 10.68,17.13 11.38,17.26 12.1,17.3 12.82,17.24 13.52,17.08 14.19,16.82 14.83,16.48 15.41,16.06 15.92,15.56 16.37,15 16.74,14.38 17.01,13.72 17.2,13.02 17.29,12.31 17.28,11.59 17.18,10.88 16.98,10.19Z"},
        {"launch", "M8,5v14l11,-7z"},
        {"export", "M9,16h6v-6h4l-7,-7 -7,7h4zM5,18h14v2H5z"},
        {"delete", "M6,19c0,1.1 0.9,2 2,2h8c1.1,0 2,-0.9 2,-2V7H6v12zM19,4h-3.5l-1,-1h-5l-1,1H5v2h14V4z"},
        {"close", "M19,6.41L17.59,5 12,10.59 6.41,5 5,6.41 10.59,12 5,17.59 6.41,19 12,13.41 17.59,19 19,17.59 13.41,12z"},
    };
    return paths.value(name);
}

// An icon in the text colour of the current palette, greyed when disabled.
inline QIcon icon(const QString& name, int size = 18) {
    QList<QPainterPath> parts;
    for (const auto& part : data(name).split('|')) parts << parsePath(part);
    QIcon out;
    const qreal ratio = 2;
    const auto palette = QApplication::palette();
    const struct { QIcon::Mode mode; QColor colour; } modes[] = {
        {QIcon::Normal, palette.color(QPalette::Active, QPalette::ButtonText)},
        {QIcon::Disabled, palette.color(QPalette::Disabled, QPalette::ButtonText)}};
    for (const auto& m : modes) {
        QPixmap pixmap(int(size * ratio), int(size * ratio));
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        for (const auto& part : parts) painter.fillPath(part, m.colour);
        painter.end();
        out.addPixmap(pixmap, m.mode);
    }
    return out;
}

}  // namespace frondIcons
