#ifndef _C_SEARCH_SCALAR_H
#define _C_SEARCH_SCALAR_H
#include "Defs.h"
#include "CFastaSWScalar.h"
#include "CParams.h"
#include "CSearch.h"

#define DEFAULT_CUDA_ARRAY_WIDTH			7680

class SSEThreadParams;
class CSearchScalar: public CSearch
{
public:
	CSearchScalar(CParams* params);
	virtual ~CSearchScalar();

protected:
	virtual int loaddb(char* dbFile);
	virtual int dbsearch(char* query);

private:
	/*4-lane vector computing*/
	Database<uint4>* createInterDBQuad(CFastaSW* cudasw, DatabaseHash *hash,
			int initWidth = DEFAULT_CUDA_ARRAY_WIDTH >> 2);
	void loadInterDBQuad(CFastaSW* cudasw, DatabaseHash* hashQuad,
			Database<uint4>* db);
	void unloadInterDBQuad(CFastaSW* cudasw);

};
#endif

