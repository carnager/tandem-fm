#include "core/Job.h"

#include "core/Util.h"

#include <chrono>
#include <future>

namespace tandem {

Job::Job(QString title) : m_title(std::move(title)), m_self(this) {}

Job::~Job()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void Job::start()
{
    m_thread = std::thread([this] {
        setState(State::Running);
        run();
        const State s = m_state.load();
        if (isCancelled())
            setState(State::Cancelled);
        else if (s != State::Failed)
            setState(State::Done);
        m_finished.store(true);
        postToUi([self = m_self] {
            if (self)
                emit self->finished();
        });
    });
}

void Job::cancel()
{
    m_cancel.store(true);
    std::lock_guard lock(m_pauseMutex);
    m_pauseCv.notify_all();
}

void Job::setPaused(bool paused)
{
    m_paused.store(paused);
    std::lock_guard lock(m_pauseMutex);
    m_pauseCv.notify_all();
}

bool Job::checkpoint()
{
    if (m_paused.load(std::memory_order_relaxed)) {
        std::unique_lock lock(m_pauseMutex);
        m_pauseCv.wait(lock, [this] { return !m_paused.load() || m_cancel.load(); });
    }
    return isCancelled();
}

Job::Snapshot Job::snapshot() const
{
    Snapshot s;
    s.state = m_state.load();
    s.paused = m_paused.load();
    s.bytesDone = m_bytesDone.load();
    s.bytesTotal = m_bytesTotal.load();
    s.filesDone = m_filesDone.load();
    s.filesTotal = m_filesTotal.load();
    QMutexLocker lock(&m_textMutex);
    s.current = m_current;
    s.errorCount = int(m_errors.size());
    return s;
}

QStringList Job::errors() const
{
    QMutexLocker lock(&m_textMutex);
    return m_errors;
}

void Job::setCurrent(const QString& s)
{
    QMutexLocker lock(&m_textMutex);
    m_current = s;
}

void Job::addError(const QString& e)
{
    QMutexLocker lock(&m_textMutex);
    m_errors.append(e);
}

Answer Job::prompt(PromptRequest req)
{
    req.jobTitle = m_title;
    if (m_syncPrompt)
        return m_syncPrompt(req);

    auto promise = std::make_shared<std::promise<Answer>>();
    auto answered = std::make_shared<std::atomic<bool>>(false);
    std::future<Answer> fut = promise->get_future();
    postToUi([self = m_self, req, promise, answered] {
        auto reply = [promise, answered](Answer a) {
            if (!answered->exchange(true))
                promise->set_value(a);
        };
        if (!self || !self->m_promptHandler)
            reply(Answer::Cancel);
        else
            self->m_promptHandler(req, reply);
    });

    const State prev = m_state.exchange(State::Waiting);
    while (fut.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (isCancelled()) {
            m_state.store(prev);
            return Answer::Cancel;
        }
    }
    m_state.store(prev);
    const Answer a = fut.get();
    if (a == Answer::Cancel)
        cancel();
    return a;
}

Answer Job::askError(const QString& message, const QString& path, int err)
{
    return askErrorText(message, path, errnoString(err));
}

Answer Job::askErrorText(const QString& message, const QString& path, const QString& detail)
{
    const QString line = QStringLiteral("%1: %2 (%3)").arg(message, path, detail);
    if (m_skipAllErrors) {
        addError(line);
        return Answer::Skip;
    }
    PromptRequest req;
    req.kind = PromptRequest::Kind::Error;
    req.message = message;
    req.srcPath = path;
    req.detail = detail;
    Answer a = prompt(req);
    if (a == Answer::SkipAll) {
        m_skipAllErrors = true;
        a = Answer::Skip;
    }
    if (a == Answer::Skip)
        addError(line);
    if (a != Answer::Retry && a != Answer::Skip)
        a = Answer::Cancel;
    return a;
}

}  // namespace tandem
