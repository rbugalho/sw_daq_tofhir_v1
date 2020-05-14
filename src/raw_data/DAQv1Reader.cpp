#include <shm_raw.hpp>
#include "DAQv1Reader.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <ctype.h>


using namespace std;
using namespace PETSYS;


static unsigned ELINK_MAP(unsigned n)
{
	switch (n) {
		case 15: return 0;
		case 16: return 1;
		case 17: return 2;
		case 18: return 3;
		case 19: return 4;
		case 20: return 5;
		default: return -1;
	}
}

DAQv1Reader::DAQv1Reader() :
	dataFile(NULL)

{
}

DAQv1Reader::~DAQv1Reader()
{
	fclose(dataFile);
}

DAQv1Reader *DAQv1Reader::openFile(const char *fName)
{
	FILE * rawFile = fopen(fName, "r");
	if(rawFile == NULL) {
		fprintf(stderr, "Could not open '%s' for reading: %s\n", fName, strerror(errno));
                exit(1);
	}

	DAQv1Reader *reader = new DAQv1Reader();
	reader->dataFile = rawFile;

	return reader;
}

int DAQv1Reader::getNSteps()
{
	return 1;
}

double DAQv1Reader::getFrequency()
{
	return 160E6;
}

bool DAQv1Reader::isQDC()
{
	return 0;
}

void DAQv1Reader::getStepValue(int n, float &step1, float &step2)
{
	step1 = 0;
	step2 = 0;
}



void DAQv1Reader::processStep(int n, bool verbose, EventSink<RawHit> *sink)
{
	
	sink->pushT0(0);
	int nEvents = 0;
	
	RawDataFrame *dataFrame = new RawDataFrame;
	EventBuffer<RawHit> *outBuffer = NULL; 
	const long outBlockSize = 4*1024;
	long long currentBufferFirstFrame = 0;
	
	char *text_line = new char[256];
	size_t text_length;
	while(getline(&text_line, &text_length, dataFile) > 0) {
//		fprintf(stderr, "line = '%s'\n", text_line);
		unsigned link;
		unsigned elink;
		unsigned event_number;
		char word_hex[256];
		int r = sscanf(text_line, "%u; %u; %u; 0x%[0-9a-f]L\n", &link, &elink, &event_number, word_hex);
//		fprintf(stderr, "r = %d\n", r);
 		if (r != 4) continue;
 		
// 		fprintf(stderr, "%3d %3d %3d %4d, '%s'\n", link, elink, event_number, 4*strlen(word_hex), word_hex );
		
		
		// Convert HEX to unsigned __int128
		unsigned __int128 word = 0;
		for(int k = 0; k < strlen(word_hex); k++) {
			word = word << 4;
			switch(toupper(word_hex[k])) {
			case '0': word |= 0x0; break;
			case '1': word |= 0x1; break;
			case '2': word |= 0x2; break;
			case '3': word |= 0x3; break;
			case '4': word |= 0x4; break;
			case '5': word |= 0x5; break;
			case '6': word |= 0x6; break;
			case '7': word |= 0x7; break;
			case '8': word |= 0x8; break;
			case '9': word |= 0x9; break;
			case 'A': word |= 0xA; break;
			case 'B': word |= 0xB; break;
			case 'C': word |= 0xC; break;
			case 'D': word |= 0xD; break;
			case 'E': word |= 0xE; break;
			case 'F': word |= 0xF; break;
			}
		}

		
		unsigned word_type = (word >> 86) & 0x3;
		if(word_type != 0) {
			// Not an event word
			fprintf(stderr, "BAD  %3d %3d %3d '%22s'\n", link, elink, event_number, word_hex );
			continue;
		}
		
		
		if(outBuffer == NULL) {
			currentBufferFirstFrame = dataFrame->getFrameID();
			outBuffer = new EventBuffer<RawHit>(outBlockSize, currentBufferFirstFrame * 1024);
			
		}
		else if(outBuffer->getSize() + 1 > outBlockSize) {
			sink->pushEvents(outBuffer);
			currentBufferFirstFrame = dataFrame->getFrameID();
			outBuffer = new EventBuffer<RawHit>(outBlockSize, currentBufferFirstFrame * 1024);
		}
		
		
		RawHit &e = outBuffer->getWriteSlot();
		// There is no frame ID
		// For now lets create a fake frame ID from event_number
		unsigned long long frameID = event_number << 4;

		e.frameID 	= frameID;
		e.channelID	= (ELINK_MAP(elink) << 6) | ((word >> 0) % 16);
		e.tacID		= ((word >> 4) % 4);
		e.qfine		= ((word >> 6) % 1024);
		e.t2fine	= ((word >> 16) % 1024);
		e.t1fine	= ((word >> 26) % 1024);
		e.idleTime	= ((word >> 36) % 1024);
		e.qcoarse	= ((word >> 46) % 1024);
		e.t2coarse	= ((word >> 56) % 1024);
		e.t1coarse	= ((word >> 66) % 65536);
		e.triggerBits	= ((word >> 82) % 16);
		
		fprintf(stderr, "GOOD %3d %3d %3d '%22s'", link, elink, event_number, word_hex );
		fprintf(stderr, ": %2hu %2hu %1hu ; %6hu %4hu ; %4hu %4hu %4hu\n", elink, e.channelID % 16, e.tacID, e.t1coarse, e.t2coarse, e.t1fine, e.t2fine, e.qfine);
		
		e.time = (frameID - currentBufferFirstFrame) * 1024 + e.t1coarse;
		e.timeEnd = (frameID - currentBufferFirstFrame) * 1024 + e.t2coarse;
		if((e.timeEnd - e.time) < -256) e.timeEnd += 1024;
		
		e.valid = true;
		
		outBuffer->pushWriteSlot();
		nEvents += 1;

		outBuffer->setTMax((frameID + 1) * 1024);
		
		
		
	}
	delete [] text_line;
	
	if(outBuffer != NULL) {
		sink->pushEvents(outBuffer);
		outBuffer = NULL;
	}
	
	sink->finish();
	if(verbose) {
		fprintf(stderr, "DAQv1Reader report\n");
		fprintf(stderr, " events\n");
		fprintf(stderr, " %10lld total\n", nEvents);
		sink->report();
	}
	
	delete dataFrame;
	delete sink;
	
}
