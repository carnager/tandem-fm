#pragma once

#include <QMutex>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace tandem {

// Answers to prompts. Not every answer is valid for every prompt kind.
enum class Answer : int {
    Cancel = 0,
    Overwrite,
    OverwriteAll,
    OverwriteOlderAll,
    Skip,
    SkipAll,
    Rename,
    RenameAll,
    Retry,
    DeletePermanently,
    DeletePermanentlyAll,
};

struct PromptRequest {
    enum class Kind { Conflict, Error, TrashFailed };
    Kind kind = Kind::Error;
    QString jobTitle;
    QString message;
    QString srcPath;
    QString dstPath;
    int64_t srcSize = -1, dstSize = -1;
    int64_t srcTime = 0, dstTime = 0;
    QString detail;  // e.g. strerror text
};

// A long-running file operation on its own thread. Progress is published
// through atomics and polled by the UI (no signal storm while copying).
class Job : public QObject {
    Q_OBJECT
public:
    enum class State { Pending, Queued, Scanning, Running, Waiting, Done, Failed, Cancelled };

    struct Snapshot {
        State state = State::Pending;
        bool paused = false;
        int64_t bytesDone = 0, bytesTotal = 0;
        int64_t filesDone = 0, filesTotal = 0;
        QString current;
        int errorCount = 0;
    };

    using AsyncPrompt = std::function<void(const PromptRequest&, std::function<void(Answer)> reply)>;
    using SyncPrompt = std::function<Answer(const PromptRequest&)>;

    explicit Job(QString title);
    ~Job() override;

    void start();
    void cancel();
    void setPaused(bool paused);
    // Queue control: skip the per-drive queue, or go to its front.
    void startNow();
    void runNext();
    void setStartImmediately(bool on) { m_force.store(on); }

    QString title() const { return m_title; }
    Snapshot snapshot() const;
    bool isFinished() const { return m_finished.load(); }
    QStringList errors() const;
    // What this job created or moved, as (path now, path before) pairs;
    // "before" is empty for things it created. Used for undo.
    QList<QPair<QString, QString>> undoRecords() const;

    // UI-thread prompt handler (dialogs). Called on the UI thread.
    void setPromptHandler(AsyncPrompt h) { m_promptHandler = std::move(h); }
    // Called directly on the worker thread; for tests and headless use.
    void setSyncPromptHandler(SyncPrompt h) { m_syncPrompt = std::move(h); }

signals:
    void finished();  // emitted on the UI thread

protected:
    virtual void run() = 0;

    bool isCancelled() const { return m_cancel.load(std::memory_order_relaxed); }
    // Blocks while paused. Returns true if the job was cancelled.
    bool checkpoint();
    // Waits (state Queued) until no other job uses these paths' drives.
    // Returns false if cancelled while waiting.
    bool waitForTurn(const QStringList& paths);
    Answer prompt(PromptRequest req);
    // Error prompt honoring "skip all". Returns Retry, Skip or Cancel.
    Answer askError(const QString& message, const QString& path, int err);
    Answer askErrorText(const QString& message, const QString& path, const QString& detail);

    // Target exists: ask (or apply a remembered "for all" choice). On Rename,
    // dst is changed to a free name. nullopt means the user cancelled.
    enum class Resolution { Overwrite, Skip, Rename };
    struct ConflictSide {
        QString path;
        int64_t size = -1;  // -1 for folders
        int64_t mtime = 0;
    };
    std::optional<Resolution> resolveConflict(const ConflictSide& src, QByteArray& dst, const ConflictSide& existing,
                                              bool sameFile = false);

    void setState(State s) { m_state.store(s); }
    void setCurrent(const QString& s);
    void addError(const QString& e);
    void recordUndo(const QString& now, const QString& before = {});

    std::atomic<int64_t> m_bytesDone{0}, m_bytesTotal{0};
    std::atomic<int64_t> m_filesDone{0}, m_filesTotal{0};

private:
    QString m_title;
    QPointer<Job> m_self;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_force{false};
    std::atomic<State> m_state{State::Pending};
    std::mutex m_pauseMutex;
    std::condition_variable m_pauseCv;
    mutable QMutex m_textMutex;
    QString m_current;
    QStringList m_errors;
    QList<QPair<QString, QString>> m_undo;
    bool m_skipAllErrors = false;
    enum class ConflictPolicy { Ask, OverwriteAll, OverwriteOlder, SkipAll, RenameAll };
    ConflictPolicy m_conflictPolicy = ConflictPolicy::Ask;
    AsyncPrompt m_promptHandler;
    SyncPrompt m_syncPrompt;
};

}  // namespace tandem
