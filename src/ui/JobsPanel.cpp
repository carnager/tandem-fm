#include "ui/JobsPanel.h"

#include "core/Job.h"
#include "core/Util.h"
#include "ui/IconCache.h"
#include "ui/JobManager.h"
#include "ui/Theme.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

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
    m_startNow = new QToolButton(this);
    m_startNow->setAutoRaise(true);
    m_startNow->setFocusPolicy(Qt::NoFocus);
    m_startNow->setIcon(IconCache::themed("media-skip-forward", "go-next"));
    m_startNow->setToolTip(tr("Start now, alongside the running operation"));
    m_startNow->hide();
    connect(m_startNow, &QToolButton::clicked, this, [this] { m_job->startNow(); });
    layout->addWidget(m_startNow);
    layout->addWidget(m_pause);
    layout->addWidget(m_close);

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (m_job->isFinished())
            return;
        QMenu menu(this);
        const bool queued = m_job->snapshot().state == Job::State::Queued;
        if (queued) {
            menu.addAction(tr("Run next"), this, [this] { m_job->runNext(); });
            menu.addAction(tr("Start now"), this, [this] { m_job->startNow(); });
            menu.addSeparator();
        }
        const bool paused = m_job->snapshot().paused;
        menu.addAction(paused ? tr("Resume") : tr("Pause"), this, [this, paused] { m_job->setPaused(!paused); });
        menu.addAction(tr("Cancel"), this, [this] { m_manager->dismiss(m_job); });
        menu.exec(mapToGlobal(pos));
    });
    m_clock.start();
}

double JobRow::speed() const
{
    return m_job->isFinished() ? 0 : m_speed;
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
    case Job::State::Queued:
        state = tr("queued: waits for its drive");
        break;
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
    m_startNow->setVisible(s.state == Job::State::Queued);
    if (s.state == Job::State::Queued) {
        m_bar->setRange(0, 1000);
        m_bar->setValue(0);
    } else if (s.state == Job::State::Scanning && !m_job->isFinished())
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

// ---------------------------------------------------------------- JobsPill

JobsPill::JobsPill(QWidget* parent) : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
}

void JobsPill::setState(const QString& text, double fraction, bool attention)
{
    const bool resize = text.size() != m_text.size();
    m_text = text;
    m_fraction = std::clamp(fraction, 0.0, 1.0);
    m_attention = attention;
    if (resize)
        updateGeometry();
    update();
}

QSize JobsPill::sizeHint() const
{
    const QFontMetrics fm(font());
    return {qMax(160, fm.horizontalAdvance(m_text) + 36), fm.height() + 14};
}

void JobsPill::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2;
    QPainterPath shape;
    shape.addRoundedRect(r, radius, radius);
    p.fillPath(shape, underMouse() ? t.headerActive : t.header);
    if (m_fraction > 0) {
        p.save();
        p.setClipPath(shape);
        QColor fill = m_attention ? t.error : t.accent;
        fill.setAlpha(70);
        p.fillRect(QRectF(r.left(), r.top(), r.width() * m_fraction, r.height()), fill);
        p.restore();
    }
    p.setPen(QPen(m_attention ? t.error : t.accent, 1.2));
    p.drawPath(shape);
    p.setPen(m_attention ? t.error : t.text);
    p.drawText(rect(), Qt::AlignCenter, m_text);
}

// --------------------------------------------------------------- JobsPanel

JobsPanel::JobsPanel(JobManager* manager, QWidget* overlayParent) : QWidget(overlayParent), m_manager(manager)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);  // room for the shadows
    layout->setSpacing(8);

    m_list = new QFrame(this);
    m_list->setObjectName(QStringLiteral("jobsList"));
    m_list->setStyleSheet(theme().css(QStringLiteral(
        "#jobsList { background: @header; border: 1px solid @border; border-radius: 10px; }")));
    m_rows = new QVBoxLayout(m_list);
    m_rows->setContentsMargins(4, 4, 4, 4);
    m_rows->setSpacing(0);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(12, 4, 6, 2);
    auto* title = new QLabel(tr("Operations"), m_list);
    title->setStyleSheet(theme().css(QStringLiteral("color: @dimText;")));
    m_pauseAll = new QToolButton(m_list);
    m_pauseAll->setAutoRaise(true);
    m_pauseAll->setFocusPolicy(Qt::NoFocus);
    m_pauseAll->setText(tr("Pause all"));
    m_pauseAll->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_pauseAll, &QToolButton::clicked, this, [this] {
        bool anyRunning = false;
        for (Job* j : m_rowFor.keys())
            anyRunning |= !j->isFinished() && !j->snapshot().paused;
        for (Job* j : m_rowFor.keys())
            if (!j->isFinished())
                j->setPaused(anyRunning);
        tick();
    });
    header->addWidget(title);
    header->addStretch();
    header->addWidget(m_pauseAll);
    m_rows->addLayout(header);
    auto* listShadow = new QGraphicsDropShadowEffect(m_list);
    listShadow->setBlurRadius(24);
    listShadow->setOffset(0, 4);
    listShadow->setColor(QColor(0, 0, 0, theme().dark ? 150 : 60));
    m_list->setGraphicsEffect(listShadow);
    m_list->hide();
    layout->addWidget(m_list);

    m_pill = new JobsPill(this);
    auto* pillShadow = new QGraphicsDropShadowEffect(m_pill);
    pillShadow->setBlurRadius(18);
    pillShadow->setOffset(0, 3);
    pillShadow->setColor(QColor(0, 0, 0, theme().dark ? 140 : 50));
    m_pill->setGraphicsEffect(pillShadow);
    m_pill->setToolTip(tr("Background operations: click to show or hide"));
    connect(m_pill, &QAbstractButton::clicked, this, [this] { setExpanded(!m_expanded); });
    layout->addWidget(m_pill, 0, Qt::AlignRight);

    overlayParent->installEventFilter(this);
    hide();

    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &JobsPanel::tick);
    connect(manager, &JobManager::jobAdded, this, &JobsPanel::onAdded);
    connect(manager, &JobManager::jobFinished, this, &JobsPanel::onFinished);
    connect(manager, &JobManager::jobRemoved, this, &JobsPanel::onRemoved);
}

void JobsPanel::setBottomMargin(int px)
{
    m_bottomMargin = px;
    reposition();
}

void JobsPanel::setExpanded(bool on)
{
    m_expanded = on;
    m_list->setVisible(on && !m_rowFor.isEmpty());
    reposition();
}

bool JobsPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == parent() && e->type() == QEvent::Resize)
        reposition();
    return QWidget::eventFilter(obj, e);
}

void JobsPanel::reposition()
{
    auto* p = parentWidget();
    if (!p || m_rowFor.isEmpty())
        return;
    const int width = m_expanded ? qMin(480, p->width() - 16) : m_pill->sizeHint().width() + 20;
    m_list->setFixedWidth(width - 20);
    layout()->activate();
    const int height = layout()->sizeHint().height();
    setGeometry(p->width() - width - 2, p->height() - height - m_bottomMargin + 10, width, height);
    raise();
}

void JobsPanel::onAdded(Job* job)
{
    auto* row = new JobRow(job, m_manager, m_list);
    m_rowFor.insert(job, row);
    m_rows->addWidget(row);
    row->refresh();
    m_list->setVisible(m_expanded);
    show();
    updatePill();
    reposition();
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
    updatePill();
}

void JobsPanel::onRemoved(Job* job)
{
    if (JobRow* row = m_rowFor.take(job))
        row->deleteLater();
    if (m_rowFor.isEmpty()) {
        hide();
        m_timer.stop();
        m_expanded = false;
        m_list->hide();
        return;
    }
    updatePill();
    QTimer::singleShot(0, this, &JobsPanel::reposition);  // after the row is gone
}

void JobsPanel::tick()
{
    for (JobRow* row : std::as_const(m_rowFor))
        if (!row->job()->isFinished())
            row->refresh();
    updatePill();
}

void JobsPanel::updatePill()
{
    int64_t done = 0, total = 0, filesDone = 0, filesTotal = 0;
    double speed = 0;
    int running = 0, problems = 0, waiting = 0, queued = 0, paused = 0;
    Job* only = nullptr;
    for (auto it = m_rowFor.cbegin(); it != m_rowFor.cend(); ++it) {
        Job* j = it.key();
        const Job::Snapshot s = j->snapshot();
        done += s.bytesDone;
        total += s.bytesTotal;
        filesDone += s.filesDone;
        filesTotal += s.filesTotal;
        speed += it.value()->speed();
        if (!j->isFinished()) {
            ++running;
            only = j;
        }
        if (s.state == Job::State::Waiting)
            ++waiting;
        if (!j->isFinished() && s.state == Job::State::Queued)
            ++queued;
        if (!j->isFinished() && s.paused)
            ++paused;
        if (j->isFinished() && s.errorCount)
            ++problems;
    }
    const double fraction = total > 0 ? double(done) / double(total)
                                      : (filesTotal > 0 ? double(filesDone) / double(filesTotal) : 0.0);
    QString text;
    if (waiting)
        text = tr("Needs your answer");
    else if (running == 1)
        text = QStringLiteral("%1  %2%").arg(only->title()).arg(int(fraction * 100));
    else if (running > 1)
        text = tr("%1 operations  %2%").arg(running).arg(int(fraction * 100));
    else if (problems)
        text = tr("%n problem(s): click to see", nullptr, problems);
    else
        text = tr("Done");
    if (queued)
        text += QStringLiteral("  ·  ") + tr("%1 queued").arg(queued);
    if (running && speed > 1)
        text += QStringLiteral("  ·  ") + formatSpeed(speed);
    m_pauseAll->setText(running && paused == running ? tr("Resume all") : tr("Pause all"));
    m_pill->setState(text, running ? fraction : (problems ? 1.0 : 1.0), waiting > 0 || problems > 0);
    if (!m_expanded)
        reposition();
}

}  // namespace tandem
