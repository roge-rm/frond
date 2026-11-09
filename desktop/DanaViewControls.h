#pragma once
#include <QWidget>
#include "DanaView.h"
class QComboBox;
class QDoubleSpinBox;
class DanaViewControls : public QWidget {
    Q_OBJECT
 public:
    explicit DanaViewControls(QWidget* parent = nullptr);
    frondDana::Preferences values() const;
    void setValues(const frondDana::Preferences& prefs);
 signals:
    void changed();
 private:
    QComboBox *keyboard, *zoom, *pointer;
    QDoubleSpinBox *panX, *panY;
    frondDana::Preferences current;
};
