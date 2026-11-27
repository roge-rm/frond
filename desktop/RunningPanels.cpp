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
#include <QXmlStreamReader>

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
    emit aboutToChange(page);
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
    installButton = add(tr("Install…"), "installApp");
    connect(installButton, &QPushButton::clicked, this, &AppsPanel::installRequested);
    otherButton = add(tr("Other…"), "otherApp");
    connect(otherButton, &QPushButton::clicked, this, &AppsPanel::otherRequested);
    exportAllButton = add(tr("Export all…"), "exportRam");
    connect(exportAllButton, &QPushButton::clicked, this, &AppsPanel::exportAllRequested);
    refreshButton = add(tr("Refresh"), "refreshInstalledPrograms");
    refreshButton->hide();
    connect(refreshButton, &QPushButton::clicked, this, &AppsPanel::refreshInstalledPrograms);
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
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (!installOnly) emit launchRequested(item->data(Qt::UserRole).toString());
    });
}

void AppsPanel::setInstallOnly(bool on, bool windowsMobile) {
    if (on == installOnly && windowsMobile == listWindowsMobile) return;
    installOnly = on;
    listWindowsMobile = windowsMobile;
    otherButton->setEnabled(!on);
    exportAllButton->setEnabled(!on);
    refreshButton->setVisible(windowsMobile);
    list->setEnabled(!on || windowsMobile);
    list->setVisible(!on || windowsMobile);
    setDatabases({});
}

void AppsPanel::setPackageQueue(bool on) {
    if (on == packageQueue) return;
    packageQueue = on;
    installButton->setText(on ? tr("Queue IPK…") : tr("Install…"));
    empty->setVisible(!on);
}

void AppsPanel::loadingInstalledPrograms() {
    readingWindowsMobile = true;
    refreshButton->setEnabled(false);
    empty->setText(tr("Reading installed programs…"));
    empty->show();
}

void AppsPanel::showInstalledPrograms(bool ok, const QString& message) {
    if (!readingWindowsMobile) return;
    readingWindowsMobile = false;
    refreshButton->setEnabled(true);
    if (!ok) {
        empty->setText(message);
        empty->show();
        return;
    }
    QXmlStreamReader xml(message);
    QStringList programs;
    bool uninstall = false, found = false;
    int depth = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD) break;
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            const auto type = xml.attributes().value(QLatin1String("type"));
            if (depth == 1 && xml.name() != QLatin1String("wap-provisioningdoc")) break;
            if (depth == 2) {
                uninstall = xml.name() == QLatin1String("characteristic") && type == QLatin1String("UnInstall");
                found |= uninstall;
            }
            if (depth == 3 && uninstall && xml.name() == QLatin1String("characteristic") && !type.isEmpty())
                programs.push_back(type.toString());
        } else if (token == QXmlStreamReader::EndElement) {
            --depth;
        }
    }
    if (!found || xml.hasError() || xml.tokenType() != QXmlStreamReader::EndDocument) {
        empty->setText(tr("Windows Mobile returned an invalid program list."));
        empty->show();
        return;
    }
    programs.removeDuplicates();
    programs.sort(Qt::CaseInsensitive);
    list->clear();
    list->addItems(programs);
    empty->setText(tr("No installed programs"));
    empty->setVisible(programs.empty());
}

QStringList AppsPanel::names() const {
    QStringList out;
    for (int i = 0; i < list->count(); ++i) out << list->item(i)->data(Qt::UserRole).toString();
    return out;
}

void AppsPanel::setDatabases(const std::vector<runningRules::Database>& all, const QString& note) {
    // An install-only panel has no list, whatever comes in.
    const auto apps = installOnly ? std::vector<runningRules::Database>{} : runningRules::applications(all);
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
    readingWindowsMobile = false;
    refreshButton->setEnabled(true);
    empty->setText(listWindowsMobile ? tr("Installed programs haven’t been read") :
                   installOnly ? tr("Not available yet") : note.isEmpty() ? tr("No applications") : note);
    empty->setVisible(apps.empty() && !packageQueue);
}
