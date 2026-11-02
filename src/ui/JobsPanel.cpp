#include "ui/JobsPanel.h"

#include "core/Job.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/JobManager.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace tandem {

JobRow::JobRow(Job* job, JobManager* manager, QWidget* parent) : QWidget(parent), m_job(job), m_manager(manager)
{
    const Theme& t = theme();
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 8, 6);
    layout->setSpacing(10);

    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    m_title = new QLabel(this);
    QFont bold = m_title->font();
    bold.setWeight(QFont::DemiBold);
    m_title->setFont(bold);
    m_detail = new QLabel(this);
    m_detail->setStyleSheet(t.css(QStringLiteral("color: @dimText;")));
    m_detail->setMinimumWidth(100);
    m_detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text->addWidget(m_title);
    text->addWidget(m_detail);
    layout->addLayout(text, 3);

    m_bar = new QProgressBar(this);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    m_bar->setRange(0, 1000);
    m_bar->setStyleSheet(t.css(QStringLiteral(
        "QProgressBar { background: @border; border: none; border-radius: 3px; }"
        "QProgressBar::chunk { background: @accent; border-radius: 3px; }")));
    layout->addWidget(m_bar, 2);

    m_pause = new QToolButton(this);
    m_pause->setAutoRaise(true);
    m_pause->setFocusPolicy(Qt::NoFocus);
    m_pause->setIcon(IconCache::themed("media-playback-pause"));
    m_pause->setToolTip(tr("Pause"));
    connect(m_pause, &QToolButton::clicked, this, [this] {
        const bool paused = !m_job->snapshot().paused;
        m_job->setPaused(paused);
        refresh();
    });
    m_close = new QToolButton(this);
    m_close->setAutoRaise(true);
    m_close->setFocusPolicy(Qt::NoFocus);
    m_close->setIcon(IconCache::themed("process-stop", "window-close"));
    m_close->setToolTip(tr("Cancel"));
    // Cancels a running job; dismisses a finished one.
    connect(m_close, &QToolButton::clicked, this, [this] { m_manager->dismiss(m_job); });
    layout->addWidget(m_pause);
    layout->addWidget(m_close);
    m_clock.start();
}

void JobRow::refresh()
{
    const Job::Snapshot s = m_job->snapshot();
    const qint64 now = m_clock.elapsed();
    if (now - m_lastMs >= 400) {
        const double inst = double(s.bytesDone - m_lastBytes) * 1000.0 / double(qMax<qint64>(1, now - m_lastMs));
        m_speed = m_speed <= 0 ? inst : m_speed * 0.6 + inst * 0.4;
        m_lastBytes = s.bytesDone;
        m_lastMs = now;
    }

    QString state;
    switch (s.state) {
    case Job::State::Pending:
    case Job::State::Scanning:
        state = tr("scanning…");
        break;
    case Job::State::Waiting:
        state = tr("waiting for your answer");
        break;
    case Job::State::Running:
        state = s.paused ? tr("paused") : QString();
        break;
    case Job::State::Done:
        state = s.errorCount ? tr("finished with %n problem(s)", nullptr, s.errorCount) : tr("done");
        break;
    case Job::State::Failed:
        state = tr("failed");
        break;
    case Job::State::Cancelled:
        state = tr("cancelled");
        break;
    }

    QString title = m_job->title();
    if (s.filesTotal > 0)
        title += QStringLiteral("  %1 / %2").arg(s.filesDone).arg(s.filesTotal);
    if (!state.isEmpty())
        title += QStringLiteral("  ·  ") + state;
    m_title->setText(title);

    QString detail;
    if (!m_job->isFinished()) {
        detail = s.current;
        if (s.bytesTotal > 0) {
            detail = QStringLiteral("%1 / %2   %3")
                         .arg(formatSize(s.bytesDone), formatSize(s.bytesTotal),
                              s.paused ? QString() : formatSpeed(m_speed));
            if (m_speed > 1 && !s.paused)
                detail += QStringLiteral("   ") +
                          tr("%1 left").arg(formatDuration(int64_t(double(s.bytesTotal - s.bytesDone) / m_speed)));
            detail += QStringLiteral("   ·   ") + s.current;
        }
    } else if (s.errorCount) {
        detail = m_job->errors().constFirst();
    }
    m_detail->setText(detail);
    m_detail->setToolTip(s.errorCount ? m_job->errors().join(QLatin1Char('\n')) : s.current);

    int permille = 0;
    if (s.bytesTotal > 0)
        permille = int(1000 * s.bytesDone / s.bytesTotal);
    else if (s.filesTotal > 0)
        permille = int(1000 * s.filesDone / s.filesTotal);
    if (m_job->isFinished())
        permille = 1000;
    if (s.state == Job::State::Scanning && !m_job->isFinished())
        m_bar->setRange(0, 0);  // busy indicator
    else {
        m_bar->setRange(0, 1000);
        m_bar->setValue(permille);
    }

    m_pause->setVisible(!m_job->isFinished());
    m_pause->setIcon(IconCache::themed(s.paused ? "media-playback-start" : "media-playback-pause"));
    m_pause->setToolTip(s.paused ? tr("Resume") : tr("Pause"));
    m_close->setIcon(m_job->isFinished() ? IconCache::themed("window-close") : IconCache::themed("process-stop", "window-close"));
    m_close->setToolTip(m_job->isFinished() ? tr("Dismiss") : tr("Cancel"));
}

// --------------------------------------------------------------- JobsPanel

JobsPanel::JobsPanel(JobManager* manager, QWidget* parent) : QFrame(parent), m_manager(manager)
{
    setObjectName(QStringLiteral("jobsPanel"));
    setStyleSheet(theme().css(QStringLiteral(
        "#jobsPanel { background: @header; border: 1px solid @border; border-radius: 8px; }")));
    m_rows = new QVBoxLayout(this);
    m_rows->setContentsMargins(2, 2, 2, 2);
    m_rows->setSpacing(0);
    hide();

    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &JobsPanel::tick);
    connect(manager, &JobManager::jobAdded, this, &JobsPanel::onAdded);
    connect(manager, &JobManager::jobFinished, this, &JobsPanel::onFinished);
    connect(manager, &JobManager::jobRemoved, this, &JobsPanel::onRemoved);
}

void JobsPanel::onAdded(Job* job)
{
    auto* row = new JobRow(job, m_manager, this);
    m_rowFor.insert(job, row);
    m_rows->addWidget(row);
    row->refresh();
    show();
    if (!m_timer.isActive())
        m_timer.start();
}

void JobsPanel::onFinished(Job* job)
{
    if (JobRow* row = m_rowFor.value(job))
        row->refresh();
    // Clean successes vanish on their own; problems stay until dismissed.
    const Job::Snapshot s = job->snapshot();
    if (s.errorCount == 0)
        QTimer::singleShot(s.state == Job::State::Done ? 1500 : 3000, this, [this, job] { m_manager->dismiss(job); });
}

void JobsPanel::onRemoved(Job* job)
{
    if (JobRow* row = m_rowFor.take(job))
        row->deleteLater();
    if (m_rowFor.isEmpty()) {
        hide();
        m_timer.stop();
    }
}

void JobsPanel::tick()
{
    for (JobRow* row : std::as_const(m_rowFor))
        if (!row->job()->isFinished())
            row->refresh();
}

}  // namespace tandem
