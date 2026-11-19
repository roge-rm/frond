#include <QtTest>
#include <QTemporaryDir>
#include "SessionStore.h"
#include "LinkedCards.h"

class DanaCardsTest : public QObject {
    Q_OBJECT
    const QByteArray key="12345678000000000000000000000000", second="23456789000000000000000000000000";
    QByteArray manifest(bool portable=false) {
        if (portable) return "frond-portable-sd 3\n0 sd sd0.frondsd "+key+" 305419896 33554432 1 0 123 sd\n2 sd sd2.frondsd "+second+" 591751049 33554432 0 1 456 mmc\n";
        return "frond-linked-sd 3\n0 sd \"/cards/a.frondsd\" "+key+" 305419896 33554432 1 0 1 2 3 sd 9\n2 sd \"/cards/b.frondsd\" "+second+" 591751049 33554432 0 1 1 4 5 mmc 7\n";
    }
    void write(const QString& path, const QByteArray& manifest,bool portable=false,const QByteArray& name="alphasmart-dana",quint32 version=8) {
        QByteArray machine(80,'\0');
        auto put=[](QByteArray& bytes,int at,quint32 value) { for (int n=0;n<4;++n) bytes[at+n]=char(value>>(8*n)); };
        put(machine,0,frondSessions::magic); put(machine,4,version); machine.replace(8,14,name);
        QByteArray descriptor(12,'\0'); put(descriptor,0,manifest.size()); put(descriptor,4,frondDanaSession::hash(machine)); put(descriptor,8,frondDanaSession::hash(manifest));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(portable?"FRONDPORTSD1\n":"FRONDLINKSD1\n"); file.write(descriptor); file.write(manifest); file.write(machine);
    }
 private slots:
    void inventory() {
        const auto* dana=deviceProfile5(deviceTypeDana);
        QVERIFY(dana->frontend.available);
        const auto device=frondFirmware::deviceFromId("alphasmart-dana");
        QVERIFY(device!=frondFirmware::Device::automatic);
        QVERIFY(dana->capabilities.keyboard); QVERIFY(!frondFirmware::infraredKeyboard(device)); QVERIFY(dana->capabilities.infrared);
        QVERIFY(frondFirmware::infraredKeyboard(frondFirmware::Device::tx));
        for (auto slot : {DeviceCardSlot5::sd0,DeviceCardSlot5::sd2}) {
            QCOMPARE(frondDanaCards::sizes(*dana,slot,DeviceCardFamily5::sdsc),QList<int>({32,64,128,256,512}));
            QCOMPARE(frondDanaCards::sizes(*dana,slot,DeviceCardFamily5::legacyMmc),QList<int>({32,64,128,256}));
        }
    }
    void importedCardSizeMatchesFamily() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto card=[&](quint64 payload) {
            QByteArray h("FRONDSD\0",8); h.append(QByteArray(56,'\0'));
            h[8]=1; h[13]=2;
            for (int n=0;n<8;++n) h[16+n]=char(payload>>(8*n));
            const auto path=dir.filePath(QString::number(payload)+".frondsd");
            QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(h);
            return path;
        };
        for (auto slot : {DeviceCardSlot5::sd0,DeviceCardSlot5::sd2}) {
            QVERIFY(frondDanaCards::fits(card(512ull<<20),slot,DeviceCardFamily5::sdsc));
            QVERIFY(!frondDanaCards::fits(card(512ull<<20),slot,DeviceCardFamily5::legacyMmc));
            QVERIFY(frondDanaCards::fits(card(256ull<<20),slot,DeviceCardFamily5::legacyMmc));
            QVERIFY(!frondDanaCards::fits(card(1024ull<<20),slot,DeviceCardFamily5::sdsc));
        }
        QVERIFY(!frondDanaCards::fits(dir.filePath("missing.frondsd"),DeviceCardSlot5::sd0,DeviceCardFamily5::sdsc));
    }
    void wirelessSessionHeader() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path=dir.path()+"/machine.frond";
        write(path,manifest(),false,"alphasmart-dana-wireless");
        QVERIFY(frondDanaSession::inspect(path).valid);
        const auto header=frondSessions::readHeader(path);
        QCOMPARE(header.device,QString("alphasmart-dana-wireless"));
        QVERIFY(frondSessions::openable(header));
        write(path,manifest(),false,"alphasmart-dana-other");
        QVERIFY(!frondDanaSession::inspect(path).valid);
    }
    void currentVersionWrapper() {
        // The core saves wrapped Dana sessions at the current session version.
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path=dir.path()+"/machine.frond";
        for (bool portable : {false,true}) {
            write(path,manifest(portable),portable,"alphasmart-dana",frondSessions::newestVersion);
            QVERIFY(frondDanaSession::inspect(path).valid);
            QVERIFY(frondSessions::openable(frondSessions::readHeader(path)));
        }
        write(path,manifest(),false,"alphasmart-dana",frondSessions::newestVersion+1);
        QVERIFY(!frondSessions::openable(frondSessions::readHeader(path)));
        write(path,manifest(),false,"alphasmart-dana",5);
        QVERIFY(!frondDanaSession::inspect(path).valid);
    }
    void refusalMessages() {
        // Core refusals the user can meet get plain text; anything else passes through.
        QCOMPARE(frondDanaCards::message("error: duplicate identity or serial"),QString("That card is already in the other slot."));
        QCOMPARE(frondDanaCards::message("error: cannot lock card file"),QString("That card is open somewhere else."));
        QCOMPARE(frondDanaCards::message("error: something new"),QString("error: something new"));
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(frondDanaCards::exportProblem(dir.filePath("card.frondsd")).isEmpty());
        QFile existing(dir.filePath("old.frondsd")); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.close();
        QVERIFY(frondDanaCards::exportProblem(existing.fileName()).contains("new file"));
        QVERIFY(QDir(dir.path()).mkdir("locked"));
        QVERIFY(QFile::setPermissions(dir.filePath("locked"),QFile::ReadOwner|QFile::ExeOwner));
        QCOMPARE(frondDanaCards::exportProblem(dir.filePath("locked/card.frondsd")),QString("Can't write to that folder."));
        QVERIFY(QFile::setPermissions(dir.filePath("locked"),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
    }
    void state() {
        const auto card=frondDanaCards::state("ejected "+key+" protected io-error mmc");
        QVERIFY(card.known && card.present && !card.inserted && card.protectedCard && card.failed); QCOMPARE(card.protocol,QString("mmc"));
        QVERIFY(frondDanaCards::state("none").known);
        for (auto text : {"inserted", "ejected bad protected", "inserted 12345678000000000000000000000000 writable mmc mmc"}) QVERIFY(!frondDanaCards::state(text).known);
    }
    void wrappedReopenAndTransferGuards() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path=dir.path()+"/machine.frond";
        for (bool portable : {false,true}) {
            write(path,manifest(portable),portable);
            const auto inspection=frondDanaSession::inspect(path);
            QVERIFY(inspection.valid); QCOMPARE(inspection.portable,portable); QCOMPARE(inspection.version,8u);
            QVERIFY(frondSessions::openable(frondSessions::readHeader(path)));
            QVERIFY(!frondSessions::importFile(dir.path()+"/sessions",path).isEmpty());
            frondSessions::Info info; info.path=path; info.saved=true;
            QVERIFY(!frondSessions::exportFile(info,dir.path()+"/copy").isEmpty());
            QVERIFY(!frondSessions::duplicate(dir.path()+"/sessions",info).isEmpty());
            QVERIFY(!QFileInfo::exists(dir.path()+"/copy"));
        }
    }
    void malformedWrappers() {
        QTemporaryDir dir; const auto path=dir.path()+"/machine.frond";
        for (const auto& text : {manifest().replace("sd 3","sd 2"),manifest().replace("2 sd","0 sd"),manifest().replace(second,key),
                                 manifest().replace("33554432","1073741824"),manifest().replace(" mmc "," unknown ")}) {
            write(path,text); QVERIFY(!frondDanaSession::inspect(path).valid); QVERIFY(!frondSessions::openable(frondSessions::readHeader(path)));
        }
        write(path,manifest()); QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(file.size()-1)); file.write("x"); file.close();
        QVERIFY(!frondDanaSession::inspect(path).valid);
    }
    // The Dana's two slots, a VZ Palm's one and the T415's stick take library cards, MMC included.
    void librarySlots() {
        const auto dana=frondFirmware::deviceFromId("alphasmart-dana"), m125=frondFirmware::deviceFromId("palm-m125"),
                   t415=frondFirmware::deviceFromId("sony-clie-t415"), tx=frondFirmware::deviceFromId("palm-tungsten-tx");
        QVERIFY(frondCards::secondSlot(dana)); QVERIFY(!frondCards::secondSlot(m125)); QVERIFY(!frondCards::secondSlot(t415));
        for (int n : {0,1}) {
            QVERIFY(frondCards::deviceTakes(dana,"sd",32,nullptr,n)); QVERIFY(frondCards::deviceTakes(dana,"mmc",256,nullptr,n));
            bool big=false;
            QVERIFY(!frondCards::deviceTakes(dana,"mmc",512,&big,n)); QVERIFY(big);
            QVERIFY(!frondCards::deviceTakesKind(dana,"ms",n));
        }
        QVERIFY(frondCards::deviceTakes(m125,"sd",64)); QVERIFY(!frondCards::deviceTakesKind(m125,"sd",1));
        QVERIFY(frondCards::deviceTakesKind(t415,"ms")); QVERIFY(!frondCards::deviceTakesKind(t415,"sd")); QVERIFY(!frondCards::deviceTakesKind(t415,"ms",1));
        QVERIFY(!frondCards::deviceTakesKind(tx,"mmc"));
        QVERIFY(frondCards::typesTaken().contains("mmc"));
        QCOMPARE(frondCards::sizesFor("mmc"),QList<int>({32,64,128,256}));
        // card.json keeps the type and key; a card without them is an SD card with no key.
        QTemporaryDir dir; const auto entry=dir.filePath("abc");
        QVERIFY(QDir().mkpath(entry));
        frondCards::Card card{"abc","Notes","sd",frondCards::now(),{},32ll<<20,frondCards::newIdentity(),true};
        card.type="mmc"; card.key=QString(key);
        QVERIFY(frondCards::writeCard(entry,card));
        frondCards::Card back; QVERIFY(frondCards::readCard(entry,&back));
        QCOMPARE(back.type,QString("mmc")); QCOMPARE(back.key,QString(key)); QCOMPARE(frondCards::takeKind(back),QString("mmc"));
        QCOMPARE(frondCards::cardSizeText(back),QString("MMC · 32 MB"));
        QFile json(entry+"/card.json"); QVERIFY(json.open(QIODevice::ReadOnly));
        auto o=QJsonDocument::fromJson(json.readAll()).object(); json.close();
        o.remove("type"); o.remove("key");
        QVERIFY(json.open(QIODevice::WriteOnly)); json.write(QJsonDocument(o).toJson()); json.close();
        QVERIFY(frondCards::readCard(entry,&back)); QCOMPARE(back.type,QString("sd")); QVERIFY(back.key.isEmpty());
    }
    // A Dana's slot holding a library card: frond-68k gets --card-swap only and the card by path,
    // with a key made and kept the first time.
    void attachFromLibrary() {
        QTemporaryDir root; const auto sessions=root.filePath("sessions"), dir=sessions+"/Dana";
        QVERIFY(QDir().mkpath(dir)); QVERIFY(QDir().mkpath(root.filePath("cards/c1")));
        write(dir+"/Dana.frond",manifest());
        frondCards::Card card{"c1","Notes","sd",frondCards::now(),"Dana",64ll<<20,frondCards::newIdentity(),false};
        card.type="mmc";
        QVERIFY(frondCards::writeCard(root.filePath("cards/c1"),card));
        QFile file(dir+"/sd2.frondsd"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QVERIFY(frondCards::writeSlot(dir,{true,"c1",{},"Notes",true},1));
        QCOMPARE(frondCards::launchArguments(sessions,dir,true),QStringList{"--card-swap"});
        const auto argument=frondCards::attachArgument(sessions,dir,1);
        QVERIFY(frondCards::readCard(root.filePath("cards/c1"),&card)); QVERIFY(frondCards::validKey(card.key));
        QCOMPARE(argument,frondCards::quotePath(dir+"/sd2.frondsd")+' '+card.key+" writable mmc");
        QCOMPARE(frondCards::attachArgument(sessions,dir,1),argument);
        QVERIFY(frondCards::attachArgument(sessions,dir,0).isEmpty());
        // The manifest's slot 2 row describes card 2, so a save that linked it clears the swap.
        frondCards::clearSwap(dir); QVERIFY(!frondCards::readSlot(dir,1).swap);
        QCOMPARE(frondCards::quotePath("/a \"b\"\\c"),QString("\"/a \\\"b\\\"\\\\c\""));
    }
    // dana-cards/ cards linked from a save move into the machine folder with their saved changes,
    // once, and never over a file; a card with an unfinished write stays where it is.
    void moveFromDanaCards() {
        const auto tool=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../../out/frond");
        if (!QFileInfo(tool).isExecutable()) QSKIP("needs out/frond");
        QTemporaryDir root; const auto sessions=root.filePath("sessions"), dir=sessions+"/Dana", old=root.filePath("Frond/dana-cards");
        QVERIFY(QDir().mkpath(dir)); QVERIFY(QDir().mkpath(old));
        const QByteArray keys[2]={key,second};
        for (const auto& k : keys) {
            QProcess p; p.start(tool,{"--card","make","sd","32",old+'/'+k+".frondsd"}); QVERIFY(p.waitForFinished(60000)); QCOMPARE(p.exitCode(),0);
            QFile meta(old+'/'+k+".json"); QVERIFY(meta.open(QIODevice::WriteOnly)); meta.write("{\"protocol\":\"sd\",\"name\":\"Notes "+k.left(2)+"\"}"); meta.close();
        }
        // Card 1 has saved changes; card 2 a write cut off.
        for (const auto* name : {"-root","-1","-1.idx"}) { QFile g(old+"/.gen-"+key+".frondsd"+name); QVERIFY(g.open(QIODevice::WriteOnly)); g.write("x"); }
        { QFile g(old+"/.gen-"+second+".frondsd-tmp-1-2"); QVERIFY(g.open(QIODevice::WriteOnly)); }
        auto text=manifest(); text.replace("/cards/a.frondsd",(old+'/'+key+".frondsd").toUtf8()).replace("/cards/b.frondsd",(old+'/'+second+".frondsd").toUtf8());
        write(dir+"/Dana.frond",text);
        QCOMPARE(frondCards::moveLinkedIn(sessions,dir,old,tool),1);
        QVERIFY(!QFileInfo::exists(old+'/'+key+".frondsd")); QVERIFY(QFileInfo::exists(dir+"/sd.frondsd"));
        for (const auto* name : {"-root","-1","-1.idx"}) QVERIFY(QFileInfo::exists(dir+"/.gen-sd.frondsd"+QString(name)));
        QVERIFY(QFileInfo::exists(old+'/'+second+".frondsd")); QVERIFY(!QFileInfo::exists(dir+"/sd2.frondsd"));
        const auto slot=frondCards::readSlot(dir,0); QVERIFY(!slot.card.isEmpty()); QVERIFY(!slot.swap);
        frondCards::Card card; QVERIFY(frondCards::readCard(root.filePath("cards/"+slot.card),&card));
        QCOMPARE(card.key,QString(key)); QCOMPARE(card.name,QString("Notes 12")); QVERIFY(card.writeProtected); QCOMPARE(card.bytes,32ll<<20);
        QVERIFY(!QFileInfo::exists(old+'/'+key+".json")); QVERIFY(QFileInfo::exists(old+'/'+second+".json"));
        QFile log(dir+"/card-moves.log"); QVERIFY(log.open(QIODevice::ReadOnly));
        const auto lines=QString::fromUtf8(log.readAll()).split('\n',Qt::SkipEmptyParts); log.close();
        QCOMPARE(lines.size(),2); QVERIFY(lines[0].contains("moved "+key)); QVERIFY(lines[1].contains("left "+second));
        // The write finished: card 2 moves now. A file already in its place is never replaced.
        QVERIFY(QFile::remove(old+"/.gen-"+second+".frondsd-tmp-1-2"));
        { QFile in(dir+"/sd2.frondsd"); QVERIFY(in.open(QIODevice::WriteOnly)); in.write("other"); }
        QCOMPARE(frondCards::moveLinkedIn(sessions,dir,old,tool),0);
        QVERIFY(QFileInfo::exists(old+'/'+second+".frondsd")); QCOMPARE(QFileInfo(dir+"/sd2.frondsd").size(),5);
        QVERIFY(QFile::remove(dir+"/sd2.frondsd"));
        QCOMPARE(frondCards::moveLinkedIn(sessions,dir,old,tool),1);
        QVERIFY(QFileInfo::exists(dir+"/sd2.frondsd")); QVERIFY(!QFileInfo::exists(old+'/'+second+".frondsd"));
        QVERIFY(frondCards::readCard(root.filePath("cards/"+frondCards::readSlot(dir,1).card),&card)); QCOMPARE(card.type,QString("mmc"));
        QCOMPARE(frondCards::moveLinkedIn(sessions,dir,old,tool),0);
        // Reconcile keeps both in the machine: the save links them.
        QVERIFY(frondCards::reconcile(sessions).isEmpty());
        QVERIFY(QFileInfo::exists(dir+"/sd.frondsd")); QVERIFY(QFileInfo::exists(dir+"/sd2.frondsd"));
        QCOMPARE(frondCards::entries(root.filePath("cards")).size(),2);
    }
    void unlinkedToDrawer() {
        const auto tool=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../../out/frond");
        if (!QFileInfo(tool).isExecutable()) QSKIP("needs out/frond");
        QTemporaryDir root; const auto sessions=root.filePath("sessions"), dir=sessions+"/Dana", old=root.filePath("Frond/dana-cards");
        const auto library=root.filePath("cards");
        QVERIFY(QDir().mkpath(dir)); QVERIFY(QDir().mkpath(old));
        const QByteArray loose="34567890000000000000000000000000", busy="45678901000000000000000000000000";
        for (const auto& k : {key,second,loose,busy}) {
            QProcess p; p.start(tool,{"--card","make","sd","32",old+'/'+k+".frondsd"}); QVERIFY(p.waitForFinished(60000)); QCOMPARE(p.exitCode(),0);
            QFile meta(old+'/'+k+".json"); QVERIFY(meta.open(QIODevice::WriteOnly));
            meta.write("{\"protocol\":\""+QByteArray(k==loose?"mmc":"sd")+"\",\"name\":\"Notes "+k.left(2)+"\"}");
        }
        // The save links card 1; card 2's row points somewhere else.
        auto text=manifest(); text.replace("/cards/a.frondsd",(old+'/'+key+".frondsd").toUtf8());
        write(dir+"/Dana.frond",text);
        // The loose card has saved changes the card tool can't read (so it stays, with them); the
        // other one a write cut off.
        for (const auto* name : {"-root","-1","-1.idx"}) { QFile g(old+"/.gen-"+loose+".frondsd"+name); QVERIFY(g.open(QIODevice::WriteOnly)); g.write("x"); }
        { QFile g(old+"/.gen-"+busy+".frondsd-tmp-1-2"); QVERIFY(g.open(QIODevice::WriteOnly)); }
        QCOMPARE(frondCards::moveUnlinkedIn(sessions,old,tool),1);
        QVERIFY(QFileInfo::exists(old+'/'+key+".frondsd")); QVERIFY(QFileInfo::exists(old+'/'+key+".json"));
        QVERIFY(QFileInfo::exists(old+'/'+busy+".frondsd")); QVERIFY(QFileInfo::exists(old+'/'+busy+".json"));
        QVERIFY(QFileInfo::exists(old+'/'+loose+".frondsd")); QVERIFY(!QFileInfo::exists(old+'/'+second+".json"));
        for (const auto* name : {"-root","-1","-1.idx"}) QVERIFY(QFileInfo::exists(old+"/.gen-"+loose+".frondsd"+QString(name)));
        const auto all=frondCards::entries(library);
        QCOMPARE(all.size(),1);
        QVERIFY(all[0].inDrawer); QVERIFY(all[0].card.machine.isEmpty()); QCOMPARE(all[0].card.key,QString(second));
        // No machine links them.
        QVERIFY(frondCards::readSlot(dir,0).link.isEmpty()); QVERIFY(frondCards::readSlot(dir,1).link.isEmpty());
        QFile log(library+"/card-moves.log"); QVERIFY(log.open(QIODevice::ReadOnly));
        const auto lines=QString::fromUtf8(log.readAll()).split('\n',Qt::SkipEmptyParts);
        QCOMPARE(lines.size(),3);
        QVERIFY(lines.filter("left "+busy).size()==1); QVERIFY(lines.filter("left "+loose).size()==1);
        // Reconcile keeps the drawer card; a second run moves nothing more.
        QVERIFY(frondCards::reconcile(sessions).isEmpty());
        QCOMPARE(frondCards::entries(library).size(),1);
        QCOMPARE(frondCards::moveUnlinkedIn(sessions,old,tool),0);
    }
    void unlinkedMoveCutOff() {
        // A move into the drawer cut off after some saved changes went: reconcile keeps the entry
        // and the next start-up finishes the move into it, generations and all.
        const auto tool=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../../out/frond");
        if (!QFileInfo(tool).isExecutable()) QSKIP("needs out/frond");
        QTemporaryDir root; const auto sessions=root.filePath("sessions"), old=root.filePath("Frond/dana-cards");
        const auto library=root.filePath("cards");
        QVERIFY(QDir().mkpath(sessions)); QVERIFY(QDir().mkpath(old)); QVERIFY(QDir().mkpath(library));
        { QProcess p; p.start(tool,{"--card","make","sd","32",old+'/'+key+".frondsd"}); QVERIFY(p.waitForFinished(60000)); QCOMPARE(p.exitCode(),0); }
        for (const auto* name : {"-root","-1","-1.idx"}) { QFile g(old+"/.gen-"+key+".frondsd"+name); QVERIFY(g.open(QIODevice::WriteOnly)); g.write(name); }
        frondCards::Card card; card.id=frondCards::newId(); card.kind="sd"; card.name="Notes"; card.created=frondCards::now();
        card.identity=frondCards::newIdentity(); card.key=key; card.bytes=32ll<<20;
        const auto entry=frondCards::entryDir(library,card.id);
        QVERIFY(QDir().mkpath(entry)); QVERIFY(frondCards::writeCard(entry,card));
        QVERIFY(QFile::rename(old+"/.gen-"+key+".frondsd-1",entry+"/.gen-sd.frondsd-1"));
        QVERIFY(frondCards::reconcile(sessions).isEmpty());
        QVERIFY(QFileInfo::exists(entry+"/card.json")); QVERIFY(QFileInfo::exists(entry+"/.gen-sd.frondsd-1"));
        QCOMPARE(frondCards::moveUnlinkedIn(sessions,old,tool),1);
        QVERIFY(QFileInfo(entry+"/sd.frondsd").isFile()); QVERIFY(!QFileInfo::exists(old+'/'+key+".frondsd"));
        for (const auto* name : {"-root","-1","-1.idx"}) QVERIFY(QFileInfo::exists(entry+"/.gen-sd.frondsd"+QString(name)));
        QCOMPARE(frondCards::entries(library).size(),1);
        // Nothing more to move; reconcile keeps the drawer card.
        QCOMPARE(frondCards::moveUnlinkedIn(sessions,old,tool),0);
        QVERIFY(frondCards::reconcile(sessions).isEmpty());
        QCOMPARE(frondCards::entries(library).size(),1);
    }
    void sharedCardSecondSave() {
        // Two saves link one dana-cards card. The first to open moves it in; the second loads with
        // that slot swapped (empty) instead of being refused, and a save outside the sessions
        // folder linking it gets --card-swap too.
        const auto tool=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../../out/frond");
        if (!QFileInfo(tool).isExecutable()) QSKIP("needs out/frond");
        QTemporaryDir root; const auto sessions=root.filePath("sessions"), old=root.filePath("Frond/dana-cards");
        const auto a=sessions+"/Dana", b=sessions+"/Dana 2";
        QVERIFY(QDir().mkpath(a)); QVERIFY(QDir().mkpath(b)); QVERIFY(QDir().mkpath(old)); QVERIFY(QDir().mkpath(root.filePath("outside")));
        { QProcess p; p.start(tool,{"--card","make","sd","32",old+'/'+key+".frondsd"}); QVERIFY(p.waitForFinished(60000)); QCOMPARE(p.exitCode(),0); }
        auto text=manifest(); text.replace("/cards/a.frondsd",(old+'/'+key+".frondsd").toUtf8());
        text=text.left(text.indexOf("\n2 sd")+1);
        write(a+"/Dana.frond",text); write(b+"/Dana 2.frond",text); write(root.filePath("outside/Dana.frond"),text);
        QVERIFY(frondCards::outsideArguments(root.filePath("outside/Dana.frond")).isEmpty());
        QCOMPARE(frondCards::moveLinkedIn(sessions,a,old,tool),1);
        QVERIFY(QFileInfo::exists(a+"/sd.frondsd")); QVERIFY(!frondCards::readSlot(a,0).swap);
        QVERIFY(frondCards::launchArguments(sessions,a,true).isEmpty());
        QCOMPARE(frondCards::moveLinkedIn(sessions,b,old,tool),0);
        QVERIFY(frondCards::readSlot(b,0).swap); QVERIFY(frondCards::readSlot(b,0).card.isEmpty());
        QCOMPARE(frondCards::launchArguments(sessions,b,true),QStringList({"--card-swap"}));
        QFile log(b+"/card-moves.log"); QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(log.readAll()).contains("is in Cards as Dana card"));
        QCOMPARE(frondCards::outsideArguments(root.filePath("outside/Dana.frond")),QStringList({"--card-swap"}));
        // Opening again changes nothing more.
        QCOMPARE(frondCards::moveLinkedIn(sessions,b,old,tool),0);
        QVERIFY(log.seek(0)); QCOMPARE(QString::fromUtf8(log.readAll()).count('\n'),1);
    }
    // Run by tests/dana-card-migrate.py on a real frond-68k save: FROND_MIGRATE="sessions|machine|old".
    void migrateFixture() {
        const auto spec=qEnvironmentVariable("FROND_MIGRATE").split('|');
        if (spec.size()!=3) QSKIP("run by tests/dana-card-migrate.py");
        const auto tool=QDir::cleanPath(QCoreApplication::applicationDirPath()+"/../../out/frond");
        const int moved=frondCards::moveLinkedIn(spec[0],spec[1],spec[2],tool);
        QVERIFY(moved>0);
        QVERIFY(frondCards::reconcile(spec[0]).isEmpty());
        for (int n=0;n<2;++n) {
            const auto argument=frondCards::attachArgument(spec[0],spec[1],n);
            if (!argument.isEmpty()) std::printf("attach %d %s\n",n,qPrintable(argument));
        }
        std::printf("moved %d\n",moved); std::fflush(stdout);
    }
    void independentSocketCommandsAndUncertainExport() {
        QTemporaryDir dir; QFile fake(dir.filePath("core.py")); QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(R"PY(import socket,sys
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
expected=[b'sd2-state',b'sd-state',b'sd2-insert',b'sd2-eject',b'sd2-take-out',b'export-portable',b'quit']
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');assert cmd==expected.pop(0)
    message=b'none';ok=b'ok';code=b'card'
    if cmd==b'sd2-state':message=b'ejected 23456789000000000000000000000000 protected mmc'
    if cmd==b'export-portable':
        assert bytes.fromhex(arg.decode())==b'/new folder'
        ok=b'error';code=b'durability-uncertain';message=b'Output published, durability uncertain'
    s.sendall(i+b'\t'+ok+b'\t'+code+b'\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
assert not expected
)PY"); fake.close();
        DesktopClient client; QSignalSpy ready(&client,&DesktopClient::started),finished(&client,&DesktopClient::finished);
        QVERIFY(client.start("/usr/bin/python3",{fake.fileName()})); QTRY_COMPARE(ready.count(),1);
        const auto second=frondDanaCards::request(client,"sd2-state");
        QVERIFY(second.ok); QCOMPARE(frondDanaCards::state(second.message).protocol,QString("mmc"));
        const auto first=frondDanaCards::request(client,"sd-state"); QVERIFY(first.ok); QCOMPARE(first.message,QString("none"));
        for (const auto* op : {"sd2-insert","sd2-eject","sd2-take-out"}) QVERIFY(frondDanaCards::request(client,op).ok);
        const auto copy=frondDanaCards::request(client,"export-portable","/new folder"); QVERIFY(!copy.ok); QVERIFY(copy.message.contains("uncertain"));
        QVERIFY(client.request("quit")); QTRY_COMPARE(finished.count(),1);
    }
};
QTEST_MAIN(DanaCardsTest)
#include "desktop-dana-cards.moc"
