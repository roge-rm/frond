#include "RunningPanels.h"
#include "Icons.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

PanelHost::PanelHost(QWidget* parent) : QFrame(parent) {
    setObjectName("panelHost");
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 8);
    auto* header = new QHBoxLayout;
    title = new QLabel(this);
    title->setObjectName("panelTitle");
    auto font = title->font(); font.setBold(true); font.setPointSizeF(font.pointSizeF() * 1.15);
    title->setFont(font);
    auto* close = new QToolButton(this);
    close->setObjectName("panelClose");
    close->setIcon(frondIcons::icon("close"));
    close->setToolTip(tr("Close"));
    close->setAutoRaise(true);
    header->addWidget(title, 1);
    header->addWidget(close);
    layout->addLayout(header);
    pages = new QStackedWidget(this);
    layout->addWidget(pages, 1);
    connect(close, &QToolButton::clicked, this, [this] { open(-1); });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { open(-1); });
    hide();
}

QVBoxLayout* PanelHost::addPage(const QString& pageTitle) {
    auto* scroll = new QScrollArea(pages);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 4, 0);
    scroll->setWidget(content);
    pages->addWidget(scroll);
    titles << pageTitle;
    return layout;
}

void PanelHost::setTitle(int page, const QString& text) {
    if (page < 0 || page >= titles.size()) return;
    titles[page] = text;
    if (page == index) title->setText(text);
}

void PanelHost::open(int page) {
    if (page < 0 || page >= pages->count()) page = -1;
    if (page == index) return;
    index = page;
    if (page >= 0) {
        pages->setCurrentIndex(page);
        title->setText(titles[page]);
    }
    setVisible(page >= 0);
    emit changed(index);
}

AppsPanel::AppsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("appsPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* buttons = new QHBoxLayout;
    auto add = [&](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(name);
        buttons->addWidget(b);
        return b;
    };
    connect(add(tr("Install…"), "installApp"), &QPushButton::clicked, this, &AppsPanel::installRequested);
    connect(add(tr("Other…"), "otherApp"), &QPushButton::clicked, this, &AppsPanel::otherRequested);
    connect(add(tr("Export all…"), "exportRam"), &QPushButton::clicked, this, &AppsPanel::exportAllRequested);
    layout->addLayout(buttons);
    list = new QListWidget(this);
    list->setObjectName("appsList");
    list->setMinimumHeight(240);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(list, 1);
    empty = new QLabel(tr("No applications"), this);
    empty->setObjectName("appsEmpty");
    empty->setEnabled(false);
    layout->addWidget(empty);
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { emit launchRequested(item->data(Qt::UserRole).toString()); });
}

QStringList AppsPanel::names() const {
    QStringList out;
    for (int i = 0; i < list->count(); ++i) out << list->item(i)->data(Qt::UserRole).toString();
    return out;
}

void AppsPanel::setDatabases(const std::vector<runningRules::Database>& all) {
    const auto apps = runningRules::applications(all);
    const int row = list->currentRow();
    list->clear();
    for (const auto& app : apps) {
        auto* item = new QListWidgetItem(list);
        item->setData(Qt::UserRole, app.name);
        auto* widget = new QWidget(list);
        auto* line = new QHBoxLayout(widget);
        line->setContentsMargins(6, 2, 2, 2);
        auto* name = new QLabel(app.name, widget);
        name->setToolTip(app.name);
        line->addWidget(name, 1);
        auto tool = [&](const char* icon, const char* object, const QString& tip) {
            auto* b = new QToolButton(widget);
            b->setObjectName(object);
            b->setIcon(frondIcons::icon(icon));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            line->addWidget(b);
            return b;
        };
        const QString appName = app.name;
        connect(tool("launch", "appLaunch", tr("Launch")), &QToolButton::clicked, this, [this, appName] { emit launchRequested(appName); });
        connect(tool("export", "appExport", tr("Export")), &QToolButton::clicked, this, [this, appName] { emit exportRequested(appName); });
        connect(tool("delete", "appDelete", tr("Delete")), &QToolButton::clicked, this, [this, appName] { emit deleteRequested(appName); });
        item->setSizeHint(widget->sizeHint());
        list->setItemWidget(item, widget);
    }
    if (row >= 0 && row < list->count()) list->setCurrentRow(row);
    empty->setVisible(apps.empty());
}
