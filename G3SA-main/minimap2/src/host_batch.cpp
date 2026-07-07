#include "gasal.h"
#include "args_parser.h"
#include "interfaces.h"
#include "host_batch.h"




// Functions for host batches handling. 

host_batch_t *gasal_host_batch_new(uint32_t batch_bytes, uint32_t offset)
{
	cudaError_t err;
	host_batch_t *res = (host_batch_t *)calloc(1, sizeof(host_batch_t));
	CHECKCUDAERROR(cudaHostAlloc(&(res->data), batch_bytes*sizeof(uint8_t), cudaHostAllocDefault));
	res->page_size = batch_bytes;
	res->data_size = 0;
	res->is_locked = 0;
	res->offset = offset;
	res->next = NULL;
	return res;
}

void gasal_host_batch_destroy(host_batch_t *res)
{
	cudaError_t err;
	if (res==NULL)
	{
		fprintf(stderr, "[GASAL ERROR] Trying to free a NULL pointer\n");
		exit(1);
	}
	// recursive function to destroy all the linked listgasal_res_destroy_host
	if (res->next != NULL)
		gasal_host_batch_destroy(res->next);
	if (res->data != NULL) 
	{
		CHECKCUDAERROR(cudaFreeHost(res->data));
	}
	
	free(res);
}

host_batch_t *gasal_host_batch_getlast(host_batch_t *arg)
{
	return (arg->next == NULL ? arg : gasal_host_batch_getlast(arg->next) );
	
}

void gasal_host_batch_reset(gasal_gpu_storage_t *gpu_storage)
{
	// reset all batch idx and data occupation
	host_batch_t *cur_page = NULL;
	for(int i = 0; i < 2; i++) {

		switch(i) {
			case 0:
				cur_page = (gpu_storage->extensible_host_unpacked_query_batch);
			break;
			case 1:
				cur_page = (gpu_storage->extensible_host_unpacked_target_batch);
			break;
			default:
			break;
		}
		while(cur_page != NULL)
		{
			cur_page->data_size = 0;
			cur_page->offset = 0;
			cur_page->is_locked = 0;
			cur_page = cur_page->next;
		}
	}
	//fprintf(stderr, "[GASAL INFO] Batch reset.\n");

}


// TODO: make a template... now that you started to go the C++/template way, just stick to it.
uint32_t gasal_host_batch_fill(gasal_gpu_storage_t *gpu_storage, uint32_t idx, const char* data, uint32_t size, data_source SRC)
{
	// since query and target are very symmetric here, we use pointers to route the data where it has to, 
	// while keeping the actual memory management 'source-agnostic'.

	host_batch_t *cur_page = NULL;
	uint32_t *p_batch_bytes = NULL;

	switch(SRC) {
		case QUERY:
			cur_page = (gpu_storage->extensible_host_unpacked_query_batch);
			p_batch_bytes = &(gpu_storage->host_max_query_batch_bytes);
		break;
		case TARGET:
			cur_page = (gpu_storage->extensible_host_unpacked_target_batch);
			p_batch_bytes = &(gpu_storage->host_max_target_batch_bytes);
		break;
		default:
		break;
	}
	
	int nbr_N = 0;
	while((size+nbr_N)%8)
		nbr_N++;

	while(cur_page->is_locked)
		cur_page = cur_page->next;

	if (cur_page->next == NULL && cur_page->page_size - cur_page->data_size < size + nbr_N)
	{
		/*
		fprintf(stderr,"[GASAL WARNING:] Trying to write %d bytes while only %d remain (%s) (block size %d, filled %d bytes).\n                 Allocating a new block of size %d, total size available reaches %d. Doing this repeadtedly slows down the execution.\n",
				size + nbr_N,
				cur_page->page_size - cur_page->data_size,
				(SRC == QUERY ? "query":"target"),
				cur_page->page_size,
				cur_page->data_size,
				cur_page->page_size * 2,
				*p_batch_bytes + cur_page->page_size * 2);
		*/
		host_batch_t *res = gasal_host_batch_new(cur_page->page_size * 2, cur_page->offset + cur_page->data_size);
		cur_page->next = res;
		cur_page->is_locked = 1;
		*p_batch_bytes = *p_batch_bytes + cur_page->page_size * 2;

		cur_page = cur_page->next;
		//fprintf(stderr, "CREATED: "); gasal_host_batch_print(cur_page);
	}
	
	if (cur_page->next != NULL && cur_page->page_size - cur_page->data_size < size + nbr_N)
	{
		// re-write offset for the next page to correspond to what has been filled on the current page.
		cur_page->next->offset = cur_page->offset + cur_page->data_size;
		cur_page->is_locked = 1;
		// then, jump to next page
		cur_page = cur_page->next;
	}


	if (cur_page->page_size - cur_page->data_size >= size + nbr_N)
	{
		// fprintf(stderr, "FILL: "); gasal_host_batch_print(cur_page);
		memcpy(&(cur_page->data[idx - cur_page->offset]), data, size);

		for(int i = 0; i < nbr_N; i++)
		{
			cur_page->data[idx + size - cur_page->offset + i] = N_CODE;
		}
		idx = idx + size + nbr_N;

		cur_page->data_size += size + nbr_N;
		//is_done = 1;
	}

	return idx;
}


uint32_t gasal_host_batch_addbase(gasal_gpu_storage_t *gpu_storage, uint32_t idx, const char base, data_source SRC )
{	 
    return gasal_host_batch_add(gpu_storage, idx, &base, 1, SRC );
}


uint32_t gasal_host_batch_add(gasal_gpu_storage_t *gpu_storage, uint32_t idx, const char *data, uint32_t size, data_source SRC )
{	

	// since query and target are very symmetric here, we use pointers to route the data where it has to, 
	// while keeping the actual memory management 'source-agnostic'.
	host_batch_t *cur_page = NULL;
	uint32_t *p_batch_bytes = NULL;
	

	switch(SRC) {
		case QUERY:
			cur_page = (gpu_storage->extensible_host_unpacked_query_batch);
			p_batch_bytes = &(gpu_storage->host_max_query_batch_bytes);
		break;
		case TARGET:
			cur_page = (gpu_storage->extensible_host_unpacked_target_batch);
			p_batch_bytes = &(gpu_storage->host_max_target_batch_bytes);
		break;
		default:
		break;
	}

	int is_done = 0;

	while (!is_done)
	{
		if (*p_batch_bytes >= idx + size && (cur_page->next == NULL || (cur_page->next->offset >= idx + size)) )
		{

			memcpy(&(cur_page->data[idx - cur_page->offset]), data, size);
			idx = idx + size;
			is_done = 1;

		} else if ((*p_batch_bytes >= idx + size) && (cur_page->next != NULL) && (cur_page->next->offset < idx + size)) {
		
			cur_page = cur_page->next;

		} else {
			/*
			fprintf(stderr,"[GASAL WARNING:] Trying to write %d bytes at position %d on host memory (%s) while only  %d bytes are available. Therefore, allocating %d bytes more on CPU. Repeating this many times can provoke a degradation of performance.\n",
					size,
					idx,
					(SRC == QUERY ? "query":"target"),
					*p_batch_bytes,
					*p_batch_bytes * 2);
			*/
	
			*p_batch_bytes += *p_batch_bytes;

			// corner case: if we allocated less than a single sequence length to begin with... it shouldn't be allowed actually, but at least it's caught here.
			while (*p_batch_bytes < size)
				*p_batch_bytes += *p_batch_bytes;

			host_batch_t *res = gasal_host_batch_new(*p_batch_bytes, idx);
	
			cur_page->next = res;
			
			cur_page = cur_page->next;
		}
	}
	//gasal_host_batch_printall(gasal_host_batch_getlast(cur_page));
	return idx;
}



// this printer displays the whole sequence. It is heavy and shouldn't be called when you have more than a couple sequences.
void gasal_host_batch_print(host_batch_t *res) 
{
	fprintf(stderr, "[GASAL PRINT] Page data: offset=%d, next_offset=%d, data size=%d, page size=%d\n", 
					res->offset, (res->next != NULL? res->next->offset : -1), res->data_size, res->page_size);
}

// this printer allows to see the linked list easily.
void gasal_host_batch_printall(host_batch_t *res)
{	
	fprintf(stderr, "[GASAL PRINT] Page data: offset=%d, next_offset=%d, data size=%d, page size=%d\n", 
					res->offset, (res->next != NULL? res->next->offset : -1), res->data_size, res->page_size);
	if (res->next != NULL)
	{
		fprintf(stderr, "+--->");
		gasal_host_batch_printall(res->next);
	}
}
