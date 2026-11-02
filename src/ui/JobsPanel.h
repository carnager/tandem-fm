#pragma once

#include <QElapsedTimer>
#include <QAbstractButton>
#include <QFrame>
#include <QHash>
#include <QTimer>

class QLabel;
class QProgressBar;
class QToolButton;
class QVBoxLayout;

namespace tandem {

class Job;
class JobManager;

class JobRow : public QWidget {
    Q_OBJECT
public:
    JobRow(Job* job, JobManager* manager, QWidget* parent);
    void refresh();
    Job* job() const { return m_job; }
    double speed() const;  // bytes/s, 0 once finished

private:
    Job* m_job;
    JobManager* m_manager;
    QLabel* m_title;
    QLabel* m_detail;
    QProgressBar* m_bar;
    QToolButton* m_pause;
    QToolButton* m_startNow;
    QToolButton* m_close;
    QElapsedTimer m_clock;
    int64_t m_lastBytes = 0;
    qint64 m_lastMs = 0;
    double m_speed = 0;
};

// Collapsed: a pill in the bottom-right corner with overall progress.
// Clicking it opens the job list above it, floating over the panels.
class JobsPill : public QAbstractButton {
    Q_OBJECT
public:
    explicit JobsPill(QWidget* parent);
    void setState(const QString& text, double fraction, bool attention);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_text;
    double m_fraction = 0;
    bool m_attention = false;
};

class JobsPanel : public QWidget {
    Q_OBJECT
public:
    JobsPanel(JobManager* manager, QWidget* overlayParent);
    void setBottomMargin(int px);
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool on);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void onAdded(Job* job);
    void onFinished(Job* job);
    void onRemoved(Job* job);
    void tick();
    void updatePill();
    void reposition();

    JobManager* m_manager;
    QFrame* m_list;
    QToolButton* m_pauseAll;
    QVBoxLayout* m_rows;
    JobsPill* m_pill;
    QHash<Job*, JobRow*> m_rowFor;
    QTimer m_timer;
    bool m_expanded = false;
    int m_bottomMargin = 48;
};

}  // namespace tandem
