#pragma once
#include <QFrame>
#include <vector>
#include "RunningRules.h"
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

// The side panels under the running bar: one at a time, each in a scrolling page with a title and
// a close button.
class PanelHost : public QFrame {
    Q_OBJECT
 public:
    explicit PanelHost(QWidget* parent = nullptr);
    // Adds a page and returns the layout its controls go in.
    QVBoxLayout* addPage(const QString& title);
    // Opens page `index`, or closes the panel for -1.
    void open(int index);
    int current() const { return index; }
    void setTitle(int page, const QString& title);
 signals:
    void changed(int index);
 private:
    QLabel* title;
    QStackedWidget* pages;
    QStringList titles;
    int index{-1};
};

// Applications: the installed apps with Launch, Export and Delete, and the buttons above them.
class AppsPanel : public QWidget {
    Q_OBJECT
 public:
    explicit AppsPanel(QWidget* parent = nullptr);
    // The RAM databases from `db-list`; only the applications are listed.
    void setDatabases(const std::vector<runningRules::Database>& all);
    QStringList names() const;
 signals:
    void installRequested();
    void otherRequested();
    void exportAllRequested();
    void launchRequested(const QString& name);
    void exportRequested(const QString& name);
    void deleteRequested(const QString& name);
 private:
    QListWidget* list;
    QLabel* empty;
};
