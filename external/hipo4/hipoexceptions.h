/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/*
 * File:   hipoexceptions.h
 * Author: gavalian
 *
 * Created on April 11, 2017, 2:06 PM
 */

#ifndef HIPOEXCEPTIONS_H
#define HIPOEXCEPTIONS_H

#include <stdexcept>

namespace hipo {

class file_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class schema_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class record_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

} // namespace hipo

#endif /* HIPOEXCEPTIONS_H */
