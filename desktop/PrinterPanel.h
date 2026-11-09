#pragma once
// The "USB printer" row of the Connections panel: the plugged-in switch, a readout, and the
// "Print received" choices.
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

class PrinterPanel : public QWidget {
    Q_OBJECT
 public:
    explicit PrinterPanel(QWidget* parent = nullptr);
    // Only devices with a USB printer show the row.
    void setSupported(bool supported) { setVisible(supported); }
    bool plugged() const;
    void setPlugged(bool plugged);  // does not emit pluggedToggled
    // bytes: everything received so far, whether held or still in the core.
    void setReadout(bool attached, qint64 bytes);
    // Shows the choices for a received job; renderable greys Save as PDF and Print when false.
    void showReceived(qint64 bytes, bool renderable);
    void hideReceived();
    bool receivedShown() const;
 signals:
    void pluggedToggled(bool plugged);
    void pdfRequested();
    void printRequested();
    void rawRequested();
    void discardRequested();
 private:
    QCheckBox* switchBox;
    QLabel *readout, *received, *note;
    QWidget* banner;
    QPushButton *pdf, *print, *raw, *discard;
};
