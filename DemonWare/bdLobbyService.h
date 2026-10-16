#pragma once
#include <universal/ptr32.h>

#include <DemonWare/bdCore/bdReference/bdReferencable.h>
#include <DemonWare/bdCore/bdSocket/bdAddr.h>
#include "bdCore/bdTiming/bdStopwatch.h"
#include <DemonWare/bdCore/bdContainers/bdQueue.h>
#include "bdCore/bdSocket/bdStreamSocket.h"
#include "bdConnection.h"
#include <DemonWare/bdCore/bdContainers/bdByteBuffer.h>
#include "bdCore/bdCrypto/bdCypher3Des.h"

struct bdTaskByteBuffer : bdByteBuffer // sizeof=0x2C
{
    Ptr32<unsigned __int8> m_taskData;
    unsigned int m_taskDataSize;
    unsigned int m_validHeaderSize;
    unsigned int m_paddingSize;
};

struct bdPendingBufferTransfer : bdReferencable // sizeof=0x14
{                                       // XREF: ?sendTask@bdLobbyConnection@@UAE_NV?$bdReference@VbdTaskByteBuffer@@@@I_N@Z/r
                                        // ?sendTask@bdLobbyConnection@@UAE_NV?$bdReference@VbdTaskByteBuffer@@@@I_N@Z/r ...
    bdReference<bdByteBuffer> m_buffer;
    Ptr32<unsigned __int8> m_txPtr;
    unsigned int m_txAvail;
};

struct bdLobbyConnection : bdReferencable // sizeof=0x408
{
    enum RecvState : __int32
    {                                       // XREF: bdLobbyConnection/r
        BD_READ_INIT     = 0x0,
        BD_READ_SIZE     = 0x1,
        BD_READ_ENCRYPT  = 0x2,
        BD_READ_MESSAGE  = 0x3,
        BD_READ_COMPLETE = 0x4,
    };

    typedef bdConnection::Status Status;

    bdReference<bdCommonAddr> m_addr;
    unsigned int m_maxSendMessageSize;
    unsigned int m_maxRecvMessageSize;
    bdLobbyConnection::RecvState m_recvState;
    unsigned __int8 m_msgSizeBuffer[4];
    unsigned int m_recvCount;
    unsigned __int8 m_recvEncryptType;
    // padding byte
    // padding byte
    // padding byte
    unsigned int m_messageSize;
    bdReference<bdTaskByteBuffer> m_recvMessage;
    bdReference<bdPendingBufferTransfer> m_recvTransfer;
    bdQueue<bdPendingBufferTransfer> m_outgoingBuffers;
    bdStreamSocket m_socket;
    bdLobbyConnection::Status m_status;
    Ptr32<struct bdLobbyConnectionListener> m_connectionListener;
    bdCypher3Des m_cypher;
    unsigned __int8 m_sessionKey[24];
    unsigned int m_messageCount;
    bdStopwatch m_keepAliveTimer;
    bdStopwatch m_lastReceivedTimer;
    bdStopwatch m_asyncConnectTimer;
};

struct bdLobbyConnectionListener // sizeof=0x4
{                                       // XREF: bdAuthService/r
                                        // bdLobbyService/r
    //bdLobbyConnectionListener_vtbl *__vftable;

    virtual ~bdLobbyConnectionListener();
    virtual void onConnect(bdReference<bdLobbyConnection>);
    virtual void onConnectFailed(bdReference<bdLobbyConnection>);
    virtual void onDisconnect(bdReference<bdLobbyConnection>);
};

struct bdAuthInfo // sizeof=0xA0
{                                       // XREF: bdAuthService/r
                                        // bdLobbyService/r
    unsigned int m_titleID;
    unsigned int m_IVSeed;
    char m_data[128];
    unsigned __int8 m_sessionKey[24];
};

struct bdLobbyService : bdLobbyConnectionListener // sizeof=0x118
{
    Ptr32<struct bdRemoteTaskManager> m_taskManager;
    Ptr32<struct bdProfiles> m_profiles;
    Ptr32<struct bdMessaging> m_messaging;
    Ptr32<struct bdMatchMaking> m_matchMaking;
    Ptr32<struct bdStats> m_statsManager;
    Ptr32<struct bdFriends> m_friendsManager;
    Ptr32<struct bdTeams> m_teamsManager;
    Ptr32<struct bdStorage> m_storageManager;
    Ptr32<struct bdContentUnlock> m_contentUnlockManager;
    Ptr32<struct bdTitleUtilities> m_titleUtilitiesManager;
    Ptr32<struct bdKeyArchive> m_keyArchive;
    Ptr32<struct bdCounter> m_counter;
    Ptr32<struct bdGroup> m_group;
    Ptr32<struct bdContentStreaming> m_contentStreamingManager;
    Ptr32<struct bdPooledStorage> m_pooledStorageManager;
    Ptr32<struct bdTags> m_tags;
    Ptr32<struct bdVoteRank> m_voteRankManager;
    Ptr32<struct bdTwitter> m_twitter;
    Ptr32<struct bdFacebook> m_facebook;
    Ptr32<struct bdLinkCode> m_linkCode;
    Ptr32<struct bdAntiCheat> m_antiCheat;
    Ptr32<struct bdDML> m_DML;
    Ptr32<struct bdCRUX> m_crux;
    Ptr32<struct bdActivityTracker> m_activityTracker;
    unsigned int m_titleID;
    Ptr32<struct bdLobbyEventHandler> m_eventHandler;
    bdReference<bdLobbyConnection> m_lobbyConnection;
    bdAuthInfo m_authInfo;
    bool m_encryptedConnection;
    // padding byte
    // padding byte
    // padding byte
    unsigned int m_errorCode;
};