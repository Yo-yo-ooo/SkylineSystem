#include <klib/algorithm/hash_table.h>

uint32_t global_seed = 2976579765;

FORCE_INLINE uint32_t rotl32 ( uint32_t x, int8_t r )
{
    return (x << r) | (x >> (32 - r));
}

FORCE_INLINE uint64_t rotl64 ( uint64_t x, int8_t r )
{
    return (x << r) | (x >> (64 - r));
}

#define ROTL32(x,y) rotl32(x,y)
#define ROTL64(x,y) rotl64(x,y)

#define BIG_CONSTANT(x) (x##LLU)

#define getblock(x, i) (x[i])

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

FORCE_INLINE uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

//----------

FORCE_INLINE uint64_t fmix64(uint64_t k)
{
    k ^= k >> 33;
    k *= BIG_CONSTANT(0xff51afd7ed558ccd);
    k ^= k >> 33;
    k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
    k ^= k >> 33;

    return k;
}

//-----------------------------------------------------------------------------

void MurmurHash3_x86_32(const void *key, int len, uint32_t seed, void *out )
{
    const uint8_t * data = (const uint8_t*)key;
    const int nblocks = len / 4;

    uint32_t h1 = seed;

    uint32_t c1 = 0xcc9e2d51;
    uint32_t c2 = 0x1b873593;

    int i;

    //----------
    // body

    const uint32_t * blocks = (const uint32_t *)(data + nblocks*4);

    for(i = -nblocks; i; i++) {
        uint32_t k1 = getblock(blocks,i);

        k1 *= c1;
        k1 = ROTL32(k1,15);
        k1 *= c2;

        h1 ^= k1;
        h1 = ROTL32(h1,13);
        h1 = h1*5+0xe6546b64;
    }

    //----------
    // tail

    const uint8_t * tail = (const uint8_t*)(data + nblocks*4);

    uint32_t k1 = 0;

    switch(len & 3) {
        case 3: k1 ^= tail[2] << 16;
        case 2: k1 ^= tail[1] << 8;
        case 1: k1 ^= tail[0];
                k1 *= c1; k1 = ROTL32(k1,15); k1 *= c2; h1 ^= k1;
    }

    //----------
    // finalization

    h1 ^= len;

    h1 = fmix32(h1);

    *(uint32_t*)out = h1;
}

//-----------------------------------------------------------------------------

void MurmurHash3_x86_128(const void *key, const int len, uint32_t seed,
        void *out)
{
    const uint8_t * data = (const uint8_t*)key;
    const int nblocks = len / 16;

    uint32_t h1 = seed;
    uint32_t h2 = seed;
    uint32_t h3 = seed;
    uint32_t h4 = seed;

    uint32_t c1 = 0x239b961b;
    uint32_t c2 = 0xab0e9789;
    uint32_t c3 = 0x38b34ae5;
    uint32_t c4 = 0xa1e38b93;

    int i;

    //----------
    // body

    const uint32_t * blocks = (const uint32_t *)(data + nblocks*16);

    for(i = -nblocks; i; i++) {
        uint32_t k1 = getblock(blocks,i*4+0);
        uint32_t k2 = getblock(blocks,i*4+1);
        uint32_t k3 = getblock(blocks,i*4+2);
        uint32_t k4 = getblock(blocks,i*4+3);

        k1 *= c1;
        k1  = ROTL32(k1,15);
        k1 *= c2;
        h1 ^= k1;

        h1 = ROTL32(h1,19);
        h1 += h2;
        h1 = h1*5+0x561ccd1b;

        k2 *= c2;
        k2  = ROTL32(k2,16);
        k2 *= c3;
        h2 ^= k2;

        h2 = ROTL32(h2,17);
        h2 += h3;
        h2 = h2*5+0x0bcaa747;

        k3 *= c3;
        k3  = ROTL32(k3,17);
        k3 *= c4;
        h3 ^= k3;

        h3 = ROTL32(h3,15);
        h3 += h4;
        h3 = h3*5+0x96cd1c35;

        k4 *= c4;
        k4  = ROTL32(k4,18);
        k4 *= c1;
        h4 ^= k4;

        h4 = ROTL32(h4,13);
        h4 += h1;
        h4 = h4*5+0x32ac3b17;
    }

    //----------
    // tail

    const uint8_t * tail = (const uint8_t*)(data + nblocks*16);

    uint32_t k1 = 0;
    uint32_t k2 = 0;
    uint32_t k3 = 0;
    uint32_t k4 = 0;

    switch(len & 15) {
        case 15: k4 ^= tail[14] << 16;
        case 14: k4 ^= tail[13] << 8;
        case 13: k4 ^= tail[12] << 0;
                 k4 *= c4;
                 k4  = ROTL32(k4,18);
                 k4 *= c1;
                 h4 ^= k4;

        case 12: k3 ^= tail[11] << 24;
        case 11: k3 ^= tail[10] << 16;
        case 10: k3 ^= tail[ 9] << 8;
        case  9: k3 ^= tail[ 8] << 0;
                 k3 *= c3;
                 k3  = ROTL32(k3,17);
                 k3 *= c4;
                 h3 ^= k3;

        case  8: k2 ^= tail[ 7] << 24;
        case  7: k2 ^= tail[ 6] << 16;
        case  6: k2 ^= tail[ 5] << 8;
        case  5: k2 ^= tail[ 4] << 0;
                 k2 *= c2;
                 k2  = ROTL32(k2,16);
                 k2 *= c3;
                 h2 ^= k2;

        case  4: k1 ^= tail[ 3] << 24;
        case  3: k1 ^= tail[ 2] << 16;
        case  2: k1 ^= tail[ 1] << 8;
        case  1: k1 ^= tail[ 0] << 0;
                 k1 *= c1;
                 k1  = ROTL32(k1,15);
                 k1 *= c2;
                 h1 ^= k1;
    }

    //----------
    // finalization

    h1 ^= len;
    h2 ^= len;
    h3 ^= len;
    h4 ^= len;

    h1 += h2;
    h1 += h3;
    h1 += h4;
    h2 += h1;
    h3 += h1;
    h4 += h1;

    h1 = fmix32(h1);
    h2 = fmix32(h2);
    h3 = fmix32(h3);
    h4 = fmix32(h4);

    h1 += h2;
    h1 += h3;
    h1 += h4;
    h2 += h1;
    h3 += h1;
    h4 += h1;

    ((uint32_t*)out)[0] = h1;
    ((uint32_t*)out)[1] = h2;
    ((uint32_t*)out)[2] = h3;
    ((uint32_t*)out)[3] = h4;
}

//-----------------------------------------------------------------------------

void MurmurHash3_x64_128(const void *key, const int len, const uint32_t seed,
       void *out)
{
    const uint8_t * data = (const uint8_t*)key;
    const int nblocks = len / 16;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    uint64_t c1 = BIG_CONSTANT(0x87c37b91114253d5);
    uint64_t c2 = BIG_CONSTANT(0x4cf5ad432745937f);

    int i;

    //----------
    // body

    const uint64_t * blocks = (const uint64_t *)(data);

    for(i = 0; i < nblocks; i++) {
        uint64_t k1 = getblock(blocks,i*2+0);
        uint64_t k2 = getblock(blocks,i*2+1);

        k1 *= c1;
        k1  = ROTL64(k1,31);
        k1 *= c2;
        h1 ^= k1;

        h1 = ROTL64(h1,27);
        h1 += h2;
        h1 = h1*5+0x52dce729;

        k2 *= c2;
        k2  = ROTL64(k2,33);
        k2 *= c1;
        h2 ^= k2;

        h2 = ROTL64(h2,31);
        h2 += h1;
        h2 = h2*5+0x38495ab5;
    }

    //----------
    // tail

    const uint8_t * tail = (const uint8_t*)(data + nblocks*16);

    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch(len & 15) {
        case 15: k2 ^= ((uint64_t)tail[14]) << 48;
        case 14: k2 ^= ((uint64_t)tail[13]) << 40;
        case 13: k2 ^= ((uint64_t)tail[12]) << 32;
        case 12: k2 ^= ((uint64_t)tail[11]) << 24;
        case 11: k2 ^= ((uint64_t)tail[10]) << 16;
        case 10: k2 ^= ((uint64_t)tail[ 9]) << 8;
        case  9: k2 ^= ((uint64_t)tail[ 8]) << 0;
                 k2 *= c2;
                 k2  = ROTL64(k2,33);
                 k2 *= c1;
                 h2 ^= k2;

        case  8: k1 ^= ((uint64_t)tail[ 7]) << 56;
        case  7: k1 ^= ((uint64_t)tail[ 6]) << 48;
        case  6: k1 ^= ((uint64_t)tail[ 5]) << 40;
        case  5: k1 ^= ((uint64_t)tail[ 4]) << 32;
        case  4: k1 ^= ((uint64_t)tail[ 3]) << 24;
        case  3: k1 ^= ((uint64_t)tail[ 2]) << 16;
        case  2: k1 ^= ((uint64_t)tail[ 1]) << 8;
        case  1: k1 ^= ((uint64_t)tail[ 0]) << 0;
                 k1 *= c1;
                 k1  = ROTL64(k1,31);
                 k1 *= c2;
                 h1 ^= k1;
    }

    //----------
    // finalization

    h1 ^= len;
    h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    ((uint64_t*)out)[0] = h1;
    ((uint64_t*)out)[1] = h2;
}

//----------------------------------
// HashEntry functions
//----------------------------------

/// @brief Creates a new hash entry.
/// @param flags Hash table flags.
/// @param key A pointer to the key.
/// @param key_size The size of the key in bytes.
/// @param value A pointer to the value.
/// @param value_size The size of the value in bytes.
/// @returns A pointer to the hash entry.
hash_entry *he_create(int flags, void *key, size_t key_size, void *value,
        size_t value_size);

/// @brief Destroys the hash entry and frees all associated memory.
/// @param flags The hash table flags.
/// @param hash_entry A pointer to the hash entry.
void he_destroy(int flags, hash_entry *entry);

/// @brief Compare two hash entries.
/// @param e1 A pointer to the first entry.
/// @param e2 A pointer to the second entry.
/// @returns 1 if both the keys and the values of e1 and e2 match, 0 otherwise.
///          This is a "deep" compare, rather than just comparing pointers.
int he_key_compare(hash_entry *e1, hash_entry *e2);

/// @brief Sets the value on an existing hash entry.
/// @param flags The hashtable flags.
/// @param entry A pointer to the hash entry.
/// @param value A pointer to the new value.
/// @param value_size The size of the new value in bytes.
void he_set_value(int flags, hash_entry *entry, void *value, size_t value_size);

//-----------------------------------
// HashTable functions
//-----------------------------------

void ht_init(hash_table *table, ht_flags flags, double max_load_factor)
{

    table->hashfunc_x86_32  = MurmurHash3_x86_32;
    table->hashfunc_x86_128 = MurmurHash3_x86_128;
    table->hashfunc_x64_128 = MurmurHash3_x64_128;

    table->array_size   = HT_INITIAL_SIZE;
    table->array        = kmalloc(table->array_size * sizeof(*(table->array)));

    if(table->array == NULL) {
        kerror("ht_init failed to allocate memory\n");
    }

    table->key_count            = 0;
    table->collisions           = 0;
    table->flags                = flags;
    table->max_load_factor      = max_load_factor;
    table->current_load_factor  = 0.0;

    unsigned int i;
    for(i = 0; i < table->array_size; i++)
    {
        table->array[i] = NULL;
    }

    return;
}

void ht_destroy(hash_table *table)
{
    unsigned int i;
    hash_entry *entry;
    hash_entry *tmp;

    if(table->array == NULL) {
        kerror("ht_destroy got a bad table\n");
    }

    // crawl the entries and delete them
    for(i = 0; i < table->array_size; i++) {
        entry = table->array[i];

        while(entry != NULL) {
            tmp = entry->next;
            he_destroy(table->flags, entry);
            entry = tmp;
        }
    }

    table->hashfunc_x86_32 = NULL;
    table->hashfunc_x86_128 = NULL;
    table->hashfunc_x64_128 = NULL;
    table->array_size = 0;
    table->key_count = 0;
    table->collisions = 0;
    kfree(table->array);
    table->array = NULL;
}

void ht_insert(hash_table *table, void *key, size_t key_size, void *value,
        size_t value_size)
{
    hash_entry *entry = he_create(table->flags, key, key_size, value,
            value_size);

    ht_insert_he(table, entry);
}

// this was separated out of the regular ht_insert
// for ease of copying hash entries around
__ffunc void ht_insert_he(hash_table *table, hash_entry *entry) {
    hash_entry *tmp;
    unsigned int index;

    entry->next = NULL;
    index = ht_index(table, entry->key, entry->key_size);
    tmp = table->array[index];

    // if true, no collision
    if(tmp == NULL)
    {
        table->array[index] = entry;
        table->key_count++;
        return;
    }

    // walk down the chain until we either hit the end
    // or find an identical key (in which case we replace
    // the value)
    while(tmp->next != NULL)
    {
        if(he_key_compare(tmp, entry))
            break;
        else
            tmp = tmp->next;
    }

    if(he_key_compare(tmp, entry))
    {
        // if the keys are identical, throw away the old entry
        // and stick the new one into the table
        he_set_value(table->flags, tmp, entry->value, entry->value_size);
        he_destroy(table->flags, entry);
    }
    else
    {
        // else tack the new entry onto the end of the chain
        tmp->next = entry;
        table->collisions += 1;
        table->key_count ++;
        table->current_load_factor = (double)table->collisions / table->array_size;

        // double the size of the table if autoresize is on and the
        // load factor has gone too high
        if(!(table->flags & HT_NO_AUTORESIZE) &&
                (table->current_load_factor > table->max_load_factor)) {
            ht_resize(table, table->array_size * 2);
            table->current_load_factor =
                (double)table->collisions / table->array_size;
        }
    }
}

void* ht_get(hash_table *table, void *key, size_t key_size, size_t *value_size)
{
    unsigned int index  = ht_index(table, key, key_size);
    hash_entry *entry   = table->array[index];
    hash_entry tmp;
    tmp.key             = key;
    tmp.key_size        = key_size;

    // once we have the right index, walk down the chain (if any)
    // until we find the right key or hit the end
    while(entry != NULL)
    {
        if(he_key_compare(entry, &tmp))
        {
            if(value_size != NULL)
                *value_size = entry->value_size;

            return entry->value;
        }
        else
        {
            entry = entry->next;
        }
    }

    return NULL;
}

void ht_remove(hash_table *table, void *key, size_t key_size)
{
    unsigned int index  = ht_index(table, key, key_size);
    hash_entry *entry   = table->array[index];
    hash_entry *prev    = NULL;
    hash_entry tmp;
    tmp.key             = key;
    tmp.key_size        = key_size;

    // walk down the chain
    while(entry != NULL)
    {
        // if the key matches, take it out and connect its
        // parent and child in its place
        if(he_key_compare(entry, &tmp))
        {
            if(prev == NULL)
                table->array[index] = entry->next;
            else
                prev->next = entry->next;

            table->key_count--;

            if(prev != NULL)
              table->collisions--;

            he_destroy(table->flags, entry);
            return;
        }
        else
        {
            prev = entry;
            entry = entry->next;
        }
    }
}

int ht_contains(hash_table *table, void *key, size_t key_size)
{
    unsigned int index  = ht_index(table, key, key_size);
    hash_entry *entry   = table->array[index];
    hash_entry tmp;
    tmp.key             = key;
    tmp.key_size        = key_size;

    // walk down the chain, compare keys
    while(entry != NULL)
    {
        if(he_key_compare(entry, &tmp))
            return 1;
        else
            entry = entry->next;
    }

    return 0;
}

unsigned int ht_size(hash_table *table)
{
    return table->key_count;
}

void** ht_keys(hash_table *table, unsigned int *key_count)
{
    void **ret;

    if(table->key_count == 0){
      *key_count = 0;
      return NULL;
    }

    // array of pointers to keys
    ret = kmalloc(table->key_count * sizeof(void *));
    if(ret == NULL) {
        kerror("ht_keys failed to allocate memory\n");
    }
    *key_count = 0;

    unsigned int i;
    hash_entry *tmp;

    // loop over all of the chains, walk the chains,
    // add each entry to the array of keys
    for(i = 0; i < table->array_size; i++)
    {
        tmp = table->array[i];

        while(tmp != NULL)
        {
            ret[*key_count]=tmp->key;
            *key_count += 1;
            tmp = tmp->next;
            // sanity check, should never actually happen
            if(*key_count >= table->key_count) {
                kerror("ht_keys: too many keys, expected %d, got %d\n",
                        table->key_count, *key_count);
            }
        }
    }

    return ret;
}

void ht_clear(hash_table *table)
{
    ht_destroy(table);

    ht_init(table, table->flags, table->max_load_factor);
}

unsigned int ht_index(hash_table *table, void *key, size_t key_size)
{
    uint32_t index;
    // 32 bits of murmur seems to fare pretty well
    table->hashfunc_x86_32(key, key_size, global_seed, &index);
    index %= table->array_size;
    return index;
}

// new_size can be smaller than current size (downsizing allowed)
void ht_resize(hash_table *table, unsigned int new_size)
{
    hash_table new_table;

    kinfo("ht_resize(old=%d, new=%d)\n",table->array_size,new_size);
    new_table.hashfunc_x86_32 = table->hashfunc_x86_32;
    new_table.hashfunc_x86_128 = table->hashfunc_x86_128;
    new_table.hashfunc_x64_128 = table->hashfunc_x64_128;
    new_table.array_size = new_size;
    new_table.array = kmalloc(new_size * sizeof(hash_entry*));
    new_table.key_count = 0;
    new_table.collisions = 0;
    new_table.flags = table->flags;
    new_table.max_load_factor = table->max_load_factor;

    unsigned int i;
    for(i = 0; i < new_table.array_size; i++)
    {
        new_table.array[i] = NULL;
    }

    hash_entry *entry;
    hash_entry *next;
    for(i = 0; i < table->array_size; i++)
    {
        entry = table->array[i];
        while(entry != NULL)
        {
            next = entry->next;
            ht_insert_he(&new_table, entry);
            entry = next;
        }
        table->array[i]=NULL;
    }

    ht_destroy(table);

    table->hashfunc_x86_32 = new_table.hashfunc_x86_32;
    table->hashfunc_x86_128 = new_table.hashfunc_x86_128;
    table->hashfunc_x64_128 = new_table.hashfunc_x64_128;
    table->array_size = new_table.array_size;
    table->array = new_table.array;
    table->key_count = new_table.key_count;
    table->collisions = new_table.collisions;

}

void ht_set_seed(uint32_t seed){
    global_seed = seed;
}

//---------------------------------
// hash_entry functions
//---------------------------------

hash_entry *he_create(int flags, void *key, size_t key_size, void *value,
        size_t value_size)
{
    hash_entry *entry = kmalloc(sizeof(*entry));
    if(entry == NULL) {
        kerror("Failed to create hash_entry\n");
        return NULL;
    }

    entry->key_size = key_size;
    if (flags & HT_KEY_CONST){
        entry->key = key;
    }
    else {
        entry->key = kmalloc(key_size);
        if(entry->key == NULL) {
            kerror("Failed to create hash_entry\n");
            kfree(entry);
            return NULL;
        }
        _memcpy(entry->key, key, key_size);
    }

    entry->value_size = value_size;
    if (flags & HT_VALUE_CONST){
        entry->value = value;
    }
    else {
        entry->value = kmalloc(value_size);
        if(entry->value == NULL) {
            kerror("Failed to create hash_entry\n");
            kfree(entry->key);
            kfree(entry);
            return NULL;
        }
        _memcpy(entry->value, value, value_size);
    }

    entry->next = NULL;

    return entry;
}

void he_destroy(int flags, hash_entry *entry)
{
    if (!(flags & HT_KEY_CONST))
        kfree(entry->key);
    if (!(flags & HT_VALUE_CONST))
        kfree(entry->value);
    kfree(entry);
}

int he_key_compare(hash_entry *e1, hash_entry *e2)
{
    char *k1 = e1->key;
    char *k2 = e2->key;

    if(e1->key_size != e2->key_size)
        return 0;

    return (_memcmp(k1,k2,e1->key_size) == 0);
}

void he_set_value(int flags, hash_entry *entry, void *value, size_t value_size)
{
    if (!(flags & HT_VALUE_CONST)) {
        if(entry->value)
            kfree(entry->value);

        entry->value = kmalloc(value_size);
        if(entry->value == NULL) {
            kerror("(HashTable)Failed to set entry value\n");
            return;
        }
        _memcpy(entry->value, value, value_size);
    } else {
        entry->value = value;
    }
    entry->value_size = value_size;

    return;
}
