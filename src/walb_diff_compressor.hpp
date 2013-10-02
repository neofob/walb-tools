#pragma once
/**
 * @file
 * @brief parallel compressor/uncompressor class
 * @author MITSUNARI Shigeo
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <memory>
#include <vector>
#include <atomic>
#include <queue>
#include <mutex>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <cybozu/thread.hpp>
#include <cybozu/event.hpp>
#include "walb_diff.h"
#include "compressor.hpp"
#include "checksum.hpp"
#include "stdout_logger.hpp"

namespace walb {

inline Compressor::Mode convertCompressionType(int type)
{
    switch (type) {
    case WALB_DIFF_CMPR_NONE: return walb::Compressor::AsIs;
    case WALB_DIFF_CMPR_GZIP: return walb::Compressor::Zlib;
    case WALB_DIFF_CMPR_SNAPPY: return walb::Compressor::Snappy;
    case WALB_DIFF_CMPR_LZMA: return walb::Compressor::Xz;
    default: throw cybozu::Exception("walb:Compressor:convertCompressionType") << type;
    }
}

namespace compressor {

/*
 * convert pack data
 * @param conv [in] PackCompressor or PackUncompressor
 * @param inPackTop [in] top address of pack data
 * @param maxOutSize max output size excluding pack header block.
 * @return buffer of converted pack data
 */
template<class Convertor>
std::unique_ptr<char[]> convert(Convertor& conv, const char *inPackTop, size_t maxOutSize)
{
    const walb_diff_pack& inPack = *(const walb_diff_pack*)inPackTop;
    std::unique_ptr<char[]> ret(new char [WALB_DIFF_PACK_SIZE + maxOutSize]);
    walb_diff_pack& outPack = *(walb_diff_pack*)ret.get();
    const char *in = inPackTop + WALB_DIFF_PACK_SIZE;
    char *out = ret.get() + WALB_DIFF_PACK_SIZE;

    memset(ret.get(), 0, WALB_DIFF_PACK_SIZE);
    uint32_t outOffset = 0;
    for (int i = 0, n = inPack.n_records; i < n; i++) {
        const walb_diff_record& inRecord = inPack.record[i];
        walb_diff_record& outRecord = outPack.record[i];

        assert(inRecord.flags & WALB_DIFF_FLAG(EXIST));
        if (inRecord.flags & (WALB_DIFF_FLAG(ALLZERO) | WALB_DIFF_FLAG(DISCARD))) {
            outRecord = inRecord;
        } else {
            conv.convertRecord(out, maxOutSize - outOffset, outRecord, in, inRecord);
        }
        outRecord.data_offset = outOffset;
        outOffset += outRecord.data_size;
        assert(outOffset <= maxOutSize);
        out += outRecord.data_size;
        in += inRecord.data_size;
    }
    outPack.n_records = inPack.n_records;
    outPack.total_size = outOffset;
    outPack.checksum = 0; // necessary to the following calcChecksum
    outPack.checksum = cybozu::util::calcChecksum(&outPack, WALB_DIFF_PACK_SIZE, 0);
    return ret;
}

inline uint32_t calcTotalBlockNum(const walb_diff_pack& pack)
{
    uint32_t num = 0;
    for (int i = 0; i < pack.n_records; i++) {
        num += pack.record[i].io_blocks;
    }
    return num;
}

struct PackCompressorBase {
    virtual ~PackCompressorBase() {}
    virtual void convertRecord(char *out, size_t maxOutSize, walb_diff_record& outRecord, const char *in, const walb_diff_record& inRecord) = 0;
    virtual std::unique_ptr<char[]> convert(const char *inPackTop) = 0;
};

} // compressor

class PackCompressor : public compressor::PackCompressorBase {
    int type_;
    walb::Compressor c_;
public:
    PackCompressor(int type, size_t compressionLevel = 0)
        : type_(type), c_(convertCompressionType(type), compressionLevel)
    {
    }
    void convertRecord(char *out, size_t maxOutSize, walb_diff_record& outRecord, const char *in, const walb_diff_record& inRecord)
    {
        outRecord = inRecord;
        const size_t inSize = inRecord.data_size;
        size_t encSize = inSize;
        try {
            encSize = c_.run(out, maxOutSize, in, inSize);
        } catch (std::bad_alloc&) {
            throw;
        } catch (std::exception& e) {
            LOGd("encode error %s\n", e.what());
            // through
        }
        if (encSize < inSize) {
            outRecord.compression_type = type_;
            outRecord.data_size = encSize;
        } else {
            // not compress
            outRecord.compression_type = WALB_DIFF_CMPR_NONE;
            ::memcpy(out, in, inSize);
        }
        outRecord.checksum = cybozu::util::calcChecksum(out, outRecord.data_size, 0);
    }
    /*
     * compress pack data
     * @param inPackTop [in] top address of pack data
     * @return buffer of compressed pack data
     */
    std::unique_ptr<char[]> convert(const char *inPackTop)
    {
        const walb_diff_pack& inPack = *(const walb_diff_pack*)inPackTop;
        return compressor::convert(*this, inPackTop, inPack.total_size);
    }
};

class PackUncompressor : public compressor::PackCompressorBase {
    int type_;
    walb::Uncompressor d_;
public:
    PackUncompressor(int type, size_t para = 0)
        : type_(type), d_(convertCompressionType(type), para)
    {
    }
    void convertRecord(char *out, size_t maxOutSize, walb_diff_record& outRecord, const char *in, const walb_diff_record& inRecord)
    {
        outRecord = inRecord;
        const size_t inSize = inRecord.data_size;
        if (inRecord.compression_type == WALB_DIFF_CMPR_NONE) {
            if (inSize > maxOutSize) throw cybozu::Exception("PackUncompressor:convertRecord:small maxOutSize") << inSize << maxOutSize;
            ::memcpy(out, in, inSize);
            return;
        } else if (inRecord.compression_type != type_) {
            throw cybozu::Exception("PackUncompressor:convertRecord:type mismatch") << inRecord.compression_type << type_;
        }
        size_t decSize = d_.run(out, maxOutSize, in, inSize);
        outRecord.compression_type = WALB_DIFF_CMPR_NONE;
        outRecord.data_size = decSize;
        assert(decSize == outRecord.io_blocks * 512);
        outRecord.checksum = cybozu::util::calcChecksum(out, outRecord.data_size, 0);
    }
    /*
     * uncompress pack data
     * @param inPackTop [in] top address of pack data
     * @return buffer of compressed pack data
     */
    std::unique_ptr<char[]> convert(const char *inPackTop)
    {
        const walb_diff_pack& inPack = *(const walb_diff_pack*)inPackTop;
        const size_t uncompressedSize = compressor::calcTotalBlockNum(inPack) * 512;
        return compressor::convert(*this, inPackTop, uncompressedSize);
    }
};

namespace compressor_local {

typedef std::unique_ptr<char[]> Buffer;
typedef std::pair<Buffer, std::exception_ptr> MaybeBuffer;

class Queue {
    size_t maxQueSize_;
    std::queue<MaybeBuffer> q_;
    mutable std::mutex m_;
    std::condition_variable avail_;
    std::condition_variable notFull_;
public:
    explicit Queue(size_t maxQueSize) : maxQueSize_(maxQueSize) {}
    /*
        allocate reserved buffer where will be stored laster and return it
        @note lock if queue is fill
    */
    MaybeBuffer *push()
    {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [this] { return q_.size() < maxQueSize_; });
        q_.push(MaybeBuffer());
        return &q_.back();
    }
    /*
        notify to pop() that buffer is released
    */
    void notify()
    {
        avail_.notify_one();
    }
    Buffer pop()
    {
        MaybeBuffer ret;
        {
            std::unique_lock<std::mutex> lk(m_);
            avail_.wait(lk, [this] { return this->q_.empty() || (this->q_.front().first || this->q_.front().second); });
            if (q_.empty()) return nullptr;
            ret = std::move(q_.front());
            q_.pop();
        }
        notFull_.notify_one();
        if (ret.second) std::rethrow_exception(ret.second);
        return std::move(ret.first);
    }
    bool empty() const
    {
        std::lock_guard<std::mutex> lk(m_);
        return q_.empty();
    }
};

template<class Conv, class UnConv>
struct EngineT : cybozu::ThreadBase {
private:
    compressor::PackCompressorBase *e_;
    std::atomic<bool> using_;
    Queue *que_;
    cybozu::Event startEv_;
    Buffer inBuf_;
    MaybeBuffer *outBuf_;
    EngineT(const EngineT&) = delete;
    void operator=(const EngineT&) = delete;
public:
    EngineT()
        : e_(nullptr)
        , using_(false)
        , que_(nullptr)
        , outBuf_(nullptr)
    {
    }
    ~EngineT()
    {
        delete e_;
    }
    void init(bool doCompress, int type, size_t para, Queue *que)
    {
        assert(e_ == nullptr);
        if (doCompress) {
            e_ = new Conv(type, para);
        } else {
            e_ = new UnConv(type, para);
        }
        que_ = que;
        beginThread();
    }
    void threadEntry()
        try
    {
        assert(e_);
        for (;;) {
            startEv_.wait();
            /*
             * case 1 (process task): set inBuf_ and outBuf_ and wakeup().
             * case 2 (quit): just call wakeup() wihtout setting inBuf_ and outBuf_.
             */
            if (!inBuf_) {
                que_->notify();
                break;
            }
            assert(outBuf_);
            try {
                outBuf_->first = e_->convert(inBuf_.get());
            } catch (...) {
                outBuf_->second = std::current_exception();
            }
            inBuf_ = nullptr;
            outBuf_ = nullptr;
            using_ = false;
            que_->notify();
        }
    } catch (std::exception& e) {
        printf("compress_local::Engine::threadEntry %s\n", e.what());
    }
    void wakeup()
    {
        startEv_.wakeup();
    }
    bool tryToRun(MaybeBuffer *outBuf, Buffer& inBuf)
    {
        if (using_.exchange(true)) return false;
        inBuf_ = std::move(inBuf);
        outBuf_ = outBuf;
        startEv_.wakeup();
        return true;
    }
    bool isUsing() const { return using_; }
};

template<class Conv = PackCompressor, class UnConv = PackUncompressor>
class ConverterQueueT {
    typedef EngineT<Conv, UnConv> Engine;
    std::atomic<bool> quit_;
    std::atomic<bool> joined_;
    Queue que_;
    std::vector<Engine> enginePool_;
    void runEngine(MaybeBuffer *outBuf, Buffer& inBuf)
    {
        for (;;) {
            for (Engine& e : enginePool_) {
                if (e.tryToRun(outBuf, inBuf)) return;
            }
            Sleep1msec();
        }
    }
    bool isFreeEngine() const
    {
        for (const Engine& e : enginePool_) {
            if (e.isUsing()) return false;
        }
        return true;
    }
    void Sleep1msec() const
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
public:
    ConverterQueueT(size_t maxQueueNum, size_t threadNum, bool doCompress, int type, size_t para = 0)
        : quit_(false)
        , joined_(false)
        , que_(maxQueueNum)
        , enginePool_(threadNum)
    {
        for (Engine& e : enginePool_) {
            e.init(doCompress, type, para, &que_);
        }
    }
    ~ConverterQueueT()
        try
    {
        join();
    } catch (std::exception& e) {
        printf("ConverterQueue:dstr:%s\n", e.what());
        exit(1);
    } catch (...) {
        printf("ConverterQueue:dstr:unknown exception\n");
        exit(1);
    }
    /*
     * join all thread
     */
    void join()
    {
        if (joined_.exchange(true)) return;
        quit();
        while (!isFreeEngine()) {
            Sleep1msec();
        }
        while (!que_.empty()) {
            Sleep1msec();
        }
        for (Engine& e : enginePool_) {
            e.wakeup();
            e.joinThread();
        }
    }
    /*
     * start to quit all thread
     */
    void quit() { quit_ = true; }
    /*
     * push buffer and return true
     * return false if quit_
     */
    bool push(Buffer& inBuf)
    {
        if (quit_) return false;
        MaybeBuffer *outBuf = que_.push();
        runEngine(outBuf, inBuf); // no throw
        return true;
    }
    /*
     * return nullptr if quit_ and data is empty
     * otherwise return buffer after blocking until data comes
     */
    Buffer pop()
    {
        if (quit_ && que_.empty()) return nullptr;
        return que_.pop();
    }
};

} // compressor_local

typedef compressor_local::ConverterQueueT<> ConverterQueue;

} //namespace walb

