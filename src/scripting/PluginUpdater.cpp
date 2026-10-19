#include "PluginUpdater.h"

#include "JsPluginLoader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <utility>

namespace {
const QString apiRoot = QStringLiteral("https://api.github.com/repos/JKWTCN/BatteryMonitor");
constexpr qsizetype maxPluginBytes = 1024 * 1024;
constexpr qsizetype maxTotalBytes = 16 * 1024 * 1024;

QByteArray blobHash(const QByteArray &data)
{
    QByteArray header = "blob " + QByteArray::number(data.size());
    header.append('\0');
    return QCryptographicHash::hash(header + data, QCryptographicHash::Sha1).toHex();
}

bool safePath(const QString &path)
{
    static const QRegularExpression component(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9_.-]*$"));
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (path.size() > 200 || !path.endsWith(QStringLiteral(".js")))
        return false;
    const auto parts = path.split('/');
    for (const auto &part : parts) {
        if (!component.match(part).hasMatch() || reserved.match(part).hasMatch()
            || part.endsWith('.') || part.size() > 100)
            return false;
    }
    return !parts.last().startsWith('_');
}

// Reject symlinks and Windows junctions in every existing destination component.
bool safeDestination(const QString &root, const QString &relative)
{
    QString path = root;
    const auto parts = relative.split('/');
    for (int i = -1; i < parts.size(); ++i) {
        if (i >= 0) path += '/' + parts[i];
        const QFileInfo info(path);
        if (info.isSymbolicLink() || info.isJunction()) return false;
        if (info.exists() && (i == parts.size() - 1 ? !info.isFile() : !info.isDir()))
            return false;
    }
    return true;
}
}

PluginUpdater::PluginUpdater(QObject *parent) : QObject(parent) {}

void PluginUpdater::fail(const QString &error)
{
    m_running = false;
    m_plugins.clear();
    emit finished(false, 0, error);
}

void PluginUpdater::request(const QString &url, bool raw, qsizetype limit,
                            std::function<void(const QByteArray &)> callback)
{
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", "BatteryMonitor-PluginUpdater");
    req.setRawHeader("Accept", raw ? "application/vnd.github.raw+json" : "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    req.setTransferTimeout(15000);
    auto *reply = m_network.get(req);
    reply->setReadBufferSize(64 * 1024);
    auto buffer = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(30000);
    connect(reply, &QIODevice::readyRead, this, [reply, buffer, tooLarge, limit] {
        buffer->append(reply->readAll());
        if (buffer->size() > limit) {
            *tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, deadline, buffer, tooLarge, limit, callback = std::move(callback)] {
        deadline->stop();
        buffer->append(reply->readAll());
        reply->deleteLater();
        if (*tooLarge || buffer->size() > limit) {
            fail(tr("The GitHub response exceeds the download size limit."));
        } else if (reply->error() != QNetworkReply::NoError
                   || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            fail(tr("GitHub download failed (HTTP %1): %2")
                     .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                     .arg(reply->errorString()));
        } else {
            callback(*buffer);
        }
    });
}

void PluginUpdater::start()
{
    if (m_running) return;
    m_running = true;
    m_plugins.clear();
    m_index = 0;
    m_bytes = 0;
    emit progress(0, 0);
    request(apiRoot + QStringLiteral("/git/trees/master?recursive=1"), false,
            8 * 1024 * 1024, [this](const QByteArray &data) {
        const auto doc = QJsonDocument::fromJson(data);
        const auto obj = doc.object();
        if (!doc.isObject() || !obj.value("tree").isArray()
            || !obj.value("truncated").isBool() || obj.value("truncated").toBool()) {
            fail(tr("GitHub returned an invalid or incomplete plugin list."));
            return;
        }
        QSet<QString> paths;
        static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{40}$"));
        for (const auto &value : obj.value("tree").toArray()) {
            const auto entry = value.toObject();
            const QString path = entry.value("path").toString();
            if (!path.startsWith(QStringLiteral("plugins/")) || !path.endsWith(QStringLiteral(".js")))
                continue;
            const QString relative = path.mid(8);
            const QString sha = entry.value("sha").toString();
            const auto size = entry.value("size").toInteger(-1);
            if (!safePath(relative) || entry.value("type").toString() != QStringLiteral("blob")
                || entry.value("mode").toString() != QStringLiteral("100644")
                || !shaPattern.match(sha).hasMatch() || size < 0 || size > maxPluginBytes
                || paths.contains(relative.toCaseFolded()) || m_plugins.size() >= 1000) {
                fail(tr("GitHub returned an unsafe or unsupported plugin entry: %1").arg(path));
                return;
            }
            paths.insert(relative.toCaseFolded());
            m_plugins.append({relative, sha.toLatin1(), {}});
        }
        if (m_plugins.isEmpty()) {
            fail(tr("No plugins were found in the GitHub repository."));
            return;
        }
        downloadNext();
    });
}

void PluginUpdater::downloadNext()
{
    emit progress(m_index, m_plugins.size());
    if (m_index == m_plugins.size()) {
        install();
        return;
    }
    request(apiRoot + QStringLiteral("/git/blobs/") + QString::fromLatin1(m_plugins[m_index].sha),
            true, maxPluginBytes, [this](const QByteArray &data) {
        auto &plugin = m_plugins[m_index];
        if (blobHash(data) != plugin.sha) {
            fail(tr("Plugin integrity check failed: %1").arg(plugin.path));
            return;
        }
        m_bytes += data.size();
        if (m_bytes > maxTotalBytes) {
            fail(tr("The GitHub response exceeds the download size limit."));
            return;
        }
        plugin.data = data;
        ++m_index;
        downloadNext();
    });
}

void PluginUpdater::install()
{
    const QString root = QString::fromStdWString(JsPluginLoader::defaultPluginDir());
    QTemporaryDir staging(QFileInfo(root).absolutePath() + QStringLiteral("/.plugin-update-XXXXXX"));
    if (!staging.isValid()) {
        fail(tr("Cannot write to the application directory. Check its write permissions."));
        return;
    }
    QVector<int> changed;
    for (int i = 0; i < m_plugins.size(); ++i) {
        const auto &plugin = m_plugins[i];
        if (!safeDestination(root, plugin.path)) {
            fail(tr("Unsafe plugin destination: %1").arg(plugin.path));
            return;
        }
        QFile existing(root + '/' + plugin.path);
        if (existing.exists()) {
            if (!existing.open(QIODevice::ReadOnly)) {
                fail(tr("Cannot read the existing plugin: %1").arg(plugin.path));
                return;
            }
            if (existing.size() == plugin.data.size() && existing.readAll() == plugin.data)
                continue;
        }
        QFile staged(staging.filePath(QString::number(i)));
        if (!staged.open(QIODevice::WriteOnly) || staged.write(plugin.data) != plugin.data.size()
            || !staged.flush()) {
            fail(tr("Cannot stage the plugin update: %1").arg(plugin.path));
            return;
        }
        changed.append(i);
    }

    QVector<int> installed;
    QString failedPath;
    QStringList recoveryFailures;
    for (int i : changed) {
        const auto &plugin = m_plugins[i];
        const QString target = root + '/' + plugin.path;
        const QString backup = staging.filePath(QStringLiteral("backup/") + plugin.path);
        if (!safeDestination(root, plugin.path) || !QDir().mkpath(QFileInfo(target).absolutePath())) {
            failedPath = plugin.path;
            break;
        }
        const bool existed = QFileInfo::exists(target);
        if (existed && (!QDir().mkpath(QFileInfo(backup).absolutePath()) || !QFile::rename(target, backup))) {
            failedPath = plugin.path;
            break;
        }
        if (!QFile::rename(staging.filePath(QString::number(i)), target)) {
            if (existed && !QFile::rename(backup, target)) recoveryFailures.append(plugin.path);
            failedPath = plugin.path;
            break;
        }
        installed.append(i);
    }
    if (!failedPath.isEmpty()) {
        for (auto it = installed.crbegin(); it != installed.crend(); ++it) {
            const auto &plugin = m_plugins[*it];
            const QString target = root + '/' + plugin.path;
            const QString backup = staging.filePath(QStringLiteral("backup/") + plugin.path);
            if (!QFile::remove(target) || (QFileInfo::exists(backup) && !QFile::rename(backup, target)))
                recoveryFailures.append(plugin.path);
        }
        QString error = tr("Cannot install the plugin update: %1. Check write permissions and file locks.").arg(failedPath);
        if (!recoveryFailures.isEmpty()) {
            staging.setAutoRemove(false);
            error += '\n' + tr("Some files could not be restored. Backups are in: %1").arg(staging.path());
        }
        fail(error);
        return;
    }
    const int count = changed.size();
    m_plugins.clear();
    m_running = false;
    emit finished(true, count, {});
}
