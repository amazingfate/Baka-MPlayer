#ifndef UPDATEMANAGER_H
#define UPDATEMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMap>
#include <QString>

// todo
class UpdateManager : public QObject
{
    Q_OBJECT
public:
    explicit UpdateManager(QObject *parent = 0);

public slots:
    void CheckForUpdates();
#if defined(Q_OS_WIN)
    void DownloadUpdate();
signals:
    void Downloaded(int);
#else
signals:
#endif
    void Update(QMap<QString, QString> info);

    void errorSignal(QString err);

private:
};

#endif // UPDATEMANAGER_H
