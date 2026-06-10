/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include "recordbuilder.h"
#include "constants.h"
//#include "hipoexceptions.h"

#ifdef __LZ4__
#include <lz4.h>
#endif

namespace hipo {
  /**
  * Default constructor sets number of max events to 100000
  * and the buffer size to 8MB.
  */
  recordbuilder::recordbuilder(){
    bufferIndex.resize(4*defaultNumberOfEvents);
    bufferEvents.resize(defaultRecordSize);
    bufferData.resize(defaultRecordSize+4*defaultNumberOfEvents);
    bufferRecord.resize(defaultRecordSize+4*defaultNumberOfEvents+512*1024);
    bufferIndexEntries   = 0;
    bufferEventsPosition = 0;
  }
  /**
   * Constructor with custom max event size and maximum record
   * size provided by user.
   */
  recordbuilder::recordbuilder(int maxEvents, int maxLength){
    bufferIndex.resize(4*maxEvents);
    bufferEvents.resize(maxLength);
    bufferData.resize(  maxLength+4*maxEvents + 1024);
    bufferRecord.resize(maxLength+4*maxEvents+512*1024);
    bufferIndexEntries   = 0;
    bufferEventsPosition = 0;
  }

  /**
   * add event object to the record builder buffer.
  */
  bool recordbuilder::addEvent(hipo::event &evnt){
      return addEvent(evnt.getEventBuffer(),0,evnt.getSize());
  }
  /**
   * add a content of a vector to the record builder buffer.
   * offset in the buffer and number of bytes to add provided
   * by user.
   */
  bool recordbuilder::addEvent(std::vector<char> &vec, int start, int length){
      if(static_cast<size_t>(bufferEventsPosition+length)>=bufferEvents.size()) return false;
      if(static_cast<size_t>((bufferIndexEntries+1)*4)>=bufferIndex.size()) return false;
      *reinterpret_cast<int*>(&bufferIndex[bufferIndexEntries*4]) = length;
      bufferIndexEntries++;
      memcpy(&bufferEvents[bufferEventsPosition],&vec[start],length);
      bufferEventsPosition += length;
      return true;
  }
  /**
   * Resets the counters for number of events and sets the
   * position for writing new events to the begining of the
   * event buffer.
   */
  void recordbuilder::reset(){
    bufferIndexEntries   = 0;
    bufferEventsPosition = 0;
  }
  /**
   * returns record length in bytes rounded to first integer.
   * the length comes out divisible by 4.
   */
  int  recordbuilder::getRecordLengthRounding(int bufferSize){
    if(bufferSize%4==0) return 0;
    int nwords = bufferSize/4;
    int nbytes = 4*(nwords+1);
    return (nbytes-bufferSize);
  }
  /**
   * Returns number of events in the record.
   */
  int  recordbuilder::getEntries(){
    int nentries = *reinterpret_cast<int*>(&bufferRecord[RH_EVENT_COUNT_OFFSET]);
    return nentries;
  }
  /**
   * returns the size of the record.
   */
  int  recordbuilder::getRecordSize(){
      int size = *reinterpret_cast<int*>(&bufferRecord[RH_RECORD_LENGTH_OFFSET]);
      return size*4;
  }

  long recordbuilder::getUserWordOne(){
    long wOne = *reinterpret_cast<long*>(&bufferRecord[RH_USER_WORD1_OFFSET]);
    return wOne;
  }

  long recordbuilder::getUserWordTwo(){
    long wTwo = *reinterpret_cast<long*>(&bufferRecord[RH_USER_WORD2_OFFSET]);
    return wTwo;
  }

  void recordbuilder::setUserWordOne(long userWordOne){
    bufferUserWordOne = userWordOne;
  }

  void recordbuilder::setUserWordTwo(long userWordTwo){
    bufferUserWordTwo = userWordTwo;
  } 

  void recordbuilder::build(){
      int  indexSize = bufferIndexEntries*4;
      int eventsSize = bufferEventsPosition;
      memcpy(&bufferData[0],&bufferIndex[0],indexSize);
      memcpy(&bufferData[indexSize],&bufferEvents[0],eventsSize);
      int uncompressedSize = indexSize+eventsSize;
      int   compressedSize = compressRecord(uncompressedSize);
      int         rounding = getRecordLengthRounding(compressedSize);
      int   compressedSizeToWrite = compressedSize + rounding;
      int   compressedSizeToWriteWords =  compressedSizeToWrite/4;
      int            recordLength = compressedSizeToWrite/4+RECORD_HEADER_WORDS;

      hipo::utils::writeInt(&bufferRecord[0], RH_RECORD_LENGTH_OFFSET, recordLength); // (1) - record length in words (includes header)
      hipo::utils::writeInt(&bufferRecord[0], RH_RECORD_NUMBER_OFFSET, 0); // (2) - record #
      hipo::utils::writeInt(&bufferRecord[0], RH_HEADER_LENGTH_OFFSET, RECORD_HEADER_WORDS); // (3) - record header lenght (in words)
      hipo::utils::writeInt(&bufferRecord[0], RH_EVENT_COUNT_OFFSET, bufferIndexEntries); // (4) event count in the record
      hipo::utils::writeInt(&bufferRecord[0], RH_INDEX_ARRAY_LEN_OFFSET, bufferIndexEntries*4); // (5) length of index array in bytes
      int versionWord = (rounding<<BITINFO_PAD3_SHIFT)|(HIPO_VERSION);
      hipo::utils::writeInt(&bufferRecord[0], RH_BIT_INFO_OFFSET, versionWord); // (6) record version number
      hipo::utils::writeInt(&bufferRecord[0], RH_USER_HEADER_LEN_OFFSET, 0); // (7) user header length bytes
      hipo::utils::writeInt(&bufferRecord[0], RH_MAGIC_NUMBER_OFFSET, HEADER_MAGIC); // (8) magic word
      hipo::utils::writeInt(&bufferRecord[0], RH_DATA_LENGTH_OFFSET, eventsSize); // (9) magic word
      int compressionWord = (1<<COMP_TYPE_SHIFT)|(COMP_LENGTH_MASK&compressedSizeToWriteWords);
      hipo::utils::writeInt(&bufferRecord[0], RH_COMP_WORD_OFFSET, compressionWord);
      hipo::utils::writeLong(&bufferRecord[0], RH_USER_WORD1_OFFSET, bufferUserWordOne);
      hipo::utils::writeLong(&bufferRecord[0], RH_USER_WORD2_OFFSET, bufferUserWordTwo);
      
      //printf("record::build uncompressed size = %8d, compressed size = %8d, rounding = %4d , compressed FULL = %6d, record size = %6d, version = %X, size = %5X\n",
      //      uncompressedSize,compressedSize, rounding,compressedSizeToWrite, recordLength*4,versionWord,compressionWord);
  }
  /**
   * Compresses the constructed buffer with LZ4 into internal buffer that
   * will be written to the output.
   */
  int  recordbuilder::compressRecord(int src_size){

    #ifdef __LZ4__
    //(const char* src, char* dst, int srcSize, int dstCapacity, int acceleration);
      int result = LZ4_compress_fast(&bufferData[0],&bufferRecord[RECORD_HEADER_SIZE],src_size,bufferRecord.size(),1);

      //int   result = LZ4_decompress_safe(data,output,dataLength,dataLengthUncompressed);
      //int   result = LZ4_decompress_fast(data,output,dataLengthUncompressed);
      return result;
      //printf(" FIRST (%d) = %x %x %x %x\n",result);//,destUnCompressed[0],destUnCompressed[1],
      //destUnCompressed[2],destUnCompressed[3]);
      //LZ4_decompress_fast(buffer,destUnCompressed,decompressedLength);
      //LZ4_uncompress(buffer,destUnCompressed,decompressedLength);
      #endif

      #ifndef __LZ4__
        printf("\n   >>>>> LZ4 compression is not supported.");
        printf("\n   >>>>> check if libz4 is installed on your system.");
        printf("\n   >>>>> recompile the library with liblz4 installed.\n");
        return 0;
      #endif
  }
}
