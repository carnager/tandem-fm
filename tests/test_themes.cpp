#include "core/Config.h"
#include "core/Keyring.h"
#include "ui/Theme.h"
#include "ui/ThemeProfiles.h"
#include "ui/ThemeManager.h"
#include "ui/FileColors.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/MainWindow.h"
#include "ui/Panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <sys/stat.h>
using namespace tandem;
namespace {
FileEntry file(const QString& name) { FileEntry e; e.name = name; e.mode = S_IFREG | 0644; return e; }
class PaintView : public FileView {
public:
    using FileView::drawRow;
};
}
class TestThemes : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        setKeyringHooksForTests({});
        QVERIFY(m_tmp.isValid());
        QString error;
        QVERIFY2(Config::instance().useFile(m_tmp.filePath("settings.ini"), &error, false), qPrintable(error));
    }
    void init()
    {
        qunsetenv("TANDEM_THEME");
        Settings().clear(); selectTheme("dark");
    }
    void builtInsAndSwitching()
    {
        QCOMPARE(themeProfiles().size(), 3);
        selectTheme("nord");
        QVERIFY(theme().dark);
        QCOMPARE(theme().base, QColor("#2e3440"));
        QCOMPARE(theme().altBase, QColor("#3b4252"));
        QCOMPARE(theme().accent, QColor("#88c0d0"));
        QCOMPARE(qApp->palette().color(QPalette::Base), theme().base);
        QCOMPARE(qApp->palette().color(QPalette::Highlight), theme().accent);
        selectTheme("light"); QVERIFY(!theme().dark);
        selectTheme("missing"); QCOMPARE(Settings().value("view/theme").toString(), QString("system"));
        qputenv("TANDEM_THEME", "nord"); applyTheme(*qApp); QCOMPARE(theme().base, QColor("#2e3440"));
        qunsetenv("TANDEM_THEME");
    }
    void roundTripAndIndependentProfiles()
    {
        auto first = duplicateTheme(*findThemeProfile("nord")); first.name = QString::fromUtf8("北の色");
        first.palette.alternatingRows = false; first.palette.selectionTextEnabled = true;
        first.palette.markBg = QColor("#30aabbcc");
        QString error;
        QVERIFY2(saveThemeProfile(first, &error), qPrintable(error));
        auto second = duplicateTheme(first); QVERIFY(saveThemeProfile(second));
        first.palette.base = QColor("#112233"); QVERIFY(saveThemeProfile(first));
        QCOMPARE(findThemeProfile(first.id)->palette.base, QColor("#112233"));
        QCOMPARE(findThemeProfile(second.id)->palette.base, QColor("#2e3440"));
        auto imported = importTheme(exportTheme(first), &error); QVERIFY2(imported.has_value(), qPrintable(error));
        QVERIFY(imported->id != first.id); QVERIFY(imported->name != first.name);
        QCOMPARE(imported->palette.markBg, first.palette.markBg);
        QVERIFY(!imported->palette.alternatingRows); QVERIFY(imported->palette.selectionTextEnabled);
        for (const auto& role : themeColorRoles()) QCOMPARE(imported->palette.*role.member, first.palette.*role.member);
        QVERIFY(saveThemeProfile(*imported));
        QSettings disk(Config::instance().fileName(), QSettings::IniFormat); disk.sync();
        QVERIFY(disk.value("themes/" + first.id).toString().contains(first.name));
        selectTheme(first.id); QCOMPARE(theme().base, first.palette.base);
        QVERIFY(removeThemeProfile(second.id)); QCOMPARE(theme().base, first.palette.base);
        QVERIFY(removeThemeProfile(first.id)); QCOMPARE(Settings().value("view/theme").toString(), QString("system"));
        QVERIFY(!removeThemeProfile("nord"));
    }
    void rejectsInvalidProfiles()
    {
        auto base = QJsonDocument::fromJson(exportTheme(*findThemeProfile("nord"))).object();
        QString error;
        QVERIFY(!importTheme("not json", &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!importTheme(QByteArray(256 * 1024 + 1, 'x'), &error));
        for (const auto& change : {QPair<QString, QJsonValue>{"version", 2}, {"dark", "yes"}, {"name", ""},
                                   {"alternatingRows", "yes"}, {"name", "bad\nname"}}) {
            auto obj = base; obj.insert(change.first, change.second);
            QVERIFY2(!importTheme(QJsonDocument(obj).toJson(), &error), qPrintable(change.first));
        }
        for (const auto& change : {QPair<QString, QJsonValue>{"base", "#00112233"}, {"text", "red"},
                                   {"base", "bogus"}, {"unexpected", "#112233"}, {"accent", 12}}) {
            auto obj = base; auto colors = obj["colors"].toObject(); colors.insert(change.first, change.second); obj["colors"] = colors;
            QVERIFY2(!importTheme(QJsonDocument(obj).toJson(), &error), qPrintable(change.first));
        }
        QVERIFY(!saveThemeProfile(*findThemeProfile("nord"), &error));
        auto custom = duplicateTheme(*findThemeProfile("nord")); custom.id = "../not-a-theme";
        QVERIFY(!saveThemeProfile(custom, &error));
        custom = duplicateTheme(*findThemeProfile("nord")); custom.name = "Nord";
        QVERIFY(!saveThemeProfile(custom, &error));
        QCOMPARE(themeProfiles().size(), 3);
        Settings().setValue("themes/custom-11111111-1111-1111-1111-111111111111", "broken json");
        QCOMPARE(themeProfiles().size(), 3);
    }
    void rowBackgroundsAndTextPrecedence()
    {
        auto custom = duplicateTheme(*findThemeProfile("dark"));
        auto& t = custom.palette;
        t.base = QColor("#112233"); t.altBase = QColor("#223344"); t.cursorActive = QColor("#334455");
        t.markBg = QColor("#445566"); t.mark = QColor("#ff9900"); t.extensionText = QColor("#aa99ff");
        t.selectionText = QColor("#ffffee"); t.selectionTextEnabled = true;
        QVERIFY(saveThemeProfile(custom)); selectTheme(custom.id);
        PaintView view; FileModel model;
        model.setListing(m_tmp.path(), {file("alpha.txt"), file("beta.pdf"), file("gamma.txt")});
        view.setModel(&model); view.resize(600, 200); view.setActive(true); view.show();
        view.setCurrentRow(model.rowOf("gamma.txt"));
        auto sampleRow = [&](const QString& name) {
            QImage image(view.viewport()->width(), 30, QImage::Format_ARGB32); image.fill(Qt::magenta);
            QPainter painter(&image); QStyleOptionViewItem option;
            option.rect = image.rect(); option.font = theme().listFont;
            view.drawRow(&painter, option, model.index(model.rowOf(name), 0));
            painter.end(); return image.pixelColor(image.width() - 12, 15);
        };
        QCOMPARE(sampleRow("alpha.txt"), t.altBase); // parent entry occupies row 0
        QCOMPARE(sampleRow("beta.pdf"), t.base);
        model.setMarked(model.rowOf("beta.pdf"), true);
        QCOMPARE(sampleRow("beta.pdf"), t.markBg);
        custom.palette.alternatingRows = false; QVERIFY(saveThemeProfile(custom)); selectTheme(custom.id);
        QCOMPARE(sampleRow("alpha.txt"), t.base);
        auto e = file("alpha.txt");
        QCOMPARE(FileColors::instance().nameColor(e, 0, true), t.extensionText);
        FileColorRule rule; rule.value = "txt"; rule.dark = QColor("#bb3344"); FileColors::saveRules({rule});
        QCOMPARE(FileColors::instance().nameColor(e, 0, true), rule.dark);
        QImage image(600, 30, QImage::Format_ARGB32); image.fill(t.base); QPainter painter(&image);
        QStyleOptionViewItem option; option.rect = image.rect(); option.font = theme().listFont;
        auto paint = [&](int column) { view.itemDelegate()->paint(&painter, option, model.index(model.rowOf("gamma.txt"), column)); return painter.pen().color(); };
        QCOMPARE(paint(FileModel::ColName), t.selectionText);
        QCOMPARE(paint(FileModel::ColExt), t.selectionText);
        model.setMarked(model.rowOf("gamma.txt"), true);
        QCOMPARE(paint(FileModel::ColName), t.mark);
        auto broken = file("broken"); broken.brokenLink = true;
        QCOMPARE(FileColors::instance().nameColor(broken), t.error);
    }
    void themeMenuAndEditor()
    {
        QMenu menu; populateThemeMenu(&menu, &menu);
        QAction* nord = nullptr;
        for (auto* action : menu.actions()) if (action->data().toString() == "nord") nord = action;
        QVERIFY(nord); nord->trigger(); QCOMPARE(theme().base, QColor("#2e3440"));
        bool editorOpened = false;
        QTimer::singleShot(50, [&] {
            auto* manager = QApplication::activeModalWidget();
            if (!manager) return;
            auto* add = manager->findChild<QPushButton*>("addTheme"); if (!add) { manager->close(); return; }
            QTimer::singleShot(50, [&] {
                auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!editor || editor->objectName() != "themeEditor") return;
                editorOpened = true;
                editor->findChild<QLineEdit*>("themeName")->setText("My Nord");
                editor->findChild<QLineEdit*>("themeColor_base")->setText("broken");
                auto* buttons = editor->findChild<QDialogButtonBox*>();
                QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
                editor->findChild<QLineEdit*>("themeColor_base")->setText("#123456");
                editor->findChild<QCheckBox*>("themeAlternatingRows")->setChecked(false);
                QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
                buttons->button(QDialogButtonBox::Save)->click();
            });
            add->click(); manager->close();
        });
        showThemeManager(nullptr);
        QVERIFY(editorOpened); QCOMPARE(theme().base, QColor("#123456")); QVERIFY(!theme().alternatingRows);
        const QString id = Settings().value("view/theme").toString();
        QVERIFY(findThemeProfile(id)); QCOMPARE(findThemeProfile(id)->name, QString("My Nord"));
        populateThemeMenu(&menu, &menu);
        bool customChecked = false;
        for (auto* action : menu.actions()) if (action->data().toString() == id) customChecked = action->isChecked();
        QVERIFY(customChecked);
        QTimer::singleShot(50, [&] {
            auto* manager = QApplication::activeModalWidget(); if (!manager) return;
            auto* edit = manager->findChild<QPushButton*>("editTheme");
            if (!edit) { manager->close(); return; }
            QTimer::singleShot(50, [] {
                auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!editor) return;
                editor->findChild<QLineEdit*>("themeColor_base")->setText("#654321"); editor->reject();
            });
            edit->click(); manager->close();
        });
        showThemeManager(nullptr);
        QCOMPARE(findThemeProfile(id)->palette.base, QColor("#123456"));
        QCOMPARE(theme().base, QColor("#123456"));
    }
    void syncedThemeReload()
    {
        auto custom = duplicateTheme(*findThemeProfile("nord")); QVERIFY(saveThemeProfile(custom)); selectTheme(custom.id);
        MainWindow window({m_tmp.path(), m_tmp.path()}); window.show();
        auto* left = window.findChild<Panel*>("leftPanel"); QVERIFY(left);
        const QString path = left->path();
        custom.palette.base = QColor("#142536"); custom.palette.alternatingRows = false;
        {
            QSettings external(Config::instance().fileName(), QSettings::IniFormat);
            external.setValue("themes/" + custom.id, QString::fromUtf8(exportTheme(custom))); external.sync();
        }
        Config::instance().checkForChanges();
        QTRY_COMPARE(theme().base, QColor("#142536")); QVERIFY(!theme().alternatingRows);
        QCOMPARE(left->path(), path);
        window.close();
    }
};
QTEST_MAIN(TestThemes)
#include "test_themes.moc"
