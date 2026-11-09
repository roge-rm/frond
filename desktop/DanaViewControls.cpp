#include "DanaViewControls.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSignalBlocker>

DanaViewControls::DanaViewControls(QWidget* parent) : QWidget(parent) {
    setObjectName("danaViewControls");
    auto* layout = new QFormLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    keyboard = new QComboBox(this); keyboard->setObjectName("danaKeyboard");
    keyboard->addItem(tr("Automatic"), "auto");
    keyboard->addItem(tr("Compact"), "compact");
    keyboard->addItem(tr("Full Dana keyboard"), "full");
    keyboard->addItem(tr("Hidden"), "hidden");
    zoom = new QComboBox(this); zoom->setObjectName("danaZoom");
    zoom->addItem(tr("Fit"), "fit");
    for (const auto& value : {"1", "1.5", "2", "3"}) zoom->addItem(QString::fromLatin1(value) + QChar(0x00d7), value);
    pointer = new QComboBox(this); pointer->setObjectName("danaPointer");
    pointer->addItem(tr("Touch screen"), "touch"); pointer->addItem(tr("Pan screen"), "pan");
    panX = new QDoubleSpinBox(this); panX->setObjectName("danaPanX");
    panY = new QDoubleSpinBox(this); panY->setObjectName("danaPanY");
    for (auto* spin : {panX, panY}) { spin->setDecimals(3); spin->setKeyboardTracking(false); }
    panX->setRange(0, 560); panY->setRange(0, 160);
    layout->addRow(tr("Onscreen keyboard"), keyboard);
    layout->addRow(tr("Screen zoom"), zoom);
    layout->addRow(tr("Pointer"), pointer);
    layout->addRow(tr("Horizontal pan (pixels)"), panX);
    layout->addRow(tr("Vertical pan (pixels)"), panY);
    connect(keyboard, &QComboBox::currentIndexChanged, this, [this] { current.keyboard = keyboard->currentData().toString(); emit changed(); });
    connect(zoom, &QComboBox::currentIndexChanged, this, [this] { current.zoom = zoom->currentData().toString(); emit changed(); });
    connect(pointer, &QComboBox::currentIndexChanged, this, [this] { current.pointer = pointer->currentData().toString(); emit changed(); });
    connect(panX, &QDoubleSpinBox::valueChanged, this, [this](double value) { current.panX = value; emit changed(); });
    connect(panY, &QDoubleSpinBox::valueChanged, this, [this](double value) { current.panY = value; emit changed(); });
    setValues({});
}
frondDana::Preferences DanaViewControls::values() const {
    return current;
}
void DanaViewControls::setValues(const frondDana::Preferences& prefs) {
    current = prefs;
    const QSignalBlocker a(keyboard), b(zoom), c(pointer), d(panX), e(panY);
    keyboard->setCurrentIndex(keyboard->findData(prefs.keyboard)); zoom->setCurrentIndex(zoom->findData(prefs.zoom));
    pointer->setCurrentIndex(pointer->findData(prefs.pointer)); panX->setValue(prefs.panX); panY->setValue(prefs.panY);
}
