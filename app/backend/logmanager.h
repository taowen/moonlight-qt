#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <QObject>
#include <QStringList>
#include <QDir>
#include <QFileSystemWatcher>

class LogManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString latestLogPath READ latestLogPath NOTIFY latestLogPathChanged)
    Q_PROPERTY(QStringList currentLogContent READ currentLogContent NOTIFY logContentChanged)

public:
    explicit LogManager(QObject *parent = nullptr);
    ~LogManager();
    
    // 获取单例实例
    static LogManager* getLogManagerInstance();
    
    QString latestLogPath();
    QStringList currentLogContent() const { return m_CurrentLogContent; }
    
    Q_INVOKABLE QStringList readLogFile(const QString &filePath, int maxLines = 1000);
    Q_INVOKABLE QStringList getLogFilesList();
    Q_INVOKABLE QString getLogDir();

signals:
    void latestLogPathChanged();
    void logFileChanged();
    void logContentChanged();

public slots:
    void onNewLogEntry(const QString &entry);

private slots:
    void onFileChanged(const QString &path);
    
private:
    void findLatestLogFile();
    void updateCurrentLogContent();
    
    QString m_LogDir;
    QString m_LatestLogPath;
    QStringList m_CurrentLogContent;
    QFileSystemWatcher *m_fileWatcher;
    
    static LogManager* s_instance;
};

#endif // LOGMANAGER_H 
