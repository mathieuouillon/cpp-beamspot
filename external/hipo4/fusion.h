/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/*
 * File:   fusion.h
 * Author: gavalian
 *
 * Created on April 27, 2017, 10:01 AM
 */

#ifndef FUSION_H
#define FUSION_H

#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include "reader.h"
#include <chrono>

namespace hipo {
  
class inputSource {
    
  private:
    
    hipo::reader reader;
    std::map<std::string,hipo::bank> banks;
    hipo::dictionary  factory;
    hipo::event      event;
    
  public:
    
    inputSource(){}
    inputSource(const char *filename){ open(filename); }
    inputSource(const char *filename, int tag) { open(filename, tag); }
    ~inputSource(){}
    
    void open(const char *filename);
    void open(const char *filename, int tag);
    void define(const char *bank);
    void describe(const char *bank);
    bool next();

    std::string   schema(const char *bank);
    hipo::bank   &get(const char *bank){ return banks[bank];}
    int           getSize(const char *bank);
    
    int         getInt(   const char *bank, const char *entry, int row);
    int64_t     getLong(   const char *bank, const char *entry, int row);
    float       getFloat( const char *bank, const char *entry, int row);
    double      getDouble( const char *bank, const char *entry, int row);

    void getByteArray(  const char *bank, int entry, int8_t *ptr, int length);
    void getShortArray( const char *bank, int entry, int16_t *ptr, int length);
    void getIntArray(   const char *bank, int entry, int32_t  *ptr, int length);
    void getFloatArray( const char *bank, int entry, float   *ptr, int length);

    int64_t getEntries() { return reader.getEntries(); }
  };

class fusion {

  private:

     std::map<int,inputSource*> sources;

  public:

  fusion(){}
  ~fusion(){}
  
  
  int      open(const char *filename);
  int      open(const char *filename, int tag);
  bool     next(int handle);
  
  void   define(int fid, const char *bank);
  void   describe(int fid, const char *bank);
  std::string schema(int fid, const char *bank);
  
  int    getSize(int fid, const char *bank);

  void getByteArray(  int handle, const char *bank, int entry, int8_t *ptr, int length){
    sources[handle]->getByteArray(bank,entry,ptr,length);
  }
  void getShortArray( int handle, const char *bank, int entry, int16_t *ptr, int length){
        sources[handle]->getShortArray(bank,entry,ptr,length);
  }
  void getIntArray(   int handle, const char *bank, int entry, int32_t  *ptr, int length){
        sources[handle]->getIntArray(bank,entry,ptr,length);
  }
  void getFloatArray( int handle, const char *bank, int entry, float   *ptr, int length){
        sources[handle]->getFloatArray(bank,entry,ptr,length);
  }
  
  int         getInt(   int handle, const char *bank, const char *entry, int row);
  int64_t     getLong(   int handle, const char *bank, const char *entry, int row);
  float       getFloat( int handle, const char *bank, const char *entry, int row);
  double      getDouble( int handle, const char *bank, const char *entry, int row);
  int         getType( int handle, const char *bank, const char *entry);

  int64_t getEntries(int handle) { return sources[handle]->getEntries(); }
  hipo::bank &getBank(int handle, const char *bank) {return sources[handle]->get(bank);}
};
}

#endif /* FUSION_H */
