#pragma once
/**
 * @file
 * @brief Walb log utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2012 Cybozu Labs, Inc.
 */
#include <cassert>
#include <memory>
#include <cstdlib>
#include <functional>

#include "util.hpp"
#include "checksum.hpp"
#include "fileio.hpp"
#include "walb/super.h"
#include "walb/log_device.h"
#include "walb/log_record.h"
#include "walb_log.h"

namespace walb {
namespace log {

/**
 * WalB super sector.
 */
class SuperBlock
{
private:
    /* Log device. */
    cybozu::util::BlockDevice& bd_;
    /* Physical block size */
    const unsigned int pbs_;
    /* Super block offset in the log device [physical block]. */
    const uint64_t offset_;

    /* Super block data. */
    struct FreeDeleter {
        void operator()(uint8_t *p) { ::free(p); }
    };
    std::unique_ptr<uint8_t, FreeDeleter> data_;

public:
    SuperBlock(cybozu::util::BlockDevice& bd)
        : bd_(bd)
        , pbs_(bd.getPhysicalBlockSize())
        , offset_(get1stSuperBlockOffsetStatic(pbs_))
        , data_(allocAlignedBufferStatic(pbs_)) {
#if 0
        ::printf("offset %" PRIu64 " pbs %u\n", offset_ * pbs_, pbs_);
#endif
        /* Read the superblock. */
        read();
#if 0
        print(); //debug
#endif
    }

    uint16_t getSectorType() const { return super()->sector_type; }
    uint16_t getVersion() const { return super()->version; }
    uint32_t getChecksum() const { return super()->checksum; }
    uint32_t getLogicalBlockSize() const { return super()->logical_bs; }
    uint32_t getPhysicalBlockSize() const { return super()->physical_bs; }
    uint32_t getMetadataSize() const { return super()->snapshot_metadata_size; }
    uint32_t getLogChecksumSalt() const { return super()->log_checksum_salt; }
    const uint8_t* getUuid() const { return super()->uuid; }
    const char* getName() const { return super()->name; }
    uint64_t getRingBufferSize() const { return super()->ring_buffer_size; }
    uint64_t getOldestLsid() const { return super()->oldest_lsid; }
    uint64_t getWrittenLsid() const { return super()->written_lsid; }
    uint64_t getDeviceSize() const { return super()->device_size; }

    void setOldestLsid(uint64_t oldestLsid) {
        super()->oldest_lsid = oldestLsid;
    }
    void setWrittenLsid(uint64_t writtenLsid) {
        super()->written_lsid = writtenLsid;
    }
    void setDeviceSize(uint64_t deviceSize) {
        super()->device_size = deviceSize;
    }
    void setLogChecksumSalt(uint32_t salt) {
        super()->log_checksum_salt = salt;
    }
    void setUuid(const uint8_t *uuid) {
        ::memcpy(super()->uuid, uuid, UUID_SIZE);
    }
    void updateChecksum() {
        super()->checksum = 0;
        super()->checksum = ::checksum(data_.get(), pbs_, 0);
    }

    /*
     * Offset and size.
     */

    uint64_t get1stSuperBlockOffset() const {
        return offset_;
    }

    uint64_t getMetadataOffset() const {
        return ::get_metadata_offset_2(super());
    }

    uint64_t get2ndSuperBlockOffset() const {
        UNUSED uint64_t oft = ::get_super_sector1_offset_2(super());
        assert(oft == getMetadataOffset() + getMetadataSize());
        return ::get_super_sector1_offset_2(super());
    }

    uint64_t getRingBufferOffset() const {
        uint64_t oft = ::get_ring_buffer_offset_2(super());
        assert(oft == get2ndSuperBlockOffset() + 1);
        return oft;
    }

    /**
     * Convert lsid to the position in the log device.
     *
     * @lsid target log sequence id.
     *
     * RETURN:
     *   Offset in the log device [physical block].
     */
    uint64_t getOffsetFromLsid(uint64_t lsid) const {
        if (lsid == INVALID_LSID) {
            throw RT_ERR("Invalid lsid.");
        }
        uint64_t s = getRingBufferSize();
        if (s == 0) {
            throw RT_ERR("Ring buffer size must not be 0.");
        }
        return (lsid % s) + getRingBufferOffset();
    }

    /**
     * Read super block from the log device.
     */
    void read() {
        bd_.read(offset_ * pbs_, pbs_, ptr<char>());
        if (!isValid()) {
            throw RT_ERR("super block is invalid.");
        }
    }

    /**
     * Write super block to the log device.
     */
    void write() {
        updateChecksum();
        if (!isValid()) {
            throw RT_ERR("super block is invalid.");
        }
        bd_.write(offset_ * pbs_, pbs_, ptr<char>());
    }

    void print(::FILE *fp = ::stdout) const {
        ::fprintf(fp,
                  "sectorType: %u\n"
                  "version: %u\n"
                  "checksum: %u\n"
                  "lbs: %u\n"
                  "pbs: %u\n"
                  "metadataSize: %u\n"
                  "logChecksumSalt: %u\n"
                  "name: %s\n"
                  "ringBufferSize: %" PRIu64 "\n"
                  "oldestLsid: %" PRIu64 "\n"
                  "writtenLsid: %" PRIu64 "\n"
                  "deviceSize: %" PRIu64 "\n"
                  "ringBufferOffset: %" PRIu64 "\n",
                  getSectorType(),
                  getVersion(),
                  getChecksum(),
                  getLogicalBlockSize(),
                  getPhysicalBlockSize(),
                  getMetadataSize(),
                  getLogChecksumSalt(),
                  getName(),
                  getRingBufferSize(),
                  getOldestLsid(),
                  getWrittenLsid(),
                  getDeviceSize(),
                  getRingBufferOffset());
        ::fprintf(fp, "uuid: ");
        for (int i = 0; i < UUID_SIZE; i++) {
            ::fprintf(fp, "%02x", getUuid()[i]);
        }
        ::fprintf(fp, "\n");
    }

private:
    static uint64_t get1stSuperBlockOffsetStatic(unsigned int pbs) {
        return ::get_super_sector0_offset(pbs);
    }

    static uint8_t* allocAlignedBufferStatic(unsigned int pbs) {
        uint8_t *p;
        int ret = ::posix_memalign((void **)&p, pbs, pbs);
        if (ret) {
            throw std::bad_alloc();
        }
        return p;
    }

    template <typename T>
    T *ptr() {
        return reinterpret_cast<T *>(data_.get());
    }

    template <typename T>
    const T *ptr() const {
        return reinterpret_cast<const T *>(data_.get());
    }

    struct walb_super_sector* super() {
        return ptr<struct walb_super_sector>();
    }

    const struct walb_super_sector* super() const {
        return ptr<const struct walb_super_sector>();
    }

    bool isValid(bool isChecksum = true) const {
        if (::is_valid_super_sector_raw(super(), pbs_) == 0) {
            return false;
        }
        if (isChecksum) {
            return true;
        } else {
            return ::checksum(data_.get(), pbs_, 0) == 0;
        }
    }
};

class ExceptionWithMessage
    : public std::exception
{
private:
    std::string msg_;
public:
    ExceptionWithMessage(const std::string &msg)
        : msg_(msg) {}

    virtual const char *what() const noexcept override {
        return msg_.c_str();
    }
};

class InvalidLogpackHeader
    : public ExceptionWithMessage
{
public:
    InvalidLogpackHeader()
        : ExceptionWithMessage("invalid logpack header.") {}
    InvalidLogpackHeader(const std::string &msg)
        : ExceptionWithMessage(msg) {}
    virtual ~InvalidLogpackHeader() noexcept {}
};

class InvalidLogpackData
    : public ExceptionWithMessage
{
public:
    InvalidLogpackData()
        : ExceptionWithMessage("invalid logpack data.") {}
    InvalidLogpackData(const std::string &msg)
        : ExceptionWithMessage(msg) {}
};

static inline void printLogRecord(
    ::FILE *fp, size_t idx, const struct walb_log_record &rec)
{
    ::fprintf(fp,
              "record %zu\n"
              "  checksum: %08x(%u)\n"
              "  lsid: %" PRIu64 "\n"
              "  lsid_local: %u\n"
              "  is_exist: %u\n"
              "  is_padding: %u\n"
              "  is_discard: %u\n"
              "  offset: %" PRIu64 "\n"
              "  io_size: %u\n"
              , idx
              , rec.checksum, rec.checksum
              , rec.lsid, rec.lsid_local
              , ::test_bit_u32(LOG_RECORD_EXIST, &rec.flags)
              , ::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)
              , ::test_bit_u32(LOG_RECORD_DISCARD, &rec.flags)
              , rec.offset, rec.io_size);
}

static inline void printLogRecordOneline(
    ::FILE *fp, size_t idx, const struct walb_log_record &rec)
{
    ::fprintf(fp,
              "wlog_rec %2zu:\t"
              "lsid %" PRIu64 " %u\tio %10" PRIu64 " %4u\t"
              "flags %u%u%u\tcsum %08x %u\n"
              , idx
              , rec.lsid, rec.lsid_local
              , rec.offset, rec.io_size
              , ::test_bit_u32(LOG_RECORD_EXIST, &rec.flags)
              , ::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)
              , ::test_bit_u32(LOG_RECORD_DISCARD, &rec.flags)
              , rec.checksum, rec.checksum);
}

/**
 * Logpack header.
 */
class PackHeaderRef
{
protected:
    uint8_t *data_;
    const unsigned int pbs_;
    const uint32_t salt_;

public:
    PackHeaderRef(uint8_t *data, unsigned int pbs, uint32_t salt)
        : data_(data)
        , pbs_(pbs)
        , salt_(salt) {
        ASSERT_PBS(pbs);
    }
    ~PackHeaderRef() noexcept = default;

    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }

    template <typename T>
    T* ptr() {
        return reinterpret_cast<T *>(data_);
    }

    template <typename T>
    const T* ptr() const {
        return reinterpret_cast<const T *>(data_);
    }

    struct walb_logpack_header& header() {
        checkBlock();
        return *ptr<struct walb_logpack_header>();
    }

    const struct walb_logpack_header& header() const {
        checkBlock();
        return* ptr<const struct walb_logpack_header>();
    }

    unsigned int pbs() const { return pbs_; }
    uint32_t salt() const { return salt_; }

    /*
     * Fields.
     */
    uint32_t checksum() const { return header().checksum; }
    uint16_t sectorType() const { return header().sector_type; }
    uint16_t totalIoSize() const { return header().total_io_size; }
    uint64_t logpackLsid() const { return header().logpack_lsid; }
    uint16_t nRecords() const { return header().n_records; }
    uint16_t nPadding() const { return header().n_padding; }

    /*
     * N'th log record.
     */
    struct walb_log_record& operator[](size_t pos) { return record(pos); }
    const struct walb_log_record& operator[](size_t pos) const { return record(pos); }
    struct walb_log_record& recordUnsafe(size_t pos) {
        return header().record[pos];
    }
    const struct walb_log_record& recordUnsafe(size_t pos) const {
        return header().record[pos];
    }
    struct walb_log_record& record(size_t pos) {
        checkIndexRange(pos);
        return recordUnsafe(pos);
    }
    const struct walb_log_record& record(size_t pos) const {
        checkIndexRange(pos);
        return recordUnsafe(pos);
    }

    bool isValid(bool isChecksum = true) const {
        if (isChecksum) {
            return ::is_valid_logpack_header_and_records_with_checksum(
                &header(), pbs(), salt()) != 0;
        } else {
            return ::is_valid_logpack_header_and_records(&header()) != 0;
        }
    }

    void printRecord(::FILE *fp, size_t pos) const {
        const struct walb_log_record &rec = record(pos);
        printLogRecord(fp, pos, rec);
    }

    void printRecordOneline(::FILE *fp, size_t pos) const {
        const struct walb_log_record &rec = record(pos);
        printLogRecordOneline(fp, pos, rec);
    }

    void printHeader(::FILE *fp = ::stdout) const {
        const struct walb_logpack_header &logh = header();
        ::fprintf(fp,
                  "*****logpack header*****\n"
                  "checksum: %08x(%u)\n"
                  "n_records: %u\n"
                  "n_padding: %u\n"
                  "total_io_size: %u\n"
                  "logpack_lsid: %" PRIu64 "\n",
                  logh.checksum, logh.checksum,
                  logh.n_records,
                  logh.n_padding,
                  logh.total_io_size,
                  logh.logpack_lsid);
    }

    void print(::FILE *fp = ::stdout) const {
        printHeader(fp);
        for (size_t i = 0; i < nRecords(); i++) {
            printRecord(fp, i);
        }
    }

    void printRecord(size_t pos) const { printRecord(::stdout, pos); }
    void printRecordOneline(size_t pos) const {
        printRecordOneline(::stdout, pos);
    }

    /**
     * Print each IO oneline.
     * logpack_lsid, mode(W, D, or P), offset[lb], io_size[lb].
     */
    void printShort(::FILE *fp) const {
        for (size_t i = 0; i < nRecords(); i++) {
            const struct walb_log_record &rec = record(i);
            assert(::test_bit_u32(LOG_RECORD_EXIST, &rec.flags));
            char mode = 'W';
            if (::test_bit_u32(LOG_RECORD_DISCARD, &rec.flags)) { mode = 'D'; }
            if (::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)) { mode = 'P'; }
            ::fprintf(fp,
                      "%" PRIu64 "\t%c\t%" PRIu64 "\t%u\n",
                      header().logpack_lsid,
                      mode, rec.offset, rec.io_size);
        }
    }

    void printShort() const { printShort(::stdout); }

    /**
     * Shrink.
     * Delete records from rec[invalidIdx] to the last.
     */
    void shrink(size_t invalidIdx) {
        assert(invalidIdx < nRecords());

        /* Invalidate records. */
        for (size_t i = invalidIdx; i < nRecords(); i++) {
            ::log_record_init(&record(i));
        }

        /* Set n_records and total_io_size. */
        header().n_records = invalidIdx;
        header().total_io_size = 0;
        header().n_padding = 0;
        for (size_t i = 0; i < nRecords(); i++) {
            struct walb_log_record &rec = record(i);
            if (!::test_bit_u32(LOG_RECORD_DISCARD, &rec.flags)) {
                header().total_io_size += ::capacity_pb(pbs(), rec.io_size);
            }
            if (::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)) {
                header().n_padding++;
            }
        }

        /* Calculate checksum. */
        header().checksum = 0;
        header().checksum = ::checksum(data_, pbs(), salt());

        assert(isValid());
    }

    /* Get next logpack lsid. */
    uint64_t nextLogpackLsid() const {
        if (nRecords() > 0) {
            return logpackLsid() + 1 + totalIoSize();
        } else {
            return logpackLsid();
        }
    }

    /* Update checksum field. */
    void updateChecksum() {
        header().checksum = 0;
        header().checksum = ::checksum(ptr<uint8_t>(), pbs(), salt());
    }

    /**
     * Write the logpack header block.
     */
    void write(int fd) {
        cybozu::util::FdWriter fdw(fd);
        write(fdw);
    }

    /**
     * Write the logpack header block.
     */
    void write(cybozu::util::FdWriter &fdw) {
        updateChecksum();
        if (!isValid(true)) {
            throw RT_ERR("logpack header invalid.");
        }
        fdw.write(ptr<char>(), pbs());
    }

    /**
     * Initialize logpack header block.
     */
    void init(uint64_t lsid) {
        ::memset(&header(), 0, pbs());
        header().logpack_lsid = lsid;
        header().sector_type = SECTOR_TYPE_LOGPACK;
        /*
          header().total_io_size = 0;
          header().n_records = 0;
          header().n_padding = 0;
          header().checksum = 0;
        */
    }

    /**
     * Make the header block terminator.
     * Do not forget to call updateChecksum() before write it.
     */
    void setEnd() {
        init(uint64_t(-1));
    }

    bool isEnd() const {
        return nRecords() == 0 && logpackLsid() == uint64_t(-1);
    }

    /**
     * Add a normal IO.
     *
     * @offset [logical block]
     * @size [logical block]
     *   can not be 0.
     * RETURN:
     *   true in success, or false (you must create a new header).
     */
    bool addNormalIo(uint64_t offset, uint16_t size) {
        if (::max_n_log_record_in_sector(pbs()) <= nRecords()) {
            return false;
        }
        if (MAX_TOTAL_IO_SIZE_IN_LOGPACK_HEADER <
            totalIoSize() + ::capacity_pb(pbs(), size)) {
            return false;
        }
        if (size == 0) {
            throw RT_ERR("Normal IO can not be zero-sized.");
        }
        size_t pos = nRecords();
        struct walb_log_record &rec = recordUnsafe(pos);
        rec.flags = 0;
        ::set_bit_u32(LOG_RECORD_EXIST, &rec.flags);
        rec.offset = offset;
        rec.io_size = size;
        rec.lsid_local = header().total_io_size + 1;
        rec.lsid = header().logpack_lsid + rec.lsid_local;
        rec.checksum = 0; /* You must set this lator. */

        header().n_records++;
        header().total_io_size += capacity_pb(pbs(), size);
        assert(::is_valid_logpack_header_and_records(&header()));
        return true;
    }

    /**
     * Add a discard IO.
     *
     * @offset [logical block]
     * @size [logical block]
     *   can not be 0.
     * RETURN:
     *   true in success, or false (you must create a new header).
     */
    bool addDiscardIo(uint64_t offset, uint16_t size) {
        if (::max_n_log_record_in_sector(pbs()) <= nRecords()) {
            return false;
        }
        if (size == 0) {
            throw RT_ERR("Discard IO can not be zero-sized.");
        }
        size_t pos = nRecords();
        struct walb_log_record &rec = recordUnsafe(pos);
        rec.flags = 0;
        ::set_bit_u32(LOG_RECORD_EXIST, &rec.flags);
        ::set_bit_u32(LOG_RECORD_DISCARD, &rec.flags);
        rec.offset = offset;
        rec.io_size = size;
        rec.lsid_local = header().total_io_size + 1;
        rec.lsid = header().logpack_lsid + rec.lsid_local;
        rec.checksum = 0; /* Not be used. */

        header().n_records++;
        /* You must not update total_io_size. */
        assert(::is_valid_logpack_header_and_records(&header()));
        return true;
    }

    /**
     * Add a padding.
     *
     * @size [logical block]
     *   can be 0.
     * RETURN:
     *   true in success, or false (you must create a new header).
     */
    bool addPadding(uint16_t size) {
        if (::max_n_log_record_in_sector(pbs()) <= nRecords()) {
            return false;
        }
        if (MAX_TOTAL_IO_SIZE_IN_LOGPACK_HEADER <
            totalIoSize() + ::capacity_pb(pbs(), size)) {
            return false;
        }
        if (0 < nPadding()) {
            return false;
        }
        if (size % ::n_lb_in_pb(pbs()) != 0) {
            throw RT_ERR("Padding size must be pbs-aligned.");
        }

        size_t pos = nRecords();
        struct walb_log_record &rec = recordUnsafe(pos);
        rec.flags = 0;
        ::set_bit_u32(LOG_RECORD_EXIST, &rec.flags);
        ::set_bit_u32(LOG_RECORD_PADDING, &rec.flags);
        rec.offset = 0; /* will not be used. */
        rec.io_size = size;
        rec.lsid_local = header().total_io_size + 1;
        rec.lsid = header().logpack_lsid + rec.lsid_local;
        rec.checksum = 0;  /* will not be used. */

        header().n_records++;
        header().total_io_size += ::capacity_pb(pbs(), size);
        header().n_padding++;
        assert(::is_valid_logpack_header_and_records(&header()));
        return true;
    }

    /**
     * Update all lsid entries in the logpack header.
     *
     * @newLsid new logpack lsid.
     *   If -1, nothing will be changed.
     *
     * RETURN:
     *   true in success.
     *   false if lsid overlap ocurred.
     */
    bool updateLsid(uint64_t newLsid) {
        assert(isValid(false));
        if (newLsid == uint64_t(-1)) {
            return true;
        }
        if (header().logpack_lsid == newLsid) {
            return true;
        }

        header().logpack_lsid = newLsid;
        for (size_t i = 0; i < header().n_records; i++) {
            struct walb_log_record &rec = record(i);
            rec.lsid = newLsid + rec.lsid_local;
        }
        return isValid(false);
    }

    uint64_t totalPaddingPb() const {
        if (nPadding() == 0) {
            return 0;
        }
        uint64_t t = 0;
        for (size_t i = 0; i < nRecords(); i++) {
            const struct walb_log_record &rec = record(i);
            if (::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)) {
                t += ::capacity_pb(pbs(), rec.io_size);
            }
        }
        return t;
    }

private:
    void checkBlock() const {
        if (data_ == nullptr) {
            throw RT_ERR("Header is null.");
        }
    }

    void checkIndexRange(size_t pos) const {
        if (pos >= nRecords()) {
            throw RT_ERR("index out of range.");
        }
    }

protected:
    void resetData(uint8_t *data) {
        data_ = data;
    }
};

class PackHeaderRaw : public PackHeaderRef
{
protected:
    using Block = std::shared_ptr<uint8_t>;
    Block block_;

public:
    PackHeaderRaw(Block block, unsigned int pbs, uint32_t salt)
        : PackHeaderRef(nullptr, pbs, salt)
        , block_(block) {
        resetData(block_.get());
    }
};

/**
 * Interface and default implementation.
 */
class Record
{
public:
    virtual ~Record() noexcept {}

    /* Interface to access a record */
    virtual size_t pos() const = 0;
    virtual unsigned int pbs() const = 0;
    virtual uint32_t salt() const = 0;
    virtual const struct walb_log_record &record() const = 0;
    virtual struct walb_log_record &record() = 0;

    /* default implementation. */
    uint64_t lsid() const { return record().lsid; }
    uint16_t lsidLocal() const { return record().lsid_local; }
    bool isExist() const {
        return ::test_bit_u32(LOG_RECORD_EXIST, &record().flags);
    }
    bool isPadding() const {
        return ::test_bit_u32(LOG_RECORD_PADDING, &record().flags);
    }
    bool isDiscard() const {
        return ::test_bit_u32(LOG_RECORD_DISCARD, &record().flags);
    }
    void setExist() {
        ::set_bit_u32(LOG_RECORD_EXIST, &record().flags);
    }
    void setPadding() {
        ::set_bit_u32(LOG_RECORD_PADDING, &record().flags);
    }
    void setDiscard() {
        ::set_bit_u32(LOG_RECORD_DISCARD, &record().flags);
    }
    void clearExist() {
        ::clear_bit_u32(LOG_RECORD_EXIST, &record().flags);
    }
    void clearPadding() {
        ::clear_bit_u32(LOG_RECORD_PADDING, &record().flags);
    }
    void clearDiscard() {
        ::clear_bit_u32(LOG_RECORD_DISCARD, &record().flags);
    }
    bool hasData() const {
        return isExist() && !isDiscard();
    }
    bool hasDataForChecksum() const {
        return isExist() && !isDiscard() && !isPadding();
    }
    unsigned int ioSizeLb() const { return record().io_size; }
    unsigned int ioSizePb() const { return ::capacity_pb(pbs(), ioSizeLb()); }
    uint64_t offset() const { return record().offset; }
    bool isValid() const { return ::is_valid_log_record_const(&record()); }

    virtual void print(::FILE *fp) const {
        printLogRecord(fp, pos(), record());
    }

    virtual void printOneline(::FILE *fp) const {
        printLogRecordOneline(fp, pos(), record());
    }

    virtual void print() const { print(::stdout); }
    virtual void printOneline() const { printOneline(::stdout); }
};

/**
 * Wrapper of a raw walb log record.
 */
class RecordRaw
    : public Record
{
private:
    const size_t pos_;
    const unsigned int pbs_;
    const uint32_t salt_;
    struct walb_log_record rec_;
public:
    RecordRaw(
        const struct walb_log_record &rec, size_t pos,
        unsigned int pbs, uint32_t salt)
        : pos_(pos), pbs_(pbs), salt_(salt), rec_() {
        ::memcpy(&rec_, &rec, sizeof(rec_));
    }
    RecordRaw(PackHeaderRef &logh, size_t pos)
        : RecordRaw(logh.record(pos), pos, logh.pbs(), logh.salt()) {}
    ~RecordRaw() noexcept override {}

    size_t pos() const override { return pos_; }
    unsigned int pbs() const override { return pbs_; }
    uint32_t salt() const override { return salt_; }
    const struct walb_log_record &record() const override { return rec_; }
    struct walb_log_record &record() override { return rec_; }
};

/**
 * Log data of an IO.
 * Log record is a reference.
 */
class RecordRef
    : public Record
{
private:
    PackHeaderRef& logh_;
    size_t pos_;

public:
    RecordRef(PackHeaderRef& logh, size_t pos)
        : logh_(logh)
        , pos_(pos) {
        assert(pos < logh.nRecords());
    }
    ~RecordRef() noexcept override {}
    DISABLE_COPY_AND_ASSIGN(RecordRef);
    DISABLE_MOVE(RecordRef);

    size_t pos() const override { return pos_; }
    unsigned int pbs() const override { return logh_.pbs(); }
    uint32_t salt() const override { return logh_.salt(); }
    struct walb_log_record& record() override { return logh_.record(pos_); }
    const struct walb_log_record& record() const override { return logh_.record(pos_); }
};

/**
 * Helper class to manage multiple IO blocks.
 */
class BlockData
{
private:
    using Block = std::shared_ptr<uint8_t>;

    const unsigned int pbs_; /* physical block size [byte]. */
    std::vector<Block> data_; /* Each block's size must be pbs_. */

public:
    BlockData(unsigned int pbs) : pbs_(pbs), data_() {}
    virtual ~BlockData() noexcept {}

    void addBlock(Block block) { data_.push_back(block); }
    Block getBlock(size_t idx) { return data_[idx]; }
    const Block getBlock(size_t idx) const { return data_[idx]; }

    template<typename T>
    T* rawData(size_t idx) {
        return reinterpret_cast<T *>(data_[idx].get());
    }
    template<typename T>
    const T* rawData(size_t idx) const {
        return reinterpret_cast<const T *>(data_[idx].get());
    }

    size_t nBlocks() const { return data_.size(); }

    uint32_t calcChecksum(size_t ioSizeLb, uint32_t salt) const {
        uint32_t csum = salt;
        size_t remaining = ioSizeLb * LOGICAL_BLOCK_SIZE;
        size_t i = 0;
        while (0 < remaining) {
            if (data_.size() <= i) {
                throw RT_ERR("Index out of range.");
            }
            if (pbs_ <= remaining) {
                csum = cybozu::util::checksumPartial(getBlock(i).get(), pbs_, csum);
                remaining -= pbs_;
            } else {
                csum = cybozu::util::checksumPartial(getBlock(i).get(), remaining, csum);
                remaining = 0;
            }
            i++;
        }
        return cybozu::util::checksumFinish(csum);
    }
};

/**
 * Logpack data.
 *
 * LogRecord: RecordRef or RecordRaw.
 */
template<class LogRecord>
class PackData
    : public LogRecord
    , public BlockData
{
public:
    PackData(PackHeaderRef &logh, size_t pos)
        : LogRecord(logh, pos)
        , BlockData(logh.pbs()) {}
    virtual ~PackData() noexcept {}
    DISABLE_COPY_AND_ASSIGN(PackData);
    DISABLE_MOVE(PackData);

    bool isValid(bool isChecksum = true) const {
        if (!LogRecord::isValid()) { return false; }
        if (isChecksum && this->hasDataForChecksum() &&
            this->calcIoChecksum() != this->record().checksum) {
            return false;
        }
        return true;
    }

    void print(::FILE *fp = ::stdout) const override {
        LogRecord::print(fp);
        if (this->hasDataForChecksum() && this->ioSizePb() == this->nBlocks()) {
            ::fprintf(fp, "record_checksum: %08x\n"
                      "calculated_checksum: %08x\n",
                      this->record().checksum, this->calcIoChecksum());
            for (size_t i = 0; i < this->ioSizePb(); i++) {
                ::fprintf(fp, "----------block %zu----------\n", i);
                cybozu::util::printByteArray(fp, this->getBlock(i).get(), this->pbs());
            }
        }
    }

    bool setChecksum() {
        if (!this->hasDataForChecksum()) { return false; }
        if (this->ioSizePb() != this->nBlocks()) { return false; }
        this->record().checksum = this->calcIoChecksum();
        return true;
    }

    uint32_t calcIoChecksum() const {
        return this->calcIoChecksum(this->salt());
    }

    uint32_t calcIoChecksum(uint32_t salt) const {
        assert(this->hasDataForChecksum());
        assert(0 < this->ioSizeLb());
        if (this->nBlocks() < this->ioSizePb()) {
            throw RT_ERR("There is not sufficient data block.");
        }
        return calcChecksum(this->ioSizeLb(), salt);
    }
};

using PackDataRef = PackData<RecordRef>;
using PackDataRaw = PackData<RecordRaw>;

/**
 * Walb logfile header.
 */
class FileHeader
{
private:
    std::vector<uint8_t> data_;

public:
    FileHeader()
        : data_(WALBLOG_HEADER_SIZE, 0) {}

    void init(unsigned int pbs, uint32_t salt, const uint8_t *uuid, uint64_t beginLsid, uint64_t endLsid) {
        ::memset(&data_[0], 0, WALBLOG_HEADER_SIZE);
        header().sector_type = SECTOR_TYPE_WALBLOG_HEADER;
        header().version = WALB_LOG_VERSION;
        header().header_size = WALBLOG_HEADER_SIZE;
        header().log_checksum_salt = salt;
        header().logical_bs = LOGICAL_BLOCK_SIZE;
        header().physical_bs = pbs;
        ::memcpy(header().uuid, uuid, UUID_SIZE);
        header().begin_lsid = beginLsid;
        header().end_lsid = endLsid;
    }

    void read(int fd) {
        cybozu::util::FdReader fdr(fd);
        read(fdr);
    }

    void read(cybozu::util::FdReader& fdr) {
        fdr.read(ptr<char>(), WALBLOG_HEADER_SIZE);
    }

    void write(int fd) {
        cybozu::util::FdWriter fdw(fd);
        write(fdw);
    }

    void write(cybozu::util::FdWriter& fdw) {
        updateChecksum();
        fdw.write(ptr<char>(), WALBLOG_HEADER_SIZE);
    }

    void updateChecksum() {
        header().checksum = 0;
        header().checksum = ::checksum(&data_[0], WALBLOG_HEADER_SIZE, 0);
    }

    struct walblog_header& header() {
        return *ptr<struct walblog_header>();
    }

    const struct walblog_header& header() const {
        return *ptr<struct walblog_header>();
    }

    uint32_t checksum() const { return header().checksum; }
    uint32_t salt() const { return header().log_checksum_salt; }
    unsigned int lbs() const { return header().logical_bs; }
    unsigned int pbs() const { return header().physical_bs; }
    uint64_t beginLsid() const { return header().begin_lsid; }
    uint64_t endLsid() const { return header().end_lsid; }
    const uint8_t* uuid() const { return &header().uuid[0]; }
    uint16_t sectorType() const { return header().sector_type; }
    uint16_t headerSize() const { return header().header_size; }
    uint16_t version() const { return header().version; }

    bool isValid(bool isChecksum = true) const {
        CHECKd(header().sector_type == SECTOR_TYPE_WALBLOG_HEADER);
        CHECKd(header().version == WALB_LOG_VERSION);
        CHECKd(header().begin_lsid < header().end_lsid);
        if (isChecksum) {
            CHECKd(::checksum(&data_[0], WALBLOG_HEADER_SIZE, 0) == 0);
        }
        return true;
      error:
        return false;
    }

    void print(FILE *fp = ::stdout) const {
        ::fprintf(
            fp,
            "sector_type %d\n"
            "version %u\n"
            "header_size %u\n"
            "log_checksum_salt %" PRIu32 " (%08x)\n"
            "logical_bs %u\n"
            "physical_bs %u\n"
            "uuid ",
            header().sector_type,
            header().version,
            header().header_size,
            header().log_checksum_salt,
            header().log_checksum_salt,
            header().logical_bs,
            header().physical_bs);
        for (size_t i = 0; i < UUID_SIZE; i++) {
            ::fprintf(fp, "%02x", header().uuid[i]);
        }
        ::fprintf(
            fp,
            "\n"
            "begin_lsid %" PRIu64 "\n"
            "end_lsid %" PRIu64 "\n",
            header().begin_lsid,
            header().end_lsid);
    }

private:
    template <typename T>
    T *ptr() { return reinterpret_cast<T *>(&data_[0]); }

    template <typename T>
    const T *ptr() const { return reinterpret_cast<const T *>(&data_[0]); }
};

}} //namespace walb::log
