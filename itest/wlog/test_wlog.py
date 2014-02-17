import sys, time, shutil
sys.path.append("../")
from run import *

BIN="../../binsrc"

# This script will test walb-log related functionalities.
#
# If you use loopback devices, set USE_LOOP_DEV=1.
# You must have privilege of 'disk' group to use losetup commands.
# /dev/loop0 and /dev/loop1 will be used.
#
# If you set USE_LOOP_DEV=0,
# LOOP0 and LOOP1 must be ordinal block devices.
#

LDEV="ldev32M"
DDEV="ddev32M"
DDEV_0 = DDEV + ".0"
DDEV_0z= DDEV + ".0z"
DDEV_1 = DDEV + ".1"
DDEV_1z= DDEV + ".1z"
DDEV_2 = DDEV + ".2"
DDEV_3 = DDEV + ".3"

WLOG="wlog"
WLOG_0=WLOG + ".0"
WLOG_1=WLOG + ".1"
WLOG_2=WLOG + ".2"

CTL="../../walb/tool/walbctl"
BIN="../../binsrc"
TMP_FILE = "tmp.txt"

LOOP0="/dev/loop0"
LOOP1="/dev/loop1"
#LOOP0=/dev/data/test-log
#LOOP1=/dev/data/test-data
USE_LOOP_DEV=1

def getKeyValue(fileName, keyword, pos):
	f = open(fileName, "r")
	for line in f:
		line = line.strip()
		if line.find(keyword) >= 0:
			sp = line.split(' ')
#			print "find %s at %s in %s : %s" % (keyword, line, fileName, sp)
			return sp[pos]
	raise RuntimeError("getKeyValue", (fileName, keyword, pos))

def prepare_bdev(devPath, devFile):
	# ex. /dev/loop0
	# ex. ldev32M.0
	if USE_LOOP_DEV == 1:
		run("losetup %s %s" % (devPath, devFile))
	else:
		run("dd oflag=direct if=%s of=%s bs=1M" % (devFile, devPath))

def finalize_bdev(devPath, devFile):
	if USE_LOOP_DEV == 1:
		run("losetup -d %s" % devPath)
	else:
		run("dd oflag=direct if=%s of=%s bs=1M" % (devPath, devFile))

def format_ldev():
	run("dd if=/dev/zero of=%s bs=1M count=32" % LDEV)
	run("dd if=/dev/zero of=%s bs=1M count=32" % DDEV_0)
	prepare_bdev(LOOP0, LDEV)
	prepare_bdev(LOOP1,  DDEV_0)
	run("%s format_ldev --ldev %s --ddev %s" % (CTL, LOOP0, LOOP1))
	#RING_BUFFER_SIZE=$(${BIN}/wldev-info $LOOP0 |grep ringBufferSize |awk '{print $2}')
	run("%s/wldev-info %s > %s" % (BIN, LOOP0, TMP_FILE))
	v = getKeyValue(TMP_FILE, "ringBufferSize", 1)
	global RING_BUFFER_SIZE
	RING_BUFFER_SIZE = int(v)
	print "RING_BUFFER_SIZE=", RING_BUFFER_SIZE
	time.sleep(1)
	finalize_bdev(LOOP0, LDEV)
	finalize_bdev(LOOP1, DDEV_0)

def echo_wlog_value(wlogFile, keyword):
	# $CTL show_wlog < $wlogFile |grep $keyword |awk '{print $2}' 
#	run($CTL + " show_wlog < " + wlogFile + " > " + TMP_FILE)
	run("%s show_wlog < %s > %s" % (CTL, wlogFile, TMP_FILE))
	v = getKeyValue(TMP_FILE, keyword, 1)
	return int(v)

#
# Initialization.
#
def Initialization():
	global endLsid0
	global nPacks0
	global totalPadding0

	format_ldev()
	run("%s/wlog-gen -s 32M -z 16M --maxPackSize 4M -o %s" % (BIN, WLOG_0))
	#${BIN}/wlog-gen -s 32M -z 16M --minIoSize 512 --maxIoSize 512 --maxPackSize 1M -o WLOG_0
	endLsid0 = echo_wlog_value(WLOG_0, "end_lsid_really:")
	nPacks0 = echo_wlog_value(WLOG_0, "n_packs:")
	totalPadding0 = echo_wlog_value(WLOG_0, "total_padding_size:")
	shutil.copyfile(DDEV_0, DDEV_0z)
	run("%s/wlog-redo %s < %s" % (BIN, DDEV_0, WLOG_0))
	run("%s/wlog-redo %s -z < %s" % (BIN, DDEV_0z, WLOG_0))

#
# Simple test.
#
def SimpleTest():
	run("%s/wlog-restore --verify %s < %s" % (BIN, LDEV, WLOG_0))
	run("%s/wlog-cat %s -v -o %s" % (BIN, LDEV, WLOG_1))
	run("%s/bdiff -b 512 %s %s" % (BIN, WLOG_0, WLOG_1))

def restore_test(testId, lsidDiff, invalidLsid):
	run("dd if=/dev/zero of=%s bs=1M count=32" % DDEV_1)
	run("dd if=/dev/zero of=%s bs=1M count=32" % DDEV_1z)
	run("dd if=/dev/zero of=%s bs=1M count=32" % DDEV_2)
	run("dd if=/dev/zero of=%s bs=1M count=32" % DDEV_3)
	run("%s/wlog-restore %s --verify -d %d -i %d < %s" % (BIN, LDEV, lsidDiff, invalidLsid, WLOG_0))
	run("%s/wlog-cat %s -v -o %s" % (BIN, LDEV, WLOG_1))
	prepare_bdev(LOOP0, LDEV)
	run("%s cat_wldev --wldev %s > %s" % (CTL, LOOP0, WLOG_2))
	time.sleep(1)
	finalize_bdev(LOOP0, LDEV)
	if invalidLsid == 0xffffffffffffffff:
		endLsid0a = endLsid0 + lsidDiff - nPacks0 - totalPadding0
		endLsid1 = echo_wlog_value(WLOG_1, "end_lsid_really:")
		endLsid2 = echo_wlog_value(WLOG_2, "end_lsid_really:")
		nPacks1 = echo_wlog_value(WLOG_1, "n_packs:")
		nPacks2 = echo_wlog_value(WLOG_2, "n_packs:")
		totalPadding1 = echo_wlog_value(WLOG_1, "total_padding_size:")
		totalPadding2 = echo_wlog_value(WLOG_2, "total_padding_size:")
		endLsid1a = endLsid1 - nPacks1 - totalPadding1
		endLsid2a = endLsid2 - nPacks2 - totalPadding2
		if endLsid0a != endLsid1a:
			print "endLsid0a", endLsid0a, "does not equal to endLsid1a", endLsid1a
			print "TEST" + str(testId) + "_FAILURE"
			exit(1)
		if endLsid0a != endLsid2a:
			print "endLsid0a",  endLsid0a, " does not equal to endLsid2a",  endLsid2a
			print "TEST" + str(testId) + "_FAILURE"
			exit(1)
	run("%s/bdiff -b 512 %s %s" % (BIN, WLOG_1, WLOG_2))

	run("%s/wlog-redo %s < %s" % (BIN, DDEV_1, WLOG_1))
	run("%s/wlog-redo %s -z < %s" % (BIN, DDEV_1z, WLOG_1))
	prepare_bdev(LOOP1, DDEV_2)
	run("%s redo_wlog --ddev %s < %s" % (CTL, LOOP1, WLOG_1))
	time.sleep(1)
	finalize_bdev(LOOP1, DDEV_2)
	time.sleep(1)
	prepare_bdev(LOOP0, LDEV)
	prepare_bdev(LOOP1, DDEV_3)
	run("%s redo --ldev %s --ddev %s" % (CTL, LOOP0, LOOP1))
	time.sleep(1)
	finalize_bdev(LOOP0, LDEV)
	finalize_bdev(LOOP1, DDEV_3)
	if invalidLsid == 0xffffffffffffffff:
		run("%s/bdiff -b 512 %s %s" % (BIN, DDEV_0, DDEV_1))
		run("%s/bdiff -b 512 %s %s" % (BIN, DDEV_0, DDEV_2))
		run("%s/bdiff -b 512 %s %s" % (BIN, DDEV_0, DDEV_3))
		run("%s/bdiff -b 512 %s %s" % (BIN, DDEV_0z, DDEV_1z))
	else:
		run("%s/bdiff -b 512 %s %s" % (BIN, DDEV_1, DDEV_2))
		run("%s/bdiff -b 512 %s %s" % (BIN, DDEV_1, DDEV_3))

def main():
	Initialization()
	SimpleTest()
	restore_test(3, RING_BUFFER_SIZE - 1, 0xffffffffffffffff)
	restore_test(4, RING_BUFFER_SIZE - 2, 0xffffffffffffffff)
	restore_test(5, RING_BUFFER_SIZE - 1024, 0xffffffffffffffff)
	restore_test(6, 0, 1024) #512KB
	restore_test(7, 0, 8192) #4MB
	print "TEST_SUCCESS"

if __name__ == '__main__':
    main()
