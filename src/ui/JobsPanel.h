#pragma once

#include <QElapsedTimer>
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

private:
    Job* m_job;
    JobManager* m_manager;
    QLabel* m_title;
    QLabel* m_detail;
    QProgressBar* m_bar;
    QToolButton* m_pause;
    QToolButton* m_close;
    QElapsedTimer m_clock;
    int64_t m_lastBytes = 0;
    qint64 m_lastMs = 0;
    double m_speed = 0;
};

// Bottom strip listing background operations. Polls job progress at 10 Hz
// while anything runs; hides itself when idle.
class JobsPanel : public QFrame {
    Q_OBJECT
public:
    JobsPanel(JobManager* manager, QWidget* parent);

private:
    void onAdded(Job* job);
    void onFinished(Job* job);
    void onRemoved(Job* job);
    void tick();

    JobManager* m_manager;
    QVBoxLayout* m_rows;
    QHash<Job*, JobRow*> m_rowFor;
    QTimer m_timer;
};

}  // namespace tandem
