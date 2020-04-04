#pragma once

#include "tools.hpp"
#include "allocators.hpp"

// Simple hash table implementation
template <
    typename T,
    uint32_t Bucket_Count,
    uint32_t Bucket_Size,
    uint32_t Bucket_Search_Count> struct hash_table_t {
    enum { UNINITIALIZED_HASH = 0xFFFFFFFF };
    enum { ITEM_POUR_LIMIT    = Bucket_Search_Count };

    struct item_t {
        uint32_t hash = UNINITIALIZED_HASH;
        T value = T();
    };

    struct bucket_t {
        uint32_t bucket_usage_count = 0;
        item_t items[Bucket_Size] = {};
    };

    bucket_t buckets[Bucket_Count] = {};

    void init() {
        for (uint32_t bucket_index = 0; bucket_index < Bucket_Count; ++bucket_index) {
            for (uint32_t item_index = 0; item_index < Bucket_Size; ++item_index) {
                buckets[bucket_index].items[item_index].hash = UNINITIALIZED_HASH;
            }
        }
    }

    void clean_up() {
        for (uint32_t i = 0; i < Bucket_Count; ++i) {
            buckets[i].bucket_usage_count = 0;
        }
    }

    void insert(
        uint32_t hash,
        T value) {
        bucket_t *bucket = &buckets[hash % Bucket_Count];

        for (uint32_t bucket_item = 0; bucket_item < Bucket_Size; ++bucket_item) {
            item_t *item = &bucket->items[bucket_item];
            if (item->hash == UNINITIALIZED_HASH) {
                /* found a slot for the object */
                item->hash = hash;
                item->value = value;
                return;
            }
        }
        
        assert(false);
    }
    
    void remove(
        uint32_t hash) {
        bucket_t *bucket = &buckets[hash % Bucket_Count];

        for (uint32_t bucket_item = 0; bucket_item < Bucket_Size; ++bucket_item) {

            item_t *item = &bucket->items[bucket_item];

            if (item->hash == hash && item->hash != UNINITIALIZED_HASH) {
                item->hash = UNINITIALIZED_HASH;
                item->value = T();
                return;
            }
        }

        assert(false);
    }
    
    T *get(
        uint32_t hash) {
        static int32_t invalid = -1;
        
        bucket_t *bucket = &buckets[hash % Bucket_Count];
        
        uint32_t bucket_item = 0;

        uint32_t filled_items = 0;

        for (; bucket_item < Bucket_Size; ++bucket_item) {
            item_t *item = &bucket->items[bucket_item];
            if (item->hash != UNINITIALIZED_HASH) {
                ++filled_items;
                if (hash == item->hash) {
                    return(&item->value);
                }
            }
        }

        if (filled_items == Bucket_Size) {
            assert(false);
        }

        return NULL;
    }
};

// Makes it so that when items are deleted, the array indices of all other items don't change
template <
    typename T> struct stack_container_t {
    uint32_t max_size = 0;
    uint32_t data_count = 0;
    T *data;

    uint32_t removed_count = 0;
    uint32_t *removed;

    void init(
        uint32_t max) {
        max_size = max;
        data = FL_MALLOC(T, max_size);
        removed = FL_MALLOC(uint32_t, max_size);
    }

    void destroy() {
        FL_FREE(data);
        FL_FREE(removed);
    }
    
    uint32_t add() {
        if (removed_count) {
            return removed[removed_count-- - 1];
        }
        else {
            return data_count++;
        }
    }

    T *get(
        uint32_t index) {
        return &data[index];
    }

    T &operator[](
        uint32_t i) {
        return data[i];
    }

    void remove(
        uint32_t index) {
        data[index] = T{};
        removed[removed_count++] = index;
    }
};

// Makes it so that when items are deleted, it is possible that the array indices of all other items change
template <
    typename T> struct flexible_stack_container_t {
    uint32_t max_size = 0;
    uint32_t data_count = 0;
    T *data;

    void init(
        uint32_t max) {
        max_size = max;
        data = FL_MALLOC(T, max_size);
    }

    void destroy() {
        FL_FREE(data);
    }
    
    uint32_t add() {
        return data_count++;
    }

    T *get(
        uint32_t index) {
        return &data[index];
    }

    void clear() {
        data_count = 0;
    }

    T &operator[](
        uint32_t i) {
        return data[i];
    }

    void remove(
        uint32_t index) {
        // Is at end
        if (index == data_count -1) {
            --data_count;
        }
        else {
            // Get last item and put fill the currently deleted item here
            data[index] = data[data_count - 1];
            --data_count;
        }
    }
};

template <
    typename T> struct circular_buffer_heap_t {
    uint32_t head_tail_difference = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t buffer_size;
    T *buffer;

    void initialize(
        uint32_t count) {
        buffer_size = count;
        uint32_t byte_count = buffer_size * sizeof(T);
        buffer = (T *)FL_MALLOC(uint8_t, byte_count);
    }

    void push_item(
        T *item) {
        buffer[head++] = *item;

        if (head == buffer_size) {
            head = 0;
        }

        ++head_tail_difference;
    }

    T *push_item() {
        T *new_item = &buffer[head++];

        if (head == buffer_size) {
            head = 0;
        }

        ++head_tail_difference;

        return(new_item);
    }

    T *get_next_item() {
        if (head_tail_difference > 0) {
            T *item = &buffer[tail++];

            if (tail == buffer_size) {
                tail = 0;
            }
                
            --head_tail_difference;

            return(item);
        }

        return(nullptr);
    }

    void deinitialize() {
        if (buffer) {
            FL_FREE(buffer);
        }
        buffer = nullptr;
        buffer_size = 0;
        head = 0;
        tail = 0;
        head_tail_difference = 0;
    }

    uint32_t decrement_index(
        uint32_t index) {
        if (index == 0) {
            return (buffer_size - 1);
        }
        else {
            return (index - 1);
        }
    }
};

template <
    typename T,
    uint32_t MAX> struct circular_buffer_array_t {
    uint32_t head_tail_difference = 0;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t buffer_size;
    T buffer[MAX];

    void initialize() {
        buffer_size = MAX;
        head_tail_difference = 0;
        head = 0;
        tail = 0;
        uint32_t byte_count = buffer_size * sizeof(T);
    }

    void push_item(
        T *item) {
        buffer[head++] = *item;

        if (head == buffer_size) {
            head = 0;
        }

        ++head_tail_difference;
    }

    T *push_item() {
        T *new_item = &buffer[head++];

        if (head == buffer_size) {
            head = 0;
        }

        ++head_tail_difference;

        return(new_item);
    }

    T *get_next_item() {
        if (head_tail_difference > 0) {
            T *item = &buffer[tail++];

            if (tail == buffer_size) {
                tail = 0;
            }
                
            --head_tail_difference;

            return(item);
        }

        return(nullptr);
    }

    void deinitialize() {
        buffer_size = 0;
        head = 0;
        tail = 0;
        head_tail_difference = 0;
    }

    uint32_t decrement_index(
        uint32_t index) {
        if (index == 0) {
            return (buffer_size - 1);
        }
        else {
            return (index - 1);
        }
    }
};