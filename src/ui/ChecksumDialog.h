#pragma once

#include <QDialog>

#include <atomic>
#include <memory>

class QButtonGroup;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace tandem {

// Alt+Shift+C. On checksum files (.sha256, SHA256SUMS, .md5 …): checks
// every file they list. On anything else: computes checksums, to copy or
// save as checksum files.
class ChecksumDialog : public QDialog {
    Q_OBJECT
public:
    ChecksumDialog(QStringList paths, QWidget* parent = nullptr);
    ~ChecksumDialog() override;

    bool isVerifying() const { return m_verify; }
    bool isBusy() const { return m_busy; }
    QTreeWidget* list() const { return m_list; }
    QString summary() const;
    void saveEach();     // name.sha256 next to every file
    void saveOne();      // one file in the folder, listing them all
    QString saved() const { return m_saved; }

private:
    void start();
    void computeAll();
    void verifyAll();

    QStringList m_paths;
    QString m_base;  // names are relative to this folder
    QStringList m_files;
    bool m_verify = false;
    bool m_busy = false;
    QButtonGroup* m_algo = nullptr;
    QTreeWidget* m_list;
    QLabel* m_status;
    QList<QPushButton*> m_saveButtons;
    QString m_saved;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

}  // namespace tandem
