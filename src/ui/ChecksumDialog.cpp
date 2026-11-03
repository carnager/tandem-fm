#include "ui/ChecksumDialog.h"

#include "core/Checksums.h"
#include "core/Util.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace tandem {

namespace {
enum Col { File, Value };
}

ChecksumDialog::ChecksumDialog(QStringList paths, QWidget* parent) : QDialog(parent), m_paths(std::move(paths))
{
    m_verify = std::all_of(m_paths.begin(), m_paths.end(), [](const QString& p) {
        return QFileInfo(p).isFile() && isChecksumFile(baseName(p));
    });
    m_base = parentPath(m_paths.first());
    setWindowTitle(m_verify ? tr("Check checksums") : tr("Checksums"));
    resize(900, 560);
    setThemeStyle(this, QStringLiteral(
        "QToolButton#segment { padding: 5px 12px; border: 1px solid @control; background: @base; }"
        "QToolButton#segment:checked { background: @accent; color: @accentText; border-color: @accent; }"
        "QTreeWidget::item { padding: 3px 2px; }"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    if (!m_verify) {
        auto* algos = new QHBoxLayout;
        algos->setSpacing(0);
        m_algo = new QButtonGroup(this);
        const QString saved = QSettings().value(QStringLiteral("checksums/algo"), QStringLiteral("SHA-256")).toString();
        const QList<ChecksumAlgo> all = checksumAlgos();
        for (int i = 0; i < all.size(); ++i) {
            auto* b = new QToolButton(this);
            b->setObjectName(QStringLiteral("segment"));
            b->setText(all[i].name);
            b->setCheckable(true);
            b->setChecked(all[i].name == saved);
            m_algo->addButton(b, i);
            algos->addWidget(b);
        }
        if (!m_algo->checkedButton())
            m_algo->button(0)->setChecked(true);
        algos->addStretch(1);
        layout->addLayout(algos);
        connect(m_algo, &QButtonGroup::idClicked, this, [this](int id) {
            QSettings().setValue(QStringLiteral("checksums/algo"), checksumAlgos().value(id).name);
            start();
        });
    }

    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("File"), m_verify ? tr("Result") : tr("Checksum")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->header()->setSectionResizeMode(File, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(Value, QHeaderView::Stretch);
    m_list->header()->resizeSection(File, 300);
    layout->addWidget(m_list, 1);

    auto* bottom = new QHBoxLayout;
    m_status = new QLabel(this);
    setThemeStyle(m_status, QStringLiteral("color: @dimText;"));
    bottom->addWidget(m_status, 1);
    auto* copy = new QPushButton(m_verify ? tr("Copy report") : tr("Copy"), this);
    copy->setToolTip(m_verify ? QString() : tr("As \"checksum  name\" lines, like sha256sum prints them"));
    bottom->addWidget(copy);
    if (!m_verify) {
        auto* each = new QPushButton(tr("Save next to each file"), this);
        each->setToolTip(tr("name.sha256 beside every file"));
        auto* one = new QPushButton(tr("Save as one file"), this);
        one->setToolTip(tr("One checksum file in this folder listing them all"));
        connect(each, &QPushButton::clicked, this, &ChecksumDialog::saveEach);
        connect(one, &QPushButton::clicked, this, &ChecksumDialog::saveOne);
        bottom->addWidget(each);
        bottom->addWidget(one);
        m_saveButtons = {each, one};
    }
    auto* close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    bottom->addWidget(close);
    for (QPushButton* b : findChildren<QPushButton*>())
        if (b != close)
            b->setAutoDefault(false);
    layout->addLayout(bottom);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(copy, &QPushButton::clicked, this, [this] {
        QString text;
        for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
            const QTreeWidgetItem* it = m_list->topLevelItem(i);
            text += m_verify ? it->text(File) + QStringLiteral(": ") + it->text(Value) + QLatin1Char('\n')
                             : it->text(Value) + QStringLiteral("  ") + it->text(File) + QLatin1Char('\n');
        }
        QApplication::clipboard()->setText(text);
        m_status->setText(tr("Copied"));
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        if (!m_verify && !it->text(Value).isEmpty()) {
            QApplication::clipboard()->setText(it->text(Value));
            m_status->setText(tr("Copied the checksum of %1").arg(it->text(File)));
        }
    });

    // Files: the selection, folders walked (names relative to here).
    if (!m_verify) {
        for (const QString& p : std::as_const(m_paths)) {
            if (QFileInfo(p).isDir()) {
                QDirIterator it(p, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
                QStringList inside;
                while (it.hasNext())
                    inside << it.next();
                inside.sort();
                m_files << inside;
            } else {
                m_files << p;
            }
        }
    }
    start();
}

ChecksumDialog::~ChecksumDialog()
{
    if (m_cancel)
        m_cancel->store(true);
}

void ChecksumDialog::start()
{
    if (m_cancel)
        m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_list->clear();
    m_verify ? verifyAll() : computeAll();
}

void ChecksumDialog::computeAll()
{
    const ChecksumAlgo algo = checksumAlgos().value(m_algo->checkedId());
    for (const QString& f : std::as_const(m_files))
        new QTreeWidgetItem(m_list, {QDir(m_base).relativeFilePath(f), QString()});
    m_list->setFont(QFont());
    for (QPushButton* b : std::as_const(m_saveButtons))
        b->setEnabled(false);
    m_busy = true;
    m_status->setText(tr("Computing …"));
    QPointer<ChecksumDialog> self(this);
    const QStringList files = m_files;
    auto cancel = m_cancel;
    int64_t total = 0;
    for (const QString& f : files)
        total += QFileInfo(f).size();
    ioPool()->start([self, files, algo, cancel, total] {
        int64_t before = 0;
        for (int i = 0; i < files.size() && !cancel->load(); ++i) {
            int64_t last = 0;
            const QString sum = fileChecksum(files[i], algo.algo, cancel.get(), [&](int64_t done) {
                if (done - last < (64 << 20))
                    return;
                last = done;
                const int pct = total ? int((before + done) * 100 / total) : 0;
                postToUi([self, cancel, i, pct, n = files.size()] {
                    if (self && !cancel->load())
                        self->m_status->setText(tr("Computing %1 of %2 · %3%").arg(i + 1).arg(n).arg(pct));
                });
            });
            before += QFileInfo(files[i]).size();
            postToUi([self, cancel, i, sum] {
                if (!self || cancel->load())
                    return;
                QTreeWidgetItem* it = self->m_list->topLevelItem(i);
                it->setText(Value, sum.isEmpty() ? tr("can't be read") : sum);
                it->setFont(Value, sum.isEmpty() ? QFont() : monoFont());
                if (sum.isEmpty())
                    it->setForeground(Value, theme().error);
            });
        }
        postToUi([self, cancel, algo] {
            if (!self || cancel->load())
                return;
            self->m_busy = false;
            self->m_status->setText(tr("%1 of %2 files · double click copies one").arg(algo.name).arg(self->m_files.size()));
            for (QPushButton* b : std::as_const(self->m_saveButtons))
                b->setEnabled(true);
        });
    });
}

void ChecksumDialog::verifyAll()
{
    struct Check {
        QString name, path, hash;
        QCryptographicHash::Algorithm algo;
    };
    QList<Check> checks;
    for (const QString& file : std::as_const(m_paths)) {
        QString bsdName;
        const QList<ChecksumLine> lines = readChecksumFile(file, &bsdName);
        for (const ChecksumLine& l : lines) {
            auto algo = algoForFile(baseName(file));
            if (!bsdName.isEmpty())
                for (const ChecksumAlgo& a : checksumAlgos())
                    if (QString(a.name).remove(QLatin1Char('-')).compare(QString(bsdName).remove(QLatin1Char('-')),
                                                                         Qt::CaseInsensitive) == 0)
                        algo = a;
            if (!algo || algo->hexLength != l.hash.size())
                algo = algoForHexLength(int(l.hash.size()));
            if (!algo)
                continue;
            const QString path = QDir(parentPath(file)).absoluteFilePath(l.name);
            checks.append({l.name, path, l.hash, algo->algo});
            new QTreeWidgetItem(m_list, {l.name, QString()});
        }
    }
    if (checks.isEmpty()) {
        m_status->setText(tr("No checksums found in these files"));
        return;
    }
    m_busy = true;
    m_status->setText(tr("Checking …"));
    QPointer<ChecksumDialog> self(this);
    auto cancel = m_cancel;
    ioPool()->start([self, checks, cancel] {
        for (int i = 0; i < checks.size() && !cancel->load(); ++i) {
            const Check& c = checks[i];
            const bool exists = QFileInfo::exists(c.path);
            const QString sum = exists ? fileChecksum(c.path, c.algo, cancel.get()) : QString();
            postToUi([self, cancel, i, exists, ok = sum == c.hash] {
                if (!self || cancel->load())
                    return;
                QTreeWidgetItem* it = self->m_list->topLevelItem(i);
                const Theme& t = theme();
                it->setText(Value, !exists ? tr("missing") : ok ? tr("✓ OK") : tr("✗ different"));
                it->setForeground(Value, !exists ? t.dimText : ok ? t.success : t.error);
                it->setData(Value, Qt::UserRole, !exists ? 2 : ok ? 0 : 1);
                self->m_status->setText(tr("Checking %1 of %2 …").arg(i + 1).arg(self->m_list->topLevelItemCount()));
            });
        }
        postToUi([self, cancel] {
            if (!self || cancel->load())
                return;
            self->m_busy = false;
            self->m_status->setText(self->summary());
            bool bad = false;
            for (int i = 0; i < self->m_list->topLevelItemCount(); ++i)
                bad |= self->m_list->topLevelItem(i)->data(Value, Qt::UserRole).toInt() != 0;
            setThemeStyle(self->m_status, bad ? QStringLiteral("color: @error;") : QStringLiteral("color: @text;"));
        });
    });
}

QString ChecksumDialog::summary() const
{
    int ok = 0, bad = 0, missing = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QVariant v = m_list->topLevelItem(i)->data(Value, Qt::UserRole);
        if (!v.isValid())
            continue;
        (v.toInt() == 0 ? ok : v.toInt() == 1 ? bad : missing) += 1;
    }
    return tr("%1 OK · %2 different · %3 missing").arg(ok).arg(bad).arg(missing);
}

void ChecksumDialog::saveEach()
{
    const ChecksumAlgo algo = checksumAlgos().value(m_algo->checkedId());
    int written = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QString sum = m_list->topLevelItem(i)->text(Value);
        if (sum.size() != algo.hexLength)
            continue;
        QFile f(m_files[i] + algo.extension);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(checksumLines({{sum, baseName(m_files[i])}}).toUtf8());
            ++written;
        }
    }
    m_saved = m_files.value(0) + algo.extension;
    m_status->setText(plural(tr("Saved %n checksum file(s)", nullptr, written), written));
}

void ChecksumDialog::saveOne()
{
    const ChecksumAlgo algo = checksumAlgos().value(m_algo->checkedId());
    QList<ChecksumLine> lines;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QString sum = m_list->topLevelItem(i)->text(Value);
        if (sum.size() == algo.hexLength)
            lines.append({sum, m_list->topLevelItem(i)->text(File)});
    }
    const QString name = m_files.size() == 1 ? baseName(m_files.first()) + algo.extension
                                              : (baseName(m_base).isEmpty() ? QStringLiteral("checksums") : baseName(m_base)) + algo.extension;
    QString path = joinPath(m_base, name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_status->setText(tr("Cannot write %1").arg(path));
        return;
    }
    f.write(checksumLines(lines).toUtf8());
    m_saved = path;
    m_status->setText(tr("Saved %1").arg(name));
}

}  // namespace tandem
