#pragma once
#ifndef _HASH_TABLE_
#define _HASH_TABLE_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>
#include <klib/klib.h>

/// The initial size of the hash table.
#ifndef HT_INITIAL_SIZE
#define HT_INITIAL_SIZE 64
#endif //HT_INITIAL_SIZE



typedef void (HashFunc)(const void *key, int32_t len, uint32_t seed, void *out);
HashFunc MurmurHash3_x86_32;
HashFunc MurmurHash3_x86_128;
HashFunc MurmurHash3_x64_128;

/// The hash entry struct. Acts as a node in a linked list.
typedef struct hash_entry {
    /// A pointer to the key.
    void *key;

    /// A pointer to the value.
    void *value;

    /// The size of the key in bytes.
    size_t key_size;

    /// The size of the value in bytes.
    size_t value_size;

    /// A pointer to the next hash entry in the chain (or NULL if none).
    /// This is used for collision resolution.
    struct hash_entry *next;
}hash_entry;

/// The primary hashtable struct
typedef struct hash_table {
    // hash function for x86_32
    HashFunc *hashfunc_x86_32;

    // hash function for x86_128
    HashFunc *hashfunc_x86_128;

    // hash function for x64_128
    HashFunc *hashfunc_x64_128;

    /// The number of keys in the hash table.
    uint32_t key_count;

    /// The size of the internal array.
    uint32_t array_size;

    /// The internal hash table array.
    hash_entry **array;

    /// A count of the number of hash collisions.
    uint32_t collisions;

    /// Any flags that have been set. (See the ht_flags enum).
    int32_t flags;

    /// The max load factor that is acceptable before an autoresize is triggered
    /// (where load_factor is the ratio of collisions to table size).
    double max_load_factor;

    /// The current load factor.
    double current_load_factor;

} hash_table;

/// Hashtable initialization flags (passed to ht_init)
typedef enum {

    /// No options set
    HT_NONE = 0,

    /// Constant length key, useful if your keys are going to be a fixed size.
    HT_KEY_CONST = 1,

    /// Constant length value.
    HT_VALUE_CONST = 2,

    /// Don't automatically resize hashtable when the load factor
    /// goes above the trigger value
    HT_NO_AUTORESIZE = 4

} ht_flags;

//----------------------------------
// HashTable functions
//----------------------------------

/// @brief Initializes the hash_table struct.
/// @param table A pointer to the hash table.
/// @param flags Options for the way the table behaves.
/// @param max_load_factor The ratio of collisions:table_size before an autoresize is triggered
///        for example: if max_load_factor = 0.1, the table will resize if the number
///        of collisions increases beyond 1/10th of the size of the table
void ht_init(hash_table *table, ht_flags flags, double max_load_factor);

/// @brief Destroys the hash_table struct and frees all relevant memory.
/// @param table A pointer to the hash table.
void ht_destroy(hash_table *table);

/// @brief Inserts the {key: value} pair into the hash table, makes copies of both key and value.
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @param key_size The size of the key in bytes.
/// @param value A pointer to the value.
/// @param value_size The size of the value in bytes.
void ht_insert(hash_table *table, void *key, size_t key_size, void *value, size_t value_size);

/// @brief Inserts the {key: value} pair into the hash table, makes copies of both key and value.
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @param value A pointer to the value.
void ht_insert(hash_table *table, void *key, void *value);

/// @brief Inserts an existing hash entry into the hash table.
/// @param table A pointer to the hash table.
/// @param entry A pointer to the hash entry.
void ht_insert_he(hash_table *table, hash_entry *entry);

/// @brief Returns a pointer to the value with the matching key,
///         value_size is set to the size in bytes of the value
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @param key_size The size of the key in bytes.
/// @param value_size A pointer to a size_t where the size of the return
///         value will be stored.
/// @returns A pointer to the requested value. If the return value
///           is NULL, the requested key-value pair was not in the table.
void* ht_get(hash_table *table, void *key, size_t key_size, size_t *value_size);

/// @brief Removes the entry corresponding to the specified key from the hash table.
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @param key_size The size of the key in bytes.
void ht_remove(hash_table *table, void *key, size_t key_size);

/// @brief Used to see if the hash table contains a key-value pair.
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @param key_size The size of the key in bytes.
/// @returns 1 if the key is in the table, 0 otherwise
int32_t ht_contains(hash_table *table, void *key, size_t key_size);

/// @brief Used to see if the hash table contains a key-value pair.
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @returns 1 if the key is in the table, 0 otherwise
int32_t ht_contains(hash_table *table, void *key);

/// @brief Returns the number of entries in the hash table.
/// @param table A pointer to the table.
/// @returns The number of entries in the hash table.
uint32_t ht_size(hash_table *table);

/// @brief Returns an array of all the keys in the hash table.
/// @param table A pointer to the hash table.
/// @param key_count A pointer to an uint32_t that
///        will be set to the number of keys in the returned array.
/// @returns A pointer to an array of keys.
/// TODO: Add a key_lengths return value as well?
void** ht_keys(hash_table *table, uint32_t *key_count);

/// @brief Removes all entries from the hash table.
/// @param table A pointer to the hash table.
void ht_clear(hash_table *table);

/// @brief Calulates the index in the hash table's internal array
///        from the given key (used for debugging currently).
/// @param table A pointer to the hash table.
/// @param key A pointer to the key.
/// @param key_size The size of the key in bytes.
/// @returns The index into the hash table's internal array.
uint32_t ht_index(hash_table *table, void *key, size_t key_size);

/// @brief Resizes the hash table's internal array. This operation is
///        _expensive_, however it can make an overfull table run faster
///        if the table is expanded. The table can also be shrunk to reduce
///        memory usage.
/// @param table A pointer to the table.
/// @param new_size The desired size of the table.
void ht_resize(hash_table *table, uint32_t new_size);

/// @brief Sets the global security seed to be used in hash function.
/// @param seed The seed to use.
void ht_set_seed(uint32_t seed);


#endif