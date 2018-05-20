#include <cassert>
#include <cstring>

#include "memory.hh"
#include "processor.hh"
#include "util.hh"
#include "set_assoc.hh"

int numberOfWays;

// CACHE SETUP
SetAssociativeCache::SetAssociativeCache(int64_t size, Memory& memory, Processor& processor, int ways) : 
	Cache(size, memory, processor),
	tagBits(processor.getAddrSize() - log2int((size / memory.getLineSize())/ways) - memory.getLineBits()), // Tag bits = Processor Size - # of Sets - Offset
	// # of Sets = # of Lines / ways
	// # of Lines = Cache Size / Line Size
	indexMask( ( ( size / memory.getLineSize() ) / ways ) - 1 ), // Index mask = 1 for each digit of Set # i.e. 32 sets = 11111
	tagArray( ( size / memory.getLineSize() ), 2, tagBits ), // Tag Array is # of Lines, Valid Bit, Dirty Bit, Tag bits
	dataArray(  ( size / memory.getLineSize() ), memory.getLineSize() ), // Data Array is # of Lines, Line Size
	blocked(false),
	mshr({ -1,0,0,nullptr,-1 })
{
	assert(ways > 0);
	numberOfWays = ways;
}

// ADDRESS BREAKDOWN
int64_t SetAssociativeCache::getIndex(uint64_t address) // Returns Set # off Address
{
	return (address >> memory.getLineBits()) & indexMask; // Shift off Offset and mask by how many bits make Set #
}

int SetAssociativeCache::getBlockOffset(uint64_t address) // Returns Offset off Address
{
	return address & (memory.getLineSize() - 1); // Mask by how many bits make Offset
}

uint64_t SetAssociativeCache::getTag(uint64_t address) // Returns Tag off Address
{
	return address >> (processor.getAddrSize() - tagBits); // Shifts off any bits that aren't tag bits
}

SetAssociativeCache::~SetAssociativeCache()
{

}

int SetAssociativeCache::evictedLineIndex()
{
	return (int) rand() % numberOfWays;
}

bool SetAssociativeCache::receiveRequest(uint64_t address, int size, const uint8_t* data, int request_id)
{
	assert(size <= memory.getLineSize()); // within line size									  
	assert(address < ((uint64_t)1 << processor.getAddrSize())); // within address range
	assert((address &  (size - 1)) == 0); // naturally aligned

	if (blocked) {
		DPRINT("Cache is blocked!"); // Cache is currently blocked, so it cannot receive a new request
		return false;
	}

	int setLine = hit(address); // Check if Hit;  SetLine = line in Set if hit OR -1 if miss

	if (setLine >= 0) { // HIT
		DPRINT("Hit in cache");
		uint8_t* line = dataArray.getLine(setLine); // line is the Address of the data of Set Line

		int block_offset = getBlockOffset(address); // Offset

		if (data) {  // WRITE
			memcpy(&line[block_offset], data, size); // Write data into the address of the line
			sendResponse(request_id, nullptr); 
			tagArray.setState(setLine, Dirty); // Set Set line to dirty
		}
		else { // READ
			sendResponse(request_id, &line[block_offset]);
		}
	}
	else { // MISS
		DPRINT("Miss in cache");
		setLine = dirty(address); // -1 if all lines of Set Dirty, index of Clean Line otherwise
		if (setLine < 0) {  // Every line in Set is Dirty
			DPRINT("Dirty, writing back");
			// EVICTION
			setLine = (getIndex(address) * numberOfWays) + evictedLineIndex(); // SetLine is set to the evicted line
			uint8_t* line = dataArray.getLine(setLine); // line points to data of the evicted line
			// Calculate Writeback Address
			uint64_t wb_address = tagArray.getTag(setLine) << (processor.getAddrSize() - tagBits); // Sets Tag of Writeback Address to Tag of Set Line
			wb_address |= (getIndex(address) << memory.getLineBits()); // Sets Index to Set # of Address
			// No Offset
			sendMemRequest(wb_address, memory.getLineSize(), line, -1); // Sends request to memory to receive Line of data
		}
		tagArray.setState(setLine, Invalid); // Marks the Set Line as empty (either from eviction or already empty)
		uint64_t block_address = address & ~(memory.getLineSize() - 1); // block_address = address without offset
		sendMemRequest(block_address, memory.getLineSize(), nullptr, 0); // Request from memory the data of block address bringing in each line of offset
		
		// remember the CPU's request id
		mshr.savedId = request_id;
		// Remember the address
		mshr.savedAddr = address;
		// Remember the data if it is a write.
		mshr.savedSize = size;
		mshr.savedData = data;
		mshr.savedSetLineIndex = setLine;
		// Mark the cache as blocked
		blocked = true; // The Cache is blocked while it is waiting for data from Memory
	}
	// Memory request was accepted
	return true;
}

void SetAssociativeCache::receiveMemResponse(int request_id, const uint8_t* data)
{
	assert(request_id == 0);
	assert(data);

	int index = mshr.savedSetLineIndex; // Index = to setLine from receiveMemRequest

	// Copy the data into the cache.
	uint8_t* line = dataArray.getLine(index);
	memcpy(line, data, memory.getLineSize());

	assert(tagArray.getState(index) == Invalid);

	// Mark valid
	tagArray.setState(index, Valid);

	// Set tag
	tagArray.setTag(index, getTag(mshr.savedAddr));

	// Treat as a hit
	int block_offset = getBlockOffset(mshr.savedAddr);

	if (mshr.savedData) {
		// if this is a write, copy the data into the cache.
		memcpy(&line[block_offset], mshr.savedData, mshr.savedSize);
		sendResponse(mshr.savedId, nullptr);
		// Mark dirty
		tagArray.setState(index, Dirty);
	}
	else {
		// This is a read so we need to return data
		sendResponse(mshr.savedId, &line[block_offset]);
	}

	// Default Conditions
	blocked = false; // Can write/read from cache again
	mshr.savedId = -1;
	mshr.savedAddr = 0;
	mshr.savedSize = 0;
	mshr.savedData = nullptr;
	mshr.savedSetLineIndex = -1;
}

// HIT
int SetAssociativeCache::hit(uint64_t address)
{
	uint64_t incomingTag = getTag(address);
	int index = getIndex(address); // Grab set # from address
	int LineIndex = (index * numberOfWays); // Find line index for Tag Array
	
	for (int SetIndex = 0; SetIndex < numberOfWays; SetIndex++) { // For every line in the Set
		State state = (State)tagArray.getState(LineIndex + SetIndex); // Grab state of line in Set
		uint64_t line_tag = tagArray.getTag(LineIndex + SetIndex); // Grab Tag of line in Set
		if (((state == Valid) || (state == Dirty)) && (line_tag == incomingTag)) { // If state is Valid and Tags match, it's a hit
			return (LineIndex + SetIndex); // The incoming address has the same tag as a line in the set then it's a hit; return the line in Set
		}
	}
	return -1; // Return -1 if Miss
}

// Check for Dirty
int SetAssociativeCache::dirty(uint64_t address)
{
	int index = getIndex(address);
	int LineIndex = (index * numberOfWays); // Find index of set in Tag Array

	for (int SetIndex = 0; SetIndex < numberOfWays; SetIndex++) { // For every line in the Set
		State state = (State)tagArray.getState(LineIndex + SetIndex); // Grab state of line in Set
		if (state != Dirty) {  // If the line is not dirty
			return (LineIndex + SetIndex); // Return the clean Line
		}
	}
	return -1; // Every Line in Set is Dirty
}
