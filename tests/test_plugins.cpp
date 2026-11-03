#include "core/Plugins.h"
#include "core/Backend.h"
#include "core/Config.h"
#include "core/Keyring.h"
#include "core/Util.h"
#include "ui/PluginManager.h"
#include "ui/MainWindow.h"
#include "ui/Panel.h"
#include "ui/DirTab.h"
#include "ui/FileModel.h"
#include "ui/FileView.h"
#include "TestBackend.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace tandem;
namespace {
bool write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path()); QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
QJsonObject manifest(const QString& mode = "context")
{
    return {{"schema", 1}, {"name", "Test plugin"}, {"actions", QJsonArray{QJsonObject{
        {"id", "test"}, {"label", "Test action"}, {"run", QJsonArray{QCoreApplication::applicationFilePath(), "--fixture", mode}}}}}};
}
std::optional<Plugin> makePlugin(const QString& root, const QString& id = "test-plugin", const QJsonObject& obj = manifest())
{
    const QString folder = QDir(root).filePath(id);
    const QByteArray data = QJsonDocument(obj).toJson();
    if (!write(folder + "/plugin.json", data)) return {};
    QString error; return parsePluginManifest(data, folder, &error);
}
QJsonArray settings()
{
    return {QJsonObject{{"key", "feature"}, {"label", "Feature"}, {"kind", "toggle"}, {"default", false}},
        QJsonObject{{"key", "address"}, {"label", "Address"}, {"requires", "feature"}, {"default", "https://example.org"}, {"link", "https://example.org/setup"}},
        QJsonObject{{"key", "token"}, {"label", "Token"}, {"kind", "secret"}},
        QJsonObject{{"key", "format"}, {"label", "Format"}, {"kind", "choice"}, {"choices", QJsonArray{"text", "json"}}}};
}
}
class TestPlugins : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true); setKeyringHooksForTests({});
        QVERIFY(m_tmp.isValid()); QString error;
        QVERIFY2(Config::instance().useFile(m_tmp.filePath("settings.ini"), &error, false), qPrintable(error));
    }
    void init() { Settings().clear(); qputenv("TANDEM_PLUGIN_PATH", ""); }
    void strictManifests()
    {
        QString error; const QString folder = m_tmp.filePath("valid-plugin");
        auto good = manifest(); good["settings"] = settings();
        auto p = parsePluginManifest(QJsonDocument(good).toJson(), folder, &error); QVERIFY2(p.has_value(), qPrintable(error));
        QCOMPARE(p->actions[0].timeout, 120); QCOMPARE(p->actions[0].locations, QStringList{"local"});
        for (const auto& change : {QPair<QString, QJsonValue>{"schema", 2}, {"schema", "1"}, {"unknown", true}, {"actions", QJsonArray{}}, {"name", 12}}) {
            auto bad = good; bad[change.first] = change.second;
            QVERIFY2(!parsePluginManifest(QJsonDocument(bad).toJson(), folder, &error), qPrintable(change.first)); QVERIFY(!error.isEmpty());
        }
        for (const auto& change : {QPair<QString, QJsonValue>{"timeout", 0}, {"timeout", 1.5}, {"timeout", 3601}, {"result", "delete"},
                 {"for", "albums"}, {"locations", QJsonArray{"ftp"}}, {"run", QJsonArray{"./../escape"}}, {"run", QJsonArray{"bin/tool"}},
                 {"run", QJsonArray{12}}, {"run", QJsonArray{""}}, {"unknown", true}}) {
            auto bad = good; auto action = bad["actions"].toArray().first().toObject(); action[change.first] = change.second; bad["actions"] = QJsonArray{action};
            QVERIFY2(!parsePluginManifest(QJsonDocument(bad).toJson(), folder, &error), qPrintable(change.first));
        }
        auto bad = good; auto actions = bad["actions"].toArray(); actions.append(actions.first()); bad["actions"] = actions;
        QVERIFY(!parsePluginManifest(QJsonDocument(bad).toJson(), folder, &error));
        bad = good; auto opts = settings(); auto secret = opts[2].toObject(); secret["default"] = "no-default-secrets"; opts[2] = secret; bad["settings"] = opts;
        QVERIFY(!parsePluginManifest(QJsonDocument(bad).toJson(), folder, &error));
        bad = good; opts = settings(); auto required = opts[1].toObject(); required["requires"] = "unknown"; opts[1] = required; bad["settings"] = opts;
        QVERIFY(!parsePluginManifest(QJsonDocument(bad).toJson(), folder, &error));
        QVERIFY(!parsePluginManifest(QByteArray(256 * 1024 + 1, 'x'), folder, &error));
        QVERIFY(!parsePluginManifest("{", folder, &error));
        QVERIFY(!parsePluginManifest(QJsonDocument(good).toJson(), m_tmp.filePath("../INVALID"), &error));
    }
    void discoveryAndOffByDefault()
    {
        QTemporaryDir high, low; QVERIFY(makePlugin(high.path())); QVERIFY(makePlugin(low.path())); QVERIFY(makePlugin(low.path(), "second"));
        qputenv("TANDEM_PLUGIN_PATH", QFile::encodeName(high.path() + QDir::listSeparator() + low.path()));
        auto found = discoverPlugins(); QCOMPARE(found.size(), 2); QCOMPARE(found.first().folder, high.filePath("test-plugin"));
        QVERIFY(!pluginEnabled("test-plugin")); setPluginEnabled("test-plugin", true); QVERIFY(pluginEnabled("test-plugin"));
        QVERIFY(write(high.filePath("test-plugin/plugin.json"), "broken"));
        found = discoverPlugins(); QVERIFY(!found.first().plugin); QVERIFY(!found.first().problem.isEmpty());
        QCOMPARE(found.first().folder, high.filePath("test-plugin")); // invalid override never silently runs the lower copy
        qputenv("TANDEM_PLUGIN_PATH", ""); QVERIFY(discoverPlugins().isEmpty());
    }
    void settingsAndPrivateSecrets()
    {
        QTemporaryDir root; auto obj = manifest(); obj["settings"] = settings(); auto plugin = makePlugin(root.path(), "settings-test", obj); QVERIFY(plugin);
        auto values = pluginSettings(*plugin); QCOMPARE(values["address"].toString(), QString("https://example.org")); QVERIFY(!values["feature"].toBool());
        values["feature"] = true; values["address"] = "https://custom.example.org"; values["token"] = "TEST-ONLY-secret-value"; values["format"] = "json";
        QString error; QVERIFY2(savePluginSettings(*plugin, values, &error), qPrintable(error)); QCOMPARE(pluginSettings(*plugin), values);
        QFile config(Config::instance().fileName()); QVERIFY(config.open(QIODevice::ReadOnly)); const auto bytes = config.readAll();
        QVERIFY(bytes.contains("custom.example.org")); QVERIFY(!bytes.contains("TEST-ONLY-secret-value")); QVERIFY(!Settings().contains(pluginSettingKey(plugin->id, "token")));
        const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/plugin-private/secrets.json";
        struct stat st{}; QVERIFY(::stat(QFile::encodeName(local).constData(), &st) == 0); QCOMPARE(st.st_mode & 0777, mode_t(0600));
        auto invalid = values; invalid["format"] = "xml"; QVERIFY(!savePluginSettings(*plugin, invalid, &error)); QCOMPARE(pluginSettings(*plugin), values);
        values["token"] = ""; QVERIFY(savePluginSettings(*plugin, values, &error)); QCOMPARE(pluginSettings(*plugin)["token"].toString(), QString());
    }
    void processContextAndLiteralArguments()
    {
        QTemporaryDir root; auto obj = manifest(); auto action = obj["actions"].toArray().first().toObject(); auto args = action["run"].toArray();
        const QString literal = QString::fromUtf8("a 'quote' 日本語 ; $(touch SHOULD_NOT_EXIST)"); args.append(literal); action["run"] = args; obj["actions"] = QJsonArray{action};
        auto plugin = makePlugin(root.path(), "runner", obj); QVERIFY(plugin); setPluginEnabled(plugin->id, true);
        PluginContext context; context.active = {{"path", "/example with spaces"}}; context.selected.append(QJsonObject{{"name", literal}, {"path", "/" + literal}});
        const QByteArray input = pluginInput(*plugin, plugin->actions[0], context);
        PluginRun run(*plugin, plugin->actions[0], input); QSignalSpy done(&run, &PluginRun::finished); run.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000); QVERIFY2(run.succeeded(), qPrintable(run.text()));
        const auto output = QJsonDocument::fromJson(run.text().toUtf8()).object();
        QCOMPARE(output["cwd"].toString(), QFileInfo(plugin->folder).canonicalFilePath()); QCOMPARE(output["args"].toArray().last().toString(), literal);
        QCOMPARE(output["request"].toObject(), QJsonDocument::fromJson(input).object()); QVERIFY(!QFile::exists(plugin->folder + "/SHOULD_NOT_EXIST"));
        QTest::qWait(30); QCOMPARE(done.count(), 1);
    }
    void processFailuresAndLimits()
    {
        QTemporaryDir root;
        for (const QString& mode : {QString("fail"), QString("flood"), QString("sleep")}) {
            auto obj = manifest(mode); auto action = obj["actions"].toArray()[0].toObject(); action["timeout"] = 1; obj["actions"] = QJsonArray{action};
            auto plugin = makePlugin(root.path(), "failure", obj); QVERIFY(plugin); setPluginEnabled(plugin->id, true);
            PluginRun run(*plugin, plugin->actions[0], "{}"); QSignalSpy done(&run, &PluginRun::finished); run.start();
            QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000); QVERIFY(!run.succeeded());
            QVERIFY2(run.text().contains(mode == "fail" ? "fixture failure" : mode == "flood" ? "1 MB" : "timed out"), qPrintable(run.text()));
            QTest::qWait(30); QCOMPARE(done.count(), 1);
        }
        auto plugin = makePlugin(root.path(), "disabled"); QVERIFY(plugin);
        PluginRun disabled(*plugin, plugin->actions[0], "{}"); QSignalSpy done(&disabled, &PluginRun::finished); disabled.start(); QCOMPARE(done.count(), 1); QVERIFY(!disabled.succeeded());
        setPluginEnabled(plugin->id, true);
        PluginRun large(*plugin, plugin->actions[0], QByteArray(4 * 1024 * 1024 + 1, 'a')); QSignalSpy largeDone(&large, &PluginRun::finished); large.start(); QCOMPARE(largeDone.count(), 1); QVERIFY(!large.succeeded());
        auto changed = manifest("fail"); QVERIFY(write(plugin->folder + "/plugin.json", QJsonDocument(changed).toJson()));
        PluginRun stale(*plugin, plugin->actions[0], "{}"); QSignalSpy staleDone(&stale, &PluginRun::finished); stale.start(); QCOMPARE(staleDone.count(), 1); QVERIFY(stale.text().contains("manifest changed"));
        auto missingObj = manifest(); auto a = missingObj["actions"].toArray()[0].toObject(); a["run"] = QJsonArray{"/no/such/tandem-plugin-program"}; missingObj["actions"] = QJsonArray{a};
        plugin = makePlugin(root.path(), "missing", missingObj); QVERIFY(plugin); setPluginEnabled(plugin->id, true);
        PluginRun missing(*plugin, plugin->actions[0], "{}"); QSignalSpy missingDone(&missing, &PluginRun::finished); missing.start();
        QTRY_COMPARE_WITH_TIMEOUT(missingDone.count(), 1, 5000); QVERIFY(!missing.succeeded());
    }
    void cancellationAndOwnerDestruction()
    {
        QTemporaryDir root; auto plugin = makePlugin(root.path(), "cancel", manifest("descendant")); QVERIFY(plugin); setPluginEnabled(plugin->id, true);
        for (bool destroy : {false, true}) {
            QFile::remove(plugin->folder + "/child.pid");
            auto* owner = new QObject; auto* run = new PluginRun(*plugin, plugin->actions[0], "{}", owner); QSignalSpy done(run, &PluginRun::finished); run->start();
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(plugin->folder + "/child.pid"), 5000);
            QFile pidfile(plugin->folder + "/child.pid"); QVERIFY(pidfile.open(QIODevice::ReadOnly)); const pid_t child = pid_t(pidfile.readAll().trimmed().toInt()); QVERIFY(child > 0); QVERIFY(::kill(child, 0) == 0);
            if (destroy) { delete owner; owner = nullptr; }
            else { run->cancel(); QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000); QVERIFY(!run->succeeded()); }
            QTRY_VERIFY_WITH_TIMEOUT(::kill(child, 0) != 0, 5000);
            delete owner;
        }
    }
    void installIsExclusiveAndRejectsUnsafePackages()
    {
        QTemporaryDir source, target; auto plugin = makePlugin(source.path(), "installed"); QVERIFY(plugin);
        QVERIFY(write(plugin->folder + "/sub/file 日本語.txt", "original"));
        QVERIFY(write(plugin->folder + "/tool", "#!/bin/sh\nexit 0\n")); QVERIFY(QFile::setPermissions(plugin->folder + "/tool", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto result = installPluginFolder(plugin->folder, target.path()); QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(QFileInfo(target.filePath("installed/tool")).isExecutable());
        QVERIFY(write(plugin->folder + "/sub/file 日本語.txt", "replacement"));
        result = installPluginFolder(plugin->folder, target.path()); QVERIFY(!result.error.isEmpty());
        QFile original(target.filePath("installed/sub/file 日本語.txt")); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("original"));
        plugin = makePlugin(source.path(), "linked"); QVERIFY(plugin);
        QVERIFY(::symlink("/etc/passwd", QFile::encodeName(plugin->folder + "/link").constData()) == 0);
        result = installPluginFolder(plugin->folder, target.path()); QVERIFY(!result.error.isEmpty()); QVERIFY(!QFile::exists(target.filePath("linked")));
        plugin = makePlugin(source.path(), "special"); QVERIFY(plugin); QVERIFY(::mkfifo(QFile::encodeName(plugin->folder + "/fifo").constData(), 0600) == 0);
        result = installPluginFolder(plugin->folder, target.path()); QVERIFY(!result.error.isEmpty()); QVERIFY(!QFile::exists(target.filePath("special")));
        plugin = makePlugin(source.path(), "cancelled"); std::atomic<bool> cancel{true}; result = installPluginFolder(plugin->folder, target.path(), &cancel);
        QVERIFY(!result.error.isEmpty()); QVERIFY(!QFile::exists(target.filePath("cancelled")));
        const auto stage = QDir(target.path()).entryList({".install-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot); QVERIFY(stage.isEmpty());
        const QString nestedDestination = plugin->folder + "/must-not-be-created/plugins";
        result = installPluginFolder(plugin->folder, nestedDestination);
        QVERIFY(!result.error.isEmpty()); QVERIFY(!QFile::exists(plugin->folder + "/must-not-be-created"));
    }
    void availabilityAndListValidation()
    {
        PluginAction action; PluginContext context; QVERIFY(!pluginActionAvailable(action, context));
        context.selected.append(QJsonObject{{"is_directory", false}}); QVERIFY(pluginActionAvailable(action, context));
        action.target = "folders"; QVERIFY(!pluginActionAvailable(action, context)); action.target = "files"; QVERIFY(pluginActionAvailable(action, context));
        context.location = "remote"; QVERIFY(!pluginActionAvailable(action, context)); action.locations << "remote"; QVERIFY(pluginActionAvailable(action, context));
        context.location = "archive"; QVERIFY(!pluginActionAvailable(action, context)); action.locations << "archive"; QVERIFY(pluginActionAvailable(action, context));
        context.selected = {}; action.target = "always"; QVERIFY(pluginActionAvailable(action, context)); context.error = "bad"; QVERIFY(!pluginActionAvailable(action, context));
        QString error;
        const QJsonObject good{{"title", "Results"}, {"items", QJsonArray{QJsonObject{{"text", "<b>Plain</b>"}, {"detail", "Detail"}, {"url", "https://example.org"}}}}};
        QVERIFY(parsePluginList(QJsonDocument(good).toJson(), &error));
        for (const auto& url : {QString("file:///etc/passwd"), QString("javascript:alert(1)"), QString("https://user:secret@example.org/")}) {
            auto bad = good; auto item = bad["items"].toArray()[0].toObject(); item["url"] = url; bad["items"] = QJsonArray{item};
            QVERIFY(!parsePluginList(QJsonDocument(bad).toJson(), &error));
        }
        auto bad = good; bad["run"] = "something"; QVERIFY(!parsePluginList(QJsonDocument(bad).toJson(), &error));
        QVERIFY(!parsePluginList("bad json", &error));
    }
    void settingsPageSaveAndCancel()
    {
        QTemporaryDir root; auto obj = manifest(); obj["settings"] = settings(); auto plugin = makePlugin(root.path(), "ui-settings", obj); QVERIFY(plugin);
        qputenv("TANDEM_PLUGIN_PATH", QFile::encodeName(root.path())); PluginManagerPage page; page.show();
        auto* table = page.findChild<QTreeWidget*>("pluginTable"); QCOMPARE(table->topLevelItemCount(), 1);
        QCOMPARE(table->topLevelItem(0)->checkState(0), Qt::Unchecked); table->topLevelItem(0)->setCheckState(0, Qt::Checked); QVERIFY(pluginEnabled(plugin->id));
        auto* button = page.findChild<QPushButton*>("pluginSettings"); QVERIFY(button->isEnabled());
        QTimer::singleShot(50, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            auto* address = dialog->findChild<QLineEdit*>("pluginSetting_address"); QVERIFY(!address->isEnabled());
            dialog->findChild<QCheckBox*>("pluginSetting_feature")->setChecked(true); QVERIFY(address->isEnabled()); address->setText("saved-address");
            dialog->findChild<QLineEdit*>("pluginSetting_token")->setText("UI-TEST-token");
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        }); button->click();
        QCOMPARE(pluginSettings(*plugin)["address"].toString(), QString("saved-address"));
        QTimer::singleShot(50, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QLineEdit*>("pluginSetting_address")->setText("discard-address"); dialog->reject();
        }); button->click(); QCOMPARE(pluginSettings(*plugin)["address"].toString(), QString("saved-address"));
    }
    void realPanelContextAndBundledActions()
    {
        QTemporaryDir files, other;
        const QString weird = QString::fromUtf8("-quote ' 日本語; $(none).txt "); QVERIFY(write(files.filePath(weird), "hello"));
        qputenv("TANDEM_PLUGIN_PATH", QFile::encodeName(QStringLiteral(TANDEM_SOURCE_DIR) + "/plugins"));
        MainWindow window({files.path(), other.path()}); window.show();
        auto* left = window.findChild<Panel*>("leftPanel"); auto* right = window.findChild<Panel*>("rightPanel"); QVERIFY(left && right);
        QTRY_COMPARE(left->tab()->model()->path(), files.path());
        left->view()->setCurrentRow(left->tab()->model()->rowOf(weird));
        auto context = capturePluginContext(left, right); QVERIFY2(context.error.isEmpty(), qPrintable(context.error)); QCOMPARE(context.selected.size(), 1);
        QCOMPARE(context.selected[0].toObject()["path"].toString(), files.filePath(weird)); QCOMPARE(context.selected[0].toObject()["size"].toInteger(), qint64(5));
        QCOMPARE(context.other["path"].toString(), other.path());
        auto found = discoverPlugins(); QCOMPARE(found.size(), 1); QVERIFY(found[0].plugin); setPluginEnabled(found[0].id, true);
        auto* controller = window.findChild<PluginController*>(); QVERIFY(controller);
        QMenu menu; controller->populateMenu(&menu, left, right); QVERIFY(menu.actions().first()->menu());
        QApplication::clipboard()->setText("unchanged"); menu.actions().first()->menu()->actions().first()->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(controller->runningCount(), 0, 5000); QCOMPARE(QApplication::clipboard()->text(), files.filePath(weird));
        const auto plugin = *found[0].plugin;
        controller->invoke(plugin, plugin.actions[2], context, left);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QDialog*>("pluginResultList") != nullptr, 5000);
        auto* results = window.findChild<QDialog*>("pluginResultList"); QCOMPARE(results->findChild<QTreeWidget*>()->topLevelItemCount(), 1); results->close();
        window.close();
    }
    void remoteContextStripsCredentials()
    {
        QTemporaryDir root; QVERIFY(write(root.filePath("remote.txt"), "test"));
        auto backend = std::make_shared<TestBackend>(root.path()); registerBackend("test", backend);
        Panel panel; panel.addTab("test://user:private-pass@server/");
        QTRY_COMPARE(panel.tab()->model()->path(), QString("test://user:private-pass@server/"));
        panel.view()->setCurrentRow(panel.tab()->model()->rowOf("remote.txt"));
        const auto context = capturePluginContext(&panel, nullptr); QCOMPARE(context.location, QString("remote"));
        QVERIFY(!QJsonDocument(context.active).toJson().contains("private-pass"));
        QVERIFY(!QJsonDocument(context.selected).toJson().contains("private-pass"));
        QVERIFY(!context.selected.first().toObject()["is_local"].toBool());
        panel.navigate(root.path()); QTRY_COMPARE(panel.tab()->model()->path(), root.path()); QVERIFY(ioPool()->waitForDone(5000)); registerBackend("test", nullptr);
    }
};
int main(int argc, char** argv)
{
    if (argc >= 3 && QByteArray(argv[1]) == "--fixture") {
        const QByteArray mode(argv[2]);
        if (mode == "fail") { ::write(2, "fixture failure", 15); return 7; }
        if (mode == "flood") { QByteArray chunk(65536, 'x'); for (int i = 0; i < 40; ++i) ::write(1, chunk.constData(), chunk.size()); return 0; }
        if (mode == "sleep") { ::sleep(30); return 0; }
        if (mode == "descendant") {
            const pid_t child = ::fork();
            if (child == 0) { for (;;) ::sleep(1); }
            if (child < 0) return 3;
            write("child.pid", QByteArray::number(child)); for (;;) ::sleep(1);
        }
        QFile input; if (!input.open(stdin, QIODevice::ReadOnly)) return 1; const auto bytes = input.readAll();
        QJsonArray args; for (int i = 3; i < argc; ++i) args.append(QString::fromLocal8Bit(argv[i]));
        const auto result = QJsonDocument(QJsonObject{{"cwd", QDir::currentPath()}, {"args", args}, {"request", QJsonDocument::fromJson(bytes).object()}}).toJson(QJsonDocument::Compact);
        QFile output; if (!output.open(stdout, QIODevice::WriteOnly)) return 1;
        return output.write(result) == result.size() && output.flush() ? 0 : 1;
    }
    QApplication app(argc, argv); TestPlugins test; return QTest::qExec(&test, argc, argv);
}
#include "test_plugins.moc"
