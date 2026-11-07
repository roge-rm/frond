#include "LicencesDialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QVBoxLayout>

static QString resourceText(const QString& name) {
    QFile file(":/licences/" + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString thirdPartyNotices() {
    Q_INIT_RESOURCE(licences);
    QString text = QObject::tr("Frond is free software under the GNU General Public License, version 3 or later, "
                               "with no warranty. The full text is in the LICENSE file. Qt, SDL and libslirp are "
                               "system libraries with their own licences.\n\nFrond also contains:\n");
    for (const QString& line : resourceText("index.tsv").split('\n', Qt::SkipEmptyParts)) {
        const QStringList fields = line.split('\t');
        if (fields.size() < 4 || !fields[3].contains('d')) continue;
        text += "\n\n" + QString(72, '=') + "\n" + fields[1] + " (" + fields[2] + ")\n" + QString(72, '=') + "\n";
        for (const QString& file : fields[0].split(',')) text += "\n" + resourceText(file).trimmed() + "\n";
    }
    return text;
}

void showLicences(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Licences"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* view = new QPlainTextEdit(&dialog);
    view->setObjectName("licencesText");
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setPlainText(thirdPartyNotices());
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(720, 520);
    dialog.exec();
}
