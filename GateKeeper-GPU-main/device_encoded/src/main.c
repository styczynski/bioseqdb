/*
* Zulal Bingol
* GateKeeper driver functions for host side
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <omp.h>

#include "../include/common.h"
#include "../include/cuda_config.h"
#include "../include/kernel.cuh"

// WINDOW_COUNT, A_WINDOW_COUNT, TOTAL_MASKS are defined in makefile.
float longest_gpu_time;
double time_io, time_gkgpu; // GateKeeper-GPU time from host side

// For time calculations
double dtime()
{
	double tseconds = 0.0;
	struct timeval mytime;
	gettimeofday(&mytime,(struct timezone*)0);
	tseconds = (double)(mytime.tv_sec + mytime.tv_usec*1.0e-6);
	return( tseconds );
}

/* Reader Function */
int reader(FILE *in_file, long int batch_size, int *eof, int *file_index, int maxThreadPerBlock,
			int *remaining_batch_size, int last_index, int device, int start_index)
{
	char *line = NULL;
	size_t line_size = 0;
	long int batch_counter = 0; 
	int line_tracer, read_l, ref_l;
	
	while(batch_counter < batch_size){

		if (getline(&line, &line_size, in_file) == -1 || (*file_index) == last_index){ // eof or end of portion
			(*eof) = EOF_;
			free(line);
			if (batch_counter == 0){ // file content is perfectly aligned with device count & batch size
				return SUCCESSFUL_MISSION;
			}
			else {
				(*remaining_batch_size) = (int)batch_counter;
				return PARTIAL_BATCH;
			}
		}
		else {

			line_tracer = 0, read_l = 0, ref_l = 0;
			while (read_l != READ_LENGTH){
				h_read_buffer[device][(start_index + batch_counter) * READ_LENGTH + read_l] = line[line_tracer];
				line_tracer++;
				read_l++;
			}

			line_tracer++;
			while (ref_l != READ_LENGTH){
				h_ref_buffer[device][(start_index + batch_counter) * READ_LENGTH + ref_l] = line[line_tracer];
				line_tracer++;
				ref_l++;
			}

			batch_counter++;
			(*file_index)++;
		}// else eof
	} // while : MAIN LOOP ends here
	
	free(line);
	return SUCCESSFUL_MISSION;
}

/*
 * Result reports
 */
void reporter(FILE *filter_file, FILE *edit_file, long int batch_size, RES_NODE **res_head, 
	int last_index, int startIndex, int *passer_counts, int *total_rejected, int *undefined)
{
	int inner_index = 0, file_tracer = startIndex;
	int written = 0, unwritten = 0; //for reporting file status
	int rejected = 0;
	RES_NODE *current_node = (*res_head) -> next;
	int i = 0, tracer = 0, bound = 0;
	int loop_count = (*res_head) -> loop_count;
	
	i = 1; // loop counter
	while (i <= loop_count){
		if (current_node == NULL){
			printf("something is wrong..., file_tracer = %d, start = %d, loop = %d, file index = %d\n",
				file_tracer, startIndex, loop_count, last_index);
			return;
		}

		inner_index = 0;
		bound = current_node -> batch_size;
		
		while (inner_index < bound){ // current_node's filt_result range

			if (current_node != NULL){ // index corresponds to a valid comparison
				fprintf(filter_file, "%hi\n", (current_node -> filt_result)[inner_index]);
				fprintf(edit_file, "%hi\n", (current_node -> edits)[inner_index]);

				if ((current_node -> filt_result)[inner_index] == PASS){
					passer_counts[((current_node -> edits)[inner_index])]++;
				}
				else{
					rejected++;
				}

				if ((current_node -> edits)[inner_index] == UNDEFINED_STR_EDIT){
					(*undefined)++;
				}

				inner_index++;
				written++;
				file_tracer++;
			}
			else {
				unwritten++;
				printf("SOMETHING IS VERY WRONG, and this is hurtful...sorry buddy: i = %d, inner_index = %d\n",
								i,  inner_index);
			}
		}
		current_node = current_node -> next;
		i++;
	} //while loop count

	(*total_rejected) += rejected;
	if (debug_mode == ON){
		printf("---Reporting Report(lol): written = %d, unwritten = %d, file_tracer = %d\n",
			written, unwritten, file_tracer);
	}
}

/* Main driver function of the program */
double singleGPU_driver(char *inputfile, int error_threshold, char *filter_file_name, char *edit_file_name,
						int numThreads)
{
	int currentDevice = 0; // TODO: recognize this in cuda_config.c
	int i, j, tracer;
	FILE *in_file, *filter_file, *edit_file;
	int file_index = 0, pass_status = 0, undefined = 0, r_batch_size = 0;
	int num_blocks = 0, deviceCount = 0, maxThreadPerBlock = 0;
	long int batch_size = 0;
	int reader_status = SUCCESSFUL_MISSION, eof = 0; // remaining batch size
	RES_NODE *res_head = NULL, *current_node = NULL, *new_node = NULL;
	int loop_count = 0,  rejected = 0;
	int passer_counts[ERROR_THRESHOLD+1];
	double tstart_pre = 0, tstop_pre = 0,  tstart_exe = 0, tstop_exe = 0, t_exe = 0;
	double tstart_repo = 0, tstop_repo = 0;
	double time_io_start = 0, time_gkgpu_start = 0;
	time_io = 0, time_gkgpu = 0;

	// CUDA CONFIG: Calculation of GPU params
	// batch_size : # simultaneous operations per GPU 
	cuda_config_(&num_blocks, &batch_size, &deviceCount, 1, &maxThreadPerBlock, currentDevice);
	
	for (i = 0; i <= ERROR_THRESHOLD; i++){
		passer_counts[i] = 0;
	} 

	if (debug_mode == ON){
		tstart_pre = dtime(); // time
	}
	// Result Header 
	res_head = (RES_NODE *)malloc(sizeof(RES_NODE));
	if (res_head == NULL) {
		fprintf(stderr, "Results head formation error. Exiting...\n");
		exit(EXIT_FAILURE);
	} 
	res_head -> next = NULL;
	res_head -> filt_result = NULL;
	res_head -> edits = NULL;
	current_node = res_head;
	
	// Device Allocations    
	setDevice(currentDevice);
	singleDeviceAlloc(batch_size, currentDevice);

	// Multithreading operations
	int line_num = 0;
	char * line = NULL;
	size_t line_size = 0;
	int start_indices[numThreads];
	int finish_indices[numThreads];
	long int start_pos[numThreads];
	long int finish_pos[numThreads];
	long int file_size = 0, sample_size = 0;
	int thread;

	// Finding file size and comparison count
	time_io_start = dtime();
	in_file = fopen(inputfile, "r");
	if (getline(&line, &line_size, in_file) != -1){
		sample_size = ftell(in_file);
	}
	fseek(in_file, 0, SEEK_END);
	file_size = ftell(in_file);
	fclose(in_file);
	line_num = file_size / sample_size;
	int temp_loop_count = line_num / batch_size;
	r_batch_size = line_num - (batch_size * temp_loop_count);
	long int portion_size = batch_size / numThreads;
	long int last_portion_size = batch_size - (portion_size * (numThreads-1));
	time_io += dtime() - time_io_start;
	
	if (debug_mode == ON){
		tstop_pre = dtime();
		printf("---Time Report: Preprocessing took %10.3lf seconds.\n", tstop_pre);
	}
	
	// MAIN OPERATION
	tstart_exe = dtime();
	do {

		loop_count++;
		// Result node formation
		new_node = (RES_NODE *)malloc(sizeof(RES_NODE));
		if (new_node == NULL) {
			fprintf(stderr, "Node formation error. Exiting...\n");
			fclose(in_file);
			clearSingleDevice(res_head, currentDevice);
			exit(EXIT_FAILURE);
		} 
		new_node -> next = NULL;
		new_node -> filt_result = NULL;

		// Reading + Data Transfer to Unified Memory
		time_io_start = dtime();
		if (loop_count == temp_loop_count+1){
			eof = EOF_;
			if (r_batch_size != 0){
				reader_status = PARTIAL_BATCH;
				portion_size = r_batch_size / numThreads;
				last_portion_size = r_batch_size - (portion_size * (numThreads-1));
			}
			// fprintf(stderr, "entering r batch = %d\n", r_batch_size);
		}

		//File Padding
		#pragma omp parallel for num_threads(numThreads)
		for (thread = 0; thread < numThreads; thread++){
			start_indices[thread] = file_index + (portion_size * thread);
			finish_indices[thread] = start_indices[thread] + portion_size - 1;

			start_pos[thread] = (long int)start_indices[thread] * sample_size;
		}

		if (loop_count == temp_loop_count+1){
			finish_indices[numThreads-1] = line_num - 1;
		}
		finish_indices[numThreads-1] = file_index + batch_size - 1;
		start_pos[0] = file_index * sample_size;

		#pragma omp parallel for num_threads(numThreads) 
		for (thread = 0; thread < numThreads; thread++){

			// Local vars
			FILE *thr_file = fopen(inputfile, "r");
			fseek(thr_file, start_pos[thread], SEEK_SET);

			int thr_file_index = start_indices[thread];
			int thr_reader_status = 0, thr_eof = 0; // remaining batch size
			int thr_r_batch_size = 0;
			int thr_portion_size = portion_size;
			if (thread == numThreads - 1){
				thr_portion_size = last_portion_size;
			}

			thr_reader_status = reader(thr_file, thr_portion_size, &thr_eof, &thr_file_index, maxThreadPerBlock,
									&thr_r_batch_size, finish_indices[thread]+1, currentDevice, thread * portion_size);

			if (thr_reader_status == ABORT_MISSION){
				reader_status = ABORT_MISSION;
			}
			fclose(thr_file);
		}

		if (reader_status == ABORT_MISSION){
			fprintf(stderr, "\nError: Reader Status = ABORT MISSION, exiting...\n");
			clearSingleDevice(res_head, currentDevice);
			exit(EXIT_FAILURE);
		}
		time_io += dtime() - time_io_start;

		// New node binding for results node
		current_node -> next = new_node;
		current_node = current_node -> next;

		// Kernel Call
		time_gkgpu_start = dtime();
		if (reader_status == PARTIAL_BATCH){
			num_blocks = ceil((double)r_batch_size / (double)maxThreadPerBlock);
			int dummy_r_batch_size = r_batch_size;
			if (maxThreadPerBlock * num_blocks > dummy_r_batch_size){ 
				dummy_r_batch_size = maxThreadPerBlock * num_blocks;
			}
			new_node -> reader_status = PARTIAL_BATCH;
			new_node -> batch_size = r_batch_size;
			run_singleGPU_unified(dummy_r_batch_size, num_blocks,
				maxThreadPerBlock, new_node, currentDevice);
		}
		else {
			new_node -> reader_status = SUCCESSFUL_MISSION;
			new_node -> batch_size = batch_size;
			run_singleGPU_unified(batch_size, num_blocks,
				maxThreadPerBlock, new_node, currentDevice);
		}
		time_gkgpu += dtime() - time_gkgpu_start;
		file_index = file_index + (new_node -> batch_size);
		
	} while(eof != EOF_) ;// while : MAIN LOOP ends here

	tstop_exe = dtime();
	t_exe = tstop_exe - tstart_exe;
	if (debug_mode == ON){  
		printf("---Time Report: Main Operation took %10.3lf seconds.\n", t_exe);
	}
	res_head -> loop_count = loop_count;
	
	// REPORTING
	filter_file = fopen(filter_file_name, "w");
	edit_file = fopen(edit_file_name, "w");
	if (debug_mode == ON){
		tstart_repo = dtime();
	}
	reporter(filter_file, edit_file, batch_size, &res_head, file_index-1, 0, passer_counts,
		&rejected, &undefined);
	if (debug_mode == ON){
		tstop_repo = dtime();
		printf("---Time Report: Reporting took %10.3lf seconds.\n", tstop_repo - tstart_repo);
	}

	for (i = 0; i <= ERROR_THRESHOLD; i++){
		printf("passer[%d] = %d\n", i, passer_counts[i]);
	}

	printf(" Accepted = %d(undefined included), rejected = %d, + %d undefined\n", file_index - rejected,
		rejected, undefined);
	
	// Free remaining
	clearSingleDevice(res_head, currentDevice);
	fclose(filter_file);
	fclose(edit_file);

	longest_gpu_time = gpu_time[currentDevice];
	return t_exe;

}


/* main multi-GPU driver */
double multiGPU_driver(char *inputfile, int error_threshold, int deviceCount_cmd, char *filter_file_name,
	char *edit_file_name)
{
	// CUDA CONFIG: Calculation of GPU params
	int num_blocks = 0, deviceCount = 0, maxThreadPerBlock = 0;
	long int batch_size = 0;
	cuda_config_(&num_blocks, &batch_size, &deviceCount, deviceCount_cmd, &maxThreadPerBlock, -1);
	omp_set_num_threads(deviceCount); // creating CPU threads for GPU devices
	
	int i, j, tracer, device;
	FILE *in_file, *filter_file, *edit_file;
	int pass_status = 0, startIndex = 0;
	RES_NODE* res_head[deviceCount];
	RES_NODE* current_node[deviceCount];
	int rejected = 0, total_undefined = 0, undefined = 0;
	
	int line_num = 0;
	char * line = NULL;
	size_t line_size = 0;
	int start_indices[deviceCount];
	int finish_indices[deviceCount];
	long int start_pos[deviceCount];
	long int finish_pos[deviceCount];
	long int file_size = 0, sample_size = 0;
	int portion_size = 0;
	int passer_counts[ERROR_THRESHOLD+1];
	double tstart_pre = 0, tstop_pre = 0,  tstart_exe = 0, tstop_exe = 0, t_exe = 0;
	double tstart_repo = 0, tstop_repo = 0, long_t_exe;
	double  exe_times[deviceCount];
	
	if (debug_mode == ON){
		tstart_pre = dtime();
	}
	
	// INITS
	#pragma omp parallel for
	for (i = 0; i <= ERROR_THRESHOLD; i++){
		passer_counts[i] = 0;
	}

	#pragma omp parallel for
	for (device = 0; device < deviceCount; device++){
		// Result Headers
		res_head[device] = (RES_NODE *)malloc(sizeof(RES_NODE));
		if (res_head[device] == NULL) {
			fprintf(stderr, "Results head formation error. Exiting...\n");
			exit(EXIT_FAILURE);
		}
		res_head[device] -> next = NULL;
		res_head[device] -> filt_result = NULL;
		res_head[device] -> edits = NULL;

		exe_times[device] = 0;
	}

	// Device Allocations
	#pragma omp parallel for
	for (device = 0; device < deviceCount; device++){
		setDevice(device);
		singleDeviceAlloc(batch_size, device);
	}

	// Finding file size and comparison count
	in_file = fopen(inputfile, "r");
	if (getline(&line, &line_size, in_file) != -1){
		sample_size = ftell(in_file);
	}
	fseek(in_file, 0, SEEK_END);
	file_size = ftell(in_file);
	line_num = file_size / sample_size;
	fclose(in_file);
	
	// File Padding : Indices and positions are determined
	portion_size = line_num / deviceCount;
	
	#pragma omp parallel for
	for (device = 0; device < deviceCount; device++){
		start_indices[device] = portion_size * device;
		finish_indices[device] = start_indices[device] + portion_size - 1;

		start_pos[device] = start_indices[device] * sample_size;
		finish_pos[device] = (finish_indices[device]+1) * sample_size;
	}
	finish_pos[deviceCount-1] = file_size;
	finish_indices[deviceCount-1] = line_num - 1;
	start_pos[0] = 0;

	if (debug_mode == ON){
		tstop_pre = dtime();
		printf("---Cuda Config. Report: Number of filtrations/GPU = %d\n", portion_size);
		printf("---Time Report: Preprocessing took %10.3lf seconds.\n", tstop_pre - tstart_pre);
	}
	
	// MAIN OPERATION
	tstart_exe = dtime();
	#pragma omp parallel for
	for (device = 0; device < deviceCount; device++){

		setDevice(device);

		//Device Locals
		FILE *dev_file = fopen(inputfile, "r");
		fseek(dev_file, start_pos[device], SEEK_SET);
		current_node[device] = res_head[device];
		int dev_file_index = start_indices[device], dev_loop_count = 0;
		RES_NODE *new_node;
		int reader_status = 0, eof = 0; // remaining batch size
		int r_batch_size = 0;
		int dev_num_blocks = num_blocks;
		double  d_t_exe, d_tstop_exe;
		
		res_head[device] -> loop_count = ceil((double)(finish_indices[device] - start_indices[device]) / batch_size);

		do {

			// Result node formation
			new_node = (RES_NODE *)malloc(sizeof(RES_NODE));
			if (new_node == NULL) {
				fprintf(stderr, "Node formation error. Exiting...\n");
				fclose(in_file);
				clearSingleDevice(res_head[device], device);
				exit(EXIT_FAILURE);
			}
			new_node -> next = NULL;
			new_node -> filt_result = NULL;

			// Reading Procedure
			reader_status = reader(dev_file, batch_size, &eof, &dev_file_index, maxThreadPerBlock,
				&r_batch_size, finish_indices[device]+1, device, 0);

			if (reader_status == ABORT_MISSION){
				fclose(in_file);
				clearSingleDevice(res_head[device], device);
				exit(EXIT_FAILURE); // TODO : report exit status of each device and exit completely
			}

			// New node binding for results node
			current_node[device] -> next = new_node;
			current_node[device] = current_node[device] -> next;

			// Kernel Call
			if (reader_status == PARTIAL_BATCH){
				dev_num_blocks = ceil((double)r_batch_size / (double)maxThreadPerBlock);
				int dummy_r_batch_size = r_batch_size;
				if (maxThreadPerBlock * num_blocks > dummy_r_batch_size){
					dummy_r_batch_size = maxThreadPerBlock * dev_num_blocks;
				}
				new_node -> reader_status = PARTIAL_BATCH;
				new_node -> batch_size = (long int)r_batch_size;

				run_singleGPU_unified(dummy_r_batch_size, dev_num_blocks, maxThreadPerBlock, new_node, device);
			}
			else {
				new_node -> reader_status = SUCCESSFUL_MISSION;
				new_node -> batch_size = batch_size;

				run_singleGPU_unified(batch_size, dev_num_blocks, maxThreadPerBlock, new_node, device);
			}

			dev_loop_count++;
		} while(dev_loop_count < res_head[device] -> loop_count); // while : MAIN LOOP ends here

		// Timing calc for corresponding device
		d_tstop_exe = dtime();
		d_t_exe = d_tstop_exe - tstart_exe;
		exe_times[device] = d_t_exe;
		if (debug_mode == ON){
			printf("---Time Report: Dev %d Main Operation took %10.3lf seconds.\n", device, d_t_exe);
		}

		fclose(dev_file);

	} // for : device (main operation)
	
	// REPORTING
	filter_file = fopen(filter_file_name, "w");
	edit_file = fopen(edit_file_name, "w");

	long_t_exe = findLongest(exe_times, deviceCount); // Finding longest GPU process time
	if (kernel_analysis_mode == ON){
		longest_gpu_time = gpu_time[0];
		for (i = 1; i < deviceCount; i++){
			if (longest_gpu_time < gpu_time[i])
				longest_gpu_time = gpu_time[i];
		}
	}
	
	if (debug_mode == ON){
		printf("\n---Time Report: Longest time for single GPU is %10.3lf seconds.\n", long_t_exe);
		printf("---Filling files...%d comparisons in total.\n", finish_indices[deviceCount-1]+1);
		tstart_repo = dtime();
	}

	for (device = 0; device < deviceCount; device++){

		undefined = 0;
		reporter(filter_file, edit_file, batch_size, &res_head[device], 
			finish_indices[device], start_indices[device], passer_counts, &rejected, &undefined);

		if (debug_mode == ON){
			printf("---DEV = %d, start = %d, finish = %d, undef = %d\n", device, start_indices[device],
				finish_indices[device], undefined);
		}
		
		total_undefined += undefined;
	}
	if (debug_mode == ON){
		tstop_repo = dtime();
		printf("---Time Report: Reporting took %10.3lf seconds.\n", tstop_repo - tstart_repo);
	}
	
	for (i = 0; i <= ERROR_THRESHOLD; i++){
		printf("passer[%d] = %d\n", i, passer_counts[i]);
	}

	printf(" Accepted = %d(undefined included), rejected = %d, + %d undefined\n",
		finish_indices[deviceCount-1] + 1 - rejected, rejected, total_undefined);
	
	//Free remaining
	for (device = 0; device < deviceCount; device++){
		setDevice(device);
		clearSingleDevice(res_head[device], device);
	}
	
	fclose(filter_file);
	fclose(edit_file);

	return long_t_exe;

}

int main(int argc, char *argv[])
{

	if(argc < 5){
		fprintf(stderr, "Missing Argument, exiting...\n");
		exit(1);
	}

	else {

		char *inputfile = argv[1];
		int deviceCount_cmd = atoi(argv[2]);
		debug_mode = atoi(argv[3]); //in common.h
		int numThreads = atoi(argv[4]);
		kernel_analysis_mode = (debug_mode == 1 ? ON : OFF);
		if (debug_mode == ON){
			printf("---Debugging mode activated.\n");
		}

		FILE *file = fopen(inputfile, "r");
		if (file == NULL){
			fprintf(stderr, "Error: Input file does not exist or cannot be opened.\n");
			exit(1);
		}

		else
		{
		// Performance metrics
			double tstart, tstop, ttime, t_exe;
			fclose(file);

		// File Name Declaration
			char edit_dist[sizeof(int) * 4 + 1];
			char dev_cnt_str[sizeof(int) * 4 + 1];
			sprintf(edit_dist, "%d", ERROR_THRESHOLD);
			sprintf(dev_cnt_str, "%d", deviceCount_cmd);

			char filter_file_name[42];
			strcpy(filter_file_name, "gateKeeperGPU_");
			strcat(filter_file_name, dev_cnt_str);
			strcat(filter_file_name, "G_filter_output_err");
			strcat(filter_file_name, edit_dist);
			strcat(filter_file_name, ".txt");

			char edit_file_name[40];
			strcpy(edit_file_name, "gateKeeperGPU_");
			strcat(edit_file_name, dev_cnt_str);
			strcat(edit_file_name, "G_edit_output_err");
			strcat(edit_file_name, edit_dist); 
			strcat(edit_file_name, ".txt");

			if (deviceCount_cmd > MAX_GPU){
				printf("\tMaximum GPU support is %d\n", MAX_GPU);
				printf("\tPlease change 'MAX_GPU' variable in makefile if you want to use more GPU's.\n");
				printf("\tExiting...\n");
				exit(1);
			}
			else if (deviceCount_cmd == 1){
				exe_mode = SINGLE;
				printf("\nRunning on single GPU mode, device_encoded...\n");
				printf("Execution begins with error threshold %d, %d threads\n", ERROR_THRESHOLD, numThreads);
				tstart = dtime();
				t_exe = singleGPU_driver(inputfile, ERROR_THRESHOLD, filter_file_name, edit_file_name, numThreads);
				tstop = dtime();
			}
			else {
				exe_mode = MULTIPLE;
				printf("\nRunning on multi-GPU mode(%d), device_encoded...\n", deviceCount_cmd);
				printf("Execution begins with error threshold %d...\n", ERROR_THRESHOLD);
				tstart = dtime();
				t_exe = multiGPU_driver(inputfile, ERROR_THRESHOLD, deviceCount_cmd, filter_file_name, edit_file_name);
				tstop = dtime();
			}

			ttime = tstop - tstart;
			if (kernel_analysis_mode == ON){
				// measures filtration from kernel side, cuda-calculated
				printf("\n0-Kernel filtration time (msecs): %f\n", longest_gpu_time); 
			}
			printf("\n1-GateKeeper-GPU time (from host) (secs): %10.3lf\n", time_gkgpu);	
			printf("2-IO reading + data transfer time (secs): %10.3lf\n", time_io); // data transfer = filling unified memory buffers
			printf("3-Total filtration time (secs): %10.3lf\n", t_exe); 	//measures filtration from host side, wraps the while loop of all batches: edlib included
			printf("4-Overall execution time (secs): %10.3lf\n", ttime);
		} // else : file check
	}

	return 0;
}
