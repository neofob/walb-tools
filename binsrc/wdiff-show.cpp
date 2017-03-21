/**
 * @file
 * @brief Show walb diff file.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include "util.hpp"
#include "walb_diff_file.hpp"
#include "walb_diff_stat.hpp"
#include "cybozu/option.hpp"
#include "walb_util.hpp"
#include "fileio.hpp"

using namespace walb;

struct Option
{
    bool isDebug, doSearch, doStat, noHead, noRec, isIndexed;
    uint64_t addr;
    std::string filePath;
    std::vector<std::string> filePathV;

    Option(int argc, char *argv[]) {
        cybozu::Option opt;

        std::string desc("wdiff-show: show the contents of wdiff files.\n");
        desc += "Records description:\n  ";
        desc += DiffRecord::getHeader();
        opt.setDescription(desc);

        opt.appendBoolOpt(&doSearch, "search", ": search a specific block.");
        opt.appendOpt(&addr, 0, "addr", ": search address [logical block].");
        opt.appendBoolOpt(&doStat, "stat", ": put statistics.");
        opt.appendBoolOpt(&noHead, "nohead", ": does not put header..");
        opt.appendBoolOpt(&noRec, "norec", "; does not put records.");
        opt.appendBoolOpt(&isDebug, "debug", ": put debug messages.");
        opt.appendBoolOpt(&isIndexed, "indexed", ": use indexed format instead of sorted format.");
        opt.appendParamVec(&filePathV, "WDIFF_PATH_LIST", ": wdiff file list (default: stdin)");
        opt.appendHelp("h", ": put this message.");

        if (!opt.parse(argc, argv)) {
            opt.usage();
            ::exit(1);
        }
    }
};

template <typename Record>
inline bool matchAddress(uint64_t addr, const Record& rec)
{
    return rec.io_address <= addr && addr < rec.endIoAddress();
}

void printWdiff(DiffReader &reader, DiffStatistics &stat, const Option &opt)
{
    DiffFileHeader wdiffH;
    reader.readHeader(wdiffH);
    if (!opt.noHead) wdiffH.print();

    DiffRecord rec;
    DiffIo io;
    while (reader.readDiff(rec, io)) {
        if (!opt.doSearch || matchAddress(opt.addr, rec)) {
            if (!opt.noRec) {
                if (!rec.isValid()) ::printf("Invalid record: ");
                rec.printOneline();
            }
            if (opt.doStat) stat.update(rec);
        }
    }
}


void printIndexedWdiff(IndexedDiffReader &reader, const Option &opt)
{
    DiffFileHeader wdiffH;
    wdiffH = reader.header();
    if (!opt.noHead) wdiffH.print();

    DiffIndexRecord rec;
    AlignedArray data;
    while (reader.readDiff(rec, data)) {
        if (!opt.doSearch || matchAddress(opt.addr, rec)) {
            if (!opt.noRec) {
                if (!rec.isValid()) ::printf("Invalid record: ");
                rec.printOneline();
            }
            //if (opt.doStat) stat.update(rec); // QQQ
        }
    }
}


void printIndexedWdiffs(const Option &opt)
{
    IndexedDiffReader reader;
    IndexedDiffCache cache;
    cache.setMaxSize(32 * MEBI);
    reader.setCache(&cache);

    if (opt.filePathV.empty()) {
        throw cybozu::Exception("Indexed format does not support stream.");
    }
    for (const std::string &path : opt.filePathV) {
        reader.setFile(cybozu::util::File(path, O_RDWR));
        printIndexedWdiff(reader, opt);
    }
}


int doMain(int argc, char *argv[])
{
    Option opt(argc, argv);
    util::setLogSetting("-", opt.isDebug);
    if (opt.isIndexed) {
        printIndexedWdiffs(opt);
        return 0;
    }

    DiffReader reader;
    DiffStatistics stat;
    if (opt.filePathV.empty()) {
        reader.setFd(0);
        printWdiff(reader, stat, opt);
    } else {
        for (const std::string &path : opt.filePathV) {
            reader.open(path);
            printWdiff(reader, stat, opt);
            reader.close();
        }
    }
    if (opt.doStat) stat.print(::stdout, "wdiff_stat: ");
    return 0;
}

DEFINE_ERROR_SAFE_MAIN("wdiff-show")
