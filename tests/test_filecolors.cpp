#include "core/Config.h"
#include "ui/FileColors.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <sys/stat.h>
using namespace tandem;
namespace {
FileColorRule rule(const QString& match, const QString& value, const QString& light = "#1234ed", const QString& dark = "#edca12")
{
    FileColorRule out; out.match = match; out.value = value; out.light = QColor(light); out.dark = QColor(dark); return out;
}
FileEntry file(const QString& name)
{
    FileEntry entry; entry.name = name; entry.mode = S_IFREG | 0644; return entry;
}
QColor color(const FileEntry& entry, char git = 0) { return FileColors::instance().nameColor(entry, git); }
}
class TestFileColors : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init()
    {
        Settings().clear();
        Settings().setValue("view/theme", "light");
        applyTheme(*qApp);
    }
    void extensionListsCaseAndCompoundSuffixes()
    {
        const auto pdf = rule("extension", ".PDF; *.tar.gz, epub");
        QVERIFY(pdf.error().isEmpty()); FileColors::saveRules({pdf});
        for (const QString& name : {QString("report.PdF"), QString::fromUtf8("日本語.pdf"), QString("archive.tar.GZ"), QString("book.epub")})
            QCOMPARE(color(file(name)), pdf.light);
        QCOMPARE(color(file("archive.tar.gz.bak")), theme().text);
        QCOMPARE(color(file("reportpdf")), theme().text);
        QCOMPARE(color(file(".pdf")), theme().dimText);
        auto folder = file("folder.pdf"); folder.isDir = true;
        QCOMPARE(color(folder), theme().text);
    }
    void mimeTypesFromNames()
    {
        const auto images = rule("mime", "image/*");
        FileColors::saveRules({images, rule("mime", "application/pdf", "#551122")});
        QCOMPARE(color(file("photo.png")), images.light);
        QCOMPARE(color(file("photo.JPEG")), images.light);
        QCOMPARE(color(file("report.pdf")), QColor("#551122"));
        QCOMPARE(color(file("unknown.unrecognized-tandem-extension")), theme().text);
        FileColors::saveRules({rule("mime", "application/octet-stream")});
        QCOMPARE(color(file("unknown.unrecognized-tandem-extension")), QColor("#1234ed"));
        FileColors::saveRules({rule("mime", "inode/directory")});
        auto folder = file("folder"); folder.isDir = true;
        QCOMPARE(color(folder), QColor("#1234ed"));
    }
    void fileKindsAndExecutableLinks()
    {
        FileColors::saveRules({rule("kind", "executable", "#111111"), rule("kind", "link", "#222222"),
                              rule("kind", "folder", "#333333"), rule("kind", "no-extension", "#444444")});
        auto folder = file("folder"); folder.isDir = true; folder.mode = S_IFDIR | 0755;
        QCOMPARE(color(folder), QColor("#333333"));
        auto link = file("link"); link.isLink = true; link.mode = S_IFLNK | 0777;
        QCOMPARE(color(link), QColor("#222222")); // link mode bits do not make it executable
        link.isDir = true;
        QCOMPARE(color(link), QColor("#222222"));
        auto executable = file("run.sh"); executable.mode = S_IFREG | 0755;
        QCOMPARE(color(executable), QColor("#111111"));
        QCOMPARE(color(file("README")), QColor("#444444"));
        QCOMPARE(color(file(".gitignore")), QColor("#444444"));
        QCOMPARE(color(file("readme.txt")), theme().text);
    }
    void orderingDisabledRulesAndMasterSwitch()
    {
        auto first = rule("extension", "pdf", "#ff0011");
        auto second = rule("kind", "file", "#11aa22");
        FileColors::saveRules({first, second});
        QCOMPARE(color(file("a.pdf")), first.light);
        FileColors::saveRules({second, first});
        QCOMPARE(color(file("a.pdf")), second.light);
        second.enabled = false; FileColors::saveRules({second, first});
        QCOMPARE(color(file("a.pdf")), first.light);
        FileColors::setEnabled(false);
        QCOMPARE(color(file("a.pdf")), theme().text);
        FileColors::setEnabled(true);
        QCOMPARE(color(file("a.pdf")), first.light);
    }
    void highlightsAndThemeSwitching()
    {
        const auto pdf = rule("extension", "pdf"); FileColors::saveRules({pdf});
        auto entry = file(".private.pdf");
        QCOMPARE(color(entry, '!'), pdf.light); // explicit colors beat ordinary hidden/ignored dimming
        entry.marked = true; QCOMPARE(color(entry), theme().mark);
        entry.marked = false; entry.brokenLink = true; QCOMPARE(color(entry), theme().error);
        entry.brokenLink = false; entry.isParent = true; QCOMPARE(color(entry), theme().text);
        entry.isParent = false;
        Settings().setValue("view/theme", "dark"); applyTheme(*qApp);
        QCOMPARE(color(entry), pdf.dark);
        Settings().setValue("view/theme", "light"); applyTheme(*qApp);
        QCOMPARE(color(entry), pdf.light);
    }
    void invalidConfigAndPortableRoundTrip()
    {
        QVERIFY(!rule("extension", "*.p*f").error().isEmpty());
        QVERIFY(!rule("extension", "").error().isEmpty());
        QVERIFY(!rule("mime", "not-a-mime").error().isEmpty());
        QVERIFY(!rule("kind", "unknown").error().isEmpty());
        QVERIFY(!rule("extension", "pdf", "#00112233").error().isEmpty());
        Settings().setValue("view/fileColors", "[broken");
        QCOMPARE(color(file("a.pdf")), theme().text);
        Settings().setValue("view/fileColors", "[{\"match\":\"kind\",\"value\":\"file\",\"light\":\"bad-color\",\"dark\":\"#123456\"}]");
        QCOMPARE(color(file("a.pdf")), theme().text);
        auto disabled = rule("mime", "audio/*"); disabled.enabled = false;
        FileColors::saveRules({rule("extension", "pdf"), disabled});
        QSettings portable(Config::instance().fileName(), QSettings::IniFormat); portable.sync();
        QVERIFY(portable.value("view/fileColors").toString().contains("audio/*"));
        const auto saved = FileColors::rules();
        QCOMPARE(saved.size(), 2); QCOMPARE(saved[0].light, QColor("#1234ed"));
        QVERIFY(!saved[1].enabled);
    }
    void actualDelegateColorsNamesAndExtensionsOnly()
    {
        const auto pdf = rule("extension", "pdf"); FileColors::saveRules({pdf});
        FileView view; FileModel model;
        model.setListing("/fixture", {file("colored-document.pdf")});
        view.setModel(&model);
        auto* delegate = view.itemDelegate();
        QImage image(600, 40, QImage::Format_ARGB32); image.fill(theme().base);
        QPainter painter(&image);
        QStyleOptionViewItem option; option.rect = image.rect(); option.font = theme().listFont; option.fontMetrics = QFontMetrics(option.font);
        const int row = model.rowOf("colored-document.pdf"); QVERIFY(row >= 0);
        delegate->paint(&painter, option, model.index(row, FileModel::ColName)); QCOMPARE(painter.pen().color(), pdf.light);
        delegate->paint(&painter, option, model.index(row, FileModel::ColExt)); QCOMPARE(painter.pen().color(), pdf.light);
        delegate->paint(&painter, option, model.index(row, FileModel::ColTime)); QCOMPARE(painter.pen().color(), theme().dimText);
        model.setMarked(row, true);
        delegate->paint(&painter, option, model.index(row, FileModel::ColName)); QCOMPARE(painter.pen().color(), theme().mark);
    }
};
QTEST_MAIN(TestFileColors)
#include "test_filecolors.moc"
