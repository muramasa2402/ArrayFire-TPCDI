#ifndef ARRAYFIRE_TPCDI_TPCDI_UTILS_H
#define ARRAYFIRE_TPCDI_TPCDI_UTILS_H
#include <arrayfire.h>
#include <iostream>
#include <tuple>
#include <rapidxml.hpp>
#include <unordered_map>
#include "Enums.h"

template<typename T>
inline void print(T i, std::ostream &out = std::cout) { af::sync(); out << i << std::endl; }
void printStr(af::array str_array, std::ostream &out = std::cout);

namespace TPCDI_Utils {
    std::string loadFile(char const *filename);
    std::string collect(std::vector<std::string> const &files, bool hasHeader = false);;
    af::array where64(af::array const &input);
    inline af::array hflat(af::array const &arr) { return moddims(flat(arr), af::dim4(1, arr.elements())); }
}

namespace XML_Parser {
    typedef std::unordered_map<std::string, int> StrToInt;
    typedef std::string String;
    typedef rapidxml::xml_node<> Node;
    void fillBlanks(int &count, std::string fieldName, StrToInt &tracker, String &data, bool isAtt = false);

    void depthFirstAppend(String &data, Node *node, StrToInt &tracker,  String branch, Node *root);

    void learnFieldNames(Node *node, StrToInt &tracker, String branch, Node *root);

    std::string flattenCustomerMgmt(char const *directory);
}

#endif //ARRAYFIRE_TPCDI_TPCDI_UTILS_H
