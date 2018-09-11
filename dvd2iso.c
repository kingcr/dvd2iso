// dvd2iso
// Copy the full contents of a DVD to an ISO image, using libdvdcss.


#include "stdafx.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <tchar.h>
#include <ctime>

// libdvdcss definitions

#define DVDCSS_BLOCK_SIZE      2048
#define DVDCSS_NOFLAGS         0
#define DVDCSS_READ_DECRYPT    (1 << 0)
#define DVDCSS_SEEK_MPEG       (1 << 0)
#define DVDCSS_SEEK_KEY        (1 << 1)
typedef struct dvdcss_s* dvdcss_t;

typedef dvdcss_t(*dvdcss_open_t)(const char*);
typedef int(*dvdcss_is_scrambled_t)(dvdcss_t);
typedef int(*dvdcss_seek_t)(dvdcss_t, int, int);
typedef int(*dvdcss_read_t)(dvdcss_t, void*, int, int);
typedef int(*dvdcss_error_t)(const dvdcss_t);
typedef int(*dvdcss_close_t)(dvdcss_t);

// Calls to libdvdcss

dvdcss_open_t dvdcss_open;
dvdcss_is_scrambled_t dvdcss_is_scrambled;
dvdcss_seek_t dvdcss_seek;
dvdcss_read_t dvdcss_read;
dvdcss_error_t dvdcss_error;
dvdcss_close_t dvdcss_close;

// block formatting constants
#define UNIT_FORMAT_KIB 1
#define UNIT_FORMAT_MIB 2
#define UNIT_FORMAT GIB 3

// buffer size (512 blocks * 2048 bytes = 1MB)
#define BUFFER_BLOCK_SIZE 512 

// program version
#define VERSION "1.2"

 /**
	Load libdvdcss and map function calls
	@returns the library handle, or NULL if loading fails or the library is not usable
*/
HMODULE load_libdvdcss() {
	HMODULE hModule = LoadLibrary(_T("dvdcss.dll"));
	if (!hModule){
		return NULL;
	}
	dvdcss_open = (dvdcss_open_t)GetProcAddress(hModule, "dvdcss_open");
	dvdcss_is_scrambled = (dvdcss_is_scrambled_t)GetProcAddress(hModule, "dvdcss_is_scrambled");
	dvdcss_seek = (dvdcss_seek_t)GetProcAddress(hModule, "dvdcss_seek");
	dvdcss_read = (dvdcss_read_t)GetProcAddress(hModule, "dvdcss_read");
	dvdcss_error = (dvdcss_error_t)GetProcAddress(hModule, "dvdcss_error");
	dvdcss_close = (dvdcss_close_t)GetProcAddress(hModule, "dvdcss_close");
	if (dvdcss_open &&
		dvdcss_is_scrambled &&
		dvdcss_seek &&
		dvdcss_read &&
		dvdcss_error &&
		dvdcss_close) {
		return hModule;
	}
	else {
		FreeLibrary(hModule);
		return NULL;
	}
}

/**
	Write a given number of blocks from buffer to file
	@param buffer	- the buffer to write
	@param blocks	- number of blocks to write from the buffer 
	@param file		- file to write to
	@param flush	- whether or not to flush after writing
*/
static bool write_data(void *buffer, int blocks, FILE* file, bool flush) {
	int iBytes = (int) fwrite(buffer, DVDCSS_BLOCK_SIZE*blocks, 1, file);
	if (flush) {
		fflush(file);
	}
	return (iBytes == blocks);
}

/**
	Utility function to convert a time to a formatted string
	@param theTime			- the time to convert
	@param buffer			- output string buffer
	@param bufferLength		- length of output string buffer
	@param formatString		- time format for conversion
*/
static void time_to_string(time_t & theTime, char* buffer, int bufferLength, const char* formatString) {
	struct tm tmTime;
	localtime_s(&tmTime, &theTime);
	strftime(buffer, bufferLength, formatString, &tmTime);
}

/**
	Find the size of a given disk, in blocks
	@param path	- the disk root path (e.g. "D:")
	@returns the total size of the disk in DVDCSS_BLOCK_SIZE blocks
*/
static int get_disk_blocks(char* path) {
	DWORD dwSectorsPerCluster, dwBytesPerSector, dwNumFreeClusters, dwTotalNumClusters;
	size_t outSize;
	wchar_t* szWPath = new wchar_t[strlen(path) + 1];
	mbstowcs_s(&outSize, szWPath, strlen(path) + 1, path, strlen(path) + 1 - 1);
	BOOL result = GetDiskFreeSpace(szWPath, &dwSectorsPerCluster, &dwBytesPerSector, &dwNumFreeClusters, &dwTotalNumClusters);
	if (result)
		return (int)((long long)dwTotalNumClusters * (long long)dwSectorsPerCluster * (long long)dwBytesPerSector / (long long)DVDCSS_BLOCK_SIZE);
	else
		return -1;
}

/**
	Utility function to convert blocks to a formatted string
	@param blocks			- number of blocks to convert
	@param buffer			- target for output string
	@param bufferSize		- length of output string buffer
	@param maxUnitFormat	- limit the conversion to a particular unit (one of UNIT_FORMAT_*)
	@param temporal			- append "/s" to the end of the units or not
*/
static void blocks_to_string(long blocks, char* buffer, int bufferLength, int maxUnitFormat, bool temporal) {
	const char* units = "KiB";
	long kBlocks = 2 * blocks; // blocks in KiB (blocks * DVDCSS_BLOCK_SIZE / 1024)
	if (kBlocks >= 1024 && maxUnitFormat != UNIT_FORMAT_KIB) {
		kBlocks /= 1024;
		units = "MiB";
		if (kBlocks >= 1024 && maxUnitFormat != UNIT_FORMAT_MIB) {
			kBlocks /= 1024;
			units = "GiB";
		}
	}
	if(temporal)
		sprintf_s(buffer, bufferLength, "%i %s/s", kBlocks, units);
	else
		sprintf_s(buffer, bufferLength, "%i %s", kBlocks, units);
}


int main(int argc, char *argv[]) {

	HMODULE hLibDVDCSS = NULL;
	dvdcss_t dvdcss;
	FILE* fOutputFile;
	void* pDataBuffer = malloc(DVDCSS_BLOCK_SIZE*BUFFER_BLOCK_SIZE);
	int iReadBufferSize = BUFFER_BLOCK_SIZE, iDVDCSSResult, iDiskSizeBlocks, iPercentComplete,
		iCurrentBlock = 0, iReadErrorBlock = 0,
		iReadLimitBlocks, iReadRate, iReadRateBlock = 0, iAvgReadRate;
	char *szOutputFilename,
		szDiskSize[20], szProcessedSize[20], szReadRate[10], szAvgReadRate[20],
		szStartTime[80], szEndTime[80], szRemainingTime[80], szElapsedTime[80];
	bool bFinishedProcessing = false, bReadLimit = false;
	time_t tmStartTime, tmEndTime, tmElapsedTime, tmReadRateTime, tmRemainingTime;

	// check params a bit
	if (argc != 3) {
		fprintf(stderr, "\n%s version %s.\n\n", argv[0], VERSION);
		fprintf(stderr, "Usage: %s <src> <output file>\n", argv[0]);
		fprintf(stderr, "Example:\n");
		fprintf(stderr, "  %s d: foo.iso\n", argv[0]);
		return -1;
	}

	szOutputFilename = argv[2];

	// load dvdcss.dll
	hLibDVDCSS = load_libdvdcss();
	if (!hLibDVDCSS) {
		fprintf(stderr, "\nError: cannot load dvdcss.dll.\n");
		return -1;
	}

	// try to open the device
	dvdcss = dvdcss_open(argv[1]);

	if (dvdcss == NULL) {
		fprintf(stderr, "\nError: cannot open disk '%s'\n.", argv[1]);
		return -1;
	}
	
	// check output file doesn't already exist
	if (fopen_s(&fOutputFile, szOutputFilename, "r") == 0) {
		fclose(fOutputFile);
		fprintf(stderr, "Error: output file '%s' already exists.\n", szOutputFilename);
		return -1;
	}

	// open output file in binary mode, with flushing turned on
	if (!fopen_s(&fOutputFile, szOutputFilename, "wcb") == 0) {
		fprintf(stderr, "Failed to open file '%s' for output.\n", szOutputFilename);
		return -1;
	}

	// let's go...
	tmStartTime = time(0);
	tmReadRateTime = tmStartTime;
	time_to_string(tmStartTime, szStartTime, 80, "%d/%m/%y %T");

	iDiskSizeBlocks = get_disk_blocks(argv[1]);
	blocks_to_string(iDiskSizeBlocks, szDiskSize, 20, UNIT_FORMAT_MIB, false);

	fprintf(stderr, "\nCopying %s, started at %s.\n\n", szDiskSize, szStartTime);
	

	while (!bFinishedProcessing) {

		// seek to and read a buffer's worth of data
		iDVDCSSResult = dvdcss_seek(dvdcss, iCurrentBlock, DVDCSS_SEEK_KEY);
		if (iDVDCSSResult < 0) {
			fprintf(stderr, "\nError: Failed to seek to block %i.\n", iCurrentBlock);
			return -1;
		}

		iDVDCSSResult = dvdcss_read(dvdcss, pDataBuffer, iReadBufferSize, DVDCSS_READ_DECRYPT);

		if (iDVDCSSResult == 0) {
			// end of disk
			bFinishedProcessing = true;
		}

		if (iDVDCSSResult < 0) {
			// Read error:
			// libdvdcss reports an error if *any* block in the buffer cannot be read, so it's impossible to
			// tell where exactly the problem is and becomes necessary to read block-by-block until the bad 
			// block has been passed over. There are multiple ways to handle this; the strategy here is to read 
			// the current buffer's worth of data block-by-block and then resume reading full buffers again.
			if(!bReadLimit) {
				// this is a read error we weren't expecting, so step down the current buffer size...
				iReadBufferSize = 1;
				// ... start counting how many blocks we're reading one-by-one ...
				iReadLimitBlocks = 0;
				// ... from this point on, we're expecting an error ....
				bReadLimit = true;
				// ... and do not move on to the next block on the disk - force it to be re-read,
				//     but only as a single block this time (because the real bad block may be anywhere
				//     in the buffer)
			}
			else {
				// this is a single-block read error that we expect, so ignore it: flush single padded block to file
				memset(pDataBuffer, '\0', (BUFFER_BLOCK_SIZE*DVDCSS_BLOCK_SIZE));
				write_data(pDataBuffer, 1, fOutputFile, true);
				// accrue towards buffer volume
				iReadErrorBlock++;
				// move on to next block
				iCurrentBlock++;
			}
		}
		else {
			// No read error: could be a full or partial (end of disk) read
			if (bReadLimit) {
				// this is a successful read of a single block
				if (iReadLimitBlocks >= BUFFER_BLOCK_SIZE) {
					// if we've read enough single blocks to fill the orginal buffer, resume full reads again
					bReadLimit = false;
					iReadBufferSize = BUFFER_BLOCK_SIZE;
				}
				else {
					// if we haven't read enough to fill the original buffer, keep on reading single blocks
					iReadLimitBlocks++;
				}
			}
			// pad the buffer in case of a short-read, then flush to disk
			memset(&(((unsigned char*)pDataBuffer)[iDVDCSSResult*DVDCSS_BLOCK_SIZE]), '\0', (BUFFER_BLOCK_SIZE - iDVDCSSResult)*DVDCSS_BLOCK_SIZE);
			write_data(pDataBuffer, iDVDCSSResult, fOutputFile, true);
			// move on to next segment
			iCurrentBlock += iDVDCSSResult;
		}
			
		// display progress
		if (time(0) - tmReadRateTime >= 1 || bFinishedProcessing) {

			// data read
			blocks_to_string(iCurrentBlock, szProcessedSize, 20, UNIT_FORMAT_MIB, false);

			// data read per sec in the last interval
			if (bFinishedProcessing)
				iReadRate = 0; // prevent possible division-by-zero because no minimum time interval 
			else
				iReadRate = (int)((iCurrentBlock - iReadRateBlock) / (time(0) - tmReadRateTime));
			blocks_to_string(iReadRate, szReadRate, 20, UNIT_FORMAT_MIB, true);

			// avg data read per sec since the start of processing
			iAvgReadRate = (int)(iCurrentBlock / (time(0) - tmStartTime));
			blocks_to_string(iAvgReadRate, szAvgReadRate, 20, UNIT_FORMAT_MIB, true);

			// estimate time remaining based on overall average read rate
			if (iAvgReadRate > 0) {
				tmRemainingTime = (time_t)((iDiskSizeBlocks - iCurrentBlock) / iAvgReadRate);
				time_to_string(tmRemainingTime, szRemainingTime, 80, "%T");
			}
			else
				strcpy_s(szRemainingTime, 80, "unknown");

			// percentage processed, by volume
			iPercentComplete = iCurrentBlock * 100 / iDiskSizeBlocks;

			fprintf(stderr, "\rCopied %s (%i%%) | Rate %s | %i bad blocks | Time remaining %s.          \r", szProcessedSize, iPercentComplete, szReadRate, iReadErrorBlock, szRemainingTime);

			// reset interval counters
			iReadRateBlock = iCurrentBlock;
			tmReadRateTime = time(0);
		}	
	}

	tmEndTime = time(0);
	time_to_string(tmEndTime, szEndTime, 80, "%d/%m/%y %T");

	tmElapsedTime = tmEndTime - tmStartTime;
	time_to_string(tmElapsedTime, szElapsedTime, 80, "%T");

	fprintf(stderr, "\n\nCopied %s in %s at a rate of %s, completed at %s.\n", szProcessedSize, szElapsedTime, szAvgReadRate, szEndTime);

	// clean up
	dvdcss_close(dvdcss);
	free(pDataBuffer);
	fclose(fOutputFile);
	FreeLibrary(hLibDVDCSS);
	return 0;
}












