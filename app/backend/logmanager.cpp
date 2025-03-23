#include "logmanager.h"
#include "path.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QRegularExpression>

LogManager::LogManager(QObject *parent) : QObject(parent)
{
    // 获取日志目录
    m_LogDir = Path::getLogDir();
    findLatestLogFile();
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
        m_LatestLogPath = logFiles.first();
        emit latestLogPathChanged();
    }
}
