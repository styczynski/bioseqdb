
/* 
 *    Chameleon.cpp
 *
 *       Copyright (C) 2002-2004 René Nyffenegger
 *
 *          This source code is provided 'as-is', without any express or implied
 *             warranty. In no event will the author be held liable for any
 *             damages
 *                arising from the use of this software.
 *
 *                   Permission is granted to anyone to use this software for
 *                   any purpose,
 *                      including commercial applications, and to alter it and
 *                      redistribute it
 *                         freely, subject to the following restrictions:
 *
 *                            1. The origin of this source code must not be
 *                            misrepresented; you must not
 *                                  claim that you wrote the original source
 *                                  code. If you use this source code
 *                                        in a product, an acknowledgment in the
 *                                        product documentation would be
 *                                              appreciated but is not required.
 *
 *                                                 2. Altered source versions
 *                                                 must be plainly marked as
 *                                                 such, and must not be
 *                                                       misrepresented as being
 *                                                       the original source
 *                                                       code.
 *
 *                                                          3. This notice may
 *                                                          not be removed or
 *                                                          altered from any
 *                                                          source distribution.
 *
 *                                                             René Nyffenegger
 *                                                             rene.nyffenegger@adp-gmbh.ch
 *                                                             */

#include <string>
#include <sstream>
#include <cstdlib>

#include "Chameleon.h"

Chameleon::Chameleon(std::string const& value) {
    value_=value;
}

#include <iostream>

Chameleon::Chameleon(const char* c) {
    value_=c;
}

Chameleon::Chameleon(double d) {
    std::stringstream s;
    s<<d;
    value_=s.str();
}

Chameleon::Chameleon(Chameleon const& other) {
    value_=other.value_;
}

Chameleon& Chameleon::operator=(Chameleon const& other) {
    value_=other.value_;
    return *this;
}

Chameleon& Chameleon::operator=(double i) {
    std::stringstream s;
    s << i;
    value_ = s.str();
    return *this;
}

Chameleon& Chameleon::operator=(std::string const& s) {
    value_=s;
    return *this;
}

Chameleon::operator std::string() const {
    return value_;
}

Chameleon::operator double() const {
    return atof(value_.c_str());
}
