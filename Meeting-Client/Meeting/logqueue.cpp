#include "logqueue.h"
#include <QDebug>

LogQueue::LogQueue(QObject *parent) : QThread(parent)
{

}

void LogQueue::pushLog(Log* log)
{
    log_queue.push_msg(log);
}

void LogQueue::run()
{
    m_isCanRun = true;
    for(;;)
    {
        {
            QMutexLocker lock(&m_lock);
            if(m_isCanRun == false)
            {
                fclose(logfile);
                return;
            }//如果线程函数时处于关闭状态也随即关闭日志文件
        }
        Log *log = log_queue.pop_msg();//将文件压入队列
        if(log == NULL || log->ptr == NULL) continue;

        //----------------write to logfile-------------------
        errno_t r = fopen_s(&logfile, "./log.txt", "a");//如果文件不存在创建文件并且从文件结尾追加
        if(r != 0)
        {
            qDebug() << "打开文件失败:" << r;
            continue;
        }


        qint64 hastowrite = log->len;
        qint64 ret = 0, haswrite = 0;
        //数据块写入：fwrite();

        //参数1：要获取的数据的地址

        //参数2：要写入内容的单字节数

        //参数3：要写入size字节的数据项的个数

        //参数4：目标文件指针

        //返回值：返回实际写入的数据块的数目

        //作用：向文件写入数据块，以二进制形式对文件进行操作，不局限于文本文件。
        while ((ret = fwrite( (char*)log->ptr + haswrite, 1 ,hastowrite - haswrite, logfile)) < hastowrite)
        {
            if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
            {
                ret = 0;
            }
            else
            {
                qDebug() << "write logfile error";
                break;
            }
            haswrite += ret;
            hastowrite -= ret;
        }//一个经典的c语言数据写入方法，非常值得借鉴

        //free
        if(log->ptr) free(log->ptr);
        if(log) free(log);

        fflush(logfile);
        fclose(logfile);
    }
}

void LogQueue::stopImmediately()//明显是一个
{
    QMutexLocker lock(&m_lock);
    m_isCanRun = false;
}
