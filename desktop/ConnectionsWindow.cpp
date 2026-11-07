#include "ConnectionsWindow.h"
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>
#include "LinkClient.h"

namespace {
QIcon marker(bool lit, const QPalette& palette) {
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(lit ? QColor(52, 168, 83) : palette.color(QPalette::Mid));
    painter.drawEllipse(1, 1, 10, 10);
    return QIcon(pixmap);
}
QString resultText(const QString& result, const QString& name) {
    if (result == "wrongcode") return QObject::tr("Wrong code");
    if (result == "lockedout") return QObject::tr("Too many wrong codes. Try again in a minute.");
    if (result == "expired") return QObject::tr("The code ran out. Pair again.");
    if (result == "notpairing") return QObject::tr("Open Connections on %1 first.").arg(name);
    if (result == "unreachable") return QObject::tr("Couldn't reach %1.").arg(name);
    if (result == "version") return QObject::tr("%1 runs a Frond that can't pair with this one.").arg(name);
    return QObject::tr("Pairing didn't finish.");
}
}  // namespace

PairCodeDialog::PairCodeDialog(LinkClient* link, const QString& peer, QWidget* parent)
    : QDialog(parent), client(link), name(peer) {
    setWindowTitle(tr("Pair"));
    setObjectName("pairCodeDialog");
    auto* layout = new QVBoxLayout(this);
    prompt = new QLabel(tr("Connecting to %1…").arg(name), this);
    prompt->setObjectName("pairPrompt");
    code = new QLineEdit(this);
    code->setObjectName("pairCode");
    code->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9 ]{0,7}"), code));
    code->setPlaceholderText("000 000");
    problem = new QLabel(this);
    problem->setObjectName("pairProblem");
    problem->hide();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    pair = buttons->addButton(tr("Pair"), QDialogButtonBox::AcceptRole);
    pair->setObjectName("pairConfirm");
    pair->setEnabled(false);
    layout->addWidget(prompt);
    layout->addWidget(code);
    layout->addWidget(problem);
    layout->addWidget(buttons);
    connect(code, &QLineEdit::textChanged, this, [this](const QString& text) {
        pair->setEnabled(QString(text).remove(' ').size() == 6);
    });
    connect(pair, &QPushButton::clicked, this, [this] {
        problem->hide();
        pair->setEnabled(false);
        client->send({{"op", "pair-code"}, {"code", code->text().remove(' ')}});
    });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        client->send({{"op", "pair-cancel"}});
        reject();
    });
    connect(client, &LinkClient::message, this, &PairCodeDialog::message);
}

void PairCodeDialog::message(const QJsonObject& m) {
    const auto type = m.value("type").toString();
    if (type == "pair-peer") {
        name = m.value("name").toString(name);
        prompt->setText(tr("Enter the code shown on %1").arg(name));
        code->setFocus();
    } else if (type == "pair-result") {
        const auto result = m.value("result").toString();
        if (result == "paired") { accept(); return; }
        problem->setText(resultText(result, m.value("name").toString(name)));
        problem->show();
        pair->setEnabled(QString(code->text()).remove(' ').size() == 6 && result != "lockedout");
    }
}

ConnectionsWindow::ConnectionsWindow(LinkClient* link, QWidget* parent) : QDialog(parent), client(link) {
    setWindowTitle(tr("Connections"));
    setObjectName("connectionsWindow");
    resize(460, 520);
    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    header = new QLabel(tr("This Frond"), this);
    header->setObjectName("thisFrond");
    rename = new QPushButton(tr("Rename…"), this);
    rename->setObjectName("renameThisFrond");
    top->addWidget(header, 1);
    top->addWidget(rename);
    layout->addLayout(top);
    auto heading = [this](const QString& text) {
        auto* label = new QLabel(text, this);
        auto font = label->font();
        font.setBold(true);
        label->setFont(font);
        return label;
    };
    layout->addWidget(heading(tr("Connections")));
    list = new QListWidget(this);
    list->setObjectName("connectionList");
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list, 2);
    layout->addWidget(heading(tr("Nearby")));
    nearbyList = new QListWidget(this);
    nearbyList->setObjectName("nearbyList");
    layout->addWidget(nearbyList, 1);
    nearbyEmpty = new QLabel(tr("No other Frond found"), this);
    nearbyEmpty->setObjectName("nearbyEmpty");
    layout->addWidget(nearbyEmpty);
    auto* row = new QHBoxLayout;
    addAddress = new QPushButton(tr("Add by address…"), this);
    addAddress->setObjectName("addByAddress");
    pairButton = new QPushButton(tr("Pair"), this);
    pairButton->setObjectName("pairNearby");
    pairButton->setEnabled(false);
    row->addWidget(addAddress);
    row->addStretch();
    row->addWidget(pairButton);
    layout->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(client, &LinkClient::message, this, &ConnectionsWindow::message);
    connect(list, &QListWidget::customContextMenuRequested, this, &ConnectionsWindow::rowMenu);
    connect(nearbyList, &QListWidget::currentRowChanged, this, [this](int r) { pairButton->setEnabled(r >= 0); });
    connect(nearbyList, &QListWidget::itemActivated, pairButton, &QPushButton::click);
    connect(pairButton, &QPushButton::clicked, this, [this] {
        auto* item = nearbyList->currentItem();
        if (!item) return;
        startPair({{"op", "pair"}, {"id", item->data(Qt::UserRole).toString()}}, item->data(Qt::UserRole + 1).toString());
    });
    connect(addAddress, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const auto address = QInputDialog::getText(this, tr("Add by address"), tr("Address"), QLineEdit::Normal, {}, &ok).trimmed();
        if (ok && !address.isEmpty()) startPair({{"op", "pair"}, {"address", address}}, address);
    });
    connect(rename, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const auto name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal,
                                                state.value("name").toString(), &ok).trimmed();
        if (ok && !name.isEmpty()) client->send({{"op", "rename"}, {"name", name}});
    });
}

void ConnectionsWindow::showEvent(QShowEvent* event) {
    client->setSticky("watch", {{"op", "watch"}, {"on", true}});
    QDialog::showEvent(event);
}
void ConnectionsWindow::hideEvent(QHideEvent* event) {
    if (requestDialog) { client->send({{"op", "pair-dismiss"}}); requestDialog->close(); }
    client->setSticky("watch", {});
    if (client->isConnected()) client->send({{"op", "watch"}, {"on", false}});
    QDialog::hideEvent(event);
}

QString ConnectionsWindow::status(const QJsonObject& c) {
    if (c.value("keyChanged").toBool()) return tr("Key changed, pair again");
    if (c.value("forgotten").toBool()) return tr("Not paired on %1, pair again").arg(c.value("peerName").toString());
    if (c.value("up").toBool()) {
        auto text = tr("Connected · %1 ms").arg(qint64(c.value("rtt").toDouble()));
        if (!c.value("channels").toString().isEmpty()) text += " · " + c.value("channels").toString();
        return text;
    }
    if (!c.value("seen").toString().isEmpty()) return tr("Seen at %1").arg(c.value("seen").toString());
    return tr("Not seen");
}

void ConnectionsWindow::showState(const QJsonObject& s) {
    state = s;
    header->setText(tr("This Frond: %1").arg(s.value("name").toString()));
    const auto selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    list->clear();
    auto* local = new QListWidgetItem(marker(!s.value("local").toString().isEmpty(), palette()),
                                      tr("This computer") + "\n" +
                                      (s.value("local").toString().isEmpty() ? tr("Not in use") : s.value("local").toString()), list);
    local->setData(Qt::UserRole, "local");
    for (const auto& value : s.value("connections").toArray()) {
        const auto c = value.toObject();
        auto* item = new QListWidgetItem(marker(c.value("up").toBool(), palette()), c.value("name").toString() + "\n" + status(c), list);
        item->setData(Qt::UserRole, c.value("id").toString());
        item->setData(Qt::UserRole + 1, c.value("name").toString());
        item->setData(Qt::UserRole + 2, c.value("address").toString());
        if (item->data(Qt::UserRole).toString() == selected) list->setCurrentItem(item);
    }
    const auto nearSelected = nearbyList->currentItem() ? nearbyList->currentItem()->data(Qt::UserRole).toString() : QString();
    nearbyList->clear();
    for (const auto& value : s.value("nearby").toArray()) {
        const auto n = value.toObject();
        auto* item = new QListWidgetItem(n.value("name").toString() + "\n" + n.value("address").toString(), nearbyList);
        item->setData(Qt::UserRole, n.value("id").toString());
        item->setData(Qt::UserRole + 1, n.value("name").toString());
        if (n.value("id").toString() == nearSelected) nearbyList->setCurrentItem(item);
    }
    nearbyEmpty->setVisible(nearbyList->count() == 0);
    pairButton->setEnabled(nearbyList->currentItem());
}

void ConnectionsWindow::message(const QJsonObject& m) {
    const auto type = m.value("type").toString();
    if (type == "state") return showState(m);
    if (type == "pair-request") {
        if (requestDialog) requestDialog->close();
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("pairRequestDialog");
        dialog->setWindowTitle(tr("Pair"));
        auto* layout = new QVBoxLayout(dialog);
        requestLabel = new QLabel(tr("%1 wants to pair. Code %2").arg(m.value("name").toString(), m.value("code").toString()), dialog);
        requestLabel->setObjectName("pairRequestCode");
        auto font = requestLabel->font();
        font.setPointSizeF(font.pointSizeF() * 1.3);
        requestLabel->setFont(font);
        layout->addWidget(requestLabel);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, [this, dialog] {
            client->send({{"op", "pair-dismiss"}});
            dialog->close();
        });
        requestDialog = dialog;
        dialog->show();
    } else if (type == "pair-request-end") {
        if (requestDialog) { requestDialog->close(); requestDialog = nullptr; }
    }
}

void ConnectionsWindow::rowMenu(const QPoint& at) {
    auto* item = list->itemAt(at);
    if (!item || item->data(Qt::UserRole).toString() == "local") return;
    const auto id = item->data(Qt::UserRole).toString(), name = item->data(Qt::UserRole + 1).toString();
    QMenu menu(this);
    menu.setObjectName("connectionMenu");
    menu.addAction(tr("Rename…"), this, [this, id, name] {
        bool ok = false;
        const auto text = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, name, &ok).trimmed();
        if (ok && !text.isEmpty()) client->send({{"op", "peer-rename"}, {"id", id}, {"name", text}});
    })->setObjectName("renameConnection");
    const auto address = item->data(Qt::UserRole + 2).toString();
    menu.addAction(tr("Address…"), this, [this, id, address] {
        bool ok = false;
        const auto text = QInputDialog::getText(this, tr("Address"), tr("Address"), QLineEdit::Normal, address, &ok).trimmed();
        if (ok) client->send({{"op", "peer-address"}, {"id", id}, {"address", text}});
    })->setObjectName("connectionAddress");
    menu.addAction(tr("Forget"), this, [this, id, name] {
        QMessageBox box(QMessageBox::Question, tr("Forget"), tr("Forget %1?").arg(name), QMessageBox::Cancel, this);
        auto* forget = box.addButton(tr("Forget"), QMessageBox::DestructiveRole);
        box.exec();
        if (box.clickedButton() == forget) client->send({{"op", "forget"}, {"id", id}});
    })->setObjectName("forgetConnection");
    menu.exec(list->viewport()->mapToGlobal(at));
}

void ConnectionsWindow::startPair(const QJsonObject& op, const QString& name) {
    if (codeDialog) codeDialog->close();
    auto* dialog = new PairCodeDialog(client, name, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    codeDialog = dialog;
    client->send(op);
    dialog->show();
}
