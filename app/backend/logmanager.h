#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <QObject>
#include <QStringList>
#include <QDir>

class LogManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString latestLogPath READ latestLogPath NOTIFY latestLogPathChanged)
    
public:
    explicit LogManager(QObject *parent = nullptr);
    
    Q_INVOKABLE QString latestLogPath();
    Q_INVOKABLE QStringList readLogFile(const QString &filePath, int maxLines = 1000);
    Q_INVOKABLE QStringList getLogFilesList();
    Q_INVOKABLE QString getLogDir();
    
signals:
    void latestLogPathChanged();
    
private:
    QString m_LogDir;
    QString m_LatestLogPath;
    
    void findLatestLogFile();
};

#endif // LOGMANAGER_H 