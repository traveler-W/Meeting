#include "mytcpsocket.h"
#include "netheader.h"
#include <QHostAddress>
#include <QtEndian>
#include <QMetaObject>
#include <QMutexLocker>

extern QUEUE_DATA<MESG> queue_send;
extern QUEUE_DATA<MESG> queue_recv;
extern QUEUE_DATA<MESG> audio_recv;//extern就是它的定义在其他的类中实现了

void MyTcpSocket::stopImmediately()
{
    {
        QMutexLocker lock(&m_lock);
        if(m_isCanRun == true) m_isCanRun = false;
    }
    //关闭read
    _sockThread->quit();
    _sockThread->wait();
}

void MyTcpSocket::closeSocket()
{
	if (_socktcp && _socktcp->isOpen())
	{
		_socktcp->close();
	}
}

MyTcpSocket::MyTcpSocket(QObject *par):QThread(par)
{
    qRegisterMetaType<QAbstractSocket::SocketError>();
	_socktcp = nullptr;

    _sockThread = new QThread(); //发送数据线程
    this->moveToThread(_sockThread);
	connect(_sockThread, SIGNAL(finished()), this, SLOT(closeSocket()));
    sendbuf =(uchar *) malloc(4 * MB);
    recvbuf = (uchar*)malloc(4 * MB);
    hasrecvive = 0;

}


void MyTcpSocket::errorDetect(QAbstractSocket::SocketError error)
{
    qDebug() <<"Sock error" <<QThread::currentThreadId();
    MESG * msg = (MESG *) malloc(sizeof (MESG));
    if (msg == NULL)
    {
        qDebug() << "errdect malloc error";
    }
    else
    {
        memset(msg, 0, sizeof(MESG));
		if (error == QAbstractSocket::RemoteHostClosedError)
		{
			msg->msg_type = RemoteHostClosedError;
		}
		else
		{
			msg->msg_type = OtherNetError;
		}
		queue_recv.push_msg(msg);
    }
}



void MyTcpSocket::sendData(MESG* send)
{
	if (_socktcp->state() == QAbstractSocket::UnconnectedState)
	{
        emit sendTextOver();
		if (send->data) free(send->data);
		if (send) free(send);
		return;
	}
	quint64 bytestowrite = 0;
	//构造消息头
	sendbuf[bytestowrite++] = '$';

	//消息类型
	qToBigEndian<quint16>(send->msg_type, sendbuf + bytestowrite);
	bytestowrite += 2;

	//发送者ip
	quint32 ip = _socktcp->localAddress().toIPv4Address();
	qToBigEndian<quint32>(ip, sendbuf + bytestowrite);
	bytestowrite += 4;

    if (send->msg_type == CREATE_MEETING || send->msg_type == AUDIO_SEND || send->msg_type == CLOSE_CAMERA || send->msg_type == IMG_SEND || send->msg_type == TEXT_SEND) //创建会议,发送音频,关闭摄像头，发送图片
	{
		//发送数据大小
		qToBigEndian<quint32>(send->len, sendbuf + bytestowrite);
		bytestowrite += 4;
	}
	else if (send->msg_type == JOIN_MEETING)
	{
		qToBigEndian<quint32>(send->len, sendbuf + bytestowrite);
		bytestowrite += 4;
		uint32_t room;
		memcpy(&room, send->data, send->len);
		qToBigEndian<quint32>(room, send->data);
	}
    //qDebug()<<"文本数据大小:"<<send->len;
	//将数据拷入sendbuf
	memcpy(sendbuf + bytestowrite, send->data, send->len);
	bytestowrite += send->len;
    //qDebug()<<bytestowrite;
	sendbuf[bytestowrite++] = '#'; //结尾字符

	//----------------write to server-------------------------
	qint64 hastowrite = bytestowrite;
	qint64 ret = 0, haswrite = 0;
	while ((ret = _socktcp->write((char*)sendbuf + haswrite, hastowrite - haswrite)) < hastowrite)
	{
		if (ret == -1 && _socktcp->error() == QAbstractSocket::TemporaryError)
		{
            ret = 0;
		}
		else if (ret == -1)
		{
			qDebug() << "network error";
			break;
		}
		haswrite += ret;
		hastowrite -= ret;
	}

	_socktcp->waitForBytesWritten();

    if(send->msg_type == TEXT_SEND)
    {
        emit sendTextOver(); //成功往内核发送文本信息
    }


	if (send->data)
	{
		free(send->data);
	}
	//free
	if (send)
	{
		free(send);
	}
}

/*
 * 发送线程
 */
void MyTcpSocket::run()//不断从消息队列中取出消息进行发送
{
    //qDebug() << "send data" << QThread::currentThreadId();
    m_isCanRun = true; //标记可以运行
    /*
    *$_MSGType_IPV4_MSGSize_data_# //
    * 1 2 4 4 MSGSize 1
    *底层写数据线程
    */
    for(;;)
    {
        {
            QMutexLocker locker(&m_lock);
            if(m_isCanRun == false) return; //在每次循环判断是否可以运行，如果不行就退出循环
        }
        
        //构造消息体
        MESG * send = queue_send.pop_msg();
        if(send == NULL) continue;
        QMetaObject::invokeMethod(this, "sendData", Q_ARG(MESG*, send));
        //1.因为此类没有继承于qobject所以无法使用信号与槽的机制
        //这里使用QMetaObject::invokeMethod可以通过对象传递参数，第一个参数就是要传递到的对象，第二个参数就是传递到对象的函数的函数名，第三个就是传递参数
        //的类型和数据
        //2.当然这个函数还有一个作用，那就是异步执行函数以便提高效率,主要是为了防止gui卡死，减小gui的工作量，
        //如果是无参数的调用的话QMetaObject::invokeMethod(this, "load", Qt::QueuedConnection);//异步执行load函数
        /*type常用有以下三种可能：

            Qt::DirectConnection： 立即调用
            Qt::QueuedConnection,当控制返回到接收方线程的事件循环时，将调用该槽，该槽在接收方的线程中执行；
            Qt::AutoConnection, 同一线程，同步调用；否则异步调用*/
    }
}


qint64 MyTcpSocket::readn(char * buf, quint64 maxsize, int n)
{
    quint64 hastoread = n;
    quint64 hasread = 0;
    do
    {
        qint64 ret  = _socktcp->read(buf + hasread, hastoread);
        if(ret < 0)
        {
            return -1;
        }
        if(ret == 0)
        {
            return hasread;
        }
        hasread += ret;
        hastoread -= ret;
    }while(hastoread > 0 && hasread < maxsize);
    return hasread;
}
quint16 chage_tobig(unsigned char* src)
{
    quint16 type;
    qFromBigEndian<quint16>(src + 1, 2, &type);
    return type;
}

void MyTcpSocket::recvFromSocket()
{


    //qDebug()<<"进行一次";
    //qDebug() << "recv data socket" <<QThread::currentThread();
    /*
    *$_msgtype_ip_size_data_#
    */

    qint64 availbytes = _socktcp->bytesAvailable();//socket读取读取数据缓冲区的数据个数
	if (availbytes <=0 )
	{
		return;
	}
    qint64 ret = _socktcp->read((char *) recvbuf + hasrecvive, availbytes);
    if (ret <= 0)
    {
        qDebug() << "error or no more data";
		return;
    }
    hasrecvive += ret;
    qDebug()<<"读取后的has:"<<hasrecvive<<"和"<<availbytes;
    //数据包不够
    if (hasrecvive < MSG_HEADER)
    {
        qDebug()<<"data too short";
        return;
    }
    else
    {
        //这里由拆包顺序来看，包的结构头一共是11个字节，11个字节中第一个字节是‘#’确认收包是否正确的标识符，然后从1~2是消息的类型数据
        //3~7是ip地址，8~11是数据的大小
        quint32 data_size;
        qFromBigEndian<quint32>(recvbuf + 7, 4, &data_size);
        qDebug()<<"数据大小:"<<(quint64)data_size;
        if (((quint64)data_size + 1 + MSG_HEADER) <= hasrecvive) //收够一个包
        {


            if (recvbuf[0] == '$' && recvbuf[MSG_HEADER + (quint64)data_size] == '#') //且包结构正确
            {

                //进行拆包
				MSG_TYPE msgtype;
                quint16 type;
                //qFromBigEndian<quint16>(recvbuf + 1, 2, &type);
                type = chage_tobig(recvbuf);
				msgtype = (MSG_TYPE)type;
                //qDebug()<<"第一处:"<<(quint64)data_size;
				qDebug() << "recv data type: " << msgtype;
				if (msgtype == CREATE_MEETING_RESPONSE || msgtype == JOIN_MEETING_RESPONSE || msgtype == PARTNER_JOIN2)
				{

					if (msgtype == CREATE_MEETING_RESPONSE)
					{
						qint32 roomNo;
						qFromBigEndian<qint32>(recvbuf + MSG_HEADER, 4, &roomNo);

						MESG* msg = (MESG*)malloc(sizeof(MESG));

						if (msg == NULL)
						{
							qDebug() << __LINE__ << " CREATE_MEETING_RESPONSE malloc MESG failed";
						}
						else
						{
							memset(msg, 0, sizeof(MESG));
							msg->msg_type = msgtype;
							msg->data = (uchar*)malloc((quint64)data_size);
							if (msg->data == NULL)
							{
								free(msg);
								qDebug() << __LINE__ << "CREATE_MEETING_RESPONSE malloc MESG.data failed";
							}
							else
							{
								memset(msg->data, 0, (quint64)data_size);
								memcpy(msg->data, &roomNo, data_size);
								msg->len = data_size;
								queue_recv.push_msg(msg);
							}

						}
					}
					else if (msgtype == JOIN_MEETING_RESPONSE)
					{
						qint32 c;

						memcpy(&c, recvbuf + MSG_HEADER, data_size);
                        qDebug()<<"传回的标识为:"<<c;
						MESG* msg = (MESG*)malloc(sizeof(MESG));

						if (msg == NULL)
						{
							qDebug() << __LINE__ << "JOIN_MEETING_RESPONSE malloc MESG failed";
						}
						else
						{
							memset(msg, 0, sizeof(MESG));
							msg->msg_type = msgtype;
							msg->data = (uchar*)malloc(data_size);
							if (msg->data == NULL)
							{
								free(msg);
								qDebug() << __LINE__ << "JOIN_MEETING_RESPONSE malloc MESG.data failed";
							}
							else
							{
								memset(msg->data, 0, data_size);
								memcpy(msg->data, &c, data_size);

								msg->len = data_size;
                                //qDebug()<<c;
								queue_recv.push_msg(msg);
							}
						}
					}
					else if (msgtype == PARTNER_JOIN2)
					{
						MESG* msg = (MESG*)malloc(sizeof(MESG));
						if (msg == NULL)
						{
							qDebug() << "PARTNER_JOIN2 malloc MESG error";
						}
						else
						{
							memset(msg, 0, sizeof(MESG));
							msg->msg_type = msgtype;
							msg->len = data_size;
							msg->data = (uchar*)malloc(data_size);
							if (msg->data == NULL)
							{
								free(msg);
								qDebug() << "PARTNER_JOIN2 malloc MESG.data error";
							}
							else
							{
								memset(msg->data, 0, data_size);
								uint32_t ip;
								int pos = 0;
								for (int i = 0; i < data_size / sizeof(uint32_t); i++)
								{
									qFromBigEndian<uint32_t>(recvbuf + MSG_HEADER + pos, sizeof(uint32_t), &ip);
									memcpy_s(msg->data + pos, data_size - pos, &ip, sizeof(uint32_t));
									pos += sizeof(uint32_t);
								}
								queue_recv.push_msg(msg);
							}

						}
					}
				}
                else if (msgtype == IMG_RECV || msgtype == PARTNER_JOIN || msgtype == PARTNER_EXIT || msgtype == AUDIO_RECV || msgtype == CLOSE_CAMERA || msgtype == TEXT_RECV)
				{
					//read ipv4
					quint32 ip;
					qFromBigEndian<quint32>(recvbuf + 3, 4, &ip);

					if (msgtype == IMG_RECV)
					{
						//QString ss = QString::fromLatin1((char *)recvbuf + MSG_HEADER, data_len);
						QByteArray cc((char *) recvbuf + MSG_HEADER, data_size);
                        QByteArray rc = QByteArray::fromBase64(cc);//解码
                        QByteArray rdc = qUncompress(rc);//解压
						//将消息加入到接收队列
		//                qDebug() << roomNo;
						
						if (rdc.size() > 0)
						{
							MESG* msg = (MESG*)malloc(sizeof(MESG));
							if (msg == NULL)
							{
								qDebug() << __LINE__ << " malloc failed";
							}
							else
							{
								memset(msg, 0, sizeof(MESG));
								msg->msg_type = msgtype;
								msg->data = (uchar*)malloc(rdc.size()); // 10 = format + width + width
								if (msg->data == NULL)
								{
									free(msg);
									qDebug() << __LINE__ << " malloc failed";
								}
								else
								{
									memset(msg->data, 0, rdc.size());
									memcpy_s(msg->data, rdc.size(), rdc.data(), rdc.size());
									msg->len = rdc.size();
									msg->ip = ip;
									queue_recv.push_msg(msg);
								}
							}
						}
					}
					else if (msgtype == PARTNER_JOIN || msgtype == PARTNER_EXIT || msgtype == CLOSE_CAMERA)
					{
						MESG* msg = (MESG*)malloc(sizeof(MESG));
						if (msg == NULL)
						{
							qDebug() << __LINE__ << " malloc failed";
						}
						else
						{
							memset(msg, 0, sizeof(MESG));
							msg->msg_type = msgtype;
							msg->ip = ip;
							queue_recv.push_msg(msg);
						}
					}
					else if (msgtype == AUDIO_RECV)
					{
						//解压缩
						QByteArray cc((char*)recvbuf + MSG_HEADER, data_size);
						QByteArray rc = QByteArray::fromBase64(cc);
						QByteArray rdc = qUncompress(rc);

						if (rdc.size() > 0)
						{
							MESG* msg = (MESG*)malloc(sizeof(MESG));
							if (msg == NULL)
							{
								qDebug() << __LINE__ << "malloc failed";
							}
							else
							{
								memset(msg, 0, sizeof(MESG));
								msg->msg_type = AUDIO_RECV;
								msg->ip = ip;

								msg->data = (uchar*)malloc(rdc.size());
								if (msg->data == nullptr)
								{
                                    free(msg);
									qDebug() << __LINE__ << "malloc msg.data failed";
								}
								else
								{
									memset(msg->data, 0, rdc.size());
									memcpy_s(msg->data, rdc.size(), rdc.data(), rdc.size());
									msg->len = rdc.size();
									msg->ip = ip;
									audio_recv.push_msg(msg);
								}
							}
						}
					}
                    else if(msgtype == TEXT_RECV)
                    {
                        //解压缩
                        QByteArray cc((char *)recvbuf + MSG_HEADER, data_size);

                        std::string rr = qUncompress(cc).toStdString();
                        //qDebug()<<rr.data()<<"走着了";
                        if(rr.size() > 0)
                        {
                            MESG* msg = (MESG*)malloc(sizeof(MESG));
                            if (msg == NULL)
                            {
                                qDebug() << __LINE__ << "malloc failed";
                            }
                            else
                            {
                                memset(msg, 0, sizeof(MESG));
                                msg->msg_type = TEXT_RECV;
                                msg->ip = ip;
                                msg->data = (uchar*)malloc(rr.size());
                                if (msg->data == nullptr)
                                {
                                    free(msg);
                                    qDebug() << __LINE__ << "malloc msg.data failed";
                                }
                                else
                                {
                                    memset(msg->data, 0, rr.size());
                                    memcpy_s(msg->data, rr.size(), rr.data(), rr.size());
                                    msg->len = rr.size();
                                    queue_recv.push_msg(msg);
                                }
                            }
                        }
                    }
                }
			}
            else
            {
                qDebug() << "package error";
            }
            //qDebug()<<(quint64)data_size ;
			memmove_s(recvbuf, 4 * MB, recvbuf + MSG_HEADER + data_size + 1, hasrecvive - ((quint64)data_size + 1 + MSG_HEADER));
            hasrecvive -= ((quint64)data_size + 1 + MSG_HEADER);


            qDebug()<<(quint64)data_size + 1 + MSG_HEADER<<",重置后的has:"<<hasrecvive;
        }
        else
        {
            qDebug()<<"收的数据不够一个包";
            return;
        }
    }
}


MyTcpSocket::~MyTcpSocket()
{
    delete sendbuf;
    delete _sockThread;
}



bool MyTcpSocket::connectServer(QString ip, QString port, QIODevice::OpenModeFlag flag)
{
    if(_socktcp == nullptr) _socktcp = new QTcpSocket(); //tcp
    _socktcp->connectToHost(ip, port.toUShort(), flag);
    connect(_socktcp, SIGNAL(readyRead()), this, SLOT(recvFromSocket()), Qt::UniqueConnection); //接受数据
    //处理套接字错误
    connect(_socktcp, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(errorDetect(QAbstractSocket::SocketError)),Qt::UniqueConnection);

    if(_socktcp->waitForConnected(5000))
    {
        return true;
    }
	_socktcp->close();
    return false;
}


bool MyTcpSocket::connectToServer(QString ip, QString port, QIODevice::OpenModeFlag flag)
{
	_sockThread->start(); // 开启链接，与接受
	bool retVal;
	QMetaObject::invokeMethod(this, "connectServer", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, retVal),
								Q_ARG(QString, ip), Q_ARG(QString, port), Q_ARG(QIODevice::OpenModeFlag, flag));

	if (retVal)
	{
        this->start() ; //写数据
		return true;
	}
	else
	{
		return false;
	}
}

QString MyTcpSocket::errorString()
{
    return _socktcp->errorString();
}

void MyTcpSocket::disconnectFromHost()
{
    //write
    if(this->isRunning())
    {
        QMutexLocker locker(&m_lock);
        m_isCanRun = false;
    }

    if(_sockThread->isRunning()) //read
    {
        _sockThread->quit();
        _sockThread->wait();
    }

    //清空 发送 队列，清空接受队列
    queue_send.clear();
    queue_recv.clear();
	audio_recv.clear();
}


quint32 MyTcpSocket::getlocalip()
{
    if(_socktcp->isOpen())
    {
        return _socktcp->localAddress().toIPv4Address();
    }
    else
    {
        return -1;
    }
}
