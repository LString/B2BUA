#include <pjsua-lib/pjsua.h>
#include <pjsua2.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <memory>

using namespace pj;
using namespace std;

enum LegRole {
    ROLE_UNKNOWN = 0,
    ROLE_LINPHONE,
    ROLE_ASTERISK
};

class B2bCall;

class B2bAccount : public Account {
public:
    Account *remoteAcc = nullptr; // 指向 Asterisk 侧的账号

    virtual void onIncomingCall(OnIncomingCallParam &iprm) override;
};

// B2BUA 中的一条 call leg
class B2bCall : public Call {
public:
    B2bCall(Account &acc, int call_id = PJSUA_INVALID_ID)
        : Call(acc, call_id),
          other(nullptr),
          role(ROLE_UNKNOWN),
          mediaBridged(false),
          ringbackActive(false),
          hintStarted(false),
          hintCompleted(false),
          dialAttempted(false),
          pendingDialPeer(nullptr)
    {}

    void setOther(B2bCall *o) { other = o; }
    void setRole(LegRole r)   { role = r; }

    bool hasActiveAudio() {
        CallInfo ci = getInfo();
        for (auto &m : ci.media) {
            if (m.type == PJMEDIA_TYPE_AUDIO &&
                (m.status == PJSUA_CALL_MEDIA_ACTIVE ||
                 m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD))
            {
                return true;
            }
        }
        return false;
    }

    void setPendingDial(B2bCall *peer, const string &uri) {
        pendingDialPeer = peer;
        pendingDialUri = uri;
    }

    void maybeStartHintAndDial() {
        if (role != ROLE_LINPHONE || hintStarted)
            return;

        if (!hasActiveAudio())
            return;

        hintStarted = true;

        auto *self = this;
        auto *peer = pendingDialPeer;
        string uri = pendingDialUri;

        thread([self, peer, uri]() {
            try {
                AudioMedia callMed = self->getAudioMedia(-1);
                AudioMediaPlayer player;
                player.createPlayer("unsafe_hint.wav", 0);
                player.startTransmit(callMed);
                this_thread::sleep_for(chrono::seconds(3));
                player.stopTransmit(callMed);
            } catch (Error &err) {
                cout << "Failed to play hint: " << err.info() << endl;
            }

            self->hintCompleted = true;

            if (!peer || self->dialAttempted)
                return;

            CallOpParam callPrm(true);
            try {
                cout << "Dialing " << uri << " on Asterisk side..." << endl;
                peer->makeCall(uri, callPrm);
                self->dialAttempted = true;
            } catch (Error &err) {
                cout << "makeCall() failed: " << err.info() << endl;
                CallOpParam prm;
                prm.statusCode = PJSIP_SC_INTERNAL_SERVER_ERROR;
                self->hangup(prm);
            }
        }).detach();
    }

    void startRingback() {
        if (ringbackActive)
            return;

        try {
            CallInfo ci = getInfo();
            bool hasActiveAudio = false;
            for (auto &m : ci.media) {
                if (m.type == PJMEDIA_TYPE_AUDIO &&
                    (m.status == PJSUA_CALL_MEDIA_ACTIVE ||
                     m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD))
                {
                    hasActiveAudio = true;
                    break;
                }
            }

            if (!hasActiveAudio) {
                cout << "Cannot start ringback: audio media not active yet" << endl;
                return;
            }

            ringback.createToneGenerator(8000);

            ToneDesc tone;
            tone.freq1 = 440;
            tone.freq2 = 480;
            tone.on_msec = 2000;
            tone.off_msec = 4000;

            ToneDescVector tones;
            tones.push_back(tone);

            ringback.play(tones, true);

            AudioMedia callMed = getAudioMedia(-1);
            ringback.startTransmit(callMed);
            ringbackActive = true;
        } catch (Error &err) {
            cout << "Failed to start ringback: " << err.info() << endl;
        }
    }

    void stopRingback() {
        if (!ringbackActive)
            return;

        try {
            AudioMedia callMed = getAudioMedia(-1);
            ringback.stopTransmit(callMed);
        } catch (Error &err) {
            cout << "Failed to stop ringback transmit: " << err.info() << endl;
        }

        try {
            ringback.stop();
        } catch (Error &err) {
            cout << "Failed to stop ringback: " << err.info() << endl;
        }

        ringbackActive = false;
    }

    virtual void onCallState(OnCallStateParam &prm) override {
        CallInfo ci = getInfo();
        cout << "[Call " << ci.callIdString << "] state=" << ci.stateText
             << " (" << ci.state << ")" << endl;

        // Asterisk 那一侧进入 EARLY，开始给 Linphone 侧播放本地回铃
        if (ci.state == PJSIP_INV_STATE_EARLY && role == ROLE_ASTERISK && other) {
            other->startRingback();
        }

        // Asterisk 那一侧接通后，给 Linphone 那一侧发 200 OK
        if (ci.state == PJSIP_INV_STATE_CONFIRMED && role == ROLE_ASTERISK) {
            if (other) {
                CallInfo oci = other->getInfo();
                if (oci.state == PJSIP_INV_STATE_INCOMING ||
                    oci.state == PJSIP_INV_STATE_EARLY ||
                    oci.state == PJSIP_INV_STATE_CALLING)
                {
                    CallOpParam ansPrm;
                    ansPrm.statusCode = PJSIP_SC_OK;
                    cout << "Answering caller side with 200 OK" << endl;
                    other->answer(ansPrm);
                }
            }
        }

        // 任意一条 leg 挂断后，另一条也一起挂掉
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            cout << "Call disconnected (role=" << role << ")" << endl;
            stopRingback();
            if (other) {
                other->stopRingback();
                CallInfo oci = other->getInfo();
                if (oci.state != PJSIP_INV_STATE_DISCONNECTED) {
                    CallOpParam op;
                    op.statusCode = PJSIP_SC_OK;
                    cout << "Hanging up other leg" << endl;
                    other->hangup(op);
                }
            }
        }
    }

    virtual void onCallMediaState(OnCallMediaStateParam &prm) override {
        CallInfo ci = getInfo();

        for (unsigned i = 0; i < ci.media.size(); ++i) {
            if (ci.media[i].type == PJMEDIA_TYPE_AUDIO &&
                (ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE ||
                 ci.media[i].status == PJSUA_CALL_MEDIA_REMOTE_HOLD))
            {
                cout << "Media ready on leg (role=" << role << ")" << endl;

                if (role == ROLE_LINPHONE) {
                    maybeStartHintAndDial();
                }

                if (!other || mediaBridged)
                    return;

                CallInfo oci = other->getInfo();
                if (!other->hasMedia())
                    return;

                try {
                    stopRingback();
                    other->stopRingback();

                    // 获得两侧的 AudioMedia，并互相 startTransmit
                    AudioMedia thisMed = getAudioMedia(i);
                    AudioMedia otherMed = other->getAudioMedia(-1);

                    thisMed.startTransmit(otherMed);
                    otherMed.startTransmit(thisMed);

                    mediaBridged = true;
                    cout << "Bridged media between two legs" << endl;
                } catch (Error &err) {
                    cout << "Error bridging media: " << err.info() << endl;
                }
            }
        }
    }

private:
    B2bCall *other;
    LegRole  role;
    bool     mediaBridged;
    ToneGenerator ringback;
    bool     ringbackActive;
    bool     hintStarted;
    bool     hintCompleted;
    bool     dialAttempted;
    B2bCall *pendingDialPeer;
    string   pendingDialUri;
};

static pj_bool_t registrar_on_rx_request(pjsip_rx_data *rdata);

static char registrar_mod_name[] = "simple-registrar";
static pjsip_module registrar_mod = {
    nullptr,
    nullptr,
    {registrar_mod_name, 16},
    -1,
    PJSIP_MOD_PRIORITY_APPLICATION + 1,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &registrar_on_rx_request,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

static pj_bool_t registrar_on_rx_request(pjsip_rx_data *rdata) {
    if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method, &pjsip_register_method) != 0) {
        return PJ_FALSE;
    }

    pjsip_tx_data *tdata = nullptr;
    pj_status_t status = pjsip_endpt_create_response(pjsua_get_pjsip_endpt(), rdata, PJSIP_SC_OK, nullptr, &tdata);
    if (status != PJ_SUCCESS || !tdata) {
        return PJ_FALSE;
    }

    pjsip_contact_hdr *contact_hdr = (pjsip_contact_hdr *)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CONTACT, nullptr);
    if (contact_hdr) {
        pjsip_contact_hdr *cloned = (pjsip_contact_hdr *)pjsip_hdr_clone(tdata->pool, (pjsip_hdr *)contact_hdr);
        pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *)cloned);
    }

    pjsip_expires_hdr *expires_hdr = (pjsip_expires_hdr *)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_EXPIRES, nullptr);
    if (expires_hdr) {
        pjsip_expires_hdr *cloned = (pjsip_expires_hdr *)pjsip_hdr_clone(tdata->pool, (pjsip_hdr *)expires_hdr);
        pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *)cloned);
    }

    pjsip_endpt_send_response2(pjsua_get_pjsip_endpt(), rdata, tdata, nullptr, nullptr);
    return PJ_TRUE;
}

void B2bAccount::onIncomingCall(OnIncomingCallParam &iprm) {
    cout << "=== Incoming call from Linphone side ===" << endl;

    auto *incoming = new B2bCall(*this, iprm.callId);
    incoming->setRole(ROLE_LINPHONE);

    if (!remoteAcc) {
        cout << "No remote (Asterisk) account configured, rejecting call" << endl;
        CallOpParam prm;
        prm.statusCode = PJSIP_SC_DECLINE;
        incoming->hangup(prm);
        return;
    }

    // 先接听 Linphone 侧以便后续播放提示音
    CallOpParam answerPrm(true);
    answerPrm.statusCode = PJSIP_SC_OK;
    incoming->answer(answerPrm);

    // 创建发往 Asterisk 的 leg（先不拨号，等提示音播完）
    auto *outgoing = new B2bCall(*remoteAcc);
    outgoing->setRole(ROLE_ASTERISK);

    incoming->setOther(outgoing);
    outgoing->setOther(incoming);

    // 目前“最简”：把所有来电都固定拨到 1002
    string dstUri = "sip:1002@10.0.6.91"; // TODO: 根据你实际 Asterisk IP 修改
    incoming->setPendingDial(outgoing, dstUri);
}

int main() {
    // 按实际情况修改
    const string B2BUA_IP      = "10.0.6.175";   // 运行本程序的机器 IP
    const string ASTERISK_IP   = "10.0.6.91";   // Asterisk IP
    const string EXT_USER      = "1001";        // Asterisk 中已有的分机
    const string EXT_PASSWORD  = "1001pass"; // 改成真实密码

    Endpoint ep;

    try {
        ep.libCreate();

        EpConfig epCfg;
        ep.libInit(epCfg);

        pj_status_t regStatus = pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &registrar_mod);
        if (regStatus != PJ_SUCCESS) {
            throw Error(regStatus, "Registrar registration failed", "", "", 0);
        }

        // B2BUA 不使用本机声卡，而是 null audio device
        ep.audDevManager().setNullDev();

        // 创建 UDP 5060 监听（Linphone 会往这里发 INVITE）
        TransportConfig tcfg;
        tcfg.port = 5060;
        ep.transportCreate(PJSIP_TRANSPORT_UDP, tcfg);

        ep.libStart();
        cout << "*** B2BUA started on udp:" << B2BUA_IP << ":5060 ***" << endl;

        // 1) Asterisk 侧账号：以 1001 身份注册
        AccountConfig astCfg;
        astCfg.idUri = "sip:" + EXT_USER + "@" + ASTERISK_IP;
        astCfg.regConfig.registrarUri = "sip:" + ASTERISK_IP;

        AuthCredInfo cred("digest", "*", EXT_USER, 0, EXT_PASSWORD);
        astCfg.sipConfig.authCreds.push_back(cred);
        astCfg.sipConfig.proxies.push_back("sip:" + ASTERISK_IP);

        auto *astAcc = new Account;
        astAcc->create(astCfg);
        cout << "Created account towards Asterisk as " << astCfg.idUri << endl;

        // 2) 本地账号：用于接收 Linphone 侧来话（不注册）
        auto *localAcc = new B2bAccount;
        localAcc->remoteAcc = astAcc;

        AccountConfig locCfg;
        locCfg.idUri = "sip:" + EXT_USER + "@" + B2BUA_IP;
        locCfg.regConfig.registerOnAdd = false;  // 不向任何服务器注册
        localAcc->create(locCfg);

        cout << "Local account for Linphone created as " << locCfg.idUri << endl;

        // 主事件循环
        while (true) {
            ep.libHandleEvents(50);  // 处理 50ms 内的事件
        }

    } catch (Error &err) {
        cout << "Exception: " << err.info() << endl;
        return 1;
    }

    return 0;
}
