#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

// Downloads official plugins by immutable Git blob ID. No plugin code is executed.
class PluginUpdater : public QObject
{
    Q_OBJECT
public:
    explicit PluginUpdater(QObject *parent = nullptr);
    bool isRunning() const { return m_running; }
    void start();

signals:
    void progress(int current, int total);
    void finished(bool success, int updated, const QString &error);

private:
    struct Plugin {
        QString path;
        QByteArray sha;
        QByteArray data;
    };
    void request(const QString &url, bool raw, qsizetype limit,
                 std::function<void(const QByteArray &)> callback);
    void downloadNext();
    void install();
    void fail(const QString &error);

    QNetworkAccessManager m_network;
    QVector<Plugin> m_plugins;
    int m_index = 0;
    qsizetype m_bytes = 0;
    bool m_running = false;
};
