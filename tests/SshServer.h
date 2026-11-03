#pragma once

// A throwaway OpenSSH server for tests: runs as the current user on
// 127.0.0.1 with its own host key, accepts one generated client key (and
// optionally passwords are off). Tandem's sftp backend is pointed at it via
// TANDEM_SSH_* variables, so ~/.ssh is never read or written.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

class SshServer {
public:
    bool start()
    {
        if (QStandardPaths::findExecutable("sshd", {"/usr/bin", "/usr/sbin"}).isEmpty() ||
            QStandardPaths::findExecutable("ssh-keygen").isEmpty())
            return false;
        const QString d = m_dir.path();
        for (const char* key : {"host_key", "client_key"})
            if (QProcess::execute("ssh-keygen", {"-q", "-t", "ed25519", "-N", "", "-f", d + "/" + key}) != 0)
                return false;
        QFile::copy(d + "/client_key.pub", d + "/authorized_keys");
        m_port = freePort();
        QFile cfg(d + "/sshd_config");
        if (!cfg.open(QIODevice::WriteOnly))
            return false;
        cfg.write(QStringLiteral("Port %1\nListenAddress 127.0.0.1\nHostKey %2/host_key\n"
                                 "AuthorizedKeysFile %2/authorized_keys\nPidFile %2/sshd.pid\nUsePAM no\n"
                                 "StrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n"
                                 "Subsystem sftp internal-sftp\nLogLevel ERROR\n")
                      .arg(m_port)
                      .arg(d)
                      .toUtf8());
        cfg.close();
        QFile emptyConfig(d + "/ssh_config");  // empty: ignore ~/.ssh/config
        if (!emptyConfig.open(QIODevice::WriteOnly))
            return false;
        m_proc.setProcessChannelMode(QProcess::MergedChannels);
        m_proc.start(QStandardPaths::findExecutable("sshd", {"/usr/bin", "/usr/sbin"}), {"-D", "-e", "-f", d + "/sshd_config"});
        if (!m_proc.waitForStarted(3000))
            return false;
        for (int i = 0; i < 50 && !accepting(); ++i)
            QThread::msleep(50);
        qputenv("TANDEM_SSH_KNOWN_HOSTS", (d + "/known_hosts").toUtf8());
        qputenv("TANDEM_SSH_IDENTITY", (d + "/client_key").toUtf8());
        qputenv("TANDEM_SSH_CONFIG", (d + "/ssh_config").toUtf8());
        qunsetenv("SSH_AUTH_SOCK");  // no agent: only the test key
        return accepting();
    }
    ~SshServer()
    {
        if (m_proc.state() != QProcess::NotRunning) {
            m_proc.terminate();
            m_proc.waitForFinished(3000);
        }
    }
    // Stops and starts the server again (same port and keys): every open
    // connection dies, like after a server restart or an idle timeout.
    bool restart()
    {
        // Each connection is a child process that outlives the listener: end those too.
        const qint64 master = m_proc.processId();
#ifdef Q_OS_MACOS
        QProcess ps;
        ps.start(QStringLiteral("/bin/ps"), {QStringLiteral("-axo"), QStringLiteral("pid=,ppid=")});
        if (ps.waitForFinished(3000))
            for (const QByteArray& line : ps.readAllStandardOutput().split('\n')) {
                const auto fields = line.simplified().split(' ');
                if (fields.size() == 2 && fields[1].toLongLong() == master)
                    ::kill(fields[0].toInt(), SIGTERM);
            }
#else
        for (const QString& pid : QDir("/proc").entryList(QDir::Dirs)) {
            QFile stat("/proc/" + pid + "/stat");
            if (!stat.open(QIODevice::ReadOnly))
                continue;
            const QList<QByteArray> f = stat.readAll().split(')').value(1).trimmed().split(' ');
            if (f.size() > 1 && f[1].toLongLong() == master)
                ::kill(pid.toInt(), SIGTERM);
        }
#endif
        m_proc.terminate();
        m_proc.waitForFinished(3000);
        for (int i = 0; i < 50 && accepting(); ++i)
            QThread::msleep(20);
        m_proc.start(m_proc.program(), m_proc.arguments());
        if (!m_proc.waitForStarted(3000))
            return false;
        for (int i = 0; i < 50 && !accepting(); ++i)
            QThread::msleep(50);
        return accepting();
    }
    // "sftp://user@127.0.0.1:port" + an absolute local path (same machine).
    QString location(const QString& path = {}) const
    {
        return QStringLiteral("sftp://%1@127.0.0.1:%2%3").arg(qEnvironmentVariable("USER")).arg(m_port).arg(path.isEmpty() ? "/" : path);
    }
    QString log() { return QString::fromLocal8Bit(m_proc.readAll()); }
    int port() const { return m_port; }

private:
    static int freePort()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        ::close(fd);
        return ntohs(a.sin_port);
    }
    bool accepting() const
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(m_port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        ::close(fd);
        return ok;
    }
    QTemporaryDir m_dir;
    QProcess m_proc;
    int m_port = 0;
};
