#pragma once

#include "core/Job.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQueue>

class QWidget;

namespace tandem {

// Owns running jobs and serializes their prompts: several jobs may need an
// answer at the same time, but the user sees one dialog after another.
class JobManager : public QObject {
    Q_OBJECT
public:
    explicit JobManager(QWidget* dialogParent);
    ~JobManager() override;

    void add(Job* job);  // takes ownership and starts it
    void dismiss(Job* job);
    QList<Job*> jobs() const { return m_jobs; }
    int runningCount() const;
    void cancelAll();

signals:
    void jobAdded(Job* job);
    void jobFinished(Job* job);
    void jobRemoved(Job* job);

private:
    struct PendingPrompt {
        QPointer<Job> job;
        PromptRequest request;
        std::function<void(Answer)> reply;
    };
    void showNextPrompt();

    QWidget* m_dialogParent;
    QList<Job*> m_jobs;
    QQueue<PendingPrompt> m_prompts;
    bool m_promptOpen = false;
};

}  // namespace tandem
