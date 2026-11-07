// File > Connections…: this Frond's name, the paired connections with their status, the
// installs nearby to pair with, and the pair dialogs on both sides.
#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
class LinkClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// A side: types the code the other Frond shows.
class PairCodeDialog : public QDialog {
    Q_OBJECT
 public:
    PairCodeDialog(LinkClient* client, const QString& name, QWidget* parent);
    void message(const QJsonObject& m);
    QLabel *prompt, *problem;
    QLineEdit* code;
    QPushButton* pair;
 private:
    LinkClient* client;
    QString name;
};

class ConnectionsWindow : public QDialog {
    Q_OBJECT
 public:
    ConnectionsWindow(LinkClient* client, QWidget* parent = nullptr);
    // Status line of a connection as the list shows it.
    static QString status(const QJsonObject& connection);
    QListWidget *list, *nearbyList;
    QLabel *header, *nearbyEmpty;
    QPushButton *rename, *pairButton, *addAddress;
    QPointer<PairCodeDialog> codeDialog;
    QPointer<QDialog> requestDialog;
 protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
 private:
    void message(const QJsonObject& m);
    void showState(const QJsonObject& s);
    void rowMenu(const QPoint& at);
    void startPair(const QJsonObject& op, const QString& name);
    LinkClient* client;
    QJsonObject state;
    QLabel* requestLabel{nullptr};
};
