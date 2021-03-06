// Server.cpp: implementation of the CServer class.
// 监听类
// 1.监听客户端连接请求
// 2.把接收到的客户端连接给完成端口处理
// 3.创建完成端口I/O事件处理线程
// 4.处理只连接不发送GET请求的僵尸连接===拒绝服务攻击
// 5.socket error or invalid socket 发生时，尽量恢复socket，而不是退出处理；
// author：张世光 2012.7
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "WinError.h"
#include "ClientManager.h"
#include "Loader.h"
#include "Server.h"

#include <IOSTREAM>
using namespace std;

extern Clog	g_log;
extern CFG	g_cfg;

//////////////////////////////////////////////////////////////////////

static void CServer::run( LPVOID lpParam );
void CloseAllIOProcessThread(CServer *Server);
void ServerThread( LPVOID lpParam );
bool CreatProcessThread(CServer* Server);
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
CServer::CServer():port(8080),clients_max_count(100000),works_max_count(1)
{
	WSADATA   wsaData;   
	DWORD   Ret;   
	
	if ((Ret = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0)   
	{   
		LogExt(CRITICAL_LEVEL,"[error][CServer]WSAStartup error with codes: %d\n", WSAGetLastError());
		setallthreadexitflag();
	} 
}

void CServer::setPort(int p )
{
	port = p;
}

void CServer::setMaxClient(int max_clients)
{
	clients_max_count = max_clients;
}

void CServer::setMaxWorker(int max_work)
{
	works_max_count	= max_work;
}

CServer::~CServer()
{
	stop();
	shutdown(_fd ,SD_BOTH );
	closesocket(_fd);
	
	CloseHandle(this->hiocp);
	
	WSACleanup();
}

void CServer::init(CFG * cfg,CClientManager* pcmanager,CLoader * pL)
{
	pClientManager = pcmanager;
	pLoader        = pL;
	setPort( cfg->local_port);
	setMaxClient(cfg->max_clients_num);
}

bool CServer::Listen()
{
	_set_se_translator(SeTranslator);
	try
	{				
#ifdef LINUX
		struct sockaddr_in sin;
		
		memset( &sin, 0, sizeof( sin ) );
		
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_ANY);    
		sin.sin_port = htons(port);
		_fd = socket( AF_INET, SOCK_STREAM, 0 );
		
		int optval = 1;   
		setsockopt( _fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof( sockaddr_in ) );
		
		if( bind( _fd, (struct sockaddr *)&sin, sizeof( sin ) ) == -1 ) 
		{
			LogExt(CRITICAL_LEVEL,,"[error][CServer]Listen() bind error with codes: %d\n", WSAGetLastError());
			return false;
		}
#else
		
		
		
		if ((_fd =WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET)   
		{ 
#ifdef _DEBUG
			printexception(DEBUGARGS);
#endif
			LogExt(CRITICAL_LEVEL,"[error][CServer]WSASocket() error with codes: %d\n", WSAGetLastError());  
			//setallthreadexitflag();socket err 尽量恢复而不要退出
			return false;
		}
		// set option
		//int optval = 1;   
		//setsockopt( _fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof( sockaddr_in ) );
		
		int nRecvBuf = MAXBUF ;
		setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&nRecvBuf, sizeof(int));
		
		int nSendBuf = MAXBUF ;
		setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, (const char*)&nSendBuf, sizeof(int));

		int err;
		struct  sockaddr_in mySockaddr;
		mySockaddr.sin_family      = AF_INET;
		mySockaddr.sin_port        = htons(port);
		mySockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		memset( &(mySockaddr.sin_zero), 0, 8);
		
		err = bind( _fd,(struct sockaddr *)&mySockaddr,sizeof(struct sockaddr) );
		if(err == SOCKET_ERROR)
		{
#ifdef _DEBUG
			printexception(DEBUGARGS);
#endif
			LogExt(CRITICAL_LEVEL,"[error][CServer]Listen() bind error with codes: %d\n", WSAGetLastError());
			closesocket(_fd);
			//setallthreadexitflag();socket err 尽量恢复而不要退出
			return false;
		}
#endif
		// listen, queue length 8  
		if( listen( _fd, 8 ) == -1 ) 
		{			
#ifdef _DEBUG
			printexception(DEBUGARGS);
#endif
			LogExt(CRITICAL_LEVEL,"[error][CServer]Listen() listen error\n");
			closesocket(_fd);
			//setallthreadexitflag();socket err 尽量恢复而不要退出
			return false;
		}
		#ifdef _DEBUG
		LogExt(DEBUG_ONLY_LEVEL,"[log][CServer]Listen %d ok\n",port);
        #endif
				
	}
	catch(CSeException *e)
	{
#ifdef _DEBUG
		printexception(DEBUGARGS);
#endif
		g_log.log("[exception][CServer]Listen!\n",CRITICAL_LEVEL);
		exceptiontolog(e);
		setallthreadexitflag();
		
	}
	
	return true;
	
}

void CServer::start()
{
	if (INVALID_HANDLE_VALUE == mEvent)
	{
		creatwork(CServer::run);
	}
	writelogimmediatly("[log][CServer] start.\n");
}

void CServer::stop()
{
	if (INVALID_HANDLE_VALUE!=this->mEvent)
	{
		SetEvent(this->mEvent);
		WaitForSingleObject( this->hthread ,3000);
		CloseHandle(this->mEvent);
		this->mEvent=INVALID_HANDLE_VALUE;
		
		writelogimmediatly("[log][CServer] stop.\n");
	}
}

void CServer::run( LPVOID lpParam )
{
	CServer *Server =static_cast<CServer*>(lpParam) ;


	_set_se_translator(SeTranslator);
	try
	{		
 		
		// 创建完成端口对象 
		HANDLE hIocp = ::CreateIoCompletionPort( INVALID_HANDLE_VALUE, 0, 0, 0 );
		if (NULL== hIocp)
		{
			throw exception( "[Ohmygod]");
		}
		Server->hiocp =hIocp;
		
		//
		if(!CreatProcessThread(Server))
		{
			throw exception( "[Ohmygod2]");			
		}
		//
		DWORD  result =0;
		
	
		char debuginfo[512] = {0};
		struct sockaddr_in adrFrom;
		int iAddrLen = 0;
		SOCKET sckClient;
		CCLient *client = NULL;
		WSABUF buf; 
		DWORD dwRecv = 0; 
		DWORD dwFlags = 0;

RELISTEN_F:
		if(!Server->Listen())
		{
			throw exception( "[Ohmygod3]");	
		}

		while(!allExitFlag ) 
		{	
			
			if( Server->pClientManager->CanGetNewClient() ) 
			{
				iAddrLen = sizeof( adrFrom );				
				try
				{
					sckClient = accept( Server->_fd, (struct sockaddr *)&adrFrom, (int *)&iAddrLen ); 
				}
				catch(...)
				{
#ifdef DEBUG
					int err = WSAGetLastError();

					memset(debuginfo,0,sizeof(debuginfo));
					sprintf(debuginfo,"[exception][accept][%d]\n",err);
					DEBUGOUT(debuginfo);
#endif	
					//
					shutdown(Server->_fd ,SD_BOTH );
					closesocket(Server->_fd);

					goto RELISTEN_F;
				}

				if( INVALID_SOCKET == sckClient ) 
				{

					g_log.log("[error][CServer]accept error\n",ERROR_LEVEL);					
					goto RELISTEN_F;
				}
				else
				{				
					
					// linger option
					LINGER linger = {1, 2}; //windows 单位秒，BSD单位0.01秒
					setsockopt(sckClient, SOL_SOCKET, SO_LINGER, (char *)&linger, sizeof(linger));
					int nRcvTime = 100;
					setsockopt(sckClient, SOL_SOCKET, SO_RCVTIMEO, (const char*)&nRcvTime, sizeof(int));
					DWORD dwError = 0L;
					DWORD dwBytes;
					tcp_keepalive_in sKA_Settings = {0}, sReturned = {0} ;
					sKA_Settings.onoff = 1 ;
					sKA_Settings.keepalivetime = 5500;//Keep alive in 5.5 sec //心跳//只影响单个socket （SIO_KEEPALIVE 影响所有socket）
					sKA_Settings.keepaliveinterval = 3000;//Resend if No-Reply 

					if(0!=WSAIoctl(sckClient, SIO_KEEPALIVE_VALS, &sKA_Settings, sizeof(sKA_Settings), &sReturned,
							  sizeof(sReturned), &dwBytes, NULL, NULL))
					{
						dwError = WSAGetLastError() ;
					}

					client = Server->pClientManager->GetClient();
					if (!client)
					{
						
						client = new CCLient( sckClient, adrFrom, Server->pClientManager);
						if (client)
						{
							Server->pClientManager->PushNewIn( client );
						}
						else
						{
						#ifdef _DEBUG
							printexception(DEBUGARGS);
						#endif
							setallthreadexitflag();
							LogExt(CRITICAL_LEVEL,"[exception][CServer]clients new\n");
						}								
					}
					else
					{							
						
						client->ReInit(sckClient, adrFrom );						
						
					}
					
					if (client)
					{
						
						try
						{						
						//把sock绑定到完成端口//client* 用作key
							if(NULL==::CreateIoCompletionPort( ( HANDLE)sckClient, hIocp, (DWORD)client, 0 ))
							{
								setallthreadexitflag();
								break;
							}
						}
						catch(...)
						{
							Server->pClientManager->KillClient(client);	
							DEBUGOUT("[exception][CServer]CreateIoCompletionPort\n");
							sckClient = INVALID_SOCKET;
							continue;
						}
						client->IoData.nOperationType = OP_READ; 
						
						client->resetbuf(&buf);
						
						try
						{
							dwRecv = 0; 
							dwFlags = 0;
							//This function receives data from a connected socket
							if (::WSARecv( sckClient, &buf, 1, &dwRecv, &dwFlags, &(client->IoData.ol), NULL )== -1)
							{
								int err = WSAGetLastError();
								if ( err != ERROR_IO_PENDING)
								{	
								#ifdef _DEBUG
									LogExt(DEBUG_ONLY_LEVEL,"[Error][CServer]WSARecv Error[soc%d]!\n",sckClient);
								#endif
									Server->pClientManager->KillClient(client);								
								}
								else
								{
								}
								
							}
						}
						catch(...)
						{
							DEBUGOUT("[exception][CServer]WSARecv\n");
							continue;
						}
					}
					
				}
				
			}
			else
			{	
				LogExt(LOG_LOG_LEVEL,"[log][CServer]达到客户端接入数[%d]的最大限制[%d]\n",Server->pClientManager->GetActiveSize(),Server->clients_max_count);
			}
		}
		CloseAllIOProcessThread(Server );
	}
	catch(CSeException *e)
	{
#ifdef _DEBUG
		printexception(DEBUGARGS);
#endif
		exceptiontolog(e);
		setallthreadexitflag();
		g_log.log("[exception][CServer]run !\n",CRITICAL_LEVEL);
	}
	
	if((allExitFlag))
	{
		Server->pLoader->ckSetEvent();		
	}


#ifdef _DEBUG
	DEBUGOUT("[log][CServer]run exit\n");
 	LogExt(WARNNING_LEVEL,"[log][CServer]run exit ID[0x%0x]\n",GetCurrentThreadId());
#endif

	
}


void CloseAllIOProcessThread(CServer *Server)
{

	_set_se_translator(SeTranslator);
	try
	{
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);
		for (int i = 0; i <sysInfo.dwNumberOfProcessors ; ++i) 
		{
			PostQueuedCompletionStatus(Server->hiocp, 0, 0, (OVERLAPPED*) (/*(__int64)*/ -1) );
		}
	}
	catch(CSeException *e)
	{
#ifdef _DEBUG
		printexception(DEBUGARGS);
#endif
		exceptiontolog(e);
		//setallthreadexitflag();
		g_log.log("[exception][CServer]CloseAllIOProcessThread!\n",CRITICAL_LEVEL);
	}
	
}


/****************************************************************** 
* 函数介绍：处理完成端口对象事件的线程 ；解析处理接收到的数据
* 输入参数：//  
*******************************************************************/ 
void ServerThread( LPVOID lpParam ) 
{ 
	CServer* Server=(CServer*)lpParam;	 
	HANDLE hIocp = Server->hiocp; 
    if( hIocp == INVALID_HANDLE_VALUE ) 
    { 
        return ; 
    }
#ifdef _DEBUG
	char outstr[512];
#endif

	try
	{
			
		DWORD dwTrans = 0; 
		DWORD dwErr =0;
		CCLient* lpCompletionKey=NULL; 
		PPER_IO_DATA     pPerIo=NULL; 
		
		while( !allExitFlag ) 
		{ 
			lpCompletionKey = NULL; 
			pPerIo          = NULL;
			// 在关联到此完成端口的所有套接字上等待I/O完成 
			BOOL bRet = ::GetQueuedCompletionStatus( Server->hiocp, &dwTrans, (LPDWORD)&lpCompletionKey, (LPOVERLAPPED*)&pPerIo, WSA_INFINITE ); 
			dwErr = GetLastError();
			if (allExitFlag )
			{
				break;
			}
			
			if( !bRet )     
			{ 			
				if (pPerIo && lpCompletionKey)
				{
					
					lpCompletionKey->m_cman->KillClient(lpCompletionKey);
					
				}
				else if ( (void*)-1 == pPerIo)
				{
					break;
				}

				continue;
			}
			else
			{
				// 套接字被对方关闭 
				if( 0 == dwTrans  && ( OP_READ == pPerIo->nOperationType || OP_WRITE == pPerIo->nOperationType ) ) 
				{ 
					if (lpCompletionKey)
					{	
#ifdef _DEBUG
// 						memset(outstr,0,sizeof(outstr));
// 						sprintf(outstr,"[log][CServer]终端[%s]断开连接\n",lpCompletionKey->imei);
// 						DEBUGOUT(outstr);
						LogExt(DEBUG_ONLY_LEVEL,"[log][CServer]终端[%s]断开连接\n",lpCompletionKey->imei);
#endif
						
						lpCompletionKey->m_cman->KillClient(lpCompletionKey);
					
					}
					
					continue; 
				}
				
				switch ( pPerIo->nOperationType ) 
				{ 
				case OP_READ:       
					{ 		
						
						lpCompletionKey->parse((U8*)(lpCompletionKey->IoData.buf) ,dwTrans);
						
						// 继续投递接收操作 ，可能已经关闭
						WSABUF buf; 
						
						lpCompletionKey->resetbuf(&buf);
						lpCompletionKey->IoData.nOperationType = OP_READ; 
						
						DWORD dwRecv = 0; 
						DWORD dwFlags = 0; 
						if (INVALID_SOCKET!= lpCompletionKey->_fd)
						{						
							if (::WSARecv( lpCompletionKey->_fd, &buf, 1, &dwRecv, &dwFlags, &(lpCompletionKey->IoData.ol), NULL )== -1)
							{
								int err =WSAGetLastError();
// 	#if 0 //def _DEBUG
// 								memset(outstr,0,sizeof(outstr));
// 								sprintf(outstr,"[log][CServer][WSARecv][%d][fd%d][dwRecv%d][dwFlags%d]\n",err,lpCompletionKey->_fd,dwRecv,dwFlags);
// 								DEBUGOUT(outstr);
// 	#endif					
								if ( err != ERROR_IO_PENDING)
								{	
									LogExt(DEBUG_ONLY_LEVEL,"[Error][CServer][ServerThread]WSARecv Error[soc%d][%s]!\n",lpCompletionKey->_fd,lpCompletionKey->imei);
								
								}
								else
								{//997 ERROR_IO_PENDING								
								}
							}
						}
					} 
					
					break; 
				case OP_WRITE: 
				case OP_ACCEPT: 
					break;					
				}
			}
		}
	
	}
	catch(...)
	{
		g_log.log("[exception]ServerThread exit\n",ERROR_LEVEL);
	}

#ifdef _DEBUG
	memset(outstr,0,sizeof(outstr));
	sprintf(outstr,"[log][CServer]ServerThread id[%x] exit\n",GetCurrentThreadId());
	writelogimmediatly(outstr);
#endif
    return ; 
}

//创建工作线程
bool CreatProcessThread(CServer* Server)
{
	_set_se_translator(SeTranslator);
	try
	{
		HANDLE h =NULL;
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);

		for(int i = 0; i <sysInfo.dwNumberOfProcessors; ++i) 
		{
			if( NULL != (h =(HANDLE) ::_beginthread(ServerThread, 0, (LPVOID)Server)))
			{
				Server->threadset.push_back( h );
			}
			else
			{			
				g_log.log("[error][CreatProcessThread]create works fail\n",ERROR_LEVEL);		 		
				break;
			}		 
		}
		
		
		if( NULL == h) 
		{		
			allExitFlag =true;
			CloseAllIOProcessThread(Server);
			
			return false;
		}
		else
		{
			return true;
		}
	}
	catch(CSeException *e)
	{
#ifdef _DEBUG
		printexception(DEBUGARGS);
#endif
		exceptiontolog(e);
		setallthreadexitflag();
		g_log.log("[exception][CreatProcessThread]!\n",CRITICAL_LEVEL);
	}
	return false;
}