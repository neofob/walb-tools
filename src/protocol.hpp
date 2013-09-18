#pragma once
/**
 * @file
 * @brief Protocol set.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <map>
#include <string>
#include <memory>
#include "cybozu/format.hpp"
#include "cybozu/socket.hpp"
#include "cybozu/time.hpp"
#include "cybozu/atoi.hpp"
#include "cybozu/itoa.hpp"
#include "packet.hpp"
#include "util.hpp"
#include "sys_logger.hpp"
#include "serializer.hpp"
#include "fileio.hpp"

#include "server_data.hpp"

namespace walb {

const unsigned int LBS = 512;

/**
 * Logger wrapper for protocols.
 */
class Logger
{
private:
    std::string selfId_;
    std::string remoteId_;
public:
    Logger(const std::string &selfId, const std::string &remoteId)
        : selfId_(selfId), remoteId_(remoteId) {}

    void write(cybozu::LogPriority pri, const std::string &msg) const noexcept {
        cybozu::PutLog(pri, "[%s][%s] %s", selfId_.c_str(), remoteId_.c_str(), msg.c_str());
    }
    void write(cybozu::LogPriority pri, const char *format, va_list args) const noexcept {
        try {
            std::string msg;
            cybozu::vformat(msg, format, args);
            write(pri, msg);
        } catch (...) {
            write(pri, "Logger::write() error.");
        }
    }
    void write(cybozu::LogPriority pri, const char *format, ...) const noexcept {
        try {
            va_list args;
            va_start(args, format);
            write(pri, format, args);
            va_end(args);
        } catch (...) {
            write(pri, "Logger::write() error.");
        }
    }

    void debug(UNUSED const std::string &msg) const noexcept {
#ifdef DEBUG
        write(cybozu::LogDebug, msg);
#endif
    }
    void info(const std::string &msg) const noexcept { write(cybozu::LogInfo, msg); }
    void warn(const std::string &msg) const noexcept { write(cybozu::LogWarning, msg); }
    void error(const std::string &msg) const noexcept { write(cybozu::LogError, msg); }

    void debug(UNUSED const char *format, ...) const noexcept {
#ifdef DEBUG
        va_list args;
        va_start(args, format);
        write(cybozu::LogDebug, format, args);
        va_end(args);
#endif
    }
    void info(const char *format, ...) const noexcept {
        va_list args;
        va_start(args, format);
        write(cybozu::LogInfo, format, args);
        va_end(args);
    }
    void warn(const char *format, ...) const noexcept {
        va_list args;
        va_start(args, format);
        write(cybozu::LogWarning, format, args);
        va_end(args);
    }
    void error(const char *format, ...) const noexcept {
        va_list args;
        va_start(args, format);
        write(cybozu::LogError, format, args);
        va_end(args);
    }
};

/**
 * Protocol interface.
 */
class Protocol
{
protected:
    const std::string name_; /* protocol name */
public:
    Protocol(const std::string &name) : name_(name) {}
    const std::string &name() const { return name_; }
    virtual void runAsClient(cybozu::Socket &, Logger &,
                             const std::vector<std::string> &) = 0;
    virtual void runAsServer(cybozu::Socket &, Logger &,
                             const std::vector<std::string> &) = 0;
};

/**
 * Simple echo client.
 */
class EchoProtocol : public Protocol
{
public:
    using Protocol :: Protocol;

    void runAsClient(cybozu::Socket &sock, Logger &logger,
                     const std::vector<std::string> &params) override {
        if (params.empty()) throw std::runtime_error("params empty.");
        packet::Packet packet(sock);
        uint32_t size = params.size();
        packet.write(size);
        logger.info("size: %" PRIu32 "", size);
        for (const std::string &s0 : params) {
            std::string s1;
            packet.write(s0);
            packet.read(s1);
            logger.info("s0: %s s1: %s", s0.c_str(), s1.c_str());
            if (s0 != s1) {
                throw std::runtime_error("echo-backed string differs from the original.");
            }
        }
    }
    void runAsServer(cybozu::Socket &sock, Logger &logger,
                     const std::vector<std::string> &) override {
        packet::Packet packet(sock);
        uint32_t size;
        packet.read(size);
        logger.info("size: %" PRIu32 "", size);
        for (uint32_t i = 0; i < size; i++) {
            std::string s0;
            packet.read(s0);
            packet.write(s0);
            logger.info("echoback: %s", s0.c_str());
        }
    }
};

/**
 * Utility class for protocols.
 */
class ProtocolData
{
protected:
    cybozu::Socket &sock_;
    Logger &logger_;
    const std::vector<std::string> &params_;
public:
    ProtocolData(cybozu::Socket &sock, Logger &logger, const std::vector<std::string> &params)
        : sock_(sock), logger_(logger), params_(params) {}
    virtual ~ProtocolData() noexcept = default;

    void logAndThrow(const char *fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        std::string msg = cybozu::util::formatStringV(fmt, args);
        va_end(args);
        logger_.error(msg);
        throw std::runtime_error(msg);
    }
};

/**
 * Dirty full sync.
 */
class DirtyFullSyncProtocol : public Protocol
{
private:
    class Data : public ProtocolData
    {
    protected:
        std::string name_;
        uint64_t sizeLb_;
        uint16_t bulkLb_;
        uint64_t gid_;
    public:
        using ProtocolData::ProtocolData;
        void checkParams() const {
            if (name_.empty()) logAndThrow("name param empty.");
            if (sizeLb_ == 0) logAndThrow("sizeLb param is zero.");
            if (bulkLb_ == 0) logAndThrow("bulkLb param is zero.");
            if (gid_ == uint64_t(-1)) logAndThrow("gid param must not be uint64_t(-1).");
        }
        void sendParams() {
            packet::Packet packet(sock_);
            packet.write(name_);
            packet.write(sizeLb_);
            packet.write(bulkLb_);
            packet.write(gid_);
        }
        void recvParams() {
            packet::Packet packet(sock_);
            packet.read(name_);
            packet.read(sizeLb_);
            packet.read(bulkLb_);
            packet.read(gid_);
        }
    };
    class Client : public Data
    {
    private:
        std::string path_;
    public:
        using Data::Data;
        void run() {
            loadParams();
            sendParams();
            readAndSend();
        }
    private:
        void loadParams() {
            if (params_.size() != 5) logAndThrow("Five parameters required.");
            path_ = params_[0];
            name_ = params_[1];
            uint64_t size = cybozu::util::fromUnitIntString(params_[2]);
            sizeLb_ = size / LBS;
            uint64_t bulk = cybozu::util::fromUnitIntString(params_[3]);
            if ((1U << 16) * LBS <= bulk) logAndThrow("bulk size too large. < %u\n", (1U << 16) * LBS);
            bulkLb_ = bulk / LBS;
            gid_ = cybozu::atoi(params_[4]);
        }
        void readAndSend() {
            packet::Packet packet(sock_);
            std::vector<char> buf(bulkLb_ * LBS);
            cybozu::util::BlockDevice bd(path_, O_RDONLY);

            uint64_t remainingLb = sizeLb_;
            while (0 < remainingLb) {
                uint16_t lb = bulkLb_;
                if (remainingLb < bulkLb_) lb = remainingLb;
                size_t size = lb * LBS;
                bd.read(&buf[0], size);
                packet.write(lb);
                packet.write(&buf[0], size);
                remainingLb -= lb;
            }
        }
    };
    class Server : public Data
    {
    private:
        cybozu::FilePath baseDir_;
    public:
        using Data::Data;
        void run() {
            loadParams();
            recvParams();
            logger_.info("dirty-full-sync %s %" PRIu64 " %u %" PRIu64 ""
                         , name_.c_str(), sizeLb_, bulkLb_, gid_);
            recvAndWrite();
        }
    private:
        void loadParams() {
            if (params_.size() != 1) logAndThrow("One parameter required.");
            baseDir_ = cybozu::FilePath(params_[0]);
            if (!baseDir_.stat().isDirectory()) {
                logAndThrow("Base directory %s does not exist.", baseDir_.cStr());
            }
        }
        void recvAndWrite() {
            packet::Packet packet(sock_);
            walb::ServerData sd(baseDir_.str(), name_);
            sd.reset(gid_);
            sd.createLv(sizeLb_);
            std::string lvPath = sd.getLv().path().str();
            cybozu::util::BlockDevice bd(lvPath, O_RDWR);
            std::vector<char> buf(bulkLb_ * LBS);

            uint16_t c = 0;
            uint64_t remainingLb = sizeLb_;
            while (0 < remainingLb) {
                uint16_t lb = bulkLb_;
                if (remainingLb < bulkLb_) lb = remainingLb;
                size_t size = lb * LBS;
                uint16_t lb0 = 0;
                packet.read(lb0);
                if (lb0 != lb) logAndThrow("received lb %u is invalid. must be %u", lb0, lb);
                packet.read(&buf[0], size);
                bd.write(&buf[0], size);
                remainingLb -= lb;
                c++;
            }
            logger_.info("received %" PRIu64 " packets.", c);
            bd.fdatasync();
            logger_.info("apply done.");
        }
    };

public:
    using Protocol :: Protocol;

    /**
     * @params using cybozu::loadFromStr() to convert.
     *   [0] :: string: full path of lv.
     *   [1] :: string: lv identifier.
     *   [2] :: uint64_t: lv size [byte].
     *   [3] :: uint32_t: bulk size [byte]. less than uint16_t * LBS;
     */
    void runAsClient(cybozu::Socket &sock, Logger &logger,
                     const std::vector<std::string> &params) override {
        Client client(sock, logger, params);
        client.run();
    }
    /**
     *
     * @params using cybozu::loadFromStr() to convert;
     *   [0] :: string:
     */
    void runAsServer(cybozu::Socket &sock, Logger &logger,
                     const std::vector<std::string> &params) override {
        Server server(sock, logger, params);
        server.run();
    }
};

/**
 * Protocol factory.
 */
class ProtocolFactory
{
private:
    using Map = std::map<std::string, std::unique_ptr<Protocol> >;
    Map map_;

public:
    static ProtocolFactory &getInstance() {
        static ProtocolFactory factory;
        return factory;
    }
    Protocol *find(const std::string &name) {
        Map::iterator it = map_.find(name);
        if (it == map_.end()) return nullptr;
        return it->second.get();
    }

private:
#define DECLARE_PROTOCOL(name, cls)                                     \
    map_.insert(std::make_pair(#name, std::unique_ptr<cls>(new cls(#name))))

    ProtocolFactory() : map_() {
        DECLARE_PROTOCOL(echo, EchoProtocol);
        DECLARE_PROTOCOL(dirty-full-sync, DirtyFullSyncProtocol);
        /* now editing */
    }
#undef DECLARE_PROTOCOL
};

/**
 * Run a protocol as a client.
 */
static inline void runProtocolAsClient(
    cybozu::Socket &sock, const std::string &clientId, const std::string &protocolName,
    const std::vector<std::string> &params)
{
    packet::Packet packet(sock);
    packet.write(clientId);
    packet.write(protocolName);
    packet::Version ver(sock);
    ver.send();
    std::string serverId;
    packet.read(serverId);

    Logger logger(clientId, serverId);

    packet::Answer ans(sock);
    int err;
    std::string msg;
    if (!ans.recv(&err, &msg)) {
        logger.warn("received NG: err %d msg %s", err, msg.c_str());
        return;
    }

    Protocol *protocol = ProtocolFactory::getInstance().find(protocolName);
    if (!protocol) {
        throw std::runtime_error("receive OK but protocol not found.");
    }
    protocol->runAsClient(sock, logger, params);
}

/**
 * Run a protocol as a server.
 *
 * TODO: arguments for server data.
 */
static inline void runProtocolAsServer(
    cybozu::Socket &sock, const std::string &serverId, const std::string &baseDirStr)
{
    ::printf("runProtocolAsServer start\n"); /* debug */
    packet::Packet packet(sock);
    std::string clientId;
    packet.read(clientId);
    ::printf("clientId: %s\n", clientId.c_str()); /* debug */
    std::string protocolName;
    packet.read(protocolName);
    ::printf("protocolName: %s\n", protocolName.c_str()); /* debug */
    packet::Version ver(sock);
    bool isVersionSame = ver.recv();
    ::printf("isVersionSame: %d\n", isVersionSame); /* debug */
    packet.write(serverId);

    Logger logger(serverId, clientId);
    Protocol *protocol = ProtocolFactory::getInstance().find(protocolName);

    packet::Answer ans(sock);
    if (!protocol) {
        std::string msg = cybozu::util::formatString(
            "There is not such protocol %s.", protocolName.c_str());
        logger.info(msg);
        ans.ng(1, msg);
        return;
    }
    if (!isVersionSame) {
        std::string msg = cybozu::util::formatString(
            "Version differ: server %" PRIu32 "", packet::VERSION);
        logger.info(msg);
        ans.ng(1, msg);
        return;
    }
    ans.ok();

    logger.info("initial negociation succeeded: %s", protocolName.c_str());
    try {
        protocol->runAsServer(sock, logger, { baseDirStr });
    } catch (std::exception &e) {
        logger.error("[%s] runProtocolAsServer failed: %s.", e.what());
    }
}

} //namespace walb