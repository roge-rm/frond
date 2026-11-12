#pragma once
#include <QDir>
#include <algorithm>
#include <QString>
#include <QStringList>
#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace frondWebcams {

// The /dev/video* nodes that are V4L2 capture devices, sorted by number. FROND_WEBCAMS (a
// colon separated list) replaces the search, for tests.
inline QStringList list() {
    if (qEnvironmentVariableIsSet("FROND_WEBCAMS"))
        return qEnvironmentVariable("FROND_WEBCAMS").split(':', Qt::SkipEmptyParts);
    QStringList found;
#ifdef __linux__
    auto names = QDir("/dev").entryList({"video*"}, QDir::System | QDir::Files | QDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    for (const auto& name : names) {
        const auto path = "/dev/" + name;
        const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        const bool ok = ::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
        ::close(fd);
        if (!ok) continue;
        const auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (caps & V4L2_CAP_VIDEO_CAPTURE) found << path;
    }
#endif
    return found;
}

}  // namespace frondWebcams
