#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define WORD_SIZE_BITS 32
#define BYTE_SIZE 8
#define WORD_SIZE_BYTES (WORD_SIZE_BITS / BYTE_SIZE)
#define CACHE_SIZE 1024
#define CACHE_BLOCK_SIZE 64
#define MEMORY_SIZE 65536
//ASSOCIATIVITY constant = number of blocks per set
#define ASSOCIATIVITY 2

#define MEMORY_CAP 500000
#define WORD_HIT 50
#define WORD_MISS -50

#define CACHE_TOO_LARGE_ERROR -100
#define INVALID_MEMORY_ADDRESS -200

enum WriteStyle {WRITE_THROUGH, WRITE_BACK};

typedef struct {
    int set;
    int tag;
    int clean;
    int valid;
    char byte_array[CACHE_BLOCK_SIZE];
} Block;

typedef struct {
    int size;
    int block_size;
    int numBlocks;
    Block** blocks;
    int associativity;
    enum WriteStyle write_style;

    // Meta info
    int tag_length;
    int index_length;
    int block_offset_length;
} Cache;

typedef struct {
    char bytes[WORD_SIZE_BYTES];
} Word;

typedef struct {
    int maxSize;
    int* array;
} Queue;

Queue* newQueue(int maxSize)
{
    Queue *queue = malloc(sizeof(Queue));
    queue->maxSize = maxSize;
    queue->array = malloc(sizeof(int) * maxSize);
    for (int i=0; i<maxSize; i++)
    {
        queue->array[i] = -1;
    }
    return queue;
}

int addTagQueue(Queue* queue, int tag)
{
    int oldestTag = queue->array[0];

    // Let the index default to 0, since the first item will be the one replaced if
    // the item is not within the array
    int itemIndex = 0;
    for (int i=0; i<queue->maxSize; i++)
    {
        if (queue->array[i] == tag)
        {
            itemIndex = i;
            break;
        }
    }

    // Shift the array over 1
    for (int i=itemIndex+1; i<queue->maxSize; i++)
    {
        queue->array[i-1] = queue->array[i];
    }
    // Set the back of the array to the tag
    queue->array[queue->maxSize-1] = tag;

    // Return -1 if the queue was not full
    if (oldestTag == -1)
    {
        return -1;
    }
    // Only return the oldest tag (first entry) if it was the one evicted
    return oldestTag != queue->array[0] ? oldestTag : tag;
}

typedef struct {
    int numQueues;
    Queue** queues;
} LRU;

LRU* newLRU(int numQueues)
{
    LRU *lru = malloc(sizeof(LRU));

    int queueSize = CACHE_BLOCK_SIZE*ASSOCIATIVITY;

    lru->numQueues = numQueues;
    lru->queues = malloc(sizeof(Queue *) * numQueues);
    for (int i=0; i<numQueues; i++)
    {
        lru->queues[i] = newQueue(queueSize);
    }

    return lru;
}

int getEvictedTagLRU(LRU* lru, int set, int tag)
{
    if (set>lru->queues[set]->maxSize || set<0) {
        exit(INVALID_MEMORY_ADDRESS);
    }
    return addTagQueue(lru->queues[set], tag);
}

LRU* lru;

Block *newBlock()
{
    Block *block = malloc(sizeof(Block));
    block->set = -1;
    block->tag = -1;
    block->clean = 1;
    block->valid = 1;
    for (int i=0; i<CACHE_BLOCK_SIZE; i++)
    {
        block->byte_array[i]=0;
    }
    return block;
}

Cache *newCache()
{
    Cache* cache = malloc(sizeof(Cache));

    cache->size=CACHE_SIZE;
    cache->block_size=CACHE_BLOCK_SIZE;
    cache->numBlocks=CACHE_SIZE/CACHE_BLOCK_SIZE;
    cache->associativity = ASSOCIATIVITY;
    cache->write_style=WRITE_THROUGH;

    cache->block_offset_length = (int) pow(CACHE_BLOCK_SIZE, 0.5);
    cache->index_length = (int) pow(cache->numBlocks / ASSOCIATIVITY, 0.5);
    cache->tag_length = WORD_SIZE_BITS - cache->index_length - cache->block_offset_length;

    cache->blocks = malloc(sizeof(Block *) * cache->numBlocks);
    for (int i=0; i<cache->numBlocks; i++)
    {
        cache->blocks[i] = newBlock();
    }

    return cache;
}

Word *newWord()
{
    Word *word = malloc(sizeof(Word));
    for (int i=0; i<WORD_SIZE_BYTES; i++)
    {
        word->bytes[i] = 0;
    }

    return word;
}

int extractInt(unsigned int address, int rightOffset, int length)
{
    int value = 0;
    address = address >> rightOffset;
    for (int i = 0; i < length; i++)
    {
        value += (address % 2) * pow(2, i);
        address = address >> 1;
    }
    return value;
}

void validate_address(unsigned int address) {
    if (address%WORD_SIZE_BYTES!=0 || address>MEMORY_SIZE) {
        printf("INVALID MEMORY ACCESS: %d is not a valid memory address.\n", address);
        exit(INVALID_MEMORY_ADDRESS);
    }
}

int calculateTag(Cache *cache, unsigned int address)
{
    return extractInt(address, cache->index_length+cache->block_offset_length, cache->tag_length);
}

int calculateIndex(Cache *cache, unsigned int address)
{
    return extractInt(address, cache->block_offset_length, cache->index_length);
}

int calculateBlockOffset(Cache *cache, unsigned int address)
{
    return extractInt(address, 0, cache->block_offset_length);
}

void int_to_endian(int integer, Word *word)
{
    int byte = 0;
    for (int i=0; i<WORD_SIZE_BITS; i++)
    {
        byte += integer % 2;

        if (i % BYTE_SIZE == 7)
        {
            // Need to reverse the order (little endian)
            int correct_byte = byte % 2;
            byte = byte >> 1;
            for (int j=0; j<BYTE_SIZE-1; j++)
            {
                correct_byte = correct_byte << 1;
                correct_byte += byte % 2;
                byte = byte >> 1;
            }
            word->bytes[i/BYTE_SIZE] = correct_byte;
            byte = 0;
        }
        else
        {
            byte = byte << 1;
        }

        integer /= 2;
    }
}

int endian_to_int(Word *word)
{
    int integer = 0;
    for (int i=0; i<WORD_SIZE_BYTES; i++)
    {
        char byte = word->bytes[i];
        // We get the bits from from byte in reverse order, but that's okay
        // since the byte is in little endian form, which is reversed already.
        // Therefore we get the bits in the 'correct' order.
        for (int j=0; j<BYTE_SIZE; j++)
        {
            integer += abs(byte % 2) * pow(2, j + i * BYTE_SIZE);
            byte = byte >> 1;
        }
    }
    return integer;
}

void validate_cache_size() {
    if (CACHE_SIZE>MEMORY_CAP||CACHE_SIZE>MEMORY_SIZE) {
        exit(CACHE_TOO_LARGE_ERROR);
    }
}

Cache *initialize_cache() {
    validate_cache_size();
    return newCache();
}



char *initialize_memory() {
    char* mem = malloc(MEMORY_SIZE);

    for (int i=0;i<MEMORY_SIZE;i+=WORD_SIZE_BYTES) {
        Word word;
        int_to_endian(i, &word);
        for (int j=0; j<WORD_SIZE_BYTES; j++)
        {
            mem[i+j] = word.bytes[j];
        }
    }

    return mem;
}

int check_cache(unsigned int address, Cache* cache) {
    if (cache->associativity==1) {
        int block = address%cache->numBlocks;

        int tag = calculateTag(cache, address);

        if (tag == cache->blocks[block]->tag)
        {
            return WORD_HIT;
        }
        return WORD_MISS;
    }

    //check n-way associative cache
    char set = address%(cache->numBlocks/cache->associativity);

    int tag = calculateTag(cache, address);

    getEvictedTagLRU(lru, set, tag);

    for (int i=0; i<cache->associativity; i++)
    {
        if (tag == cache->blocks[(cache->associativity*set)+i]->tag) {
            return WORD_HIT;
        }
    }
    return WORD_MISS;
}

void pass_word_to_cache(unsigned int address, char* memory, Cache* cache) {
    if (cache->associativity == 1) {
        int destination_block = address%cache->numBlocks;
        Block *block = cache->blocks[destination_block];
        int block_offset = calculateBlockOffset(cache, address);
        //printf("Tag: %d, Index: %d, Offset: %d\n", calculateTag(cache, address), calculateIndex(cache, address), calculateBlockOffset(cache, address));

        block->tag = calculateTag(cache, address);
        block->valid = 1;
        block->clean = 1;
        for(int i=0;i<WORD_SIZE_BYTES;i++){
            block->byte_array[block_offset+i] = memory[address+i];
        }

    }
    if (cache->associativity > 1) {
        //adding word to n-way associative cache code goes here
        int destination_set = address%(cache->numBlocks/cache->associativity);
        int block_offset = calculateBlockOffset(cache, address);
        //need to use LRU to find which block in the set this will map to

        Block *block;
        int evictedTag = getEvictedTagLRU(lru, destination_set, calculateTag(cache, address));
        if (evictedTag != -1)
        {
            for (int i=destination_set*ASSOCIATIVITY; i<destination_set*ASSOCIATIVITY+ASSOCIATIVITY; i++)
            {
                if (cache->blocks[i]->tag == evictedTag)
                {
                    block = cache->blocks[i];
                }
            }
        }
        else
        {
            for (int i=destination_set*ASSOCIATIVITY; i<destination_set*ASSOCIATIVITY+ASSOCIATIVITY; i++)
            {
                int found = 1;
                for (int j=0; j<WORD_SIZE_BYTES; j++)
                {
                    if (cache->blocks[i]->byte_array[calculateBlockOffset(cache, address) + j] != 0)
                    {
                        found = 0;
                        break;
                    }
                }
                if (found)
                {
                    block = cache->blocks[i];
                    break;
                }
            }
        }

        block->tag = calculateTag(cache, address);
        block->valid = 1;
        block->clean = 1;
        for(int i=0;i<WORD_SIZE_BYTES;i++){
            block->byte_array[block_offset+i] = memory[address+i];
        }
    }
}

Word read_word(unsigned int address, char* memory, Cache* cache) {
    validate_address(address);
    Word word;  // TODO SET WORD
    int hit = check_cache(address, cache);
    int mem_range = address-(address%64);
    if (hit==WORD_MISS){ //Word is not in the cache
        pass_word_to_cache(address, memory, cache);

        printf("[addr=%d index=%d tag=%d: read miss; word=%d (%d - %d)]\n", address, calculateIndex(cache,address), calculateTag(cache,address), address, mem_range, mem_range+63);
        if (cache->associativity>1) {
            printf("[");
            for (int i=0; i<lru->queues[address%(cache->numBlocks/cache->associativity)]->maxSize;i++) {
                if (lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]!=-1) {
                    printf(" %d ", lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]);
                }
            }
            printf("]\n");
        }
        else {
            printf("[ %d ]\n", calculateTag(cache, address));
        }
    }
    else { //Word is already in the cache
        //Print statement formatted like directions show
        printf("[addr=%d index=%d tag=%d: read hit; word=%d (%d - %d)]\n", address, calculateIndex(cache,address), calculateTag(cache,address), address, mem_range, mem_range+63);
        if (cache->associativity>1) {
            printf("[");
            for (int i=0; i<lru->queues[address%(cache->numBlocks/cache->associativity)]->maxSize;i++) {
                if (lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]!=-1) {
                    printf(" %d ", lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]);
                }
            }
            printf("]\n");
        }
        else {
            printf("[ %d ]\n", calculateTag(cache, address));
        }
    }
    return word;
}


void write_word(unsigned int address, Word word, char* memory, Cache* cache) {
    if (cache->write_style==WRITE_THROUGH) {
        int hit = check_cache(address, cache);
        int mem_range = address-(address%64);
        if (hit==WORD_MISS) {
            pass_word_to_cache(address,memory,cache);
            for (int i = 0; i<WORD_SIZE_BYTES; i++){
                cache->blocks[address%cache->numBlocks]->byte_array[calculateBlockOffset(cache, address)+i] = word.bytes[i];
                memory[address+i] = word.bytes[i];
            }

            printf("[addr=%d index=%d tag=%d: write miss; word=%d (%d - %d)]\n", address, calculateIndex(cache,address), calculateTag(cache,address), endian_to_int(&word), mem_range, mem_range+63);
            if (cache->associativity>1) {
                printf("[");
                for (int i=0; i<lru->queues[address%(cache->numBlocks/cache->associativity)]->maxSize;i++) {
                    if (lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]!=-1) {
                        printf(" %d ", lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]);
                    }
                }
                printf("]\n");
                printf("[ Write-through ]\n");
            }
            else {
                printf("[ %d ]\n", calculateTag(cache, address));
                printf("[ Write-through ]\n");
            }
        }
        else { //Word is already in the cache
            //Print statement formatted like directions show
            for (int i = 0; i<WORD_SIZE_BYTES; i++){
                cache->blocks[address%cache->numBlocks]->byte_array[calculateBlockOffset(cache, address)+i] = word.bytes[i];
                memory[address+i] = word.bytes[i];
            }
            printf("[addr=%d index=%d tag=%d: write hit; word=%d (%d - %d)]\n", address, calculateIndex(cache,address), calculateTag(cache,address), endian_to_int(&word), mem_range, mem_range+63);
            if (cache->associativity>1) {
                printf("[");
                for (int i=0; i<lru->queues[address%(cache->numBlocks/cache->associativity)]->maxSize;i++) {
                    if (lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]!=-1) {
                        printf(" %d ", lru->queues[address%(cache->numBlocks/cache->associativity)]->array[i]);
                    }
                }
                printf("]\n");
                printf("[ Write-back ]\n");

            }
            else {
                printf("[ %d ]\n", calculateTag(cache, address));
                printf("[ Write-back ]\n");

            }
        }



    }
    else {

    }
}

void run_simulation() {
    Cache* cache = initialize_cache();
    char* memory = initialize_memory();
    Word word;
    if (cache->associativity>1) {
        lru = newLRU((CACHE_SIZE/CACHE_BLOCK_SIZE)/ASSOCIATIVITY);
    }
    word = read_word(17536,memory,cache);
    word = read_word(17536,memory,cache);
    word = read_word(1000,memory,cache);
    word = read_word(1000,memory,cache);
    word = read_word(17536,memory,cache);
    word = read_word(20000,memory,cache);
//    pass_word_to_cache(10000, memory, cache);
//    for (int i=0;i<CACHE_BLOCK_SIZE;i++) {
//        printf("%d\n", cache->blocks[0]->byte_array[i]);
//    }

}



int main() {
    run_simulation();
    return 0;
}
