#pragma once
// The "USB port" row of the Connections panel: Off, Printer or Wi-Fi adapter, a readout, and the
// "Print received" choices. The Dana Wireless also gets a Wi-Fi row for its built-in module.
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

class PrinterPanel : public QWidget {
    Q_OBJECT
 public:
    explicit PrinterPanel(QWidget* parent = nullptr);
    // The row shows on a device with a USB printer or a Wi-Fi adapter, and lists what it has.
    void setSupported(bool printer, bool wifi);
    // The built-in Wi-Fi module's row (On or Off and a readout), for the Dana Wireless.
    void setModuleSupported(bool supported);
    bool moduleOn() const;
    void setModuleOn(bool on);  // does not emit moduleChosen
    void setModuleReadout(const QString& text);
    QString port() const;  // off, printer or wifi
    bool plugged() const { return port() == "printer"; }
    void setPort(const QString& port);  // does not emit portChosen
    // bytes: everything received so far, whether held or still in the core.
    void setReadout(bool attached, qint64 bytes);
    // The Wi-Fi adapter's link state.
    void setLinkReadout(const QString& text);
    // Shows the choices for a received job; renderable greys Save as PDF and Print when false.
    void showReceived(qint64 bytes, bool renderable);
    void hideReceived();
    bool receivedShown() const;
 signals:
    void portChosen(const QString& port);
    void moduleChosen(bool on);
    void pdfRequested();
    void printRequested();
    void rawRequested();
    void discardRequested();
 private:
    QComboBox *portBox, *moduleBox;
    QWidget* moduleRow;
    QLabel *readout, *moduleReadout, *received, *note;
    QWidget* banner;
    QPushButton *pdf, *print, *raw, *discard;
};
