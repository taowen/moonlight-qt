#include "logmanager.h"
#include "path.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QRegularExpression>
#include <QFileSystemWatcher>

// 初始化静态实例指针
LogManager* LogManager::s_instance = nullptr;

LogManager::LogManager(QObject *parent) : QObject(parent)
{
    // 获取日志目录
    m_LogDir = Path::getLogDir();
    
    // 创建文件系统监视器
    m_fileWatcher = new QFileSystemWatcher(this);
    
    // 连接文件变化信号
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, 
            this, &LogManager::onFileChanged);
    
    findLatestLogFile();
    
    // 设置单例实例
    s_instance = this;
}

LogManager* LogManager::getLogManagerInstance()
{
    return s_instance;
}

QString LogManager::latestLogPath()
{
    return m_LatestLogPath;
}

QStringList LogManager::readLogFile(const QString &filePath, int maxLines)
{
    QStringList logLines;
    QFile file(filePath);
    
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        
        // 读取所有行
        QStringList allLines;
        while (!in.atEnd()) {
            allLines.append(in.readLine());
        }
        
        // 只返回最后的 maxLines 行
        int startIndex = qMax(0, allLines.size() - maxLines);
        for (int i = startIndex; i < allLines.size(); i++) {
            logLines.append(allLines[i]);
        }
        
        file.close();
    }
    
    // 返回的日志行是从旧到新排序的
    return logLines;
}

QStringList LogManager::getLogFilesList()
{
    QDir logDir(m_LogDir);
    QStringList filters;
    filters << "Moonlight-*.log";
    
    // 按时间排序，最新的在前面
    QStringList logFiles = logDir.entryList(filters, QDir::Files, QDir::Time);
    
    // 转换为完整路径
    QStringList fullPaths;
    for (const QString &file : logFiles) {
        fullPaths.append(logDir.filePath(file));
    }
    
    return fullPaths;
}

QString LogManager::getLogDir()
{
    return m_LogDir;
}

void LogManager::findLatestLogFile()
{
    QStringList logFiles = getLogFilesList();
    if (!logFiles.isEmpty()) {
        // 如果当前已有监视的文件，先移除
        if (!m_LatestLogPath.isEmpty() && m_fileWatcher->files().contains(m_LatestLogPath)) {
            m_fileWatcher->removePath(m_LatestLogPath);
        }
        
        m_LatestLogPath = logFiles.first();
        
        // 添加新文件到监视器
        m_fileWatcher->addPath(m_LatestLogPath);
        
        // 更新当前日志内容
        updateCurrentLogContent();
        
        emit latestLogPathChanged();
    }
}

void LogManager::onFileChanged(const QString &path)
{
    // 文件变化时更新日志内容
    if (path == m_LatestLogPath) {
        updateCurrentLogContent();
        emit logFileChanged();
        
        // 某些系统在文件被修改后可能会删除监视，需要重新添加
        if (!m_fileWatcher->files().contains(path)) {
            m_fileWatcher->addPath(path);
        }
    }
}

void LogManager::updateCurrentLogContent()
{
    if (!m_LatestLogPath.isEmpty()) {
        m_CurrentLogContent = readLogFile(m_LatestLogPath);
        emit logContentChanged();
    }
}

void LogManager::onNewLogEntry(const QString &entry)
{
    // 添加新的日志条目到当前内容
    if (!entry.isEmpty()) {
        // 移除换行符
        QString cleanEntry = entry;
        cleanEntry.remove(QRegularExpression("[\r\n]"));
        
        if (!cleanEntry.isEmpty()) {
            m_CurrentLogContent.append(cleanEntry);
            
            // 限制日志行数，保持最新的1000行
            while (m_CurrentLogContent.size() > 1000) {
                m_CurrentLogContent.removeFirst();
            }
            
            emit logContentChanged();
        }
    }
}
