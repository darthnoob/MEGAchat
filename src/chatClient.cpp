#include "contactList.h"
#include "ITypes.h" //for IPtr
#ifdef _WIN32
    #include <winsock2.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mstrophepp.h>
#include "rtcModule/IRtcModule.h"
#include "dummyCrypto.h" //for makeRandomString
#include "strophe.disco.h"
#include "base/services.h"
#include "sdkApi.h"
#include "megaCryptoFunctions.h"
#include <serverListProvider.h>
#include <memory>
#include "chatClient.h"
#include <chatd.h>
#include <db.h>
#include <buffer.h>
#include <chatdDb.h>
#include <megaapi_impl.h>
#include <autoHandle.h>
#include <asyncTools.h>
#include <codecvt> //for nonWhitespaceStr()
#include <locale>

#define _QUICK_LOGIN_NO_RTC
using namespace promise;

namespace karere
{
void Client::sendPong(const std::string& peerJid, const std::string& messageId)
{
    strophe::Stanza pong(*conn);
    pong.setAttr("type", "result")
        .setAttr("to", peerJid)
        .setAttr("from", conn->fullJid())
        .setAttr("id", messageId);

    conn->send(pong);
}

Client::Client(IGui& aGui, Presence pres)
 :mAppDir(getAppDir()), db(openDb()), conn(new strophe::Connection(services_strophe_get_ctx())),
  api(new MyMegaApi("karere-native", mAppDir.c_str())), userAttrCache(*this), gui(aGui),
  mOwnPresence(pres),
  mXmppContactList(*this),
  mXmppServerProvider(new XmppServerProvider("https://gelb530n001.karere.mega.nz", "xmpp", KARERE_FALLBACK_XMPP_SERVERS))
{
    try
    {
        gui.onOwnPresence(Presence::kOffline);
    } catch(...){}

    SqliteStmt stmt(db, "select value from vars where name='sid'");
    if (stmt.step())
        mSid = stmt.stringCol(0);

    if (mSid.empty())
    {
        KR_LOG_DEBUG("No session id found in local db");
        return;
    }

    SqliteStmt stmt2(db, "select value from vars where name='my_handle'");
    if (stmt2.step())
        mMyHandle = stmt2.uint64Col(0);

    if (mMyHandle.val == 0 || mMyHandle.val == mega::UNDEF)
    {
        mSid.clear();
        KR_LOG_WARNING("Local db inconsisency: Session id found, but out userhandle is invalid. Invalidating session");
        return;
    }
    chatd.reset(new chatd::Client(mMyHandle));
    contactList.reset(new ContactList(*this));
    chats.reset(new ChatRoomList(*this));
}

KARERE_EXPORT const std::string& createAppDir(const char* dirname, const char *envVarName)
{
    static std::string path;
    if (!path.empty())
        return path;
    const char* dir = getenv(envVarName);
    if (dir)
    {
        path = dir;
    }
    else
    {
        const char* homedir = getenv(
            #ifndef _WIN32
                    "HOME"
            #else
                    "HOMEPATH"
            #endif
        );
        if (!homedir)
            throw std::runtime_error("Cant get HOME env variable");
        path = homedir;
        path.append("/").append(dirname);
    }
    struct stat info;
    auto ret = stat(path.c_str(), &info);
    if (ret == 0)
    {
        if ((info.st_mode & S_IFDIR) == 0)
            throw std::runtime_error("Application directory path is taken by a file");
    }
    else
    {
        ret = mkdir(path.c_str(), 0700);
        if (ret)
        {
            char buf[512];
#ifdef _WIN32
            strerror_s(buf, 511, ret);
#else
            strerror_r(ret, buf, 511);
#endif
            buf[511] = 0; //just in case
            throw std::runtime_error(std::string("Error creating application directory: ")+buf);
        }
    }
    return path;
}
sqlite3* Client::openDb()
{
    sqlite3* database = nullptr;
    std::string path = mAppDir+"/karere.db";
    struct stat info;
    bool existed = (stat(path.c_str(), &info) == 0);
    int ret = sqlite3_open(path.c_str(), &database);
    if (ret != SQLITE_OK || !database)
        throw std::runtime_error("Can't access application database at "+path);
    if (!existed)
    {
        KR_LOG_WARNING("Initializing local database, did not exist");
        MyAutoHandle<char*, void(*)(void*), sqlite3_free, (char*)nullptr> errmsg;
        ret = sqlite3_exec(database, gKarereDbSchema, nullptr, nullptr, errmsg.handlePtr());
        if (ret)
        {
            sqlite3_close(database);
            if (errmsg)
                throw std::runtime_error("Error initializing database: "+std::string(errmsg));
            else
                throw std::runtime_error("Error "+std::to_string(ret)+" initializing database");
        }
    }
    return database;
}


Client::~Client()
{
    //when the strophe::Connection is destroyed, its handlers are automatically destroyed
}

#define TOKENPASTE2(a,b) a##b
#define TOKENPASTE(a,b) TOKENPASTE2(a,b)

#define SHARED_STATE(varname, membtype)             \
    struct TOKENPASTE(SharedState,__LINE__){membtype value;};  \
    std::shared_ptr<TOKENPASTE(SharedState, __LINE__)> varname(new TOKENPASTE(SharedState,__LINE__))

promise::Promise<void> Client::init()
{
    ApiPromise pmsMegaLogin((promise::Empty()));
    if (mSid.empty()) //mMyHandle is also invalid
    {
        mLoginDlg.reset(gui.createLoginDialog());
        pmsMegaLogin = asyncLoop(
        [this](Loop& loop){
            return mLoginDlg->requestCredentials()
            .then([this](const std::pair<std::string, std::string>& cred)
            {
                return api->call(&mega::MegaApi::login, cred.first.c_str(), cred.second.c_str());
            })
            .then([&loop](ReqResult res)
            {
                loop.breakLoop();
                return res;
            })
            .fail([this](const promise::Error& err) -> ApiPromise
            {
                if (err.code() != mega::API_ENOENT && err.code() != mega::API_EARGS)
                    return err;

                mLoginDlg->setState(IGui::ILoginDialog::kBadCredentials);
                return ApiPromise(ReqResult());
            });
        }, [](int) { return true; });
    }
    else
    {
        gui.show();
        pmsMegaLogin = api->call(&mega::MegaApi::fastLogin, mSid.c_str());
    }
    pmsMegaLogin.then([this](ReqResult result) mutable
    {
        mIsLoggedIn = true;
        KR_LOG_DEBUG("Login to Mega API successful");
        if (mLoginDlg)
            mLoginDlg->setState(IGui::ILoginDialog::kLoggingIn);
        SdkString uh = api->getMyUserHandle();
        if (!uh.c_str() || !uh.c_str()[0])
            throw std::runtime_error("Could not get our own user handle from API");
        chatd::Id handle(uh.c_str());
        if (mLoginDlg)
        {
            assert(mMyHandle.val == mega::UNDEF || mMyHandle.val == 0);
            KR_LOG_DEBUG("Obtained our own handle (%s), recording to database", handle.toString().c_str());
            mMyHandle = handle;
            sqliteQuery(db, "insert or replace into vars(name,value) values('my_handle', ?)", mMyHandle);
            chatd.reset(new chatd::Client(mMyHandle));
            contactList.reset(new ContactList(*this));
            chats.reset(new ChatRoomList(*this));
        }
        else
        {
            assert(mMyHandle);
            if (handle != mMyHandle)
                throw std::runtime_error("Local DB inconsistency: Own userhandle returned from API differs from the one in local db");
        }
//        if (onChatdReady)
//            onChatdReady();
    });
    auto pmsMegaGud = pmsMegaLogin.then([this](ReqResult result)
    {
        return api->call(&mega::MegaApi::getUserData);
    })
    .then([this](ReqResult result)
    {
        api->userData = result;
    });
    auto pmsMegaFetch = pmsMegaLogin.then([this](ReqResult result)
    {
        if (mLoginDlg)
        {
            const char* sid = api->dumpSession();
            assert(sid);
            mSid = sid;
            sqliteQuery(db, "insert or replace into vars(name,value) values('sid',?)", sid);
            mLoginDlg->setState(IGui::ILoginDialog::kFetchingNodes);
        }
        return api->call(&mega::MegaApi::fetchNodes);
    })
    .then([this](ReqResult result)
    {
        mLoginDlg.reset();
        gui.show();
        userAttrCache.onLogin();
        api->addGlobalListener(this);

        userAttrCache.getAttr(mMyHandle, mega::MegaApi::USER_ATTR_LASTNAME, this,
        [](Buffer* buf, void* userp)
        {
            if (buf)
                static_cast<Client*>(userp)->mMyName = buf->buf()+1;
        });
        contactList->syncWithApi(*api->getContacts());
        return api->call(&mega::MegaApi::fetchChats);
    })
    .then([this](ReqResult result)
    {
        auto chatRooms = result->getMegaTextChatList();
        if (chatRooms)
        {
            chats->syncRoomsWithApi(*chatRooms);
        }
    });

    auto pmsGelb = mXmppServerProvider->getServer()
    .then([this](const std::shared_ptr<HostPortServerInfo>& server) mutable
    {
        return server;
    });
    auto pmsXmpp = promise::when(pmsMegaLogin, pmsGelb)
    .then([this, pmsGelb, pmsMegaGud]()
    {
        return connectXmpp(pmsGelb.value());
    });
    return promise::when(pmsMegaGud, pmsMegaFetch, pmsXmpp)
    .fail([this](const promise::Error& err)
    {
        if (mLoginDlg)
            mLoginDlg.reset();
        return err;
    });
}

promise::Promise<void> Client::connectXmpp(const std::shared_ptr<HostPortServerInfo>& server)
{
//we assume gui.onOwnPresence(Presence::kOffline) has been called at application start
    gui.onOwnPresence(mOwnPresence.val() | Presence::kInProgress);
    assert(server);
    SdkString xmppPass = api->dumpXMPPSession();
    if (!xmppPass)
        return promise::Error("SDK returned NULL session id");
    if (xmppPass.size() < 16)
        throw std::runtime_error("Mega session id is shorter than 16 bytes");
    ((char&)xmppPass.c_str()[16]) = 0;

    //xmpp_conn_set_keepalive(*conn, 10, 4);
    // setup authentication information
    std::string jid = useridToJid(mMyHandle);
    jid.append("/kn_").append(rtcModule::makeRandomString(10));
    xmpp_conn_set_jid(*conn, jid.c_str());
    xmpp_conn_set_pass(*conn, xmppPass.c_str());
    KR_LOG_DEBUG("xmpp user = '%s', pass = '%s'", jid.c_str(), xmppPass.c_str());
    setupXmppHandlers();
    setupXmppReconnectHandler();
    Promise<void> pms = static_cast<Promise<void>&>(mReconnectController->start());
    return pms.then([this]()
    {
        KR_LOG_INFO("XMPP login successful");
// create and register disco strophe plugin
        conn->registerPlugin("disco", new disco::DiscoPlugin(*conn, "Karere Native"));

// Create and register the rtcmodule plugin
// the MegaCryptoFuncs object needs api->userData (to initialize the private key etc)
// To use DummyCrypto: new rtcModule::DummyCrypto(jid.c_str());
        rtc = rtcModule::create(*conn, this, new rtcModule::MegaCryptoFuncs(*this), KARERE_DEFAULT_TURN_SERVERS);
        conn->registerPlugin("rtcmodule", rtc);

/*
// create and register text chat plugin
        mTextModule = new TextModule(*this);
        conn->registerPlugin("textchat", mTextModule);
*/
        KR_LOG_DEBUG("webrtc plugin initialized");
        return mXmppContactList.ready();
    })
    .then([this]()
    {
        KR_LOG_DEBUG("XMPP contactlist initialized");
        //startKeepalivePings();
    });
}

void Client::setupXmppHandlers()
{
    conn->addHandler([this](strophe::Stanza stanza, void*, bool &keep) mutable
    {
            sendPong(stanza.attr("from"), stanza.attr("id"));
    }, "urn::xmpp::ping", "iq", nullptr, nullptr);
}

void Client::setupXmppReconnectHandler()
{
    mReconnectController.reset(mega::createRetryController(
        [this](int no) -> promise::Promise<void>
    {
        if (no < 2)
        {
            auto& host = mXmppServerProvider->lastServer()->host;
            KR_LOG_INFO("Connecting to xmpp server %s...", host.c_str());
            gui.onOwnPresence(mOwnPresence.val()|Presence::kInProgress);
            return conn->connect(host.c_str(), 0);
        }
        else
        {
            return mXmppServerProvider->getServer()
            .then([this](std::shared_ptr<HostPortServerInfo> server)
            {
                KR_LOG_WARNING("Connecting to new xmpp server: %s...", server->host.c_str());
                gui.onOwnPresence(mOwnPresence | Presence::kInProgress);
                return conn->connect(server->host.c_str(), 0);
            });
        }
    },
    [this]()
    {
        xmpp_disconnect(*conn, -1);
        gui.onOwnPresence(Presence::kOffline);
    },
    KARERE_LOGIN_TIMEOUT, 60000, KARERE_RECONNECT_DELAY_MAX, KARERE_RECONNECT_DELAY_INITIAL));

    mReconnectConnStateHandler = conn->addConnStateHandler(
       [this](xmpp_conn_event_t event, int error,
        xmpp_stream_error_t* stream_error, bool& keepHandler) mutable
    {
        if (event == XMPP_CONN_CONNECT)
        {
            mega::marshallCall([this]() //notify async, safer
            {
                mXmppContactList.fetch(); //waits for roster
                setPresence(mOwnPresence, true); //initiates roster fetch
                mXmppContactList.ready()
                .then([this]()
                {
                    gui.onOwnPresence(mOwnPresence);
                });
            });
            return;
        }
        //we have a disconnect
        xmppContactList().notifyOffline();
        gui.onOwnPresence(Presence::kOffline);

        if (mOwnPresence.status() == Presence::kOffline) //user wants to be offline
            return;
        if (isTerminating) //no need to handle
            return;
        assert(xmpp_conn_get_state(*conn) == XMPP_STATE_DISCONNECTED);
        if (mReconnectController->state() & mega::rh::kStateBitRunning)
            return;

        if (mReconnectController->state() == mega::rh::kStateFinished) //we had previous retry session, reset the retry controller
            mReconnectController->reset();
        mReconnectController->start(1); //need the 1ms delay to start asynchronously, in order to process(i.e. ignore) all stale libevent messages for the old connection so they don't get interpreted in the context of the new connection
    });
#if 0
    //test
    mega::setInterval([this]()
    {
        printf("simulating disconnect\n");
        xmpp_disconnect(*conn, -1);
    }, 6000);
#endif
}


void Client::notifyNetworkOffline()
{
    KR_LOG_WARNING("Network offline notification received, starting reconnect attempts");
    if (xmpp_conn_get_state(*conn) == XMPP_STATE_DISCONNECTED)
    {
        //if we are disconnected, the retry controller must never be at work, so not 'finished'
        assert(mReconnectController->state() != mega::rh::kStateFinished);
        if (mReconnectController->currentAttemptNo() > 2)
            mReconnectController->restart();
    }
    else
    {
        conn->disconnect(-1); //this must trigger the conn state handler which will start the reconnect controller
    }
}


void Client::notifyNetworkOnline()
{
    if (xmpp_conn_get_state(*conn) == XMPP_STATE_CONNECTED)
        return;

    if (mReconnectController->state() == mega::rh::kStateFinished)
    {
        KR_LOG_WARNING("notifyNetworkOnline: reconnect controller is in 'finished' state, but connection is not connected. Resetting reconnect controller.");
        mReconnectController->reset();
    }
    mReconnectController->restart();
}

promise::Promise<void> Client::terminate()
{
    if (isTerminating)
    {
        KR_LOG_WARNING("Client::terminate: Already terminating");
        return promise::Promise<void>();
    }
    isTerminating = true;
    if (mReconnectConnStateHandler)
    {
        conn->removeConnStateHandler(mReconnectConnStateHandler);
        mReconnectConnStateHandler = 0;
    }
    if (mReconnectController)
        mReconnectController->abort();
    if (rtc)
        rtc->hangupAll();
    chatd.reset();
    sqlite3_close(db);
    promise::Promise<void> pms;
    conn->disconnect(2000)
    //resolve output promise asynchronously, because the callbacks of the output
    //promise may free the client, and the resolve()-s of the input promises
    //(mega and conn) are within the client's code, so any code after the resolve()s
    //that tries to access the client will crash
    .then([this, pms](int) mutable
    {
        return api->call(&::mega::MegaApi::localLogout);
    })
    .then([pms](ReqResult result) mutable
    {
        mega::marshallCall([pms]() mutable { pms.resolve(); });
    })
    .fail([pms](const promise::Error& err) mutable
    {
        mega::marshallCall([pms, err]() mutable { pms.reject(err); });
        return err;
    });
    return pms;
}

void Client::startKeepalivePings()
{
    mega::setInterval([this]()
    {
        if (!xmpp_conn_is_authenticated(*conn))
            return;
        if (mLastPingTs) //waiting for pong
        {
            if (xmpp_time_stamp()-mLastPingTs > 9000)
            {
                KR_LOG_WARNING("Keepalive ping timeout");
                notifyNetworkOffline();
            }
        }
        else
        {
            mLastPingTs = xmpp_time_stamp();
            pingPeer(nullptr)
            .then([this](strophe::Stanza s)
            {
                mLastPingTs = 0;
                return 0;
            });
        }
    }, 10000);
}


strophe::StanzaPromise Client::pingPeer(const char* peerJid)
{
    strophe::Stanza ping(*conn);
    ping.setName("iq")
        .setAttr("type", "get")
        .c("ping")
                .setAttr("xmlns", "urn:xmpp:ping");
    if (peerJid)
        ping.setAttr("to", peerJid);

    return conn->sendIqQuery(ping, "png")
    .fail([](const promise::Error& err)
    {
        KR_LOG_ERROR("Error receiving pong\n");
        return err;
    });
}

promise::Promise<void> Client::setPresence(Presence pres, bool force)
{
    if ((pres.status() == mOwnPresence.status()) && !force)
        return promise::Void();
    auto previous = mOwnPresence;
    mOwnPresence = pres;

    if (pres.status() == Presence::kOffline)
    {
        mReconnectController->abort();
        conn->disconnect(4000);
        gui.onOwnPresence(Presence::kOffline);
        return promise::Void();
    }
    if (previous.status() == Presence::kOffline) //we were disconnected
    {
        mReconnectController->reset();
        gui.onOwnPresence(pres.val() | Presence::kInProgress);
        return static_cast<promise::Promise<void>&>(mReconnectController->start());
    }
    gui.onOwnPresence(pres.val() | Presence::kInProgress);
    strophe::Stanza msg(*conn);
    msg.setName("presence")
       .c("show")
           .t(pres.toString())
           .up()
       .c("status")
           .t(pres.toString())
           .up();

    return conn->sendQuery(msg)
    .then([this, pres](strophe::Stanza)
    {
        gui.onOwnPresence(pres.status());
    });
}

UserAttrDesc attrDesc[mega::MegaApi::USER_ATTR_LAST_INTERACTION+1] =
{ //getData func | changeMask
  //0 - avatar
   { [](const mega::MegaRequest& req)->Buffer* { return new Buffer(req.getFile(), strlen(req.getFile())); }, mega::MegaUser::CHANGE_TYPE_AVATAR},
  //firstname and lastname are handled specially, so we don't use a descriptor for it
  //1 - first name
   { [](const mega::MegaRequest& req)->Buffer* { return new Buffer(req.getText(), strlen(req.getText())); }, mega::MegaUser::CHANGE_TYPE_FIRSTNAME},
  //2 = last name
   { [](const mega::MegaRequest& req)->Buffer* { return new Buffer(req.getText(), strlen(req.getText())); }, mega::MegaUser::CHANGE_TYPE_LASTNAME},
  //keyring
   { [](const mega::MegaRequest& req)->Buffer* { throw std::runtime_error("not implemented"); }, mega::MegaUser::CHANGE_TYPE_AUTH},
  //last interaction
   { [](const mega::MegaRequest& req)->Buffer* { throw std::runtime_error("not implemented"); }, mega::MegaUser::CHANGE_TYPE_LSTINT}
};

UserAttrCache::~UserAttrCache()
{
    mClient.api->removeGlobalListener(this);
}

void UserAttrCache::dbWrite(UserAttrPair key, const Buffer& data)
{
        sqliteQuery(mClient.db,
            "insert or replace into userattrs(userid, type, data) values(?,?,?)",
            key.user, key.attrType, data);
}

void UserAttrCache::dbWriteNull(UserAttrPair key)
{
    sqliteQuery(mClient.db,
        "insert or replace into userattrs(userid, type, data) values(?,?,NULL)",
        key.user, key.attrType);
}

UserAttrCache::UserAttrCache(Client& aClient): mClient(aClient)
{
    //load only api-supported types, skip 'virtual' types >= 128 as they can't be fetched in the normal way
    SqliteStmt stmt(mClient.db, "select userid, type, data from userattrs where type < 128");
    while(stmt.step())
    {
        std::unique_ptr<Buffer> data(new Buffer((size_t)sqlite3_column_bytes(stmt, 2)));
        stmt.blobCol(2, *data);

        emplace(std::piecewise_construct,
            std::forward_as_tuple(stmt.uint64Col(0), stmt.intCol(1)),
            std::forward_as_tuple(std::make_shared<UserAttrCacheItem>(data.release(), false)));
    }
    mClient.api->addGlobalListener(this);
}

void Client::onUsersUpdate(mega::MegaApi* api, mega::MegaUserList *aUsers)
{
    if (!aUsers)
        return;
    std::shared_ptr<mega::MegaUserList> users(aUsers->copy());
    mega::marshallCall([this, users]()
    {
        auto count = users->size();
        for (int i=0; i<count; i++)
        {
            auto& user = *users->get(i);
            if (user.getChanges())
                userAttrCache.onUserAttrChange(user);
            else
                contactList->onUserAddRemove(user);
        };
    });
}
const char* nonWhitespaceStr(const char* str)
{
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;
    std::u16string u16 = convert.from_bytes(str);
    for (auto s: u16)
    {
        if (!iswblank(s))
            return str;
    }
    return nullptr;
}

void UserAttrCache::onUserAttrChange(mega::MegaUser& user)
{
    int changed = user.getChanges();
    for (auto t = 0; t <= mega::MegaApi::USER_ATTR_LAST_INTERACTION; t++)
    {
        if ((changed & attrDesc[t].changeMask) == 0)
            continue;
        UserAttrPair key(user.getHandle(), t);
        auto it = find(key);
        if (it == end()) //we don't have such attribute
            continue;
        auto& item = it->second;
        dbInvalidateItem(key); //immediately invalidate parsistent cache
        if (item->cbs.empty()) //we aren't using that item atm
        { //delete it from memory as well, forcing it to be freshly fetched if it's requested
            erase(key);
            continue;
        }
        if (item->pending)
            continue;
        item->pending = kCacheFetchUpdatePending;
        fetchAttr(key, item);
    }
}
void UserAttrCache::dbInvalidateItem(const UserAttrPair& key)
{
    sqliteQuery(mClient.db, "delete from userattrs where userid=? and type=?",
                key.user, key.attrType);
}

void UserAttrCacheItem::notify()
{
    for (auto it=cbs.begin(); it!=cbs.end();)
    {
        auto curr = it;
        it++;
        curr->cb(data.get(), curr->userp); //may erase curr
    }
}
UserAttrCacheItem::~UserAttrCacheItem()
{
}

uint64_t UserAttrCache::addCb(iterator itemit, UserAttrReqCbFunc cb, void* userp)
{
    auto& cbs = itemit->second->cbs;
    auto it = cbs.emplace(cbs.end(), cb, userp);
    mCallbacks.emplace(std::piecewise_construct, std::forward_as_tuple(++mCbId),
                       std::forward_as_tuple(itemit, it));
    return mCbId;
}

bool UserAttrCache::removeCb(const uint64_t& cbid)
{
    auto it = mCallbacks.find(cbid);
    if (it == mCallbacks.end())
        return false;
    auto& cbDesc = it->second;
    cbDesc.itemit->second->cbs.erase(cbDesc.cbit);
    return true;
}

uint64_t UserAttrCache::getAttr(const uint64_t& userHandle, unsigned type,
            void* userp, UserAttrReqCbFunc cb)
{
    UserAttrPair key(userHandle, type);
    auto it = find(key);
    if (it != end())
    {
        auto& item = *it->second;
        if (cb)
        { //TODO: not optimal to store each cb pointer, as these pointers would be mostly only a few, with different userp-s
            auto cbid = addCb(it, cb, userp);
            if (item.pending != kCacheFetchNewPending)
                cb(item.data.get(), userp);
            return cbid;
        }
        else
        {
            return 0;
        }
    }

    auto item = std::make_shared<UserAttrCacheItem>(nullptr, kCacheFetchNewPending);
    it = emplace(key, item).first;
    uint64_t cbid = cb ? addCb(it, cb, userp) : 0;
    fetchAttr(key, item);
    return cbid;
}

void UserAttrCache::fetchAttr(const UserAttrPair& key, std::shared_ptr<UserAttrCacheItem>& item)
{
    if (!mClient.isLoggedIn())
        return;
    if (key.attrType != mega::MegaApi::USER_ATTR_LASTNAME)
    {
        auto& attrType = attrDesc[key.attrType];
        mClient.api->call(&mega::MegaApi::getUserAttribute,
            base64urlencode(&key.user, sizeof(key.user)).c_str(), (int)key.attrType)
        .then([this, &attrType, key, item](ReqResult result)
        {
            item->pending = kCacheFetchNotPending;
            item->data.reset(attrType.getData(*result));
            dbWrite(key, *item->data);
            item->notify();
        })
        .fail([this, key, item](const promise::Error& err)
        {
            item->pending = kCacheFetchNotPending;
            item->data = nullptr;
            if (err.code() == mega::API_ENOENT)
                dbWriteNull(key);
            item->notify();
            return err;
        });
    }
    else
    {
        std::string strUh = base64urlencode(&key.user, sizeof(key.user));
        mClient.api->call(&mega::MegaApi::getUserAttribute, strUh.c_str(),
                  (int)MyMegaApi::USER_ATTR_FIRSTNAME)
        .fail([this, item](const promise::Error& err)
        {
            return ReqResult(nullptr); //silently ignore errors for the first name, in case we can still retrieve the second name
        })
        .then([this, strUh, key, item](ReqResult result)
        {
            const char* name;
            if (result && ((name = nonWhitespaceStr(result->getText()))))
            {
                item->data.reset(new Buffer);
                size_t len = strlen(name);
                if (len > 255)
                {
                    item->data->append<unsigned char>(255);
                    item->data->append(name, 252);
                    item->data->append("...", 3);
                }
                else
                {
                    item->data->append<unsigned char>(len);
                    item->data->append(name);
                }
            }
            return mClient.api->call(&mega::MegaApi::getUserAttribute, strUh.c_str(),
                    (int)MyMegaApi::USER_ATTR_LASTNAME);
        })
        .then([this, key, item](ReqResult result)
        {
            const char* name = nonWhitespaceStr(result->getText());
            if (name)
            {
                if (!item->data)
                    item->data.reset(new Buffer);

                auto& data = *item->data;
                data.append(' ');
                data.append(name).append<char>(0);
                dbWrite(key, data);
            }
            else
            {
                dbWriteNull(key);
            }
            item->pending = kCacheFetchNotPending;
            item->notify();
        })
        .fail([this, key, item](const promise::Error& err)
        {
            item->pending = kCacheFetchNotPending;
            if (err.code() == mega::API_ENOENT)
            {
                if (!item->data)
                {
                    dbWriteNull(key);
                }
                else
                {
                    dbWrite(key, *item->data);
                }
            } //event if we have error here, we don't chear item->data as we may have the first name, but won't cache it in db, so the next app run will retry
            item->notify();
        });
    }
}

void UserAttrCache::onLogin()
{
    for (auto& item: *this)
    {
        if (item.second->pending != kCacheFetchNotPending)
            fetchAttr(item.first, item.second);
    }
}

promise::Promise<Buffer*> UserAttrCache::getAttr(const uint64_t &user, unsigned attrType)
{
    struct State
    {
        Promise<Buffer*> pms;
        UserAttrCache* self;
        uint64_t cbid;
    };
    State* state = new State;
    state->self = this;
    state->cbid = getAttr(user, attrType, state, [](Buffer* buf, void* userp)
    {
        auto s = static_cast<State*>(userp);
        s->self->removeCb(s->cbid);
        if (buf)
            s->pms.resolve(buf);
        else
            s->pms.reject("failed");
        delete s;
    });
    return state->pms;
}


ChatRoom::ChatRoom(ChatRoomList& aParent, const uint64_t& chatid, bool aIsGroup, const std::string& aUrl, unsigned char aShard,
  chatd::Priv aOwnPriv)
:parent(aParent), mChatid(chatid), mUrl(aUrl), mShardNo(aShard), mIsGroup(aIsGroup), mOwnPriv(aOwnPriv)
{}

void ChatRoom::join()
{
    parent.client.chatd->join(mChatid, mShardNo, mUrl, this, new chatd::ICrypto);
}

GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const uint64_t& chatid, const std::string& aUrl, unsigned char aShard,
    chatd::Priv aOwnPriv, const std::string& title)
:ChatRoom(parent, chatid, true, aUrl, aShard, aOwnPriv), mTitleString(title),
  mHasUserTitle(!title.empty())
{
    SqliteStmt stmt(parent.client.db, "select userid, priv from chat_peers where chatid=?");
    stmt << mChatid;
    while(stmt.step())
    {
        addMember(stmt.uint64Col(0), (chatd::Priv)stmt.intCol(1), false);
    }
    mContactGui = parent.client.gui.contactList().createGroupChatItem(*this);
    if (!mTitleString.empty())
        mContactGui->updateTitle(mTitleString);
    join();
}
PeerChatRoom::PeerChatRoom(ChatRoomList& parent, const uint64_t& chatid, const std::string& aUrl,
    unsigned char aShard, chatd::Priv aOwnPriv, const uint64_t& peer, chatd::Priv peerPriv)
:ChatRoom(parent, chatid, false, aUrl, aShard, aOwnPriv), mPeer(peer), mPeerPriv(peerPriv)
{
    parent.client.contactList->attachRoomToContact(peer, *this);
    join();
}

PeerChatRoom::PeerChatRoom(ChatRoomList& parent, const mega::MegaTextChat& chat)
    :ChatRoom(parent, chat.getHandle(), false, chat.getUrl(), chat.getShard(),
     (chatd::Priv)chat.getOwnPrivilege()),
    mPeer((uint64_t)-1), mPeerPriv(chatd::PRIV_RDONLY)
{
    assert(!chat.isGroup());
    auto peers = chat.getPeerList();
    assert(peers);
    assert(peers->size() == 1);
    mPeer = peers->getPeerHandle(0);
    mPeerPriv = (chatd::Priv)peers->getPeerPrivilege(0);

    sqliteQuery(parent.client.db, "insert into chats(chatid, url, shard, peer, peer_priv, own_priv) values (?,?,?,?,?,?)",
        mChatid, mUrl, mShardNo, mPeer, mPeerPriv, mOwnPriv);
//just in case
    sqliteQuery(parent.client.db, "delete from chat_peers where chatid = ?", mChatid);
    parent.client.contactList->attachRoomToContact(mPeer, *this);
    KR_LOG_DEBUG("Added 1on1 chatroom '%s' from API", chatd::Id(mChatid).toString().c_str());
    join();
}

bool PeerChatRoom::syncOwnPriv(chatd::Priv priv)
{
    if (mOwnPriv == priv)
        return false;

    mOwnPriv = priv;
    sqliteQuery(parent.client.db, "update chats set own_priv = ? where chatid = ?",
                priv, mChatid);
    return true;
}

bool PeerChatRoom::syncPeerPriv(chatd::Priv priv)
{
    if (mPeerPriv == priv)
        return false;
    mPeerPriv = priv;
    sqliteQuery(parent.client.db, "update chats set peer_priv = ? where chatid = ?",
                priv, mChatid);
    return true;
}

bool PeerChatRoom::syncWithApi(const mega::MegaTextChat &chat)
{
    bool changed = ChatRoom::syncRoomPropertiesWithApi(chat);
    changed |= syncOwnPriv((chatd::Priv)chat.getOwnPrivilege());
    changed |= syncPeerPriv((chatd::Priv)chat.getPeerList()->getPeerPrivilege(0));
    return changed;
}

static std::string sEmptyString;
const std::string& PeerChatRoom::titleString() const
{
    return mContact ? mContact->titleString(): sEmptyString;
}

void GroupChatRoom::addMember(const uint64_t& userid, chatd::Priv priv, bool saveToDb)
{
    assert(userid != parent.client.myHandle());
    auto it = mPeers.find(userid);
    if (it != mPeers.end())
    {
        if (it->second->mPriv == priv)
        {
            saveToDb = false;
        }
        else
        {
            it->second->mPriv = priv;
        }
    }
    else
    {
        mPeers.emplace(userid, new Member(*this, userid, priv)); //usernames will be updated when the Member object gets the username attribute
    }
    if (saveToDb)
    {
        sqliteQuery(parent.client.db, "insert or replace into chat_peers(chatid, userid, priv) values(?,?,?)",
            mChatid, userid, priv);
    }
}
bool GroupChatRoom::removeMember(const uint64_t& userid)
{
    auto it = mPeers.find(userid);
    if (it == mPeers.end())
    {
        KR_LOG_WARNING("GroupChatRoom::removeMember for a member that we don't have, ignoring");
        return false;
    }
    delete it->second;
    mPeers.erase(it);
    sqliteQuery(parent.client.db, "delete from chat_peers where chatid=? and userid=?",
                mChatid, userid);
    updateTitle();
    return true;
}

void GroupChatRoom::deleteSelf()
{
    auto db = parent.client.db;
    sqliteQuery(db, "delete from chat_peers where chatid=?", mChatid);
    sqliteQuery(db, "delete from chats where chatid=?", mChatid);
    delete this;
}

ChatRoomList::ChatRoomList(Client& aClient)
:client(aClient)
{
    loadFromDb();
}

void ChatRoomList::loadFromDb()
{
    SqliteStmt stmt(client.db, "select chatid, url, shard, own_priv, peer, peer_priv, title from chats");
    while(stmt.step())
    {
        auto chatid = stmt.uint64Col(0);
        if (find(chatid) != end())
        {
            KR_LOG_WARNING("ChatRoomList: Attempted to load from db cache a chatid that is already in memory");
            continue;
        }
        auto url = stmt.stringCol(1);
        if (url.empty())
        {
            KR_LOG_ERROR("ChatRoomList::loadFromDb: Chatroom has empty URL, ignoring and deleting from db");
            sqliteQuery(client.db, "delete from chats where chatid = ?", chatid);
            sqliteQuery(client.db, "delete from chat_peers where chatid = ?", chatid);
            continue;
        }
        auto peer = stmt.uint64Col(4);
        ChatRoom* room;
        if (peer != uint64_t(-1))
            room = new PeerChatRoom(*this, chatid, stmt.stringCol(1), stmt.intCol(2), (chatd::Priv)stmt.intCol(3), peer, (chatd::Priv)stmt.intCol(5));
        else
            room = new GroupChatRoom(*this, chatid, stmt.stringCol(1), stmt.intCol(2), (chatd::Priv)stmt.intCol(3), stmt.stringCol(6));
        emplace(chatid, room);
    }
}
void ChatRoomList::syncRoomsWithApi(const mega::MegaTextChatList& rooms)
{
    auto size = rooms.size();
    for (int i=0; i<size; i++)
    {
        addRoom(*rooms.get(i));
    }
}
ChatRoom& ChatRoomList::addRoom(const mega::MegaTextChat& room, const std::string& groupUserTitle)
{
    auto chatid = room.getHandle();
    auto it = find(chatid);
    if (it != end()) //we already have that room
    {
        it->second->syncWithApi(room);
        return *it->second;
    }
    ChatRoom* ret;
    if(room.isGroup())
        ret = new GroupChatRoom(*this, room, groupUserTitle); //also writes it to cache
    else
        ret = new PeerChatRoom(*this, room);
    emplace(chatid, ret);
    return *ret;
}
bool ChatRoomList::removeRoom(const uint64_t &chatid)
{
    auto it = find(chatid);
    if (it == end())
        return false;
    if (!it->second->isGroup())
        throw std::runtime_error("Can't delete a 1on1 chat");
    static_cast<GroupChatRoom*>(it->second)->deleteSelf();
    erase(it);
    return true;
}
void Client::onChatsUpdate(mega::MegaApi *, mega::MegaTextChatList *rooms)
{
    std::shared_ptr<mega::MegaTextChatList> copy(rooms->copy());
    mega::marshallCall([this, copy]() { chats->onChatsUpdate(copy); });
}

void ChatRoomList::onChatsUpdate(const std::shared_ptr<mega::MegaTextChatList>& rooms)
{
    for (int i=0; i<rooms->size(); i++)
    {
        auto& room = *rooms->get(i);
        auto chatid = room.getHandle();
        auto it = find(chatid);
        auto localRoom = (it != end()) ? it->second : nullptr;
        auto priv = room.getOwnPrivilege();
        if (localRoom)
        {
            if (priv == chatd::PRIV_NOTPRESENT) //we were removed by someone else
            {
                KR_LOG_DEBUG("Chatroom[%s]: API event: We were removed", chatd::Id(chatid).toString().c_str());
                removeRoom(chatid);
            }
            else
            {
                client.api->call(&mega::MegaApi::getUrlChat, chatid)
                .then([this, chatid, rooms, &room](ReqResult result)
                {
                    auto it = find(chatid);
                    if (it == end())
                        return;
                    room.setUrl(result->getLink());
                    it->second->syncWithApi(room);
                });
            }
        }
        else
        {
            if (priv != chatd::PRIV_NOTPRESENT) //we didn't remove ourself from the room
            {
                KR_LOG_DEBUG("Chatroom[%s]: Received invite to join", chatd::Id(chatid).toString().c_str());
                client.api->call(&mega::MegaApi::getUrlChat, chatid)
                .then([this, chatid, rooms, &room](ReqResult result)
                {
                    room.setUrl(result->getLink());
                    auto& createdRoom = addRoom(room);
                    client.gui.notifyInvited(createdRoom);
                });
            }
            else
            {
                KR_LOG_DEBUG("Chatroom[%s]: We should have just removed ourself from the room", chatd::Id(chatid).toString().c_str());
            }
        }
    }
}

ChatRoomList::~ChatRoomList()
{
    for (auto& room: *this)
        delete room.second;
}

GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const mega::MegaTextChat& chat, const std::string &userTitle)
:ChatRoom(parent, chat.getHandle(), true, chat.getUrl(), chat.getShard(),
  (chatd::Priv)chat.getOwnPrivilege()),
  mTitleString(userTitle), mHasUserTitle(!userTitle.empty())
{
    auto peers = chat.getPeerList();
    if (peers)
    {
        auto size = peers->size();
        for (int i=0; i<size; i++)
        {
            auto handle = peers->getPeerHandle(i);
            mPeers[handle] = new Member(*this, handle, (chatd::Priv)peers->getPeerPrivilege(i)); //may try to access mContactGui, but we have set it to nullptr, so it's ok
        }
    }
//save to db
    auto db = parent.client.db;
    sqliteQuery(db, "delete from chat_peers where chatid=?", mChatid);
    if (!userTitle.empty())
    {
        sqliteQuery(db, "insert or replace into chats(chatid, url, shard, peer, peer_priv, own_priv, title) values(?,?,?,-1,0,?,?)",
                mChatid, mUrl, mShardNo, mOwnPriv, userTitle);
    }
    else
    {
        sqliteQuery(db, "insert or replace into chats(chatid, url, shard, peer, peer_priv, own_priv) values(?,?,?,-1,0,?)",
                mChatid, mUrl, mShardNo, mOwnPriv);
        loadUserTitle();
    }
    SqliteStmt stmt(db, "insert into chat_peers(chatid, userid, priv) values(?,?,?)");
    for (auto& m: mPeers)
    {
        stmt << mChatid << m.first << m.second->mPriv;
        stmt.step();
        stmt.reset().clearBind();
    }
    mContactGui = parent.client.gui.contactList().createGroupChatItem(*this);
    if (!mTitleString.empty())
        mContactGui->updateTitle(mTitleString);
    join();
}

void GroupChatRoom::loadUserTitle()
{
    //load user title if set
    SqliteStmt stmt(parent.client.db, "select title from chats where chatid = ?");
    stmt << mChatid;
    if (!stmt.step())
    {
        mHasUserTitle = false;
        return;
    }
    std::string strTitle = stmt.stringCol(0);
    if (strTitle.empty())
    {
        mHasUserTitle = false;
        return;
    }
    mTitleString = strTitle;
    mHasUserTitle = true;
}

void GroupChatRoom::setUserTitle(const std::string& title)
{
    mTitleString = title;
    if (mTitleString.empty())
    {
        mHasUserTitle = false;
        sqliteQuery(parent.client.db, "update chats set title=NULL where chatid=?", mChatid);
    }
    else
    {
        mHasUserTitle = true;
        sqliteQuery(parent.client.db, "update chats set title=? where chatid=?", mTitleString, mChatid);
    }
}

GroupChatRoom::~GroupChatRoom()
{
    auto chatd = parent.client.chatd.get();
    if (chatd)
        chatd->leave(mChatid);
    for (auto& m: mPeers)
        delete m.second;
    parent.client.gui.contactList().removeGroupChatItem(mContactGui);
}

void GroupChatRoom::leave()
{
    //rely on actionpacket to do the actual removal of the group
    parent.client.api->call(&mega::MegaApi::removeFromChat, mChatid, parent.client.myHandle());
}

promise::Promise<void> GroupChatRoom::invite(uint64_t userid, chatd::Priv priv)
{
    return parent.client.api->call(&mega::MegaApi::inviteToChat, mChatid, userid, priv)
    .then([this, userid, priv](ReqResult)
    {
        mPeers.emplace(userid, new Member(*this, userid, priv));
    });
}

bool ChatRoom::syncRoomPropertiesWithApi(const mega::MegaTextChat &chat)
{
    bool changed = false;
    if (chat.getShard() != mShardNo)
        throw std::runtime_error("syncWithApi: Shard number of chat can't change");
    if (chat.isGroup() != mIsGroup)
        throw std::runtime_error("syncWithApi: isGroup flag can't change");
    auto db = parent.client.db;
    auto url = chat.getUrl();
    if (!url)
        throw std::runtime_error("MegaTextChat::getUrl() returned NULL");
    if (strcmp(url, mUrl.c_str()))
    {
        mUrl = url;
        changed = true;
        sqliteQuery(db, "update chats set url=? where chatid=?", mUrl, mChatid);
    }
    chatd::Priv ownPriv = (chatd::Priv)chat.getOwnPrivilege();
    if (ownPriv != mOwnPriv)
    {
        mOwnPriv = ownPriv;
        changed = true;
        sqliteQuery(db, "update chats set own_priv=? where chatid=?", ownPriv, mChatid);
    }
    return changed;
}
void ChatRoom::init(chatd::Messages& msgs, chatd::DbInterface*& dbIntf)
{
    mMessages = &msgs;
    dbIntf = new ChatdSqliteDb(msgs, parent.client.db);
    if (mChatWindow)
    {
        switchListenerToChatWindow();
    }
}

IGui::IChatWindow &ChatRoom::chatWindow()
{
    if (!mChatWindow)
    {
        mChatWindow = parent.client.gui.createChatWindow(*this);
        mChatWindow->updateTitle(titleString());
        switchListenerToChatWindow();
    }
    return *mChatWindow;
}

void ChatRoom::switchListenerToChatWindow()
{
    if (mMessages->listener() == mChatWindow)
        return;
    chatd::DbInterface* dummyIntf = nullptr;
    mChatWindow->init(*mMessages, dummyIntf);
    mMessages->setListener(mChatWindow);
}

Presence PeerChatRoom::presence() const
{
    return calculatePresence(mContact->xmppContact().presence());
}

void PeerChatRoom::updatePresence()
{
    if (mChatWindow)
        mChatWindow->updateOnlineIndication(presence());
}

void GroupChatRoom::updateAllOnlineDisplays(Presence pres)
{
    if (mContactGui)
        mContactGui->updateOnlineIndication(pres);
    if (mChatWindow)
        mChatWindow->updateOnlineIndication(pres);
}

void GroupChatRoom::onUserJoined(const chatd::Id &userid, chatd::Priv privilege)
{
    if (userid != parent.client.myHandle())
        addMember(userid, privilege, true);
}
void GroupChatRoom::onUserLeft(const chatd::Id &userid)
{
    removeMember(userid);
}

void PeerChatRoom::onUserJoined(const chatd::Id &userid, chatd::Priv privilege)
{
    if (userid == parent.client.chatd->userId())
        syncOwnPriv(privilege);
    else if (userid.val == mPeer)
        syncPeerPriv(privilege);
    else
        KR_LOG_ERROR("PeerChatRoom: Bug: Received JOIN event from chatd for a third user, ignoring");
}

void PeerChatRoom::onUserLeft(const chatd::Id &userid)
{
    KR_LOG_ERROR("PeerChatRoom: Bug: Received an user leave event from chatd on a permanent chat, ignoring");
}
void ChatRoom::onRecvNewMessage(chatd::Idx idx, chatd::Message &msg, chatd::Message::Status status)
{
    contactGui().updateOverlayCount(mMessages->unreadMsgCount());
}
void ChatRoom::onMessageStatusChange(chatd::Idx idx, chatd::Message::Status newStatus, const chatd::Message &msg)
{
    contactGui().updateOverlayCount(mMessages->unreadMsgCount());
}

IGui::IContactGui& PeerChatRoom::contactGui()
{
    return mContact->gui();
}

void PeerChatRoom::onOnlineStateChange(chatd::ChatState state)
{
    mContact->onPresence(mContact->xmppContact().presence());
}
void PeerChatRoom::onUnreadChanged()
{
//    printf("onUnreadChanged: %s, %d\n", mMessages->chatId().toString().c_str(), mMessages->unreadMsgCount());
    mContact->gui().updateOverlayCount(mMessages->unreadMsgCount());
}

void GroupChatRoom::onOnlineStateChange(chatd::ChatState state)
{
    updateAllOnlineDisplays((state == chatd::kChatStateOnline)
        ? Presence::kOnline
        : Presence::kOffline);
}

bool GroupChatRoom::syncMembers(const chatd::UserPrivMap& users)
{
    bool changed = false;
    auto db = parent.client.db;
    for (auto ourIt=mPeers.begin(); ourIt!=mPeers.end();)
    {
        auto userid = ourIt->first;
        auto it = users.find(userid);
        if (it == users.end()) //we have a user that is not in the chatroom anymore
        {
            changed = true;
            auto erased = ourIt;
            ourIt++;
            auto member = erased->second;
            mPeers.erase(erased);
            delete member;
            sqliteQuery(db, "delete from chat_peers where chatid=? and userid=?", mChatid, userid);
            KR_LOG_DEBUG("GroupChatRoom[%s]:syncMembers: Removed member %s",
                chatd::Id(mChatid).toString().c_str(), chatd::Id(userid).toString().c_str());
        }
        else
        {
            if (ourIt->second->mPriv != it->second)
            {
                changed = true;
                sqliteQuery(db, "update chat_peers where chatid=? and userid=? set priv=?",
                    mChatid, userid, it->second);
                ourIt->second->mPriv = it->second;
                KR_LOG_DEBUG("GroupChatRoom[%s]:syncMembers: Changed privilege of member %s",
                    chatd::Id(userid).toString().c_str());
            }
            ourIt++;
        }
    }
    for (auto& user: users)
    {
        if (mPeers.find(user.first) == mPeers.end())
        {
            changed = true;
            addMember(user.first, user.second, true);
        }
    }
    return changed;
}

bool GroupChatRoom::syncWithApi(const mega::MegaTextChat& chat)
{
    bool changed = ChatRoom::syncRoomPropertiesWithApi(chat);
    chatd::UserPrivMap membs;
    changed |= syncMembers(apiMembersToMap(chat, membs));
    if (changed)
    {
        if (mContactGui)
            mContactGui->onMembersUpdated();
        if (mChatWindow)
            mChatWindow->onMembersUpdated();
    }
    return changed;
}

chatd::UserPrivMap& GroupChatRoom::apiMembersToMap(const mega::MegaTextChat& chat, chatd::UserPrivMap& membs)
{
    auto members = chat.getPeerList();
    if (members)
    {
        auto size = members->size();
        for (int i=0; i<size; i++)
            membs.emplace(members->getPeerHandle(i), (chatd::Priv)members->getPeerPrivilege(i));
    }
    return membs;
}

GroupChatRoom::Member::Member(GroupChatRoom& aRoom, const uint64_t& user, chatd::Priv aPriv)
: mRoom(aRoom), mPriv(aPriv)
{
    mNameAttrCbHandle = mRoom.parent.client.userAttrCache.getAttr(user, mega::MegaApi::USER_ATTR_LASTNAME, this,
    [](Buffer* buf, void* userp)
    {
        auto self = static_cast<Member*>(userp);
        if (buf)
            self->mName.assign(buf->buf(), buf->dataSize());
        else if (self->mName.empty())
            self->mName = "\x07(error)";
        self->mRoom.updateTitle();
    });
}
GroupChatRoom::Member::~Member()
{
    mRoom.parent.client.userAttrCache.removeCb(mNameAttrCbHandle);
}

ContactList::ContactList(Client& aClient)
:client(aClient)
{
    SqliteStmt stmt(client.db, "select userid, email, since from contacts");
    while(stmt.step())
    {
        auto userid = stmt.uint64Col(0);
        emplace(userid, new Contact(*this, userid, stmt.stringCol(1), stmt.int64Col(2),
            nullptr));
    }
}

bool ContactList::addUserFromApi(mega::MegaUser& user)
{
    auto userid = user.getHandle();
    auto& item = (*this)[userid];
    if (item)
        return false;
    auto cmail = user.getEmail();
    std::string email(cmail?cmail:"");
    auto ts = user.getTimestamp();
    sqliteQuery(client.db, "insert or replace into contacts(userid, email, since) values(?,?,?)", userid, email, ts);
    item = new Contact(*this, userid, email, ts, nullptr);
    KR_LOG_DEBUG("Added new user from API: %s", email.c_str());
    return true;
}

void ContactList::syncWithApi(mega::MegaUserList& users)
{
    std::set<uint64_t> apiUsers;
    auto size = users.size();
    for (int i=0; i<size; i++)
    {
        auto& user = *users.get(i);
        if (user.getVisibility() != mega::MegaUser::VISIBILITY_VISIBLE)
            continue;
        apiUsers.insert(user.getHandle());
        addUserFromApi(user);
    }
    for (auto it = begin(); it!= end();)
    {
        auto handle = it->first;
        if (apiUsers.find(handle) != apiUsers.end())
        {
            it++;
            continue;
        }
        auto erased = it;
        it++;
        removeUser(erased);
    }
}
void ContactList::onUserAddRemove(mega::MegaUser& user)
{
    auto visibility = user.getVisibility();
    if (visibility == mega::MegaUser::VISIBILITY_HIDDEN)
    {
        removeUser(user.getHandle());
//        client.gui.notifyContactRemoved(user);
    }
    else if (visibility == mega::MegaUser::VISIBILITY_VISIBLE)
    {
        addUserFromApi(user);
    }
    else
        KR_LOG_WARNING("ContactList::onUserAddRemove: Don't know how to process user with visibility %d", visibility);
}

void ContactList::removeUser(uint64_t userid)
{
    auto it = find(userid);
    if (it == end())
    {
        KR_LOG_ERROR("ContactList::removeUser: Unknown user");
        return;
    }
    removeUser(it);
}

void ContactList::removeUser(iterator it)
{
    auto handle = it->first;
    delete it->second;
    erase(it);
    sqliteQuery(client.db, "delete from contacts where userid=?", handle);
}

promise::Promise<void> ContactList::removeContactFromServer(uint64_t userid)
{
    auto it = find(userid);
    if (it == end())
        return promise::Error("Userid not in contactlist");

    auto& api = *client.api;
    std::unique_ptr<mega::MegaUser> user(api.getContact(it->second->email().c_str()));
    if (!user)
        return promise::Error("Could not get user object from email");

    return api.call(&::mega::MegaApi::removeContact, user.get())
    .then([this, userid](ReqResult ret)->promise::Promise<void>
    {
//        auto erased = find(userid);
//        if (erased != end())
//            removeUser(erased);
        return promise::_Void();
    });
}

ContactList::~ContactList()
{
    for (auto& it: *this)
        delete it.second;
}

const std::string* ContactList::getUserEmail(uint64_t userid) const
{
    auto it = find(userid);
    if (it == end())
        return nullptr;
    return &(it->second->email());
}

void Client::onContactRequestsUpdate(mega::MegaApi*, mega::MegaContactRequestList* reqs)
{
    std::shared_ptr<mega::MegaContactRequestList> copy(reqs->copy());
    mega::marshallCall([this, copy]()
    {
        auto count = copy->size();
        for (int i=0; i<count; i++)
        {
            auto& req = *copy->get(i);
            if (req.isOutgoing())
                continue;
            if (req.getStatus() == mega::MegaContactRequest::STATUS_UNRESOLVED)
                gui.onIncomingContactRequest(req);
        }
    });
}

Contact::Contact(ContactList& clist, const uint64_t& userid,
                 const std::string& email, int64_t since, PeerChatRoom* room)
    :mClist(clist), mUserid(userid), mChatRoom(room), mEmail(email), mSince(since),
     mTitleString(email),
     mDisplay(clist.client.gui.contactList().createContactItem(*this))
{
    updateTitle(email);
    mUsernameAttrCbId = mClist.client.userAttrCache.getAttr(userid,
        mega::MegaApi::USER_ATTR_LASTNAME, this,
        [](Buffer* data, void* userp)
        {
            auto self = static_cast<Contact*>(userp);
            if (!data || data->dataSize() < 2)
                self->updateTitle(self->mEmail);
            else
                self->updateTitle(data->buf()+1);
        });
    //FIXME: Is this safe? We are passing a virtual interface to this in the ctor
    mXmppContact = mClist.client.xmppContactList().addContact(*this);
}
void Contact::updateTitle(const std::string& str)
{
    mTitleString = str;
    mDisplay->updateTitle(str);
    if (mChatRoom && mChatRoom->hasChatWindow())
        mChatRoom->chatWindow().updateTitle(str);
}

Contact::~Contact()
{
    mClist.client.userAttrCache.removeCb(mUsernameAttrCbId);
    if (mXmppContact)
        mXmppContact->setPresenceListener(nullptr);
    mClist.client.gui.contactList().removeContactItem(mDisplay);
}
promise::Promise<ChatRoom*> Contact::createChatRoom()
{
    if (mChatRoom)
    {
        KR_LOG_WARNING("Contact::createChatRoom: chat room already exists, check before caling this method");
        return Promise<ChatRoom*>(mChatRoom);
    }
    mega::MegaTextChatPeerListPrivate peers;
    peers.addPeer(mUserid, chatd::PRIV_FULL);
    return mClist.client.api->call(&mega::MegaApi::createChat, false, &peers)
    .then([this](ReqResult result) -> Promise<ChatRoom*>
    {
        auto& list = *result->getMegaTextChatList();
        if (list.size() < 1)
            return promise::Error("Empty chat list returned from API");
        auto& room = mClist.client.chats->addRoom(*list.get(0));
        return &room;
    });
}

void Contact::setChatRoom(PeerChatRoom& room)
{
    assert(!mChatRoom);
    mChatRoom = &room;
    if (room.hasChatWindow())
        room.chatWindow().updateTitle(mTitleString);
}

IGui::IContactGui*
ContactList::attachRoomToContact(const uint64_t& userid, PeerChatRoom& room)
{
    auto it = find(userid);
    if (it == end())
        throw std::runtime_error("attachRoomToContact: userid '"+chatd::Id(userid)+"' not found in contactlist");
    auto& contact = *it->second;
    if (contact.mChatRoom)
        throw std::runtime_error("attachRoomToContact: contact already has a chat room attached");
    contact.setChatRoom(room);
    room.setContact(contact);
    return contact.mDisplay;
}
uint64_t Client::useridFromJid(const std::string& jid)
{
    auto end = jid.find('@');
    if (end != 13)
    {
        KR_LOG_WARNING("useridFromJid: Invalid Mega JID '%s'", jid.c_str());
        return mega::UNDEF;
    }

    uint64_t userid;
#ifndef NDEBUG
    auto len =
#endif
    mega::Base32::atob(jid.c_str(), (byte*)&userid, end);
    assert(len == 8);
    return userid;
}

Contact* ContactList::contactFromJid(const std::string& jid) const
{
    auto userid = Client::useridFromJid(jid);
    if (userid == mega::UNDEF)
        return nullptr;
    auto it = find(userid);
    if (it == this->end())
        return nullptr;
    else
        return it->second;
}

void Client::discoAddFeature(const char *feature)
{
    conn->plugin<disco::DiscoPlugin>("disco").addFeature(feature);
}
rtcModule::IEventHandler* Client::onIncomingCallRequest(
        const std::shared_ptr<rtcModule::ICallAnswer> &ans)
{
    return gui.createCallAnswerGui(ans);
}

}
