// Built without Qt Multimedia: videos and music show a picture and details only.
#include "ui/viewer/MediaControls.h"

namespace tandem {

MediaControls::MediaControls(QWidget* parent) : QWidget(parent)
{
    hide();
}

MediaControls::~MediaControls() = default;

bool MediaControls::available() { return false; }
bool MediaControls::autoplay() { return false; }
void MediaControls::setSource(const QString& path) { m_path = path; }
void MediaControls::clear() { m_path.clear(); }
bool MediaControls::isPlaying() const { return false; }
void MediaControls::togglePlay() {}
void MediaControls::pause() {}
void MediaControls::seekBy(qint64) {}
void MediaControls::toggleMute() {}
void MediaControls::ensurePlayer() {}
void MediaControls::updateTime() {}
void MediaControls::updateButtons() {}

}  // namespace tandem
