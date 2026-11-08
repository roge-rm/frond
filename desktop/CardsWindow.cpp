#include "CardsWindow.h"
#include "CardFilesWindow.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>

// The bar at the start of a Cards row: lit (highlight colour) while the card is in a machine,
// a faint line otherwise.
namespace {
class CardMarker : public QFrame {
 public:
    using QFrame::QFrame;
 protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        auto colour = palette().color(property("lit").toBool() ? QPalette::Highlight : QPalette::Mid);
        if (!property("lit").toBool()) colour.setAlpha(60);
        p.fillRect(rect(), colour);
    }
};
}  // namespace

QString runCardJob(QWidget* parent, const QString& text, std::function<QString(const frondSessions::CopyProgress&)> work) {
    std::atomic<bool> cancel{false};
    std::atomic<qint64> done{0}, total{0};
    QString error;
    QProgressDialog dialog(text, QObject::tr("Cancel"), 0, 1000, parent);
    dialog.setObjectName("cardJob");
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.setMinimumDuration(300);
    dialog.setValue(0);
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, [&] { cancel = true; });
    QEventLoop loop;
    std::unique_ptr<QThread> thread(QThread::create([&] {
        error = work([&](qint64 d, qint64 t) {
            done = d;
            total = t;
            return !cancel.load();
        });
    }));
    QObject::connect(thread.get(), &QThread::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, &dialog, [&] {
        if (!cancel && total > 0) dialog.setValue(int(done * 1000 / total));
    });
    thread->start();
    timer.start();
    if (!thread->isFinished()) loop.exec();
    thread->wait();
    timer.stop();
    dialog.reset();
    return error;
}

static void grey(QComboBox* box, int index, bool enabled) {
    auto* model = qobject_cast<QStandardItemModel*>(box->model());
    if (auto* item = model ? model->item(index) : nullptr) item->setEnabled(enabled);
}
static bool greyed(const QComboBox* box, int index) {
    auto* model = qobject_cast<QStandardItemModel*>(box->model());
    auto* item = model ? model->item(index) : nullptr;
    return item && !item->isEnabled();
}

NewCardDialog::NewCardDialog(const QString& sessions, frondFirmware::Device device, QWidget* parent)
    : QDialog(parent), sessions(sessions), device(device) {
    setWindowTitle(tr("New card"));
    setObjectName("newCardDialog");
    auto* form = new QFormLayout(this);
    name = new QLineEdit(this);
    name->setObjectName("cardName");
    type = new QComboBox(this);
    type->setObjectName("cardType");
    sizes = new QComboBox(this);
    sizes->setObjectName("cardSize");
    for (const auto& kind : frondCards::kindsTaken())
        if (device == frondFirmware::Device::automatic || frondCards::deviceTakesKind(device, kind))
            type->addItem(frondCards::kindName(kind), kind);
    form->addRow(tr("Name"), name);
    form->addRow(tr("Type"), type);
    form->addRow(tr("Size"), sizes);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Make"));
    form->addRow(buttons);
    auto check = [this, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!name->text().trimmed().isEmpty() && sizes->currentIndex() >= 0 &&
                                                          !greyed(sizes, sizes->currentIndex()));
    };
    connect(name, &QLineEdit::textEdited, this, [this, check] { nameEdited = true; check(); });
    connect(type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, check] { fillSizes(); check(); });
    connect(sizes, qOverload<int>(&QComboBox::currentIndexChanged), this, check);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, sessions] {
        const auto wanted = name->text().trimmed();
        if (frondCards::freeCardName(frondCards::libraryFor(sessions), wanted) != wanted) {
            QMessageBox::warning(this, tr("New card"), tr("There's already a card called %1.").arg(wanted));
            return;
        }
        accept();
    });
    fillSizes();
    check();
}
void NewCardDialog::fillSizes() {
    const auto k = kind();
    sizes->clear();
    int preferred = -1;
    for (int size : frondCards::sizesFor(k)) {
        bool tooBig = false;
        const bool fits = device == frondFirmware::Device::automatic || frondCards::deviceTakes(device, k, size, &tooBig);
        // A size the machine doesn't take for another reason isn't offered.
        if (!fits && !tooBig) continue;
        auto label = frondCards::sizeLabel(size);
        if (!fits) label += tr(" · Too big for %1").arg(frondFirmware::deviceName(device));
        sizes->addItem(label, size);
        grey(sizes, sizes->count() - 1, fits);
        if (fits && (preferred < 0 || size == 128)) preferred = sizes->count() - 1;
    }
    if (preferred >= 0) sizes->setCurrentIndex(preferred);
    if (!nameEdited) name->setText(frondCards::freeCardName(frondCards::libraryFor(sessions), frondCards::kindName(k)));
}
FolderCardDialog::FolderCardDialog(const QString& sessions, const QString& folder, const frondFolderCard::Plan& plan, QWidget* parent)
    : QDialog(parent), sessions(sessions), plan(plan) {
    setWindowTitle(tr("Import folder as card"));
    setObjectName("folderCardDialog");
    auto* form = new QFormLayout(this);
    name = new QLineEdit(this);
    name->setObjectName("cardName");
    type = new QComboBox(this);
    type->setObjectName("cardType");
    sizes = new QComboBox(this);
    sizes->setObjectName("cardSize");
    for (const auto& kind : frondCards::folderKinds()) type->addItem(frondCards::kindName(kind), kind);
    auto* summary = new QLabel(tr("%1: %2 in %3 files").arg(QFileInfo(folder).fileName(),
                                                           frondCards::sizeLabel(qMax(1, int((plan.bytes + (1 << 20) - 1) >> 20))),
                                                           QString::number(plan.files)), this);
    summary->setObjectName("folderSummary");
    form->addRow(summary);
    form->addRow(tr("Name"), name);
    form->addRow(tr("Type"), type);
    form->addRow(tr("Size"), sizes);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Make"));
    form->addRow(buttons);
    auto check = [this, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!name->text().trimmed().isEmpty() && sizes->currentIndex() >= 0 &&
                                                          !greyed(sizes, sizes->currentIndex()));
    };
    name->setText(frondCards::freeCardName(frondCards::libraryFor(sessions), QFileInfo(folder).fileName()));
    connect(name, &QLineEdit::textEdited, this, [this, check] { nameEdited = true; check(); });
    connect(type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, check] { fillSizes(); check(); });
    connect(sizes, qOverload<int>(&QComboBox::currentIndexChanged), this, check);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, sessions] {
        const auto wanted = name->text().trimmed();
        if (frondCards::freeCardName(frondCards::libraryFor(sessions), wanted) != wanted) {
            QMessageBox::warning(this, tr("Import folder as card"), tr("There's already a card called %1.").arg(wanted));
            return;
        }
        accept();
    });
    fillSizes();
    check();
}
void FolderCardDialog::fillSizes() {
    sizes->clear();
    const auto k = kind();
    int preferred = -1;
    for (int size : frondCards::sizesFor(k)) {
        const bool fits = frondFolderCard::fits(plan, unsigned(size));
        sizes->addItem(frondCards::sizeLabel(size) + (fits ? QString() : tr(" · Too small")), size);
        grey(sizes, sizes->count() - 1, fits);
        if (fits && preferred < 0) preferred = sizes->count() - 1;
    }
    if (preferred >= 0) sizes->setCurrentIndex(preferred);
}
QString FolderCardDialog::kind() const { return type->currentData().toString(); }
int FolderCardDialog::size() const { return sizes->currentData().toInt(); }
QString NewCardDialog::kind() const { return type->currentData().toString(); }
int NewCardDialog::size() const { return sizes->currentData().toInt(); }

LinkDialog::LinkDialog(const QString& sessions, const QString& cardId, const QString& held, const QString& tick, QWidget* parent)
    : QDialog(parent) {
    setObjectName("linkDialog");
    frondCards::Card card;
    frondCards::readCard(frondCards::entryDir(frondCards::libraryFor(sessions), cardId), &card);
    setWindowTitle(tr("Link %1 to machines").arg(card.name));
    auto* layout = new QVBoxLayout(this);
    machines = new QListWidget(this);
    machines->setObjectName("linkMachines");
    layout->addWidget(machines);
    const auto tickCanon = tick.isEmpty() ? QString() : QFileInfo(tick).canonicalFilePath();
    QMap<QString, QString> names;
    for (const auto& e : frondCards::entries(frondCards::libraryFor(sessions))) names[e.card.id] = e.card.name;
    for (const auto& m : frondCards::machineSlots(sessions, held)) {
        if (!frondCards::deviceTakesKind(m.device, card.kind)) continue;
        bool tooBig = false;
        const bool fits = frondCards::deviceTakes(m.device, card.kind, frondCards::sizeOf(card.kind, card.bytes), &tooBig);
        if (!fits && !tooBig) continue;
        QString text = m.name + QStringLiteral(" · ") + frondFirmware::deviceName(m.device);
        bool enabled = true;
        if (!fits) { text += tr(" · Too big for %1").arg(frondFirmware::deviceName(m.device)); enabled = false; }
        else if (!m.cardFile.isEmpty() && m.slot.card != cardId) {
            text += tr(" · Has %1, take it out first").arg(names.value(m.slot.card, frondCards::kindName(frondCards::kindOfFile(m.cardFile))));
            enabled = false;
        } else if (m.busy) { text += tr(" · Running"); enabled = false; }
        else if (!m.slot.link.isEmpty() && m.slot.link != cardId) text += tr(" · Linked to %1").arg(names.value(m.slot.link));
        auto* item = new QListWidgetItem(text, machines);
        item->setData(Qt::UserRole, m.dir);
        const bool on = m.slot.link == cardId || (!tickCanon.isEmpty() && QFileInfo(m.dir).canonicalFilePath() == tickCanon && enabled);
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        item->setFlags(enabled ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable : Qt::NoItemFlags);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(420, 300);
}
QStringList LinkDialog::linked() const {
    QStringList result;
    for (int i = 0; i < machines->count(); ++i) {
        auto* item = machines->item(i);
        // A greyed machine keeps the link it has.
        if (item->checkState() == Qt::Checked) result << item->data(Qt::UserRole).toString();
    }
    return result;
}

CardPickerDialog::CardPickerDialog(const QString& sessions, const QString& machineDir, frondFirmware::Device device, const QString& current,
                                   QWidget* parent)
    : QDialog(parent), sessions(sessions), machineDir(machineDir), current(current), device(device) {
    setObjectName("cardPicker");
    const bool stick = deviceProfileStorageSlot5(frondFirmware::profile(device)) == DeviceStorageSlot5::memoryStick;
    setWindowTitle((stick ? tr("Memory Stick for %1") : tr("SD card for %1")).arg(QFileInfo(machineDir).fileName()));
    auto* layout = new QVBoxLayout(this);
    auto* area = new QScrollArea(this);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget(area);
    choices = new QVBoxLayout(inner);
    area->setWidget(inner);
    layout->addWidget(area, 1);
    auto* row = new QHBoxLayout;
    newCard = new QPushButton(tr("New card"), this);
    newCard->setObjectName("pickerNewCard");
    linkCards = new QPushButton(tr("Link to machines…"), this);
    linkCards->setObjectName("pickerLink");
    row->addWidget(newCard);
    row->addWidget(linkCards);
    row->addStretch();
    layout->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(newCard, &QPushButton::clicked, this, [this] {
        NewCardDialog dialog(this->sessions, this->device, this);
        if (dialog.exec() != QDialog::Accepted) return;
        QString id;
        const auto name = dialog.name->text(), kind = dialog.kind();
        const int size = dialog.size();
        const auto error = runCardJob(this, tr("Making the card…"), [&](const frondSessions::CopyProgress& progress) {
            return frondCards::makeCard(this->sessions, name, kind, size, progress, &id);
        });
        if (!error.isEmpty() && error != "Cancelled.") QMessageBox::warning(this, tr("New card"), error);
        fill(id.isEmpty() ? chosen() : id);
    });
    connect(linkCards, &QPushButton::clicked, this, [this] {
        const auto id = chosen();
        if (id.isEmpty()) return;
        LinkDialog dialog(this->sessions, id, this->machineDir, this->machineDir, this);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto error = frondCards::setLinks(this->sessions, id, dialog.linked(), this->machineDir);
        if (!error.isEmpty()) QMessageBox::warning(this, tr("Link"), error);
        fill(id);
    });
    fill(current);
    resize(420, 340);
}
void CardPickerDialog::fill(const QString& select) {
    for (auto* r : radios) delete r;
    radios.clear();
    while (auto* item = choices->takeAt(0)) delete item;
    auto add = [&](const QString& text, const QString& id, bool enabled) {
        auto* radio = new QRadioButton(text, choices->parentWidget());
        radio->setObjectName("cardChoice");
        radio->setProperty("cardId", id);
        radio->setEnabled(enabled);
        radio->setChecked(id == select && enabled);
        connect(radio, &QRadioButton::toggled, this, [this] { linkCards->setEnabled(!chosen().isEmpty()); });
        choices->addWidget(radio);
        radios << radio;
    };
    add(tr("No card"), {}, true);
    const auto mine = QFileInfo(machineDir).canonicalFilePath();
    for (const auto& r : frondCards::rows(sessions, machineDir)) {
        if (!frondCards::deviceTakesKind(device, r.card.kind)) continue;
        bool tooBig = false;
        const bool fits = frondCards::deviceTakes(device, r.card.kind, frondCards::sizeOf(r.card.kind, r.card.bytes), &tooBig);
        if (!fits && !tooBig) continue;
        auto text = r.card.name + QStringLiteral(" · ") + frondCards::cardSizeText(r.card);
        bool enabled = fits;
        const bool here = !r.holderDir.isEmpty() && QFileInfo(r.holderDir).canonicalFilePath() == mine;
        if (!fits) text += tr(" · Too big for %1").arg(frondFirmware::deviceName(device));
        else if (!r.holder.isEmpty() && !here) { text += tr(" · In %1").arg(r.holder); enabled = false; }
        add(text, r.card.id, enabled);
    }
    if (std::none_of(radios.cbegin(), radios.cend(), [](QRadioButton* r) { return r->isChecked(); })) radios.first()->setChecked(true);
    choices->addStretch();
    linkCards->setEnabled(!chosen().isEmpty());
}
QString CardPickerDialog::chosen() const {
    for (auto* r : radios)
        if (r->isChecked()) return r->property("cardId").toString();
    return {};
}

CardsWindow::CardsWindow(const QString& sessions, QWidget* parent) : QDialog(parent), sessions(sessions) {
    setObjectName("cardsWindow");
    setWindowTitle(tr("Cards"));
    auto* layout = new QVBoxLayout(this);
    auto heading = [&](const QString& text) {
        auto* label = new QLabel(text, this);
        auto font = label->font();
        font.setPointSizeF(font.pointSizeF() * 1.2);
        font.setBold(true);
        label->setFont(font);
        layout->addWidget(label);
        return label;
    };
    auto list = [&](const char* name) {
        auto* l = new QListWidget(this);
        l->setObjectName(name);
        l->setContextMenuPolicy(Qt::CustomContextMenu);
        l->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        l->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        connect(l, &QWidget::customContextMenuRequested, this, [this, l](const QPoint& at) {
            if (auto* item = l->itemAt(at)) {
                std::unique_ptr<QMenu> menu(menuFor(item->data(Qt::UserRole).toString(), this));
                menu->exec(l->viewport()->mapToGlobal(at));
            }
        });
        layout->addWidget(l, 1);
        return l;
    };
    auto empty = [&](const QString& text) {
        auto* label = new QLabel(text, this);
        label->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(label);
        return label;
    };
    sdHeading = heading(tr("SD cards"));
    sdList = list("sdCards");
    sdEmpty = empty(tr("No SD cards."));
    layout->addSpacing(8);
    msHeading = heading(tr("Memory Sticks"));
    msList = list("memorySticks");
    msEmpty = empty(tr("No Memory Sticks."));
    const auto kinds = frondCards::kindsTaken();
    const bool sticks = kinds.contains("ms") || kinds.contains("ms-pro");
    for (auto* w : std::initializer_list<QWidget*>{msHeading, msList, msEmpty}) w->setVisible(sticks);
    for (auto* w : std::initializer_list<QWidget*>{sdHeading, sdList, sdEmpty}) w->setVisible(kinds.contains("sd"));
    auto* buttons = new QHBoxLayout;
    auto* make = new QPushButton(tr("New card"), this);
    make->setObjectName("newCard");
    auto* import = new QPushButton(tr("Import card image"), this);
    import->setObjectName("importCard");
    auto* folder = new QPushButton(tr("Import folder as card…"), this);
    folder->setObjectName("importFolder");
    auto* close = new QPushButton(tr("Close"), this);
    buttons->addWidget(make);
    buttons->addWidget(import);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    auto* more = new QHBoxLayout;
    more->addWidget(folder);
    more->addStretch();
    layout->addLayout(more);
    connect(make, &QPushButton::clicked, this, &CardsWindow::newCard);
    connect(import, &QPushButton::clicked, this, &CardsWindow::importImage);
    connect(folder, &QPushButton::clicked, this, &CardsWindow::importFolder);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    resize(520, 520);
    frondCards::reconcile(sessions);
    refresh();
}

void CardsWindow::refresh() {
    const auto running = held();
    const auto runningCanon = running.isEmpty() ? QString() : QFileInfo(running).canonicalFilePath();
    sdList->clear();
    msList->clear();
    for (const auto& r : frondCards::rows(sessions, running)) {
        auto* list = r.card.kind == "sd" ? sdList : msList;
        auto* item = new QListWidgetItem(list);
        item->setData(Qt::UserRole, r.card.id);
        auto* line = new QWidget;
        line->setObjectName("cardRow");
        auto* box = new QHBoxLayout(line);
        box->setContentsMargins(0, 2, 2, 2);
        // The marker is lit while the card is in a machine.
        auto* marker = new CardMarker(line);
        marker->setObjectName("cardMarker");
        marker->setFixedWidth(4);
        marker->setMinimumHeight(24);
        marker->setProperty("lit", !r.holder.isEmpty());
        box->addWidget(marker);
        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        auto* name = new QLabel(r.card.name, line);
        name->setObjectName("cardName");
        auto font = name->font();
        font.setBold(true);
        name->setFont(font);
        auto* detail = new QLabel(frondCards::cardSizeText(r.card) + QStringLiteral(" · ") + frondCards::statusText(r), line);
        detail->setObjectName("cardDetail");
        detail->setForegroundRole(QPalette::PlaceholderText);
        // A linked card lists every machine, so the line wraps rather than being cut off.
        detail->setWordWrap(true);
        text->addWidget(name);
        text->addWidget(detail);
        box->addLayout(text, 1);
        auto* more = new QPushButton(QString::fromUtf8("…"), line);
        more->setObjectName("cardMenu");
        more->setToolTip(tr("More"));
        more->setFixedWidth(32);
        more->setMenu(menuFor(r.card.id, more));
        box->addWidget(more);
        list->setItemWidget(item, line);
        Q_UNUSED(runningCanon);
    }
    fitRows();
    sdEmpty->setVisible(sdHeading->isVisibleTo(this) && sdList->count() == 0);
    msEmpty->setVisible(msHeading->isVisibleTo(this) && msList->count() == 0);
}

// Row heights for the lists' width, as the status lines wrap.
void CardsWindow::fitRows() {
    for (auto* list : {sdList, msList}) {
        const int width = std::max(list->viewport()->width(), 200);
        for (int i = 0; i < list->count(); ++i)
            if (auto* line = list->itemWidget(list->item(i))) {
                const int h = line->hasHeightForWidth() ? line->heightForWidth(width) : line->sizeHint().height();
                list->item(i)->setSizeHint(QSize(0, h + 6));
            }
    }
}
void CardsWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    fitRows();
}
void CardsWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    fitRows();
}

QMenu* CardsWindow::menuFor(const QString& id, QWidget* parent) {
    auto* menu = new QMenu(parent);
    const auto running = held();
    frondCards::Row row;
    for (const auto& r : frondCards::rows(sessions, running))
        if (r.card.id == id) row = r;
    const bool inRunning = !running.isEmpty() && !row.holderDir.isEmpty() &&
                           QFileInfo(row.holderDir).canonicalFilePath() == QFileInfo(running).canonicalFilePath();
    // An exchange open on the running machine's card greys all of it.
    const bool exchange = inRunning && exchangeOpen && exchangeOpen();
    auto add = [&](const QString& text, const char* name, bool enabled, void (CardsWindow::*f)(const QString&)) {
        auto* a = menu->addAction(text, this, [this, id, f] { QTimer::singleShot(0, this, [this, id, f] { (this->*f)(id); }); });
        a->setObjectName(name);
        a->setProperty("allowed", enabled);
        a->setEnabled(enabled && !exchange);
    };
    const bool stopped = !row.holderBusy && !inRunning;
    // A classic Memory Stick has no FAT the desktop can write in place.
    add(stopped ? tr("Files…") : tr("Files… · In use"), "cardFiles", stopped && row.card.kind != "ms", &CardsWindow::openFiles);
    add(tr("Rename…"), "renameCard", true, &CardsWindow::renameCard);
    add(tr("Duplicate"), "duplicateCard", stopped, &CardsWindow::duplicateCard);
    add(tr("Export image…"), "exportCard", stopped, &CardsWindow::exportImage);
    add(tr("Link to machines…"), "linkCard", true, &CardsWindow::linkCard);
    if (!row.holder.isEmpty())
        add(tr("Take out of %1").arg(row.holder), "takeOutCard", !row.holderBusy || inRunning, &CardsWindow::takeOut);
    add(tr("Delete"), "deleteCard", row.inDrawer, &CardsWindow::deleteCard);
    // An exchange can start after the row was made: checked again each time the menu opens.
    connect(menu, &QMenu::aboutToShow, menu, [this, menu, inRunning] {
        const bool busy = inRunning && exchangeOpen && exchangeOpen();
        for (auto* a : menu->actions()) a->setEnabled(a->property("allowed").toBool() && !busy);
    });
    return menu;
}

void CardsWindow::failed(const QString& message) {
    if (message.isEmpty() || message == "Cancelled.") return;
    QMessageBox::warning(this, tr("Cards"), message);
}
static void afterChange(CardsWindow* w) {
    w->refresh();
    if (w->changed) w->changed();
}

void CardsWindow::newCard() {
    NewCardDialog dialog(sessions, frondFirmware::Device::automatic, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto name = dialog.name->text(), kind = dialog.kind();
    const int size = dialog.size();
    failed(runCardJob(this, tr("Making the card…"), [&](const frondSessions::CopyProgress& progress) {
        return frondCards::makeCard(sessions, name, kind, size, progress);
    }));
    afterChange(this);
}
void CardsWindow::importImage() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Import card image"), {},
                                                   tr("Card images (*.img *.bin *.frondms);;All files (*)"));
    if (path.isEmpty()) return;
    const auto kinds = frondCards::imageKinds(path);
    if (kinds.isEmpty()) return failed(tr("No device here takes a card like that."));
    auto kind = kinds.first();
    if (kinds.size() > 1) {
        QStringList names;
        for (const auto& k : kinds) names << frondCards::kindName(k);
        bool ok = false;
        const auto picked = QInputDialog::getItem(this, tr("Import card image"), tr("Type"), names, 0, false, &ok);
        if (!ok) return;
        kind = kinds.value(names.indexOf(picked), kind);
    }
    failed(runCardJob(this, tr("Importing the image…"), [&](const frondSessions::CopyProgress& progress) {
        return frondCards::importCard(sessions, path, kind, progress);
    }));
    afterChange(this);
}
void CardsWindow::importFolder() {
    if (!frondFolderCard::mtoolsAvailable()) return failed(tr("Install mtools to import a folder (Debian: sudo apt install mtools)."));
    const auto folder = QFileDialog::getExistingDirectory(this, tr("Import folder as card"));
    if (folder.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto plan = frondFolderCard::measure(folder);
    QApplication::restoreOverrideCursor();
    if (!plan.problem.isEmpty()) return failed(plan.problem);
    bool any = false;
    for (const auto& kind : frondCards::folderKinds())
        for (int size : frondCards::sizesFor(kind)) any = any || frondFolderCard::fits(plan, unsigned(size));
    if (!any) return failed(tr("The folder is too big for a card."));
    FolderCardDialog dialog(sessions, folder, plan, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto name = dialog.name->text(), kind = dialog.kind();
    const int size = dialog.size();
    failed(runCardJob(this, tr("Building the card…"), [&](const frondSessions::CopyProgress& progress) {
        return frondCards::importFolderCard(sessions, folder, name, kind, size, progress);
    }));
    afterChange(this);
}
void CardsWindow::renameCard(const QString& id) {
    frondCards::Card card;
    if (!frondCards::readCard(frondCards::entryDir(frondCards::libraryFor(sessions), id), &card)) return;
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, card.name, &ok);
    if (!ok) return;
    failed(frondCards::renameCard(sessions, id, name));
    afterChange(this);
}
void CardsWindow::duplicateCard(const QString& id) {
    failed(runCardJob(this, tr("Copying the card…"), [&](const frondSessions::CopyProgress& progress) {
        return frondCards::duplicateCard(sessions, id, progress);
    }));
    afterChange(this);
}
void CardsWindow::exportImage(const QString& id) {
    frondCards::Card card;
    if (!frondCards::readCard(frondCards::entryDir(frondCards::libraryFor(sessions), id), &card)) return;
    const bool classic = card.kind == "ms";
    QFileDialog dialog(this, tr("Export image"), QDir::homePath() + '/' + card.name + (classic ? ".frondms" : ".img"),
                       classic ? tr("Memory Stick images (*.frondms)") : tr("Card images (*.img)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(classic ? "frondms" : "img");
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const auto path = QFileInfo(dialog.selectedFiles().first()).absoluteFilePath();
    failed(runCardJob(this, tr("Exporting the image…"), [&](const frondSessions::CopyProgress& progress) {
        return frondCards::exportCardImage(sessions, id, path, progress);
    }));
    afterChange(this);
}
void CardsWindow::openFiles(const QString& id) {
    if (files) return;
    QString path, holderDir;
    std::unique_ptr<QLockFile> hold;
    frondCards::Card card;
    if (const auto error = frondCards::cardSource(sessions, id, &path, &hold, &card, &holderDir); !error.isEmpty()) return failed(error);
    if (card.kind == "ms") return failed(tr("A Memory Stick can't be opened here."));
    if (QFileInfo(path).fileName() != frondCards::fileOfKind(card.kind))
        return failed(tr("Open %1 once to finish its last save.").arg(QFileInfo(holderDir).fileName()));
    std::unique_ptr<frondCardFile::CardFile> opened;
    try {
        opened = frondCardFile::CardFile::open(path, !card.writeProtected);
        // A machine loads the card as changed from the first write on, even after a crash.
        if (!holderDir.isEmpty())
            opened->beforeFirstWrite = [holderDir] {
                auto slot = frondCards::readSlot(holderDir);
                if (slot.swap) return;
                slot.swap = true;
                if (!frondCards::writeSlot(holderDir, slot)) throw frondCardFile::Error(QStringLiteral("Couldn't write the card."));
            };
        frondCardFile::FatImage check(*opened);
    } catch (const frondCardFile::Error& e) {
        return failed(e.message);
    }
    auto* w = new CardFilesWindow(std::move(opened), card.name, this);
    w->hold = std::move(hold);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->setWindowModality(Qt::WindowModal);
    files = w;
    connect(w, &QDialog::finished, this, [this] {
        files = nullptr;
        afterChange(this);
    });
    w->QDialog::open();
}
void CardsWindow::linkCard(const QString& id) {
    LinkDialog dialog(sessions, id, held(), {}, this);
    if (dialog.exec() != QDialog::Accepted) return;
    failed(frondCards::setLinks(sessions, id, dialog.linked(), held()));
    afterChange(this);
}
void CardsWindow::takeOut(const QString& id) {
    for (const auto& r : frondCards::rows(sessions, held()))
        if (r.card.id == id && !held().isEmpty() &&
            QFileInfo(r.holderDir).canonicalFilePath() == QFileInfo(held()).canonicalFilePath()) {
            if (takeOutRunning) takeOutRunning(id);
            return;
        }
    failed(runCardJob(this, tr("Taking the card out…"), [&](const frondSessions::CopyProgress& progress) {
        return frondCards::takeOutStopped(sessions, id, progress);
    }));
    afterChange(this);
}
void CardsWindow::deleteCard(const QString& id) {
    frondCards::Row row;
    for (const auto& r : frondCards::rows(sessions, held()))
        if (r.card.id == id) row = r;
    if (row.card.id.isEmpty()) return;
    const auto question = row.links.isEmpty()
        ? tr("Delete %1? Its files are lost.").arg(row.card.name)
        : row.links.size() == 1 ? tr("Delete %1? It is linked to 1 machine, and its files are lost.").arg(row.card.name)
                                : tr("Delete %1? It is linked to %2 machines, and its files are lost.").arg(row.card.name).arg(row.links.size());
    if (QMessageBox::question(this, tr("Delete"), question, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    failed(frondCards::deleteCard(sessions, id, held()));
    afterChange(this);
}
