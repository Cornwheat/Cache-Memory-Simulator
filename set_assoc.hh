#ifndef CSIM_SET_ASSOC_H
#define CSIM_SET_ASSOC_H

#include "cache.hh"
#include "tag_array.hh"
#include "sram_array.hh"

class SetAssociativeCache : public Cache
{
public:
	/**
	* @param size is the *total* size of the cache in bytes
	* @param the memory that is below this cache
	* @param processor this cache is connected to
	* @param the number of ways in this set associative cache. If the number
	*        of ways cannot be realized, this will cause an error
	*/
	SetAssociativeCache(int64_t size, Memory& memory, Processor& processor, int ways);

	/**
	* Destructor
	*/
	~SetAssociativeCache() override;

	/**
	* Called when the processors sends load or store request.
	* All requests can be assummed to be naturally aligned (e.g., a 4 byte
	* request will be aligned to a 4 byte boundary)
	*
	* @param address of the request
	* @param size in bytes of the request.
	* @param data is non-null, then this is a store request.
	* @param request_id the id that must be used when replying to this request
	*
	* @return true if the request can be received, false if the cache is
	*         blocked and the request must be retried later.
	*/
	virtual bool receiveRequest(uint64_t address, int size, const uint8_t* data, int request_id) override;

	/**
	* Called when memory id finished processing a request.
	* Data will always be the length of memory.getLineSize()
	*
	* @param request_id is the id assigned to this request in sendMemRequest
	* @param data is the data from memory (length of data is line length)
	*        NOTE: This pointer will be invalid when this function returns.
	*/
	virtual void receiveMemResponse(int request_id, const uint8_t* data) override;

private:
	/// Put any code you want here.
	enum State 
	{
		Invalid = 0,
		Valid = 1,
		Invalid2 = 2,
		Dirty = 3 // Dirty implies valid
	};

	/**
	* @return the index number for the given address
	*/
	int64_t getIndex(uint64_t address);

	/**
	* @return the block offset for the given address
	*/
	int getBlockOffset(uint64_t address);

	/**
	* @return the tag for the given address
	*/
	uint64_t getTag(uint64_t address);

	/**
	* @return matching line if there is a hit. -1 if miss
	*/
	int hit(uint64_t address);

	int evictedLineIndex();

	/**
	* @return clean line if there is a clean spot. -1 if all lines in set are dirty
	*/
	int dirty(uint64_t address);

	/// Number of Ways
	int numberOfWays;

	/// Number of tag bits in the address
	int64_t tagBits;

	/// Mask for getting the index bits
	uint64_t indexMask;

	/// The cache's tag array
	TagArray tagArray;

	/// The cache's data array
	SRAMArray dataArray;

	/// If true, the cache is currently blocked
	bool blocked;

	struct MSHR 
	{
		/// This is the current request_id that is blocking the cache.
		int savedId;

		/// The address for the blocking request.
		uint64_t savedAddr;

		/// This is the size of the original request. Needed for writes.
		int savedSize;

		/// This is the data that will be written after a miss
		const uint8_t* savedData;
		
		/// Saves Set Line Index
		int savedSetLineIndex;

	};

	MSHR mshr;
};

#endif // CSIM_SET_ASSOC_H