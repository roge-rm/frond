#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QUuid>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include "CardStore.h"
#include "DesktopClient.h"

namespace frondDanaCards {
inline QString quote(QString path) { path.replace("\\","\\\\"); path.replace("\"","\\\""); return '"'+path+'"'; }
inline QList<int> sizes(const DeviceProfile5& profile, DeviceCardSlot5 slot, DeviceCardFamily5 family) {
    QList<int> result;
    for (int size : {32,64,128,256,512,1024,1886})
        if (deviceProfileCardSizeValid5(profile.deviceType,slot,family,quint64(size)<<20)) result.append(size);
    return result;
}
// Whether a .frondsd card's payload is a size this slot takes for the family.
inline bool fits(const QString& path, DeviceCardSlot5 slot, DeviceCardFamily5 family) {
    qint64 logical=0;
    if (!frondCards::cardHeader(path,nullptr,&logical) || logical<=0) return false;
    return deviceProfileCardSizeValid5(deviceTypeDana,slot,family,quint64(logical));
}
struct State {
    bool known{}, present{}, inserted{}, protectedCard{}, failed{};
    QString identity, protocol;
};
inline State state(const QString& text) {
    if (text=="none") return {true};
    const auto fields=text.split(' ');
    if (fields.size()<3 || fields.size()>5 || (fields[0]!="inserted" && fields[0]!="ejected") ||
        !QRegularExpression("^[0-9a-f]{32}$").match(fields[1]).hasMatch() ||
        (fields[2]!="protected" && fields[2]!="writable")) return {};
    QSet<QString> flags;
    for (int i=3;i<fields.size();++i) {
        if ((fields[i]!="mmc" && fields[i]!="io-error") || flags.contains(fields[i])) return {};
        flags.insert(fields[i]);
    }
    return {true,true,fields[0]=="inserted",fields[2]=="protected",flags.contains("io-error"),fields[1],flags.contains("mmc")?"mmc":"sd"};
}
}

class DanaCardsDialog : public QDialog {
 public:
    DanaCardsDialog(DesktopClient& client, const QString& tool, QWidget* parent=nullptr) : QDialog(parent), client(client), tool(tool) {
        setWindowTitle(tr("Dana cards"));
        auto* layout=new QVBoxLayout(this);
        for (int n=0;n<2;++n) {
            status[n]=new QLabel(this); layout->addWidget(status[n]);
            auto* row=new QHBoxLayout; layout->addLayout(row);
            for (const auto& operation : {QString("Choose card"),QString("Eject"),QString("Insert"),QString("Remove"),QString("Export")}) {
                auto* button=new QPushButton(operation,this); row->addWidget(button);
                connect(button,&QPushButton::clicked,this,[this,n,operation] {
                    if (operation=="Choose card") choose(n);
                    else if (operation=="Export") {
                        const auto path=QFileDialog::getSaveFileName(this,tr("Export card"),QString("Dana card %1.frondsd").arg(n+1),tr("Frond SD cards (*.frondsd)"));
                        if (!path.isEmpty()) {
                            if (QFileInfo::exists(path)) QMessageBox::warning(this,tr("Card"),tr("Choose a new file. The core won't replace an existing card."));
                            else perform(n,"export",frondDanaCards::quote(path));
                        }
                    } else if (operation=="Eject") {
                        if (QMessageBox::question(this,tr("Eject card"),tr("Close files open on the Palm first."))==QMessageBox::Yes) perform(n,"eject");
                    } else perform(n,operation=="Insert"?"insert":"take-out");
                    refresh();
                });
                buttons[n].append(button);
            }
        }
        auto* portable=new QPushButton(tr("Export portable session folder"),this); layout->addWidget(portable);
        connect(portable,&QPushButton::clicked,this,[this] {
            const auto path=QFileDialog::getSaveFileName(this,tr("New portable folder"),QString(),tr("Portable folder (*)"),nullptr,QFileDialog::DontConfirmOverwrite);
            if (!path.isEmpty()) {
                const auto result=request("export-portable",path);
                if (!result.ok) QMessageBox::warning(this,tr("Portable session"),result.message);
            }
        });
        auto* close=new QDialogButtonBox(QDialogButtonBox::Close,this); layout->addWidget(close);
        connect(close,&QDialogButtonBox::rejected,this,&QDialog::reject);
        QTimer::singleShot(0,this,[this] { refresh(); });
    }
 public:
    struct Reply { bool ok{}; QString message; };
 private:
    DesktopClient& client;
    QString tool;
    QLabel* status[2]{};
    QList<QPushButton*> buttons[2];
    frondDanaCards::State states[2];
    QString root() const {
        const auto path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/dana-cards";
        QDir().mkpath(path); return path;
    }
 public:
    static Reply requestWithClient(DesktopClient& client, const QString& command,const QString& argument={}) {
        QEventLoop loop; QTimer poll,deadline; Reply out{false,tr("Couldn't reach the Palm.")}; bool sent=false;
        deadline.setSingleShot(true); deadline.start(120000);
        const auto done=connect(&client,&DesktopClient::result,&loop,[&](const QString& name,bool ok,const QString&,const QString& message) {
            if (sent && name==command) { out={ok,message}; loop.quit(); }
        });
        connect(&client,&DesktopClient::finished,&loop,&QEventLoop::quit);
        connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
        auto send=[&] {
            if (!sent && client.available()) {
                client.setProperty("danaCardRequest",command);
                sent=client.request(command,argument);
            }
        };
        connect(&poll,&QTimer::timeout,&loop,send); poll.start(50); send(); loop.exec(); disconnect(done);
        client.setProperty("danaCardRequest",QVariant());
        return out;
    }
 private:
    Reply request(const QString& command,const QString& argument={}) { return requestWithClient(client,command,argument); }
    bool perform(int n,const QString& operation,const QString& argument={}) {
        const auto result=request(n?"sd2-"+operation:"sd-"+operation,argument);
        if (!result.ok) QMessageBox::warning(this,tr("Card"),result.message);
        return result.ok;
    }
    void refresh() {
        for (int n=1;n>=0;--n) {
            const auto reply=request(n?"sd2-state":"sd-state");
            states[n]=reply.ok?frondDanaCards::state(reply.message):frondDanaCards::State{};
            const auto& state=states[n];
            const auto label=!state.known?tr("Not known"):!state.present?tr("No card"):
                QString("%1 · %2%3%4").arg(state.protocol=="mmc"?"MMC":"SD",state.inserted?tr("Inserted"):tr("Ejected"),
                    state.protectedCard?tr(" · Write protected"):QString(),state.failed?tr(" · I/O error"):QString());
            status[n]->setText(tr("Card %1: %2").arg(n+1).arg(label));
            buttons[n][0]->setEnabled(state.known && !state.present);
            buttons[n][1]->setEnabled(state.present && state.inserted);
            buttons[n][2]->setEnabled(state.present && !state.inserted);
            buttons[n][3]->setEnabled(state.present && !state.inserted && !state.failed);
            buttons[n][4]->setEnabled(state.present && !state.failed);
        }
    }
    void choose(int n) {
        QDialog dialog(this); dialog.setWindowTitle(tr("Card %1").arg(n+1));
        auto* layout=new QFormLayout(&dialog);
        QComboBox family,size,existing; QCheckBox protect(tr("Write protected"));
        family.addItem("SD","sd"); family.addItem("MMC","mmc");
        layout->addRow(tr("Family"),&family); layout->addRow(tr("Size"),&size); layout->addRow(tr("Saved card"),&existing); layout->addRow(&protect);
        auto update=[&] {
            size.clear(); existing.clear();
            for (auto bytes : frondDanaCards::sizes(*deviceProfile5(deviceTypeDana),DeviceCardSlot5(n?2:0),
                    family.currentData()=="mmc"?DeviceCardFamily5::legacyMmc:DeviceCardFamily5::sdsc)) size.addItem(QString::number(bytes)+" MB",bytes);
            for (const auto& file : QDir(root()).entryInfoList({"*.json"},QDir::Files)) {
                QFile meta(file.absoluteFilePath()); if (!meta.open(QIODevice::ReadOnly)) continue;
                const auto object=QJsonDocument::fromJson(meta.readAll()).object();
                if (object["protocol"].toString()==family.currentData().toString() && QFileInfo::exists(root()+'/'+file.completeBaseName()+".frondsd"))
                    existing.addItem(object["name"].toString(),file.completeBaseName());
            }
        };
        connect(&family,&QComboBox::currentIndexChanged,&dialog,[&] { update(); }); update();
        auto* actions=new QDialogButtonBox(&dialog); layout->addRow(actions);
        auto* attach=actions->addButton(tr("Attach saved"),QDialogButtonBox::ActionRole);
        auto* make=actions->addButton(tr("New card"),QDialogButtonBox::ActionRole);
        auto* import=actions->addButton(tr("Import image"),QDialogButtonBox::ActionRole);
        actions->addButton(QDialogButtonBox::Cancel); connect(actions,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        auto use=[&](QString id) {
            if (!QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch()) return false;
            if (!perform(n,"attach",frondDanaCards::quote(root()+'/'+id+".frondsd")+' '+id+' '+(protect.isChecked()?"protected":"writable")+' '+family.currentData().toString())) return false;
            dialog.accept();
            return true;
        };
        connect(attach,&QPushButton::clicked,&dialog,[&] { use(existing.currentData().toString()); });
        auto create=[&](bool importing) {
            QString source;
            if (importing) { source=QFileDialog::getOpenFileName(&dialog,tr("Import card image")); if (source.isEmpty()) return; }
            const auto id=QUuid::createUuid().toString(QUuid::Id128);
            const auto path=root()+'/'+id+".frondsd";
            frondCards::ToolResult result;
            QFile input(source); QByteArray head;
            if (importing && input.open(QIODevice::ReadOnly)) head=input.read(8);
            if (head==QByteArray("FRONDSD\0",8)) {
                const int fd=::open(QFile::encodeName(source).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
                bool standalone=fd>=0 && ::flock(fd,LOCK_EX|LOCK_NB)==0;
                const QFileInfo info(source);
                for (const auto& name : QDir(info.absolutePath()).entryList(QDir::Files|QDir::Hidden))
                    if (name.startsWith(".gen-"+info.fileName()+"-")) standalone=false;
                result={standalone && QFile::copy(source,path),{},tr("Export this card from its running machine before importing it.")};
                if (fd>=0) ::close(fd);
            }
            else result=frondCards::runCardTool(frondCards::cardTool(tool),importing?QStringList{"import",source,"sd",path}:QStringList{"make","sd",size.currentData().toString(),path});
            if (!result.ok) { QFile::remove(path); QMessageBox::warning(&dialog,tr("Card"),result.message); return; }
            if (!frondDanaCards::fits(path,DeviceCardSlot5(n?2:0),family.currentData()=="mmc"?DeviceCardFamily5::legacyMmc:DeviceCardFamily5::sdsc)) {
                QFile::remove(path); QMessageBox::warning(&dialog,tr("Card"),tr("That size isn't supported by this card family.")); return;
            }
            QFile meta(root()+'/'+id+".json");
            if (!meta.open(QIODevice::WriteOnly) || meta.write(QJsonDocument(QJsonObject{{"protocol",family.currentData().toString()},{"name",importing?QFileInfo(source).fileName():family.currentText()+" "+size.currentText()}}).toJson())<0) {
                meta.remove(); QFile::remove(path); QMessageBox::warning(&dialog,tr("Card"),tr("Couldn't save card details.")); return;
            }
            meta.close();
            // A card the first attach refuses would stay in the library and never attach.
            if (!use(id)) { QFile::remove(meta.fileName()); QFile::remove(path); }
        };
        connect(make,&QPushButton::clicked,&dialog,[&] { create(false); });
        connect(import,&QPushButton::clicked,&dialog,[&] { create(true); });
        dialog.exec();
    }
};
