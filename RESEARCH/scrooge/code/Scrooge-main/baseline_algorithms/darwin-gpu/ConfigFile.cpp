
/*
Copyright 2018 Yatish Turakhia, Gill Bejerano and William Dally

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "ConfigFile.h"

#include <fstream>

std::string trim(std::string const& source, char const* delims = " \t\r\n") {
    std::string result(source);
    std::string::size_type index = result.find_last_not_of(delims);
    if(index != std::string::npos)
        result.erase(++index);

    index = result.find_first_not_of(delims);
    if(index != std::string::npos)
        result.erase(0, index);
    else
        result.erase();
    return result;
}

ConfigFile::ConfigFile(std::string const& configFile) {
    std::ifstream file(configFile.c_str());

    std::string line;
    std::string name;
    std::string value;
    std::string inSection;
    int posEqual;
    while (std::getline(file,line)) {

        if (! line.length()) continue;

        if (line[0] == '#') continue;
        if (line[0] == ';') continue;

        if (line[0] == '[') {
            inSection=trim(line.substr(1,line.find(']')-1));
            continue;
        }

        posEqual=line.find('=');
        name  = trim(line.substr(0,posEqual));
        value = trim(line.substr(posEqual+1));

        content_[inSection+'/'+name]=Chameleon(value);
    }
}

Chameleon const& ConfigFile::Value(std::string const& section, std::string const& entry) const {

    std::map<std::string,Chameleon>::const_iterator ci = content_.find(section + '/' + entry);

    if (ci == content_.end()) throw "does not exist";

    return ci->second;
}

Chameleon const& ConfigFile::Value(std::string const& section, std::string const& entry, double value) {
    try {
        return Value(section, entry);
    } catch(const char *) {
        return content_.insert(std::make_pair(section+'/'+entry, Chameleon(value))).first->second;
    }
}

Chameleon const& ConfigFile::Value(std::string const& section, std::string const& entry, std::string const& value) {
    try {
        return Value(section, entry);
    } catch(const char *) {
        return content_.insert(std::make_pair(section+'/'+entry, Chameleon(value))).first->second;
    }
}
