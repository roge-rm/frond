#pragma once
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cmath>

namespace frondDana {
struct Preferences {
    QString keyboard{"hidden"}, zoom{"2"}, pointer{"touch"};
    double panX{0}, panY{0};
    bool operator==(const Preferences& b) const {
        return keyboard == b.keyboard && zoom == b.zoom && pointer == b.pointer && panX == b.panX && panY == b.panY;
    }
    bool operator!=(const Preferences& b) const { return !(*this == b); }
};
inline bool keyboardValid(const QString& value) { return QStringList{"auto", "compact", "full", "hidden"}.contains(value); }
inline bool zoomValid(const QString& value) { return QStringList{"fit", "1", "1.5", "2", "3"}.contains(value); }
inline bool panValid(double value, double maximum) { return std::isfinite(value) && value >= 0 && value <= maximum; }
inline bool valid(const Preferences& p) {
    return keyboardValid(p.keyboard) && zoomValid(p.zoom) && (p.pointer == "touch" || p.pointer == "pan") && panValid(p.panX, 560) && panValid(p.panY, 160);
}
inline QString argument(const Preferences& p, double density = 1) {
    return QStringLiteral("keyboard=%1 zoom=%2 pan-x=%3 pan-y=%4 pointer=%5 density=%6 physical=on")
        .arg(p.keyboard, p.zoom, QString::number(p.panX, 'g', 15), QString::number(p.panY, 'g', 15), p.pointer, QString::number(density, 'g', 15));
}
inline QJsonObject json(const Preferences& p) {
    return {{"keyboard", p.keyboard}, {"zoom", p.zoom}, {"panX", p.panX}, {"panY", p.panY}, {"pointer", p.pointer}};
}
inline bool fromJson(const QJsonObject& object, Preferences* result) {
    if (!object.value("keyboard").isString() || !object.value("zoom").isString() || !object.value("pointer").isString() ||
        !object.value("panX").isDouble() || !object.value("panY").isDouble()) return false;
    Preferences p{object.value("keyboard").toString(), object.value("zoom").toString(), object.value("pointer").toString(),
                  object.value("panX").toDouble(), object.value("panY").toDouble()};
    if (!valid(p)) return false;
    *result = p;
    return true;
}
// Parse the complete native state before publishing any of its user choices.
inline bool parseState(QString text, Preferences* result, double* density = nullptr) {
    if (!text.startsWith("dana-view:")) return false;
    text = text.mid(10).trimmed();
    QJsonObject object;
    double hostDensity = 1;
    QSet<QString> seen;
    for (const auto& field : text.split(' ', Qt::SkipEmptyParts)) {
        const auto pair = field.split('=');
        if (pair.size() != 2 || seen.contains(pair[0])) return false;
        const auto key = pair[0], value = pair[1];
        seen.insert(key);
        if (key == "keyboard" || key == "zoom" || key == "pointer") object.insert(key, value);
        else if (key == "pan-x" || key == "pan-y" || key == "density" || key == "output-scale-x" || key == "output-scale-y") {
            bool ok = false;
            const double number = value.toDouble(&ok);
            if (!ok || !std::isfinite(number)) return false;
            if (key == "density" || key.startsWith("output-scale-")) {
                if (number <= 0 || number > 16) return false;
                if (key == "density") hostDensity = number;
            }
            else object.insert(key == "pan-x" ? "panX" : "panY", number);
        } else if (key == "physical") { if (value != "on" && value != "off") return false; }
        else if (key == "page") { if (!QStringList{"abc", "symbols", "more", "fn"}.contains(value)) return false; }
        else if (key == "resolved") { if (value == "auto" || !keyboardValid(value)) return false; }
        else return false;
    }
    seen.remove("page");
    seen.remove("output-scale-x"); seen.remove("output-scale-y");
    if (seen != QSet<QString>{"keyboard", "zoom", "pan-x", "pan-y", "pointer", "density", "physical", "resolved"}) return false;
    if (!fromJson(object, result)) return false;
    if (density) *density = hostDensity;
    return true;
}
}
