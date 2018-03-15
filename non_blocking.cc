
#include <cassert>
#include <cstring>

#include "non_blocking.hh"
#include "memory.hh"
#include "processor.hh"
#include "util.hh"

// int numberOfWays;
// int numberOfMSHR;

NonBlockingCache::NonBlockingCache(int64_t size, Memory& memory, Processor& processor, int ways, int mshrs)
    : SetAssociativeCache(size, memory, processor, ways),
    tagBits(processor.getAddrSize() - log2int((size / memory.getLineSize())/ways) - memory.getLineBits()), // Tag bits = Processor Size - # of Sets - Offset
    // # of Sets = # of Lines / ways
    // # of Lines = Cache Size / Line Size
    indexMask( ( ( size / memory.getLineSize() ) / ways ) - 1 ), // Index mask = 1 for each digit of Set # i.e. 32 sets = 11111
    tagArray( ( size / memory.getLineSize() ), 2, tagBits ), // Tag Array is # of Lines, Valid Bit, Dirty Bit, Tag bits
    dataArray(  ( size / memory.getLineSize() ), memory.getLineSize() ), // Data Array is # of Lines, Line Size
    blocked(false),
	mshrTable({ nullptr, nullptr, nullptr, nullptr, nullptr })
{
    assert(mshrs > 0);
    numberOfWays = ways;
	numberOfMSHR = mshrs;

	// malloc space for MHRS arrays
	mshrTable.savedId = (int *) malloc(sizeof(int) * mshrs);
	mshrTable.savedAddr = (uint64_t *) malloc(sizeof(uint64_t) * mshrs);
	mshrTable.savedSize = (int *) malloc(sizeof(int) * mshrs);
	mshrTable.savedData = (uint8_t **) malloc(sizeof(uint8_t*) * mshrs);
	mshrTable.savedSetLineIndex = (int *) malloc(sizeof(int) * mshrs);

	// initialize MHRS array values to defaults
	for (int i = 0; i < mshrs; i++)
	{
		mshrTable.savedId[i] = -1;
		mshrTable.savedAddr[i] = 0;
		mshrTable.savedSize[i] = 0;
		mshrTable.savedData[i] = nullptr;
		mshrTable.savedSetLineIndex[i] = -1;
	}
}

// ADDRESS BREAKDOWN
int64_t NonBlockingCache::getIndex(uint64_t address) // Returns Set # off Address
{
	return (address >> memory.getLineBits()) & indexMask; // Shift off Offset and mask by how many bits make Set #
}

int NonBlockingCache::getBlockOffset(uint64_t address) // Returns Offset off Address
{
	return address & (memory.getLineSize() - 1); // Mask by how many bits make Offset
}

uint64_t NonBlockingCache::getTag(uint64_t address) // Returns Tag off Address
{
	return address >> (processor.getAddrSize() - tagBits); // Shifts off any bits that aren't tag bits
}

NonBlockingCache::~NonBlockingCache()
{
	// free malloced space for MHRS table
	free(mshrTable.savedId);
	free(mshrTable.savedAddr);
	free(mshrTable.savedSize);
	free(mshrTable.savedData);
	free(mshrTable.savedSetLineIndex);
}

int NonBlockingCache::evictedLineIndex()
{
	return (int) rand() % numberOfWays;
}

bool NonBlockingCache::receiveRequest(uint64_t address, int size, const uint8_t* data, int request_id)
{
	assert(size <= memory.getLineSize()); // within line size									  
	assert(address < ((uint64_t)1 << processor.getAddrSize())); // within address range
	assert((address &  (size - 1)) == 0); // naturally aligned

	if (blocked) 
	{
		DPRINT("Cache is blocked!"); // Cache is currently blocked, so it cannot receive a new request
		return false;
	}

	int setLine = hit(address); // Check if Hit;  SetLine = line in Set if hit OR -1 if miss

	if (setLine >= 0) // HIT
	{
		DPRINT("Hit in cache");
		uint8_t* line = dataArray.getLine(setLine); // line is the Address of the data of Set Line

		int block_offset = getBlockOffset(address); // Offset

		if (data) // WRITE
		{
			memcpy(&line[block_offset], data, size); // Write data into the address of the line
			sendResponse(request_id, nullptr); 
			tagArray.setState(setLine, Dirty); // Set Set line to dirty
		}
		else // READ
		{
			sendResponse(request_id, &line[block_offset]);
		}
	}
	else // MISS
	{
		DPRINT("Miss in cache");
		setLine = dirty(address); // -1 if all lines of Set Dirty, index of Clean Line otherwise
		if (setLine < 0) // Every line in Set is Dirty
		{
			DPRINT("Dirty, writing back");
			// EVICTION
			setLine = (getIndex(address) * numberOfWays) + evictedLineIndex(); // SetLine is set to the evicted line
			uint8_t* line = dataArray.getLine(setLine); // line points to data of the evicted line
			// Calculate Writeback Address
			uint64_t wb_address = tagArray.getTag(setLine) << (processor.getAddrSize() - tagBits); // Sets Tag of Writeback Address to Tag of Set Line
			wb_address |= (getIndex(address) << memory.getLineBits()); // Sets Index to Set # of Address
			// No Offset
			sendMemRequest(wb_address, memory.getLineSize(), line, request_id); // Writeback: Sends request to memory for memory receive/write Line of data
		}
		tagArray.setState(setLine, Invalid); // Marks the Set Line as empty (either from eviction or already empty)
		uint64_t block_address = address & ~(memory.getLineSize() - 1); // block_address = address without offset
		sendMemRequest(block_address, memory.getLineSize(), nullptr, request_id); // Request to memory to get the data of block address bringing in each line of offset
		
		int availableMSHR = findEmptyMSHR();
		// remember the CPU's request id
		mshrTable.savedId[availableMSHR] = request_id;
		// Remember the address
		mshrTable.savedAddr[availableMSHR] = address;
		// Remember the data if it is a write.
		mshrTable.savedSize[availableMSHR] = size;
		//mshrTable.savedData[availableMSHR] = (uint8_t*) &data;
		mshrTable.savedData[availableMSHR] = (uint8_t*) data;
		mshrTable.savedSetLineIndex[availableMSHR] = setLine;
		if (availableMSHR == numberOfMSHR - 1) // If the MSHR used was the last available MSHR on the table block the cache.
		{
			// Mark the cache as blocked
			blocked = true; // The Cache is blocked while it is waiting for data from Memory
		}
	}
	// Memory request was accepted
	return true;
}

void NonBlockingCache::receiveMemResponse(int request_id, const uint8_t* data)
{
	assert(data);

	int MSHRTableIndex = 0;
	for (int MSHRIndex = 0; MSHRIndex < numberOfMSHR; MSHRIndex++) // For every MSHR in the MSHR Table
	{
		if (request_id == mshrTable.savedId[MSHRIndex]) // If the requested ID is the same as the ID saved in the MSHR Index
		{
			MSHRTableIndex = MSHRIndex; // Set the examined index to the one that matches ID
		}
	}

	int index = mshrTable.savedSetLineIndex[MSHRTableIndex]; // Index = to setLine from receiveMemRequest

	// Copy the data into the cache.
	uint8_t* line = dataArray.getLine(index);
	memcpy(line, data, memory.getLineSize());

	//assert(tagArray.getState(index) == Invalid);

	// Mark valid
	tagArray.setState(index, Valid);

	// Set tag
	tagArray.setTag(index, getTag(mshrTable.savedAddr[MSHRTableIndex]));

	// Treat as a hit
	int block_offset = getBlockOffset(mshrTable.savedAddr[MSHRTableIndex]);

	if (mshrTable.savedData[MSHRTableIndex]) 
	{
		// if this is a write, copy the data into the cache.
		memcpy(&line[block_offset], (void**)mshrTable.savedData[MSHRTableIndex], mshrTable.savedSize[MSHRTableIndex]);
		sendResponse(mshrTable.savedId[MSHRTableIndex], nullptr);
		// Mark dirty
		tagArray.setState(index, Dirty);
	}
	else 
	{
		// This is a read so we need to return data
		sendResponse(mshrTable.savedId[MSHRTableIndex], &line[block_offset]);
	}

	// Default Conditions
	blocked = false; // Can write/read from cache again
	mshrTable.savedId[MSHRTableIndex] = -1;
	mshrTable.savedAddr[MSHRTableIndex] = 0;
	mshrTable.savedSize[MSHRTableIndex] = 0;
	mshrTable.savedData[MSHRTableIndex] = nullptr;
	mshrTable.savedSetLineIndex[MSHRTableIndex] = -1;

}


// HIT
int NonBlockingCache::hit(uint64_t address)
{
	uint64_t incomingTag = getTag(address);
	int index = getIndex(address); // Grab set # from address
	int LineIndex = (index * numberOfWays); // Find line index for Tag Array
	
	for (int SetIndex = 0; SetIndex < numberOfWays; SetIndex++) // For every line in the Set
	{
		State state = (State)tagArray.getState(LineIndex + SetIndex); // Grab state of line in Set
		uint64_t line_tag = tagArray.getTag(LineIndex + SetIndex); // Grab Tag of line in Set
		if (((state == Valid) || (state == Dirty)) && (line_tag == incomingTag)) // If state is Valid and Tags match, it's a hit
		{
			return (LineIndex + SetIndex); // The incoming address has the same tag as a line in the set then it's a hit; return the line in Set
		}
	}
	return -1; // Return -1 if Miss
}

// Check for Dirty
int NonBlockingCache::dirty(uint64_t address)
{
	int index = getIndex(address);
	int LineIndex = (index * numberOfWays); // Find index of set in Tag Array

	for (int SetIndex = 0; SetIndex < numberOfWays; SetIndex++) // For every line in the Set
	{
		State state = (State)tagArray.getState(LineIndex + SetIndex); // Grab state of line in Set
		if (state != Dirty) // If the line is not dirty
		{
			return (LineIndex + SetIndex); // Return the clean Line
		}
	}
	return -1; // Every Line in Set is Dirty
}

int NonBlockingCache::findEmptyMSHR()
{
	for (int MSHRSlot = 0; MSHRSlot < numberOfMSHR - 1; MSHRSlot++) // Check every slot of MSHR Table
	{
		if ((mshrTable.savedId[MSHRSlot] == -1) // If Slot in MSHR Table is set at default conditions (aka empty)
			&& (mshrTable.savedAddr[MSHRSlot] == 0)
			&& (mshrTable.savedSize[MSHRSlot] == 0)
			&& (mshrTable.savedData[MSHRSlot] == nullptr)
			&& (mshrTable.savedSetLineIndex[MSHRSlot] == -1))
		{
			return MSHRSlot; // Return the empty slot
		}
	}
	return numberOfMSHR - 1; // Return the last slot of MSHR Table so that cache is blocked if MSHR Table is full.
}




