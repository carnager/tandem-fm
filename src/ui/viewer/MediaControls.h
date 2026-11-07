#pragma once

#include <QWidget>

class QAudioOutput;
class QImage;
class QLabel;
class QMediaPlayer;
class QSlider;
class QToolButton;
class QVideoSink;

namespace tandem {

// Play / pause, position and sound for the video or audio file shown in the
// viewer. Nothing is loaded until play is pressed, unless autoplay is on.
// Without Qt Multimedia (MediaControls_none.cpp) it is never shown.
class MediaControls : public QWidget {
    Q_OBJECT
public:
    explicit MediaControls(QWidget* parent = nullptr);
    ~MediaControls() override;

    static bool available();  // built with Qt Multimedia
    static bool autoplay();   // "Play videos and music right away" (off by default)

    void setSource(const QString& path);  // stops what plays now
    void clear();
    QString source() const { return m_path; }
    bool hasPlayer() const { return m_player != nullptr; }
    bool isPlaying() const;
    void togglePlay();
    void pause();
    void seekBy(qint64 ms);
    void toggleMute();

signals:
    void frame(const QImage& image);  // the video picture to show

private:
    void ensurePlayer();
    void updateTime();
    void updateButtons();

    QString m_path;
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    QVideoSink* m_sink = nullptr;
    QToolButton* m_play = nullptr;
    QSlider* m_slider = nullptr;
    QLabel* m_time = nullptr;
    QToolButton* m_mute = nullptr;
};

}  // namespace tandem
