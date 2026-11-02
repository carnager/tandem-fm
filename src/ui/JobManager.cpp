#include "ui/JobManager.h"

#include "ui/Dialogs.h"

#include <QWidget>

namespace tandem {

JobManager::JobManager(QWidget* dialogParent) : QObject(dialogParent), m_dialogParent(dialogParent) {}

JobManager::~JobManager()
{
    cancelAll();
    qDeleteAll(m_jobs);  // joins worker threads
}

void JobManager::add(Job* job)
{
    job->setParent(nullptr);
    m_jobs.append(job);
    job->setPromptHandler([this, job](const PromptRequest& req, std::function<void(Answer)> reply) {
        m_prompts.enqueue({QPointer<Job>(job), req, std::move(reply)});
        showNextPrompt();
    });
    connect(job, &Job::finished, this, [this, job] { emit jobFinished(job); });
    emit jobAdded(job);
    job->start();
}

void JobManager::dismiss(Job* job)
{
    if (!m_jobs.contains(job))
        return;
    if (!job->isFinished()) {
        job->cancel();
        return;  // removed once it reports finished
    }
    m_jobs.removeOne(job);
    emit jobRemoved(job);
    job->deleteLater();
}

int JobManager::runningCount() const
{
    int n = 0;
    for (Job* j : m_jobs)
        if (!j->isFinished())
            ++n;
    return n;
}

void JobManager::cancelAll()
{
    for (Job* j : std::as_const(m_jobs))
        j->cancel();
    while (!m_prompts.isEmpty())
        m_prompts.dequeue().reply(Answer::Cancel);
}

void JobManager::showNextPrompt()
{
    if (m_promptOpen)
        return;
    while (!m_prompts.isEmpty()) {
        PendingPrompt p = m_prompts.dequeue();
        if (!p.job || p.job->isFinished()) {
            p.reply(Answer::Cancel);
            continue;
        }
        m_promptOpen = true;
        auto* dlg = new PromptDialog(p.request, m_dialogParent);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowModality(Qt::WindowModal);
        connect(dlg, &QDialog::finished, this, [this, reply = p.reply](int result) {
            reply(Answer(result));
            m_promptOpen = false;
            showNextPrompt();
        });
        dlg->open();
        return;
    }
}

}  // namespace tandem
