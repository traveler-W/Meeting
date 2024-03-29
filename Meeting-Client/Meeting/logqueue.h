#ifndef LOGQUEUE_H
#define LOGQUEUE_H

#include <QThread>
#include <QMutex>
#include <queue>
#include "netheader.h"

class LogQueue : public QThread
{
private:
    void run();
    QMutex m_lock;//互斥锁
    bool m_isCanRun;//判断线程函数是否在运行

    QUEUE_DATA<Log> log_queue;//使用一个队列容器来存储日志文件信息，log类型就是日志文件类型
    FILE *logfile;//定义日志文件
public:
    explicit LogQueue(QObject *parent = nullptr);
    void stopImmediately();
    void pushLog(Log*);

};

#endif // LOGQUEUE_H
