#include "ui/viewer/MediaControls.h"

#include "ui/IconCache.h"
#include "ui/Theme.h"

#include <QAudioOutput>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMediaPlayer>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

namespace tandem {

namespace {

bool g_muted = false;  // for every player, until Tandem quits

QString clockText(qint64 ms)
{
    const qint64 s = ms / 1000;
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

MediaControls::MediaControls(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("mediaControls"));
    setAttribute(Qt::WA_StyledBackground);
    setThemeStyle(this, QStringLiteral(
        "#mediaControls { background: @header; border-top: 1px solid @border; }"
        "QToolButton { border: none; border-radius: 6px; padding: 3px; }"
        "QToolButton:hover { background: @border; }"
        "QLabel { color: @dimText; }"));
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(8, 4, 10, 4);
    l->setSpacing(8);
    auto button = [this](const QString& tip) {
        auto* b = new QToolButton(this);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        return b;
    };
    m_play = button(tr("Play / pause (Space)"));
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setFocusPolicy(Qt::NoFocus);
    m_slider->setRange(0, 0);
    m_slider->setToolTip(tr("Position (← / → jump 5 seconds)"));
    m_time = new QLabel(this);
    m_mute = button(tr("Sound on / off (M)"));
    l->addWidget(m_play);
    l->addWidget(m_slider, 1);
    l->addWidget(m_time);
    l->addWidget(m_mute);
    connect(m_play, &QToolButton::clicked, this, &MediaControls::togglePlay);
    connect(m_mute, &QToolButton::clicked, this, &MediaControls::toggleMute);
    connect(m_slider, &QSlider::sliderMoved, this, [this](int ms) {
        if (m_player)
            m_player->setPosition(ms);
    });
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &MediaControls::updateButtons);
    updateButtons();
    updateTime();
}

MediaControls::~MediaControls()
{
    clear();
}

bool MediaControls::available()
{
    return true;
}

bool MediaControls::autoplay()
{
    return QSettings().value(QStringLiteral("viewer/autoplay"), false).toBool();
}

void MediaControls::setSource(const QString& path)
{
    if (path == m_path)
        return;
    clear();
    m_path = path;
}

void MediaControls::clear()
{
    if (m_player) {
        m_player->disconnect(this);
        m_sink->disconnect(this);
        m_player->stop();
        m_player->deleteLater();
        m_audio->deleteLater();
        m_sink->deleteLater();
        m_player = nullptr;
        m_audio = nullptr;
        m_sink = nullptr;
    }
    m_path.clear();
    m_slider->setRange(0, 0);
    updateButtons();
    updateTime();
}

// The player is made on the first play: browsing past videos costs nothing.
void MediaControls::ensurePlayer()
{
    if (m_player || m_path.isEmpty())
        return;
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_audio->setMuted(g_muted);
    m_player->setAudioOutput(m_audio);
    m_sink = new QVideoSink(this);
    m_player->setVideoSink(m_sink);
    connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f) {
        if (f.isValid())
            emit frame(f.toImage());
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
        m_slider->setRange(0, int(ms));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (!m_slider->isSliderDown())
            m_slider->setValue(int(ms));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &MediaControls::updateButtons);
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& text) {
        m_time->setText(text.isEmpty() ? tr("Cannot play this file") : text);
    });
    m_player->setSource(QUrl::fromLocalFile(m_path));
}

bool MediaControls::isPlaying() const
{
    return m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
}

void MediaControls::togglePlay()
{
    ensurePlayer();
    if (!m_player)
        return;
    if (isPlaying())
        m_player->pause();
    else
        m_player->play();
}

void MediaControls::pause()
{
    if (isPlaying())
        m_player->pause();
}

void MediaControls::seekBy(qint64 ms)
{
    if (!m_player)
        return;
    const qint64 to = m_player->position() + ms;
    m_player->setPosition(qBound<qint64>(0, to, qMax<qint64>(0, m_player->duration())));
}

void MediaControls::toggleMute()
{
    g_muted = !g_muted;
    if (m_audio)
        m_audio->setMuted(g_muted);
    updateButtons();
}

void MediaControls::updateButtons()
{
    m_play->setIcon(IconCache::themed(isPlaying() ? "media-playback-pause" : "media-playback-start"));
    m_mute->setIcon(IconCache::themed(g_muted ? "audio-volume-muted" : "audio-volume-high"));
}

void MediaControls::updateTime()
{
    if (!m_player) {
        m_time->clear();
        return;
    }
    m_time->setText(QStringLiteral("%1 / %2").arg(clockText(m_player->position()), clockText(m_player->duration())));
}

}  // namespace tandem
