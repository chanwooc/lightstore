#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <monkit.h>
#include <semaphore.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <time.h>

// Connectal DMA interface
#include "dmaManager.h"

// Connectal HW-SW interface
#include "FlashIndication.h"
#include "FlashRequest.h"

// Test Definitions
 #define TEST_ERASE_ALL		 // eraseAll.exe's only test
// #define TEST_MINI_FUNCTION
 #define TEST_READ_SPEED
// #define TEST_HEAVY_READ
// #define TEST_WRITE_SPEED
// #define KT_WRITE
// #define KT_READ
// #define KT_MERGE
// #define KT_READ_MERGE
// #define KT_READ_ACCEL

#define DEFAULT_VERBOSE_REQ  false
#define DEFAULT_VERBOSE_RESP false

// Device Value (For test specific values, go to main)
// 256 pages/blk	=> pages 0, 1, 2 .. append only write
// 4096 blks/chip	=> erasure on blocks
// 8 chips/bus
// 8 buses
#define PAGES_PER_BLOCK 256
#define BLOCKS_PER_CHIP 4096
#define CHIPS_PER_BUS 8
#define NUM_BUSES 8

// Page Size (Physical chip support up to 8224 bytes, but using 8192 bytes for now)
//#define FPAGE_SIZE (8192*2)
//#define FPAGE_SIZE_VALID (8224)
#define FPAGE_SIZE (8192)
#define FPAGE_SIZE_VALID (8192)
#define NUM_TAGS 128

typedef enum {
	UNINIT,
	ERASED,
	WRITTEN
} FlashStatusT;

typedef struct {
	bool checkRead;
	bool busy;
	int bus;
	int chip;
	int block;
	int page;
} TagTableEntry;

typedef struct {
	bool busy;
	int ppa;
} TagKeyTableEntry;

FlashRequestProxy *device;

pthread_mutex_t flashReqMutex;

//8k * 128
size_t dstAlloc_sz = FPAGE_SIZE * NUM_TAGS * sizeof(unsigned char) * 100; //100MB
size_t srcAlloc_sz = FPAGE_SIZE * NUM_TAGS * sizeof(unsigned char) * 100; //100MB
int dstAlloc;
int srcAlloc;
unsigned int ref_dstAlloc;
unsigned int ref_srcAlloc;
unsigned int* dstBuffer;
unsigned int* srcBuffer;
unsigned int* readBuffers[NUM_TAGS];
unsigned int* writeBuffers[NUM_TAGS];
TagTableEntry readTagTable[NUM_TAGS];
TagTableEntry writeTagTable[NUM_TAGS];
TagTableEntry eraseTagTable[NUM_TAGS];
TagKeyTableEntry keyTagTable[NUM_TAGS];
FlashStatusT flashStatus[NUM_BUSES][CHIPS_PER_BUS][BLOCKS_PER_CHIP];

bool testPassed = false;
bool verbose_req  = DEFAULT_VERBOSE_REQ;
bool verbose_resp = DEFAULT_VERBOSE_RESP;

int curReadsInFlight = 0;
int curWritesInFlight = 0;
int curErasesInFlight = 0;
int curSearchInFlight = 0;


double timespec_diff_sec( timespec start, timespec end ) {
	double t = end.tv_sec - start.tv_sec;
	t += ((double)(end.tv_nsec - start.tv_nsec)/1000000000L);
	return t;
}

unsigned int hashAddrToData(int bus, int chip, int blk, int word) {
	return ((bus<<24) + (chip<<20) + (blk<<16) + word);
}

bool checkReadData(int tag) {
	bool pass = true;
	TagTableEntry e = readTagTable[tag];

	unsigned int goldenData;
	if (flashStatus[e.bus][e.chip][e.block]==WRITTEN) {
		int numErrors = 0;
		for (unsigned int word=0; word<FPAGE_SIZE_VALID/sizeof(unsigned int); word++) {
			goldenData = hashAddrToData(e.bus, e.chip, e.block, word);
			if (goldenData != readBuffers[tag][word]) {
				fprintf(stderr, "LOG: **ERROR: read data mismatch! tag=%d, %d %d %d %d, word=%d, Expected: %x, read: %x\n", tag, e.bus, e.chip, e.block, e.page, word, goldenData, readBuffers[tag][word]);
				numErrors++;
				pass = false;
				break;
			}
		}
		if (numErrors==0) {
			if(verbose_resp) fprintf(stderr, "LOG: Read data check passed on tag=%d!\n", tag);
		}
	}
	else if (flashStatus[e.bus][e.chip][e.block]==ERASED) {
		if (readBuffers[tag][0]==(unsigned int)-1) {
			if(verbose_resp) fprintf(stderr, "LOG: Read check pass on erased block!\n");
		}
		else if (readBuffers[tag][0]==0) {
			fprintf(stderr, "LOG: Warning: potential bad block, read erased data 0\n");
			pass = false;
		}
		else {
			fprintf(stderr, "LOG: **ERROR: read data mismatch! Expected: ERASED, read: %x\n", readBuffers[tag][0]);
			pass = false;
		}
	}
	else {
		fprintf(stderr, "LOG: **ERROR: flash block state unknown. Did you erase before write?\n");
		pass = false;
	}
	return pass;
}

int num_merged=-1;
int flush_done1=-1;
int flush_done2=-1;
int num_invalidated;

int cnt_readDone=0;

char *testBuf;

class FlashIndication: public FlashIndicationWrapper {
	public:
		FlashIndication(unsigned int id) : FlashIndicationWrapper(id){}
		virtual void findKeyDone(uint16_t tag, uint16_t status, uint32_t ppa) {
			if (status == 1) {
				fprintf(stderr, "findKeyDone: tag %u, status %u, ppa %x \n", (uint32_t)tag, (uint32_t)status,  ppa);
				fflush(stderr);
			}
			pthread_mutex_lock(&flashReqMutex);
			curSearchInFlight--;
			if(keyTagTable[tag].busy == false) fprintf(stderr, "Tag dup?\n");

			keyTagTable[tag].busy = false;
			pthread_mutex_unlock(&flashReqMutex);
		}

		virtual void mergeDone(unsigned int numMergedKt, uint32_t numInvalAddr, uint64_t counter) {
			fprintf(stderr, "mergeDone: kt%u inval%u timer:%" PRIu64 "\n", numMergedKt, numInvalAddr, counter);
			fflush(stderr);
			pthread_mutex_lock(&flashReqMutex);
			num_merged = (int)numMergedKt;
			num_invalidated = (int)numInvalAddr;
			pthread_mutex_unlock(&flashReqMutex);
		}

		virtual void mergeFlushDone1(unsigned int num) {
			fprintf(stderr, "mergeFlushDone1: %u \n", num);
			fflush(stderr);
			pthread_mutex_lock(&flashReqMutex);
			flush_done1 = 1;
			pthread_mutex_unlock(&flashReqMutex);
		}

		virtual void mergeFlushDone2(unsigned int num) {
			fprintf(stderr, "mergeFlushDone2: %u \n", num);
			fflush(stderr);
			pthread_mutex_lock(&flashReqMutex);
			flush_done2 = 1;
			pthread_mutex_unlock(&flashReqMutex);
		}

		virtual void readDone(unsigned int tag) {

			bool readPassed = false;

			if ( verbose_resp ) {
				fprintf(stderr, "LOG: pagedone: tag=%d; inflight=%d cnt_readDone=%d\n", tag, curReadsInFlight, cnt_readDone );
				memcpy(testBuf, readBuffers[tag], 8192);
				usleep(100);
			}
			fflush(stderr);

			if (readTagTable[tag].checkRead) readPassed = checkReadData(tag);

			pthread_mutex_lock(&flashReqMutex);
			cnt_readDone = cnt_readDone + 1;

			if ( readTagTable[tag].checkRead && readPassed == false ) {
				testPassed = false;
				fprintf(stderr, "LOG: **ERROR: check read data failed @ tag=%d\n",tag);
			}
			if ( curReadsInFlight < 0 ) {
				fprintf(stderr, "LOG: **ERROR: Read requests in flight cannot be negative %d\n", curReadsInFlight );
			}
			if ( readTagTable[tag].busy == false ) {
				fprintf(stderr, "LOG: **ERROR: received unused buffer read done (duplicate) tag=%d\n", tag);
				testPassed = false;
			} else {
				curReadsInFlight --;
			}

			readTagTable[tag].busy = false;
			pthread_mutex_unlock(&flashReqMutex);

		}

		virtual void writeDone(unsigned int tag) {
			if ( verbose_resp ) {
				fprintf(stderr, "LOG: writedone, tag=%d\n", tag);
				fflush(stderr);
			}

			pthread_mutex_lock(&flashReqMutex);
			if ( curWritesInFlight < 0 ) {
				fprintf(stderr, "LOG: **ERROR: Write requests in flight cannot be negative %d\n", curWritesInFlight );
			}
			if ( writeTagTable[tag].busy == false) {
				fprintf(stderr, "LOG: **ERROR: received unknown write done (duplicate) tag=%d\n", tag);
				testPassed = false;
			} else {
				curWritesInFlight --;
			}
			writeTagTable[tag].busy = false;
			pthread_mutex_unlock(&flashReqMutex);

			fflush(stderr);
		}

		virtual void eraseDone(unsigned int tag, unsigned int status) {
			if ( verbose_resp ) {
				fprintf(stderr, "LOG: eraseDone, tag=%d, status=%d\n", tag, status); fflush(stderr);
			}

			if (status != 0) {
				fprintf(stderr, "LOG: detected bad block with tag = %d\n", tag);
			}
			pthread_mutex_lock(&flashReqMutex);
			curErasesInFlight--;
			if ( curErasesInFlight < 0 ) {
				fprintf(stderr, "LOG: **ERROR: erase requests in flight cannot be negative %d\n", curErasesInFlight );
				curErasesInFlight = 0;
			}
			if ( eraseTagTable[tag].busy == false ) {
				fprintf(stderr, "LOG: **ERROR: received unused tag erase done %d\n", tag);
				testPassed = false;
			}
			eraseTagTable[tag].busy = false;
			pthread_mutex_unlock(&flashReqMutex);
		}

		virtual void debugDumpResp (unsigned int debug0, unsigned int debug1,  unsigned int debug2, unsigned int debug3, unsigned int debug4, unsigned int debug5) {
			fprintf(stderr, "LOG: DEBUG DUMP: gearSend = %d, gearRec = %d, aurSend = %d, aurRec = %d, readSend=%d, writeSend=%d, readAck=%d\n"
					, debug0, debug1, debug2, debug3, debug4, debug5, cnt_readDone);
		}
};


int getNumReadsInFlight() { return curReadsInFlight; }
int getNumWritesInFlight() { return curWritesInFlight; }
int getNumErasesInFlight() { return curErasesInFlight; }
int getNumSearchInFlight() { return curSearchInFlight; }

//TODO: more efficient locking
int waitIdleEraseTag() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !eraseTagTable[t].busy ) {
				eraseTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

//TODO: more efficient locking
int waitIdleWriteBuffer() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !writeTagTable[t].busy) {
				writeTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

//TODO: more efficient locking
int waitIdleReadBuffer() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !readTagTable[t].busy ) {
				readTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

int waitIdleKeyBuffer() {
	int tag = -1;
	while ( tag < 0 ) {
		pthread_mutex_lock(&flashReqMutex);
		for ( int t = 0; t < NUM_TAGS; t++ ) {
			if ( !keyTagTable[t].busy ) {
				keyTagTable[t].busy = true;
				tag = t;
				break;
			}
		}
		pthread_mutex_unlock(&flashReqMutex);
	}
	return tag;
}

void eraseBlock(uint32_t bus, uint32_t chip, uint32_t block, uint32_t tag) {
	pthread_mutex_lock(&flashReqMutex);
	curErasesInFlight ++;
	flashStatus[bus][chip][block] = ERASED;
	pthread_mutex_unlock(&flashReqMutex);

	if ( verbose_req ) fprintf(stderr, "LOG: sending erase block request with tag=%d @%d %d %d 0\n", tag, bus, chip, block );
	device->eraseBlock(bus,chip,block,tag);
}

void writePage(uint32_t bus, uint32_t chip, uint32_t block, uint32_t page, uint32_t tag) {
	pthread_mutex_lock(&flashReqMutex);
	curWritesInFlight ++;
	flashStatus[bus][chip][block] = WRITTEN;
	pthread_mutex_unlock(&flashReqMutex);

	if ( verbose_req ) fprintf(stderr, "LOG: sending write page request with tag=%d @%d %d %d %d\n", tag, bus, chip, block, page );
	device->writePage(bus,chip,block,page,tag,tag*FPAGE_SIZE);
}

void readPage(uint32_t bus, uint32_t chip, uint32_t block, uint32_t page, uint32_t tag, bool checkRead=false) {
	pthread_mutex_lock(&flashReqMutex);
	curReadsInFlight ++;
	readTagTable[tag].bus = bus;
	readTagTable[tag].chip = chip;
	readTagTable[tag].block = block;
	readTagTable[tag].page = page;
	readTagTable[tag].checkRead = checkRead;
	pthread_mutex_unlock(&flashReqMutex);

	if ( verbose_req ) fprintf(stderr, "LOG: sending read page request with tag=%d @%d %d %d %d\n", tag, bus, chip, block, page );
	device->readPage(bus,chip,block,page,tag,tag*FPAGE_SIZE);
}

// Use all BUSES & CHIPS in the device
// Can designate the range of blocks -> incl [blkStart, blkStart + blkCnt) non-incl
//	Valid block # range: 0 ~ BLOCKS_PER_CHIP-1 (=4097)
void testErase(FlashRequestProxy* device, int blkStart, int blkCnt) {
	// blkEnd can be at most BLOCKS_PER_CHIP-1
	int blkEnd = blkStart + blkCnt;
	if (blkEnd > BLOCKS_PER_CHIP) blkEnd = BLOCKS_PER_CHIP;

	for (int blk = blkStart; blk < blkEnd; blk++){
		for (int chip = 0; chip < CHIPS_PER_BUS; chip++){
			for (int bus = 0; bus < NUM_BUSES; bus++){
				eraseBlock(bus, chip, blk, waitIdleEraseTag());
			}
		}
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
		}
		else {
			elapsed--;
		}
		if ( getNumErasesInFlight() == 0 ) break;
	}
}

// Use all BUSES & CHIPS in the device
// Can designate the range of blocks -> incl [blkStart, blkStart + blkCnt) non-incl
// Can designate the range of pages  -> incl [pageStart, pageStart + pageCnt) non-incl
//	 Valid block # range: 0 ~ BLOCKS_PER_CHIP-1
//	 Valid page  # range: 0 ~ PAGES_PER_BLOCK-1
void testRead(FlashRequestProxy* device, int blkStart, int blkCnt, int pageStart, int pageCnt, bool checkRead=false, int repeat=1) {
	// blkEnd can be at most BLOCKS_PER_CHIP-1
	int blkEnd = blkStart + blkCnt;
	if (blkEnd > BLOCKS_PER_CHIP) blkEnd = BLOCKS_PER_CHIP;

	// pageEnd can be at most BLOCKS_PER_CHIP-1
	int pageEnd = pageStart + pageCnt;
	if (pageEnd > PAGES_PER_BLOCK) pageEnd = PAGES_PER_BLOCK;
	
	for (int rep = 0; rep < repeat; rep++){
		for (int page = pageStart; page < pageEnd; page++) {
			for (int blk = blkStart; blk < blkEnd; blk++){
				for (int chip = 0; chip < CHIPS_PER_BUS; chip++){
					for (int bus = 0; bus < NUM_BUSES; bus++){
						readPage(bus, chip, blk, page, waitIdleReadBuffer(), checkRead);
					}
				}
			}
		}
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
		}
		else {
			elapsed--;
		}
		if ( getNumReadsInFlight() == 0 ) break;
	}
}

// Use all BUSES & CHIPS in the device
// Can designate the range of blocks -> incl [blkStart, blkStart + blkCnt) non-incl
// Can designate the range of pages  -> incl [pageStart, pageStart + pageCnt) non-incl
//	 Valid block # range: 0 ~ BLOCKS_PER_CHIP-1
//	 Valid page  # range: 0 ~ PAGES_PER_BLOCK-1
void testWrite(FlashRequestProxy* device, int blkStart, int blkCnt, int pageStart, int pageCnt, bool genData=false) {
	// blkEnd can be at most BLOCKS_PER_CHIP-1
	int blkEnd = blkStart + blkCnt;
	if (blkEnd > BLOCKS_PER_CHIP) blkEnd = BLOCKS_PER_CHIP;

	// pageEnd can be at most BLOCKS_PER_CHIP-1
	int pageEnd = pageStart + pageCnt;
	if (pageEnd > PAGES_PER_BLOCK) pageEnd = PAGES_PER_BLOCK;
	
	for (int page = pageStart; page < pageEnd; page++) {
		for (int blk = blkStart; blk < blkEnd; blk++){
			for (int chip = 0; chip < CHIPS_PER_BUS; chip++){
				for (int bus = 0; bus < NUM_BUSES; bus++){
					int freeTag = waitIdleWriteBuffer();
					if (genData) {
						for (unsigned int w=0; w<FPAGE_SIZE/sizeof(unsigned int); w++) {
							writeBuffers[freeTag][w] = hashAddrToData(bus, chip, blk, w);
						}
					}
					writePage(bus, chip, blk, page, freeTag);
				}
			}
		}
	} 

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
		}
		else {
			elapsed--;
		}
		if ( getNumWritesInFlight() == 0 ) break;
	}
}


int main(int argc, const char **argv)
{
	testPassed=true;
	pthread_mutex_init(&flashReqMutex, NULL);

	fprintf(stderr, "Initializing Connectal & DMA...\n");

	// Device initialization
	device = new FlashRequestProxy(IfcNames_FlashRequestS2H);
	FlashIndication deviceIndication(IfcNames_FlashIndicationH2S);
	DmaManager *dma = platformInit();
	
	// Memory-allocation for DMA
	srcAlloc = portalAlloc(srcAlloc_sz, 0);
	dstAlloc = portalAlloc(dstAlloc_sz, 0);
	srcBuffer = (unsigned int *)portalMmap(srcAlloc, srcAlloc_sz);
	dstBuffer = (unsigned int *)portalMmap(dstAlloc, dstAlloc_sz);

	fprintf(stderr, "dstAlloc = %x\n", dstAlloc); 
	fprintf(stderr, "srcAlloc = %x\n", srcAlloc); 

	// Memory-mapping for DMA
	portalCacheFlush(dstAlloc, dstBuffer, dstAlloc_sz, 1);
	portalCacheFlush(srcAlloc, srcBuffer, srcAlloc_sz, 1);
	ref_dstAlloc = dma->reference(dstAlloc);
	ref_srcAlloc = dma->reference(srcAlloc);

	fprintf(stderr, "ref_dstAlloc = %x\n", ref_dstAlloc); 
	fprintf(stderr, "ref_srcAlloc = %x\n", ref_srcAlloc); 

	device->setDmaWriteRef(ref_dstAlloc);
	device->setDmaReadRef(ref_srcAlloc);

	fprintf(stderr, "Done initializing Hardware & DMA!\n" ); 

	for (int t = 0; t < NUM_TAGS; t++) {
		readTagTable[t].busy = false;
		writeTagTable[t].busy = false;
		eraseTagTable[t].busy = false;
		keyTagTable[t].busy = false;

		int byteOffset = t * FPAGE_SIZE;
		readBuffers[t] = dstBuffer + byteOffset/sizeof(unsigned int);
		writeBuffers[t] = srcBuffer + byteOffset/sizeof(unsigned int);
	}

	for (int blk=0; blk < BLOCKS_PER_CHIP; blk++) {
		for (int c=0; c < CHIPS_PER_BUS; c++) {
			for (int bus=0; bus< NUM_BUSES; bus++) {
				flashStatus[bus][c][blk] = UNINIT;
			}
		}
	}

	for (int t = 0; t < NUM_TAGS; t++) {
		for ( unsigned int i = 0; i < FPAGE_SIZE/sizeof(unsigned int); i++ ) {
			readBuffers[t][i] = 0xDEADBEEF;
			writeBuffers[t][i] = 0xBEEFDEAD;
		}
	}

	long actualFrequency=0;
	long requestedFrequency=1e9/MainClockPeriod;
	int status = setClockFrequency(0, requestedFrequency, &actualFrequency);
	fprintf(stderr, "HW Operating Freq - Requested: %5.2f, Actual: %5.2f, status=%d\n"
			,(double)requestedFrequency*1.0e-6
			,(double)actualFrequency*1.0e-6,status);

	fprintf(stderr, "Done initializing Hardware & DMA!\n" ); 
	fflush(stderr);

	fprintf(stderr, "Test Start!\n" );
	fflush(stderr);

	device->start(0);
	device->setDebugVals(0,0); // flag, delay

	device->debugDumpReq(0);   // echo-back debug message
	sleep(1);

	//int elapsed = 10000;

#if defined(TEST_ERASE_ALL)
	{
		verbose_req = true;
		verbose_resp = true;

		fprintf(stderr, "[TEST] ERASE ALL BLOCKS STARTED!\n");
		testErase(device, 0, BLOCKS_PER_CHIP);
		fprintf(stderr, "[TEST] ERASE ALL BLOCKS DONE!\n"); 
	}
#endif

#if defined(MINI_TEST_SUITE)
	{
		// Functionality test
		verbose_req = true;
		verbose_resp = true;
		int blkStart = 4086;
		int blkCnt = 10;
		int pageStart = 0;
		int pageCnt = 10;

		fprintf(stderr, "[TEST] ERASE RANGED BLOCKS (Start: %d, Cnt: %d) STARTED!\n", blkStart, blkCnt); 
		testErase(device, blkStart, blkCnt);
		fprintf(stderr, "[TEST] ERASE RANGED BLOCKS DONE!\n" ); 

		fprintf(stderr, "[TEST] READ CHECK ON ERASED BLOCKS (Start: %d, Cnt: %d) STARTED!\n", blkStart, blkCnt); 
		testRead(device, blkStart, blkCnt, pageStart, pageCnt, true /* checkRead */);
		fprintf(stderr, "[TEST] READ CHECK ON ERASED BLOCKS DONE!\n" ); 

		fprintf(stderr, "[TEST] WRITE ON ERASED BLOCKS (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testWrite(device, blkStart, blkCnt, pageStart, pageCnt, true /* genData */);
		fprintf(stderr, "[TEST] WRITE ON ERASED BLOCKS DONE!\n" ); 

		fprintf(stderr, "[TEST] READ ON WRITTEN BLOCKS (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testRead(device, blkStart, blkCnt, pageStart, pageCnt, true /* checkRead */);
		fprintf(stderr, "[TEST] READ ON WRITTEN BLOCKS DONE!\n" ); 
	}
#endif

#if defined(TEST_READ_SPEED)
	{
		// Read speed test: No printf / No data generation / No data integrity check
		verbose_req = false;
		verbose_resp = false;

		int repeat = 1;
		int blkStart = 0;
		int blkCnt = 128;
		int pageStart = 0;
		int pageCnt = 16;

		timespec start, now;
		clock_gettime(CLOCK_REALTIME, &start);

		fprintf(stderr, "[TEST] READ SPEED (Repeat: %d, BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", repeat, blkStart, blkCnt, pageStart, pageCnt); 
		testRead(device, blkStart, blkCnt, pageStart, pageCnt, false /* checkRead */, repeat);

		fprintf(stderr, "[TEST] READ SPEED DONE!\n" ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (repeat*8192.0*NUM_BUSES*CHIPS_PER_BUS*blkCnt*pageCnt/1000000)/timespec_diff_sec(start,now));
	}
#endif

#if defined(TEST_HEAVY_READ)
	{
		srand(time(NULL));
		verbose_req = false;
		verbose_resp = true;

		timespec start, now;

		int pageCnt = 2000000; // Max
		int elapsed;
		int* randPpa = (int*)malloc(sizeof(int)*pageCnt);

		for (int i = 0; i< pageCnt; i++) {
			randPpa[i] = rand();
		}

		testBuf = (char*)malloc(8192);

		// TEST 1
		clock_gettime(CLOCK_REALTIME, &start);
		pageCnt = 100000;
		for (int i = 0; i< pageCnt; i++) {
			int ppa = randPpa[i];
			int bus = ppa & 7; //7
			int chip = (ppa>>3) & 7; //7
			int page = (ppa>>6) & 0x3F;
			int blk = (ppa>>14) & 0xFFF;
			readPage(bus, chip, blk, page, waitIdleReadBuffer(), false);
		}

		elapsed = 10000;

		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
				device->debugDumpReq(0);
			}
			else {
				elapsed--;
			}
			if ( getNumReadsInFlight() == 0 ) break;
		}
		fprintf(stderr, "[TEST] READ HEAVY RANDOM VERBOSE + MEMCOPY DONE! Acks=%d\n", cnt_readDone ); 

		int count =0;
		for (int j = 0; j < 8192; j++) {
			if(testBuf[j]) count++;
		}
		fprintf(stderr, "[TEST] READ HEAVY RANDOM VERBOSE Memcpy check: (doesn't mean anything.)%d\n", count ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (8192.0*pageCnt/1000000)/timespec_diff_sec(start,now));

		verbose_resp = false;
		// TEST 2
		clock_gettime(CLOCK_REALTIME, &start);
		pageCnt = 200000;
		for (int i = 0; i< pageCnt; i++) {
			int ppa = randPpa[i];
			int bus = ppa & 2; //7
			int chip = (ppa>>3) & 3; //7
			int page = (ppa>>6) & 0x3F;
			int blk = (ppa>>14) & 0xFFF;
			readPage(bus, chip, blk, page, waitIdleReadBuffer(), false);
		}

		elapsed = 10000;
		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
				device->debugDumpReq(0);
			}
			else {
				elapsed--;
			}
			if ( getNumReadsInFlight() == 0 ) break;
		}
		fprintf(stderr, "[TEST] READ SAME BUS CHIP DONE! Acks=%d\n", cnt_readDone ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (8192.0*pageCnt/1000000)/timespec_diff_sec(start,now));


		// TEST 3
		clock_gettime(CLOCK_REALTIME, &start);
		pageCnt = 2000000;
		for (int i = 0; i< pageCnt; i++) {
			int ppa = randPpa[i];
			int bus = ppa & 7; //7
			int chip = (ppa>>3) & 7; //7
			int page = (ppa>>6) & 0x3F;
			int blk = (ppa>>14) & 0xFFF;
			readPage(bus, chip, blk, page, waitIdleReadBuffer(), false);
		}

		elapsed = 10000;

		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
				device->debugDumpReq(0);
			}
			else {
				elapsed--;
			}
			if ( getNumReadsInFlight() == 0 ) break;
		}
		fprintf(stderr, "[TEST] READ HEAVY RANDOM DONE! Acks=%d\n", cnt_readDone ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (8192.0*pageCnt/1000000)/timespec_diff_sec(start,now));


		free(randPpa);
	}
#endif

#if defined(TEST_WRITE_SPEED)
	{
		// Read speed test: No printf / No data generation / No data integrity check
		verbose_req = false;
		verbose_resp = false;

		int blkStart = 3091;
		int blkCnt = 30;
		int pageStart = 0;
		int pageCnt = 64;

		timespec start, now;
		clock_gettime(CLOCK_REALTIME, &start);

		fprintf(stderr, "[TEST] WRITE SPEED (BStart: %d, BCnt: %d, PStart: %d, PCnt: %d) STARTED!\n", blkStart, blkCnt, pageStart, pageCnt); 
		testWrite(device, blkStart, blkCnt, pageStart, pageCnt, false);

		fprintf(stderr, "[TEST] WRITE SPEED DONE!\n" ); 

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "SPEED: %f MB/s\n", (8192.0*NUM_BUSES*CHIPS_PER_BUS*blkCnt*pageCnt/1000000)/timespec_diff_sec(start,now));
	}
#endif

#if defined(KT_WRITE)
	{
		const char* pathH[] = {"uniq32/h_level.bin", "invalidate/h_level.bin", "100_1000/h_level.bin"};
		const char* pathL[] = {"uniq32/l_level.bin", "invalidate/l_level.bin", "100_1000/l_level.bin"};

		const int startPpaH[] = {0, 100, 300};
		const int startPpaL[] = {1, 200, 400};
		//const int startPpaH[] = {2001, 2101, 2301};
		//const int startPpaL[] = {2002, 2201, 2401};

		const int numPpaH[] = {1, 41, 100};
		const int numPpaL[] = {1, 88, 1000};
		for (int ii=0; ii<3; ii++){
			FILE *h_fp = fopen(pathH[ii], "rb");
			FILE *l_fp = fopen(pathL[ii], "rb");

			if(h_fp == NULL) {
				fprintf(stderr, "h_level.bin missing\n");
				return -1;
			}

			if(l_fp == NULL) {
				fprintf(stderr, "l_level.bin missing\n");
				fclose(h_fp);
				return -1;
			}

			struct stat stat_buf;
			fstat(fileno(h_fp), &stat_buf);
			size_t h_size = (size_t)stat_buf.st_size;

			if(h_size%8192 != 0) {
				fprintf(stderr, "h_level.bin size not multiple of 8192\n");
				fclose(h_fp);
				fclose(l_fp);
				return -1;
			}

			fstat(fileno(l_fp), &stat_buf);
			size_t l_size = (size_t)stat_buf.st_size;

			if(l_size%8192 != 0) {
				fprintf(stderr, "l_level.bin size not multiple of 8192\n");
				fclose(h_fp);
				fclose(l_fp);
				return -1;
			}

			// write page H
			fprintf(stderr, "Loading %s %d %d\n", pathH[ii], startPpaH[ii], numPpaH[ii]);
			for (int ppa = startPpaH[ii]; ppa < startPpaH[ii]+numPpaH[ii]; ppa++) {
				int bus = ppa & 7;
				int chip = (ppa>>3) & 7;
				int page = (ppa>>6) & 0xFF;
				int blk = (ppa>>14);

				int freeTag = waitIdleWriteBuffer();
				if(fread(writeBuffers[freeTag], 8192, 1, h_fp) != 1) {
					fprintf(stderr, "h_level.bin read failed %d %d\n", ii, ppa);
					fclose(h_fp);
					fclose(l_fp);
					return -1;
				}
				writePage(bus,chip,blk,page,freeTag);
			}

			fprintf(stderr, "Loading %s %d %d\n", pathL[ii], startPpaL[ii], numPpaL[ii]);
			for (int ppa = startPpaL[ii]; ppa < startPpaL[ii]+numPpaL[ii]; ppa++) {
				int bus = ppa & 7;
				int chip = (ppa>>3) & 7;
				int page = (ppa>>6) & 0xFF;
				int blk = (ppa>>14);

				int freeTag = waitIdleWriteBuffer();
				if(fread(writeBuffers[freeTag], 8192, 1, l_fp) != 1) {
					fprintf(stderr, "l_level.bin read failed %d %d\n", ii, ppa);
					fclose(h_fp);
					fclose(l_fp);
					return -1;
				}
				writePage(bus,chip,blk,page,freeTag);
			}

			int elapsed = 10000;
			while (true) {
				usleep(100);
				if (elapsed == 0) {
					elapsed=10000;
					device->debugDumpReq(0);
				}
				else {
					elapsed--;
				}
				if ( getNumWritesInFlight() == 0 ) break;
			}
		}
	}
#endif

#if defined(KT_READ)
	{
		const char* pathH[] = {"uniq32/h_level.bin", "invalidate/h_level.bin", "100_1000/h_level.bin"};
		const char* pathL[] = {"uniq32/l_level.bin", "invalidate/l_level.bin", "100_1000/l_level.bin"};

		const int startPpaH[] = {0, 100, 300};
		const int startPpaL[] = {1, 200, 400};
		//const int startPpaH[] = {2001, 2101, 2301};
		//const int startPpaL[] = {2002, 2201, 2401};

		const int numPpaH[] = {1, 41, 100};
		const int numPpaL[] = {1, 88, 1000};

		for (int ii=0; ii<3; ii++){
			FILE *h_fp = fopen(pathH[ii], "rb");
			FILE *l_fp = fopen(pathL[ii], "rb");

			if(h_fp == NULL) {
				fprintf(stderr, "h_level.bin missing\n");
				return -1;
			}

			if(l_fp == NULL) {
				fprintf(stderr, "l_level.bin missing\n");
				fclose(h_fp);
				return -1;
			}

			struct stat stat_buf;
			fstat(fileno(h_fp), &stat_buf);
			size_t h_size = (size_t)stat_buf.st_size;

			if(h_size%8192 != 0) {
				fprintf(stderr, "h_level.bin size not multiple of 8192\n");
				fclose(h_fp);
				fclose(l_fp);
				return -1;
			}

			fstat(fileno(l_fp), &stat_buf);
			size_t l_size = (size_t)stat_buf.st_size;

			if(l_size%8192 != 0) {
				fprintf(stderr, "l_level.bin size not multiple of 8192\n");
				fclose(h_fp);
				fclose(l_fp);
				return -1;
			}

			// write page H
			fprintf(stderr, "Reading %s %d %d\n", pathH[ii], startPpaH[ii], numPpaH[ii]);
			void *tmpbuf = malloc(8192);

			for (int ppa = startPpaH[ii]; ppa < startPpaH[ii]+numPpaH[ii]; ppa++) {
				int bus = ppa & 7;
				int chip = (ppa>>3) & 7;
				int page = (ppa>>6) & 0xFF;
				int blk = (ppa>>14);

				int freeTag = waitIdleReadBuffer();
				if(fread(tmpbuf, 8192, 1, h_fp) != 1) {
					fprintf(stderr, "h_level.bin read failed %d %d\n", ii, ppa);
					fclose(h_fp);
					fclose(l_fp);
					return -1;
				}
				readPage(bus,chip,blk,page,freeTag);

				while (true) {
					usleep(100);
					if ( getNumReadsInFlight() == 0 ) break;
				}

				if (memcmp(tmpbuf, readBuffers[freeTag], 8192) != 0) {
					fprintf(stderr, "h_level.bin read different %d %d\n", ii, ppa);
					fclose(h_fp);
					fclose(l_fp);
					return -1;
				}
			}

			fprintf(stderr, "Loading %s %d %d\n", pathL[ii], startPpaL[ii], numPpaL[ii]);
			for (int ppa = startPpaL[ii]; ppa < startPpaL[ii]+numPpaL[ii]; ppa++) {
				int bus = ppa & 7;
				int chip = (ppa>>3) & 7;
				int page = (ppa>>6) & 0xFF;
				int blk = (ppa>>14);

				int freeTag = waitIdleReadBuffer();
				if(fread(tmpbuf, 8192, 1, l_fp) != 1) {
					fprintf(stderr, "l_level.bin read failed %d %d\n", ii, ppa);
					fclose(h_fp);
					fclose(l_fp);
					return -1;
				}
				readPage(bus,chip,blk,page,freeTag);

				while (true) {
					usleep(100);
					if ( getNumReadsInFlight() == 0 ) break;
				}

				if (memcmp(tmpbuf, readBuffers[freeTag], 8192) != 0) {
					fprintf(stderr, "l_level.bin read different %d %d\n", ii, ppa);
					fclose(h_fp);
					fclose(l_fp);
					return -1;
				}
			}
		}
	}
#endif

	sleep(1);

#if defined(KT_READ_ACCEL)
	fprintf(stderr, "Kt find Key Test\n");
	int searchKeyAlloc = portalAlloc(256*128, 0);
	unsigned int (*searchKeyBuf)[64] = (unsigned int(*)[64])portalMmap(searchKeyAlloc, 256*128);
	portalCacheFlush(searchKeyAlloc, searchKeyBuf, 256*128, 1);
	unsigned int ref_searchKeyAlloc = dma->reference(searchKeyAlloc);
	device->setDmaKtSearchRef(ref_searchKeyAlloc);

	//searchKeyBuf[0][1] = 0x37393331;
	//searchKeyBuf[0][2] = 0x38393934;
	searchKeyBuf[9][1] = 0x33383531;
	searchKeyBuf[9][2] = 0x30303030;
	searchKeyBuf[9][3] = 0x30303030;
	searchKeyBuf[9][4] = 0x30303030;
	searchKeyBuf[9][5] = 0x30303030;
	searchKeyBuf[9][6] = 0x30303030;
	searchKeyBuf[9][7] = 0x30303030;
	searchKeyBuf[9][8] = 0x30303030;
	searchKeyBuf[9][9] = 0x30303030;
	searchKeyBuf[9][10] = 0x30303030;
	searchKeyBuf[9][11] = 0x30303030;

//	const int startPpaH[] = {0, 100, 300};
	const int startPpaL[] = {1, 200, 400};
//	const int numPpaH[] = {1, 41, 100};
	const int numPpaL[] = {1, 88, 1000};

	timespec start, now;
	clock_gettime(CLOCK_REALTIME, &start);

	int repeatNum = 100;
	int numPpa = numPpaL[2];
	int startPpa = startPpaL[2];

	for (int repeat = 0; repeat < repeatNum; repeat++) {
		for (int i = 0; i<numPpa; i++) {
			int freeTag = waitIdleKeyBuffer();
			device->findKey(startPpa+i, 3, freeTag);

			pthread_mutex_lock(&flashReqMutex);
			curSearchInFlight++;
			pthread_mutex_unlock(&flashReqMutex);
		}
	}

	int elapsed = 10000;
	while (true) {
		usleep(100);
		if (elapsed == 0) {
			elapsed=10000;
			device->debugDumpReq(0);
		}
		else {
			elapsed--;
		}
		if ( getNumSearchInFlight() == 0 ) break;
	}

	clock_gettime(CLOCK_REALTIME, & now);
	fprintf(stderr, "SPEED: %f MB/s\n", (8192.0*repeatNum*numPpa/1000000)/timespec_diff_sec(start,now));

	fprintf(stderr, "Kt find Key Done\n");

#endif

	sleep(1);

#if defined(KT_MERGE)
	fprintf(stderr, "Kt Merge Test\n");
	int highPpaAlloc = portalAlloc(4*1024*200, 0);
	int lowPpaAlloc = portalAlloc(4*1024*200, 0);
	int resPpaAlloc = portalAlloc(4*1024*200, 0);
	unsigned int* highPpaBuf = (unsigned int*)portalMmap(highPpaAlloc, 4*1024*200);
	unsigned int* lowPpaBuf = (unsigned int*)portalMmap(lowPpaAlloc, 4*1024*200);
	unsigned int* resPpaBuf = (unsigned int*)portalMmap(resPpaAlloc, 4*1024*200);

	fprintf(stderr, "highPpaAlloc = %x\n", highPpaAlloc); 
	fprintf(stderr, "lowPpaAlloc = %x\n", lowPpaAlloc); 
	fprintf(stderr, "resPpaAlloc = %x\n", resPpaAlloc); 
	portalCacheFlush(highPpaAlloc, highPpaBuf, 4*1024*200, 1);
	portalCacheFlush(lowPpaAlloc, lowPpaBuf, 4*1024*200, 1);
	portalCacheFlush(resPpaAlloc, resPpaBuf, 4*1024*200, 1);
	unsigned int ref_highPpaAlloc = dma->reference(highPpaAlloc);
	unsigned int ref_lowPpaAlloc = dma->reference(lowPpaAlloc);
	unsigned int ref_resPpaAlloc = dma->reference(resPpaAlloc);
	device->setDmaKtPpaRef(ref_highPpaAlloc, ref_lowPpaAlloc, ref_resPpaAlloc, ref_resPpaAlloc);
	fprintf(stderr, "Done setting up PPA DMA\n");

	int mergedKtAlloc = portalAlloc(8192*10000, 0);
	int invalPpaAlloc = portalAlloc(4*1024*200, 0);
	unsigned int* mergedKtBuf = (unsigned int*)portalMmap(mergedKtAlloc, 8192*10000);
	unsigned int* invalPpaBuf = (unsigned int*)portalMmap(invalPpaAlloc, 4*1024*200);
	fprintf(stderr, "mergedKtAlloc = %x\n", mergedKtAlloc); 
	fprintf(stderr, "invalPpaAlloc = %x\n", invalPpaAlloc); 
	portalCacheFlush(mergedKtAlloc, mergedKtBuf, 8192*10000, 1);
	portalCacheFlush(invalPpaAlloc, invalPpaBuf, 4*1024*200, 1);
	unsigned int ref_mergedKtAlloc = dma->reference(mergedKtAlloc);
	unsigned int ref_invalPpaAlloc = dma->reference(invalPpaAlloc);
	device->setDmaKtOutputRef(ref_mergedKtAlloc, ref_invalPpaAlloc);
	fprintf(stderr, "Done setting up Merged Keytable DMA\n");

	// set up LPA
	//const char* pathH[] = {"uniq32/h_level.bin", "invalidate/h_level.bin", "100_1000/h_level.bin"};
	//const char* pathL[] = {"uniq32/l_level.bin", "invalidate/l_level.bin", "100_1000/l_level.bin"};
	
	const char* pathM[] = {"uniq32_hw.bin", "invalidate_hw.bin", "100_1000_hw.bin"};
	const char* pathInval[] = {"uniq32_inval_hw.bin", "invalidate_inval_hw.bin", "100_1000_inval_hw.bin"};

	const int startPpaH[] = {0, 100, 300};
	const int startPpaL[] = {1, 200, 400};
	//const int startPpaH[] = {2001, 2101, 2301};
	//const int startPpaL[] = {2002, 2201, 2401};

	const int startPpaR[] = {10000,10100,11000};
	//const int startPpaR[] = {12000,12100,13000};
	//const int startPpaR[] = {30001,30101,31001};
	//const int startPpaR[] = {130001,130101,131001};

	const int numPpaH[] = {1, 41, 100};
	const int numPpaL[] = {1, 88, 1000};

	timespec start, now;

	for (int i = 0; i < 3; i++){
		unsigned int numTableHigh=numPpaH[i];
		num_merged = -1;
		flush_done1 = -1;
		flush_done2 = -1;
		for (unsigned int t=0; t < numTableHigh; t++) {
			highPpaBuf[t] = startPpaH[i]+t;
		}

		unsigned int numTableLow=numPpaL[i];
		for (unsigned int t=0; t < numTableLow; t++) {
			lowPpaBuf[t] = startPpaL[i]+t;
		}

		for (unsigned int t=0; t < numTableHigh+numTableLow; t++) {
			resPpaBuf[t] = startPpaR[i]+t;
		}

		// start Test
		clock_gettime(CLOCK_REALTIME, &start);
		fprintf(stderr, "Start PPA: %u %u\n", numTableHigh, numTableLow); 
		device->startCompaction(numTableHigh, numTableLow, 0);

		fflush(stderr);
		device->debugDumpReq(0);   // echo-back debug message

		int elapsed = 10000;
		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
				device->debugDumpReq(0);
			}
			else {
				elapsed--;
			}
			if ( num_merged != -1 ) break;
		}
		device->debugDumpReq(0);
		usleep(100);

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "MERGE SPEED1: %f MB/s\n", (8192.0*(numTableHigh+numTableLow)/1000000)/timespec_diff_sec(start,now));

		fprintf(stderr, "Waiting for flush\n"); 
		fflush(stderr);

		while (true) {
			usleep(100);
			if (elapsed == 0) {
				elapsed=10000;
				device->debugDumpReq(0);
			}
			else {
				elapsed--;
			}
			if ( flush_done1 != -1 || flush_done2 != -1 ) break;
		}

		clock_gettime(CLOCK_REALTIME, & now);
		fprintf(stderr, "MERGE SPEED2: %f MB/s\n", (8192.0*(numTableHigh+numTableLow)/1000000)/timespec_diff_sec(start,now));

		fflush(stderr);

		device->debugDumpReq(0);
		device->debugDumpReq(0);

		sleep(1);
		fprintf(stderr, "Dumping results..\n"); 

		// output data
		FILE *fp = fopen(pathM[i], "wb");
		if (fp == NULL) {
			fprintf(stderr, "%s open failed\n", pathM[i]);
			return -1;
		}

		if (fwrite(mergedKtBuf, (size_t)num_merged*8192, 1, fp) != 1) {
			fprintf(stderr, "%s write failed\n", pathM[i]);
			fclose(fp);
			return -1;
		}
		fclose(fp);

		if (num_invalidated>0) {
			fp = fopen(pathInval[i], "wb");
			if (fp == NULL) {
				fprintf(stderr, "%s open failed\n", pathInval[i]);
				return -1;
			}
			if (fwrite(invalPpaBuf, (size_t)num_invalidated*4, 1, fp) != 1) {
				fprintf(stderr, "%s write failed\n", pathInval[i]);
				fclose(fp);
				return -1;
			}
			fclose(fp);
			num_invalidated=0;
		}
	}

	sleep(1);
	dma->dereference(ref_highPpaAlloc);
	dma->dereference(ref_lowPpaAlloc);
	dma->dereference(ref_resPpaAlloc);
	dma->dereference(ref_mergedKtAlloc);
	dma->dereference(ref_invalPpaAlloc);
	portalMunmap(highPpaBuf, 4*1024*200);
	portalMunmap(lowPpaBuf, 4*1024*200);
	portalMunmap(resPpaBuf, 4*1024*200);
	portalMunmap(mergedKtBuf, 8192*10000);
	portalMunmap(invalPpaBuf, 4*1024*200);

	sleep(1);
#endif

#if defined(KT_READ_MERGE)
	{
		const char* pathM[] = {"uniq32_flash.bin", "invalidate_flash.bin", "100_1000_flash.bin"};
		const int startPpaR[] = {10000,10100,11000};
		//const int startPpaR[] = {12000,12100,13000};
		//const int startPpaR[] = {30001,30101,31001};
		//const int startPpaR[] = {130001,130101,131001};

		const int numPpaR[] = {2, 107, 1099};

		for (int ii=0; ii<3; ii++){
			fprintf(stderr, "Reading %s\n", pathM[ii]);
			FILE *fp = fopen(pathM[ii], "wb");

			if(fp == NULL) {
				fprintf(stderr, "%s openFailed\n", pathM[ii]);
				fclose(fp);
				return -1;
			}

			for (int ppa = startPpaR[ii]; ppa < startPpaR[ii]+numPpaR[ii]; ppa++) {
				int bus = ppa & 7;
				int chip = (ppa>>3) & 7;
				int page = (ppa>>6) & 0xFF;
				int blk = (ppa>>14);

				int freeTag = waitIdleReadBuffer();

				pthread_mutex_lock(&flashReqMutex);
				curReadsInFlight ++;
				readTagTable[freeTag].bus = bus;
				readTagTable[freeTag].chip = chip;
				readTagTable[freeTag].block = blk;
				readTagTable[freeTag].page = page;
				readTagTable[freeTag].checkRead = false;
				pthread_mutex_unlock(&flashReqMutex);

				device->readPage(bus,chip,blk,page,freeTag, (ppa-startPpaR[ii])*FPAGE_SIZE);
			}

			while (true) {
				usleep(100);
				if ( getNumReadsInFlight() == 0 ) break;
			}

			if (fwrite(dstBuffer, 8192*numPpaR[ii], 1, fp) != 1) {
				fprintf(stderr, "%s write failed\n", pathM[ii]);
				fclose(fp);
				return -1;
			}
			fclose(fp);
		}
	}
#endif

	if (testPassed==true) {
		fprintf(stderr, "LOG: TEST PASSED!\n");
	}
	else {
		fprintf(stderr, "LOG: **ERROR: TEST FAILED!\n");
	}

	dma->dereference(ref_srcAlloc);
	dma->dereference(ref_dstAlloc);
	portalMunmap(srcBuffer, srcAlloc_sz);
	portalMunmap(dstBuffer, dstAlloc_sz);

	fprintf(stderr, "Done releasing DMA!\n");
}
