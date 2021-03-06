#pragma once
/**
 * @file
 * @brief Compressed data and workers for them
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <cstdio>
#include <cassert>
#include <memory>
#include "util.hpp"
#include "packet.hpp"
#include "thread_util.hpp"
#include "compressor.hpp"
#include "walb_types.hpp"

namespace walb {

namespace cmpr_local {

inline Compressor &getSnappyCompressor() {
    static Compressor cmpr(WALB_DIFF_CMPR_SNAPPY, 0);
    return cmpr;
}

inline Uncompressor &getSnappyUncompressor() {
    static Uncompressor uncmpr(WALB_DIFF_CMPR_SNAPPY);
    return uncmpr;
}

/**
 * RETURN:
 *   true when successfully compressed, false when copied.
 */
bool compressToVec(const void *data, size_t size, AlignedArray &outV);

/**
 * Assume uncompressed size must be outSize.
 */
void uncompressToVec(const void *data, size_t size, AlignedArray &outV, size_t outSize);

} // namespace cmpr_local

/**
 * Compressed and uncompressed data.
 * This uses snappy only.
 */
class CompressedData
{
private:
    uint32_t cmpSize_; /* compressed size [byte]. 0 means not compressed. */
    uint32_t orgSize_; /* original size [byte]. must not be 0. */
    AlignedArray data_;
public:
    const char *rawData() const { return &data_[0]; }
    size_t rawSize() const { return data_.size(); }
    bool isCompressed() const { return cmpSize_ != 0; }
    size_t originalSize() const { return orgSize_; }
    void swap(CompressedData& rhs) noexcept
    {
        std::swap(cmpSize_, rhs.cmpSize_);
        std::swap(orgSize_, rhs.orgSize_);
        data_.swap(rhs.data_);
    }
    /**
     * Send data to the remote host.
     */
    void send(packet::Packet &packet) const {
        verify();
        packet.write(cmpSize_);
        packet.write(orgSize_);
        packet.write(&data_[0], data_.size());
    }
    /**
     * Receive data from the remote host.
     */
    void recv(packet::Packet &packet) {
        packet.read(cmpSize_);
        packet.read(orgSize_);
        data_.resize(dataSize());
        packet.read(&data_[0], data_.size());
        verify();
    }
    void setUncompressed(AlignedArray &&data) {
        if (data.empty()) throw cybozu::Exception(__func__) << "empty";
        setSizes(0, data.size());
        data_ = std::move(data);
        verify();
    }
    void setUncompressed(const void *data, uint32_t size) {
        if (size == 0) throw cybozu::Exception(__func__) << "empty";
        setSizes(0, size);
        data_.resize(size);
        ::memcpy(&data_[0], data, size);
        verify();
    }
    void compressFrom(const void *data, uint32_t size) {
        if (cmpr_local::compressToVec(data, size, data_)) {
            setSizes(data_.size(), size);
        } else {
            setSizes(0, size);
        }
        verify();
    }
    void getUncompressed(AlignedArray &outV) const {
        if (isCompressed()) {
            cmpr_local::uncompressToVec(&data_[0], data_.size(), outV, orgSize_);
        } else {
            outV.resize(data_.size());
            ::memcpy(&outV[0], &data_[0], outV.size());
        }
    }
    void compress() {
        if (isCompressed()) return;
        CompressedData tmp;
        tmp.compressFrom(&data_[0], data_.size());
        swap(tmp);
    }
    void uncompress() {
        if (!isCompressed()) return;
        AlignedArray dst;
        getUncompressed(dst);
        setUncompressed(std::move(dst));
    }
    void moveTo(AlignedArray &outV) {
        outV = std::move(data_);
    }
private:
    void verify() const {
        if (orgSize_ == 0) throw RT_ERR("orgSize must not be 0.");
        if (dataSize() != data_.size()) {
            throw RT_ERR("data size must be %zu but really %zu."
                         , dataSize(), data_.size());
        }
    }
    void setSizes(uint32_t cmpSize, uint32_t orgSize) {
        cmpSize_ = cmpSize;
        orgSize_ = orgSize;
        if (dataSize() == 0) throw RT_ERR("dataSize() must not be 0.");
    }
    size_t dataSize() const {
        return cmpSize_ == 0 ? orgSize_ : cmpSize_;
    }
};

} //namespace walb
