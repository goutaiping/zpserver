#include "zp_nettransthread.h"
#include <QTcpSocket>
#include <QSslSocket>
#include <assert.h>
#include <QDebug>
#include <QCoreApplication>
namespace ZPNetwork{
zp_netTransThread::zp_netTransThread(zp_net_ThreadPool *pThreadPool,int nPayLoad,QObject *parent) :
    QObject(parent)
  ,m_pThreadPool(pThreadPool)
{
    m_nPayLoad = nPayLoad;
    m_bActivated = true;
    m_bSSLConnection = true;
    assert(m_nPayLoad>=256 && m_nPayLoad<=16*1024*1024);
}
QList <QObject *> zp_netTransThread::clientsList()
{
    QList <QObject *> lsts ;
    m_mutex_protect.lock();
    lsts = m_clientList.keys();
    m_mutex_protect.unlock();
    return lsts;
}
int zp_netTransThread::CurrentClients()
{
    int nres = 0;
    m_mutex_protect.lock();
    nres = m_clientList.size();
    m_mutex_protect.unlock();
    return nres;
}
void zp_netTransThread::DeactivateImmediately(zp_netTransThread * ptr)
{
    if (ptr!=this)
        return;
    m_bActivated = false;
    this->KickAllClients(ptr);
}

void zp_netTransThread::SetPayload(int nPayload)
{
    m_nPayLoad = nPayload;
    assert(m_nPayLoad>=256 && m_nPayLoad<=16*1024*1024);
}

void zp_netTransThread::incomingConnection(QObject * threadid,qintptr socketDescriptor)
{
    if (threadid!=this)
        return;
    QTcpSocket * sock_client = 0;
    if (m_bSSLConnection)
        sock_client =  new QSslSocket(this);
    else
        sock_client =  new QTcpSocket(this);
    if (sock_client)
    {
        //Initial content
        if (true ==sock_client->setSocketDescriptor(socketDescriptor))
        {
            connect(sock_client, SIGNAL(readyRead()),this, SLOT(new_data_recieved()),Qt::QueuedConnection);
            connect(sock_client, SIGNAL(disconnected()),this,SLOT(client_closed()),Qt::QueuedConnection);
            connect(sock_client, SIGNAL(error(QAbstractSocket::SocketError)),this, SLOT(displayError(QAbstractSocket::SocketError)),Qt::QueuedConnection);
            connect(sock_client, SIGNAL(bytesWritten(qint64)), this, SLOT(some_data_sended(qint64)),Qt::QueuedConnection);
            m_mutex_protect.lock();
            m_clientList[sock_client] = 0;
            m_mutex_protect.unlock();
            if (m_bSSLConnection)
            {
                QSslSocket * psslsock = qobject_cast<QSslSocket *>(sock_client);
                assert(psslsock!=NULL);
                QString strCerPath =  QCoreApplication::applicationDirPath() + "/svr_cert.pem";
                QString strPkPath =  QCoreApplication::applicationDirPath() + "/svr_privkey.pem";
                psslsock->setLocalCertificate(strCerPath);
                psslsock->setPrivateKey(strPkPath);
                connect(psslsock, &QSslSocket::encrypted,this, &zp_netTransThread::on_encrypted,Qt::QueuedConnection);
                psslsock->startServerEncryption();
            }
            else
                emit evt_NewClientConnected(sock_client);
        }
        else
            sock_client->deleteLater();
    }

}
void zp_netTransThread::on_encrypted()
{
     QTcpSocket * pSock = qobject_cast<QTcpSocket*>(sender());
     emit evt_NewClientConnected(pSock);
}

void zp_netTransThread::client_closed()
{
    QTcpSocket * pSock = qobject_cast<QTcpSocket*>(sender());
    if (pSock)
    {
        if (m_bSSLConnection)
        {
            QSslSocket * psslsock = qobject_cast<QSslSocket *>(pSock);
            if (psslsock)
                disconnect(psslsock, &QSslSocket::encrypted,this, &zp_netTransThread::on_encrypted);
        }
        disconnect(pSock, SIGNAL(readyRead()),this, SLOT(new_data_recieved()));
        disconnect(pSock, SIGNAL(disconnected()),this,SLOT(client_closed()));
        disconnect(pSock, SIGNAL(error(QAbstractSocket::SocketError)),this, SLOT(displayError(QAbstractSocket::SocketError)));
        disconnect(pSock, SIGNAL(bytesWritten(qint64)), this, SLOT(some_data_sended(qint64)));
         m_buffer_sending.remove(pSock);
        m_buffer_sending_offset.remove(pSock);
        m_mutex_protect.lock();
        m_clientList.remove(pSock);
        m_mutex_protect.unlock();
        pSock->deleteLater();
        emit evt_ClientDisconnected(pSock);
    }
}
void zp_netTransThread::new_data_recieved()
{
    QTcpSocket * pSock = qobject_cast<QTcpSocket*>(sender());
    if (pSock)
        emit evt_Data_recieved(pSock,pSock->readAll());
}
void zp_netTransThread::some_data_sended(qint64 wsended)
{
    QTcpSocket * pSock = qobject_cast<QTcpSocket*>(sender());
    if (pSock)
    {
        emit evt_Data_transferred(pSock,wsended);
        QList<QByteArray> & list_sock_data = m_buffer_sending[pSock];
        QList<qint64> & list_offset = m_buffer_sending_offset[pSock];
        while (list_sock_data.empty()==false)
        {
            QByteArray & arraySending = *list_sock_data.begin();
            qint64 & currentOffset = *list_offset.begin();
            qint64 nTotalBytes = arraySending.size();
            assert(nTotalBytes>=currentOffset);
            qint64 nBytesWritten = pSock->write(arraySending.constData()+currentOffset,qMin((int)(nTotalBytes-currentOffset),m_nPayLoad));
            currentOffset += nBytesWritten;
            if (currentOffset>=nTotalBytes)
            {
                list_offset.pop_front();
                list_sock_data.pop_front();
            }
            else
                break;
        }
    }
}
void zp_netTransThread::displayError(QAbstractSocket::SocketError socketError)
{
    QTcpSocket * pSock = qobject_cast<QTcpSocket*>(sender());
    if (pSock)
    {
        emit evt_SocketError(pSock,socketError);
        qDebug()<<(pSock->errorString());
        pSock->abort();
    }
}

void zp_netTransThread::SendDataToClient(QObject * objClient,const QByteArray &  dtarray)
{
    m_mutex_protect.lock();
    if (m_clientList.find(objClient)==m_clientList.end())
    {
        m_mutex_protect.unlock();
        return;
    }
    m_mutex_protect.unlock();
    QTcpSocket * pSock = qobject_cast<QTcpSocket*>(objClient);
    if (pSock&&dtarray.size())
    {
        QList<QByteArray> & list_sock_data = m_buffer_sending[pSock];
        QList<qint64> & list_offset = m_buffer_sending_offset[pSock];
        if (list_sock_data.empty()==true)
        {
            qint64 bytesWritten = pSock->write(dtarray.constData(),qMin(dtarray.size(),m_nPayLoad));
            if (bytesWritten < dtarray.size())
            {
                list_sock_data.push_back(dtarray);
                list_offset.push_back(bytesWritten);
            }
        }
        else
        {
            list_sock_data.push_back(dtarray);
            list_offset.push_back(0);
        }
    }
}
void zp_netTransThread::BroadcastData(QObject * objClient,const QByteArray &  dtarray)
{
    m_mutex_protect.lock();
    QList<QObject *> clientList = m_clientList.keys();
    m_mutex_protect.unlock();
    foreach(QObject * obj,clientList)
    {
        QTcpSocket * pSock = qobject_cast<QTcpSocket*>(obj);
        if (pSock&&dtarray.size()&&pSock!=objClient)
        {
            QList<QByteArray> & list_sock_data = m_buffer_sending[pSock];
            QList<qint64> & list_offset = m_buffer_sending_offset[pSock];
            if (list_sock_data.empty()==true)
            {
                qint64 bytesWritten = pSock->write(dtarray.constData(),qMin(dtarray.size(),m_nPayLoad));
                if (bytesWritten < dtarray.size())
                {
                    list_sock_data.push_back(dtarray);
                    list_offset.push_back(bytesWritten);
                }
                else
                {
                    list_sock_data.push_back(dtarray);
                    list_offset.push_back(0);
                }
            }
        }
    }
}
void zp_netTransThread::KickAllClients(zp_netTransThread * ptr)
{
    if (ptr!=this)
        return;
    m_mutex_protect.lock();
    QList<QObject *> clientList = m_clientList.keys();
    m_mutex_protect.unlock();
    foreach(QObject * obj,clientList)
    {
        QTcpSocket * pSock = qobject_cast<QTcpSocket*>(obj);
        if (pSock)
        {
            pSock->abort();
        }
    }

}

void zp_netTransThread::KickClient(QObject * objClient)
{
    m_mutex_protect.lock();
    if (m_clientList.find(objClient)==m_clientList.end())
    {
        m_mutex_protect.unlock();
        return;
    }
    m_mutex_protect.unlock();
    QTcpSocket * pSock = qobject_cast<QTcpSocket*>(objClient);
    if (pSock)
    {
        pSock->abort();
    }
}

bool zp_netTransThread::CanExit()
{
    if (m_bActivated==true)
        return false;
    if (CurrentClients())
        return false;
    return true;
}
}
