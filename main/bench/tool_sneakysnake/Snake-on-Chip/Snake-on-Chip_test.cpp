/*
 * Copyright (c) <2017 - 2030>, ETH Zurich and Bilkent University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or other
 *   materials provided with the distribution.
 * - Neither the names of the ETH Zurich, Bilkent University,
 *   nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without specific
 *   prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  Authors: 
  Mohammed Alser
	  mohammed.alser AT inf DOT ethz DOT ch
  Date:
  September 22nd, 2019
*/


#include <stdio.h>
#include <riffa.h>
#include <cassert>
#include <string.h>
#include <iostream>
#include <cmath>
#include <fstream>
#include <thread>
#include <chrono>

using namespace std;

ofstream dna_out_file;

fpga_t* fpga;
uint* rbuf, *wrbuf;
uint dna_size = 0;
uint numOutputElements = 0;

const uint NUM_REFS = 8; // first NUM_REFS reads of each stream are used
                         // as reference dna reads

const uint NUM_RIFFA_CHANNELS = 1; // multiple channels are currently not supported. 
                                   // See doSnake-on-Chip().
thread collector_threads[NUM_RIFFA_CHANNELS];

void createWriteBuffer(){

    assert(sizeof(uint) == 4);

    //Input Buffer
    wrbuf = new uint[(dna_size/sizeof(uint)) + 4*NUM_REFS];
    
    //Fill input buffer with dummy data
    for(uint i = 0; i < ((dna_size/sizeof(uint)) + 4*NUM_REFS); i++){
        wrbuf[i] = i;
    }
}

void createReadBuffer(){
	//Output Buffer, one mapping is 128-bit. Each mapping produces 1-bit output
    uint numMappings = (dna_size/16/*128-bit*/)*NUM_REFS;
    numOutputElements = ceil(numMappings/32.0);
    cout << "numOutputElements: " << numOutputElements << endl;
    rbuf = new uint[numOutputElements];
}

//size in 4-byte words
void sendDataToFPGA(void* data, const uint size, const uint ch){
    //send dna data - it is a blocking call
    //cout << "Sending data of size: " << size << endl;
    int sent = fpga_send(fpga, ch, data, size, 0, 1, 0); //the size here is number of words (4bytes)

    //cout << "DNAData sent: " << sent << endl;
}

// TODO: To run with more than one RIFFA channels, we should send the reference reads
// to each channel separately.
void doSnake-on-Chip(){
    
    //start sender threads, send data pointer, data size, and channel as arguments
    thread sender_threads[NUM_RIFFA_CHANNELS];

    for(uint i = 0; i < NUM_RIFFA_CHANNELS; i++){
        sender_threads[i] = thread(sendDataToFPGA, (void*)(wrbuf + i*(dna_size/NUM_RIFFA_CHANNELS)/sizeof(uint)), ((dna_size/NUM_RIFFA_CHANNELS)/sizeof(uint)) + 4*NUM_REFS, i);
    }


    //join threads
    for(uint i = 0; i < NUM_RIFFA_CHANNELS; i++){
        sender_threads[i].join();
    }
}

void initFiles(const char* output_filename){
    //open a file
    dna_out_file.open(output_filename);
    if (!dna_out_file.is_open()){
        cout << "Unable to open file:" << output_filename << endl;
        exit(-1);
    }
}

//size in 4-byte words
void collectDNAErrors(void* buf, const uint size, const uint ch){

    //receive data - it is a blocking call
    uint recv = 0;
    uint* buff = (uint*)buf;

    while(recv != size){
    	recv += fpga_recv(fpga, ch, buff + recv, size - recv, 0); //the size here is number of words (4bytes) 
	

	//cout << "DNA errors collected: " << recv << endl;
    }
}

void writeOutFile(){
    //Write to file
    assert(numOutputElements > 0);
    for(uint i = 0; i < numOutputElements; i++){
        dna_out_file << rbuf[i] << endl;
    }
}

void initCollectors(){

    //start collector threads
    for(uint i = 0; i < NUM_RIFFA_CHANNELS; i++){
        collector_threads[i] = thread(collectDNAErrors, (void*)(rbuf + i*(numOutputElements/NUM_RIFFA_CHANNELS)), numOutputElements/NUM_RIFFA_CHANNELS, i);
    }

    //they will be joined later. Since we need to start sender threads
}

void printHelp(){
    cout << "A sample application that tests Snake-on-Chip implementation on FPGA" << endl;
	cout << "Usage: 1) ./Snake-on-Chip_test [INPUT_SIZE_IN_BYTES] [OUTPUT_FILE_NAME]" << endl; 
	cout << "	2) ./Snake-on-Chip_test [INPUT_FILE_NAME] [OUTPUT_FILE_NAME]" << endl; 
    cout << "The size argument should be a positive integer!" << endl;
}

uint parseSubMap(const string submap){
	uint map = 0;
	
	for(int i = 0; i < 16; i++){
		string nucleotide = submap.substr(i, 1);

		if(nucleotide.compare("A") == 0){}
			//do nothing
		else if(nucleotide.compare("C") == 0)
			map |= 1;
		else if(nucleotide.compare("G") == 0)
			map |= 2;
		else if(nucleotide.compare("T") == 0)
			map |= 3;
		else{
			cout << "Error: unexpected character in the input file:" << nucleotide << ". Exiting..." << endl;
        	exit(-1);
		}

		if(i != 15)
			map <<= 2;
	}

	return map;
}

void parseMapping(const string mapp, uint* buf){
	for(int i = 0; i < 4; i++){
		string submap = mapp.substr(i*16, 16);

		buf[i] = parseSubMap(submap);
	}
}

void readFastaFile(const char* filename){
	ifstream fasta_in;

	//open the file
	fasta_in.open(filename);
    if (!fasta_in.is_open()){
		cout << "Unable to open input DNA file:" << filename << endl;
        exit(-1);
	}
	
	string line;
	uint num_mappings;

	//Get the number of mappings
	try {
		getline(fasta_in, line);
		num_mappings = stoi(line);	
	}
	catch(...){
		fasta_in.close();
		cout << "Error: The first line of the input file should indicate the number of mappings! Exiting..." << endl;
        exit(-1);
	}

	dna_size = num_mappings*16;

	//Allocate the input buffer
	wrbuf = new uint[(num_mappings + NUM_REFS)*4];

	uint ind = 0;
	while(getline(fasta_in, line)){
		parseMapping(line, wrbuf + ind);
		ind += 4;

		if(ind == (num_mappings + NUM_REFS)*4)
			break;
	}

	if(ind != (num_mappings + NUM_REFS)*4){
		fasta_in.close();
		cout << "Error: Insufficient data in the input file! Exiting..." << endl;
        exit(-1);
	}
	
	fasta_in.close();
}

int main(int argc, char* argv[]){
	fpga_info_list info;
	int fid = 0; //fpga id
	int ch = 0; //channel id

	if(argc != 3 || strcmp(argv[1], "--help") == 0){
		printHelp();
		return -2;
	}

    string s_ref(argv[1]);

    try{
        dna_size = stoi(s_ref);  
		createWriteBuffer();  
    }catch(...){
		readFastaFile(argv[1]);
        //printHelp();
        //return -3;
    }

	// Populate the fpga_info_list struct
	if (fpga_list(&info) != 0) {
		printf("Error populating fpga_info_list\n");
		return -1;
	}
	printf("Number of devices: %d\n", info.num_fpgas);
	for (int i = 0; i < info.num_fpgas; i++) {
		printf("%d: id:%d\n", i, info.id[i]);
		printf("%d: num_chnls:%d\n", i, info.num_chnls[i]);
		printf("%d: name:%s\n", i, info.name[i]);
		printf("%d: vendor id:%04X\n", i, info.vendor_id[i]);
		printf("%d: device id:%04X\n", i, info.device_id[i]);
	}

	fpga = fpga_open(fid);

	if(!fpga){
		printf("Problem on opening the fpga \n");
		return -1;
	}
	printf("The FPGA has been opened successfully! \n");

	fpga_reset(fpga); //keep this, recovers FPGA from some unwanted state from the previous run

    /*uint* buf = new uint[dna_size];
    uint sent = fpga_send(fpga, 0, buf, dna_size, 0, 1, 0);
    cout << "Sent data of size: " << sent << endl;
    uint recvd = fpga_recv(fpga, 0, buf, dna_size, 0);
    cout << "Received data of size: " << recvd << endl;

    return 0;*/

    printf("Starting Snake-on-Chip Test with %d bytes! \n", dna_size);

    createReadBuffer();
    initFiles(argv[2]);
    initCollectors();

    //start timer
    auto start = chrono::high_resolution_clock::now();
	doSnake-on-Chip();

    //wait until the results are received from FPGA
    for(uint i = 0; i < NUM_RIFFA_CHANNELS; i++){
        collector_threads[i].join();
    }
    //end timer
    auto end = chrono::high_resolution_clock::now();

    cout << "Execution Time: " << chrono::duration <double, milli> (end - start).count()
            << " ms" << endl;
	printf("The test has been completed! Writing the results to file...\n");

    writeOutFile();
    dna_out_file.close();

	fpga_close(fpga);

    //release the buffers
    delete[] wrbuf;
    delete[] rbuf;

    printf("Finished! Exiting... \n");

	return 0;
}
