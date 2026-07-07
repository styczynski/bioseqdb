#ifndef DATADUMP_HPP
#define DATADUMP_HPP
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include "hashKMer.hpp"


/* dump an object to disk
    does not perform deep copy (data from pointers not copied)
 */
template <typename T>
void dumpObject(T object, const std::string& file_name){
    std::ofstream outfile(file_name, std::ios::binary);
    if (outfile.is_open()){
        outfile.write((char*)&object, sizeof(T));
        outfile.close();
        return;
    } else{
        std::cerr << "Failed to open file: " << file_name <<std::endl;
        exit(1);
    }
}

/* load an object from disk */
template <typename T>
T loadObject(const std::string& file_name){
    std::ifstream infile(file_name, std::ios::binary);
    T object;
    if (infile.is_open()){
        infile.read((char*)&object, sizeof(T));
        return object;
    } else {
        std::cerr << "Failed to open file: " << file_name <<std::endl;
        exit(1);
    }
}


/* dump an array to disk */
template <typename T>
void dumpArray(T *arr, unsigned long long arr_size, const std::string& file_name){
    std::ofstream outfile(file_name, std::ios::binary);
    if (outfile.is_open()){
        outfile.write((char*)arr, arr_size*sizeof(T));
        outfile.close();
        return;
    } else{
        std::cerr << "Failed to open file: " << file_name <<std::endl;
        if(file_name.find('/') != std::string::npos) {
            std::cerr << "Perhaps create the directory structure first" << std::endl;
        }
        exit(1);
    }
}

/* load an array from disk */
template <typename T=kmers_bucket_t>
T* loadArray(unsigned long long arr_size, const std::string& file_name){
    std::ifstream infile(file_name, std::ios::binary);
    T* out = (T*)malloc(arr_size*sizeof(T));
    if (infile.is_open()){
        infile.read((char*)out, arr_size*sizeof(T));
        return out;
    } else {
        std::cerr << "Failed to open file: " << file_name <<std::endl;
        exit(1);
    }
}

#endif
