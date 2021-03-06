#include "bpt.h"

#include <string.h>

int init_db(int num_buf, int flag, int log_num, char* log_path, char* logmsg_path) {
    if (init_log(log_path) != 0) return -1;
    if (init_buffer(num_buf) != 0) return -1;
    if (init_lock_table() != 0) return -1;
    recovery(flag, log_num, logmsg_path);
    return 0;
}

int shutdown_db() {
    if (shutdown_lock_table() != 0) return -1;
    if (shutdown_buffer() != 0) return -1;
    if (shutdown_log() != 0) return -1;
    file_close_table_file();
    return 0;
}

int64_t open_table(char* pathname) {
    return file_open_table_file(pathname);
}

// SEARCH & UPDATE

int db_find(int64_t table_id, int64_t key,
            char* ret_val, uint16_t* val_size, int trx_id) {
    pagenum_t p_pgnum;
    page_t* p;

    if (!trx_is_active(trx_id)) return trx_id;

    p_pgnum = find_leaf(table_id, key);
    if (p_pgnum == 0) return -1;

    buffer_read_page(table_id, p_pgnum, &p);
    int i;
    int num_keys = p->num_keys;
    for (i = 0; i < num_keys; i++) {
        if (p->slots[i].key == key) break;
    }

    if (i == num_keys) {
        buffer_unpin_page(table_id, p_pgnum);
        return -1;
    }
    if (lock_acquire(table_id, p_pgnum, i, trx_id, SHARED, &p) != 0) {
        trx_abort(trx_id);
        return trx_id;
    }

    uint16_t offset = p->slots[i].offset;
    uint16_t size = p->slots[i].size;
    *val_size = size;

    memcpy(ret_val, (char*)p + offset, size);
    buffer_unpin_page(table_id, p_pgnum);

    return 0;
}

int db_update(int64_t table_id, int64_t key,
              char* value, uint16_t new_val_size, uint16_t* old_val_size, int trx_id) {
    pagenum_t p_pgnum;
    page_t* p;

    if (!trx_is_active(trx_id)) return trx_id;

    p_pgnum = find_leaf(table_id, key);
    if (p_pgnum == 0) return -1;

    buffer_read_page(table_id, p_pgnum, &p);
    int i;
    int num_keys = p->num_keys;
    for (i = 0; i < num_keys; i++) {
        if (p->slots[i].key == key) break;
    }

    if (i == num_keys) {
        buffer_unpin_page(table_id, p_pgnum);   
        return -1;
    }
    if (lock_acquire(table_id, p_pgnum, i, trx_id, EXCLUSIVE, &p) != 0) {
        trx_abort(trx_id);
        return trx_id;
    }

    uint16_t offset = p->slots[i].offset;
    uint16_t size = p->slots[i].size;
    *old_val_size = size;

    uint64_t ret_LSN = log_write_log(trx_get_last_LSN(trx_id), trx_id, UPDATE,
                                     table_id, p_pgnum, offset, size, (char*)p + offset, value);
    trx_set_last_LSN(trx_id, ret_LSN);

    memcpy((char*)p + offset, value, new_val_size);
    p->page_LSN = ret_LSN;
    buffer_write_page(table_id, p_pgnum);

    return 0;
}

pagenum_t find_leaf(int64_t table_id, int64_t key) {
    pagenum_t p_pgnum, child_pgnum;
    page_t *p, *header;
    buffer_read_page(table_id, 0, &header);
    p_pgnum = header->root_num;
    buffer_unpin_page(table_id, 0);
    if (p_pgnum == 0) return 0;
    buffer_read_page(table_id, p_pgnum, &p);
    while (!p->is_leaf) {
        int i = 0;
        while (i < p->num_keys) {
            if (key >= p->entries[i].key)
                i++;
            else
                break;
        }
        child_pgnum = i ? p->entries[i - 1].child :
                p->left_child;
                buffer_unpin_page(table_id, p_pgnum);
                p_pgnum = child_pgnum;
                buffer_read_page(table_id, p_pgnum, &p);
        }
    buffer_unpin_page(table_id, p_pgnum);
    return p_pgnum;
}

// INSERTION

int db_insert(int64_t table_id, int64_t key, char* value, uint16_t val_size) {
    pagenum_t leaf_pgnum, root_pgnum;
    page_t *leaf, *header;

    buffer_read_page(table_id, 0, &header);
    root_pgnum = header->root_num;
    buffer_unpin_page(table_id, 0);

    if (root_pgnum == 0) {
        start_tree(table_id, key, value, val_size);
        return 0;
    }

    leaf_pgnum = find_leaf(table_id, key);

    buffer_read_page(table_id, leaf_pgnum, &leaf);
    int i;
    int num_keys = leaf->num_keys;
    for (i = 0; i < num_keys; i++) {
        if (leaf->slots[i].key == key) break;
    }
    int free_space = leaf->free_space;
    buffer_unpin_page(table_id, leaf_pgnum);

    if (i != num_keys) return -1;

    if (free_space >= SLOT_SIZE + val_size) {
        insert_into_leaf(table_id, leaf_pgnum, key, value, val_size);
    } else {
        insert_into_leaf_split(table_id, leaf_pgnum, key, value, val_size);
    }

    return 0;
}

void insert_into_leaf(int64_t table_id, pagenum_t leaf_pgnum,
                      int64_t key, char* value, uint16_t val_size) {
    page_t* leaf;

    buffer_read_page(table_id, leaf_pgnum, &leaf);

    int insertion_index = 0;
    while (insertion_index < leaf->num_keys && leaf->slots[insertion_index].key < key) {
        insertion_index++;
    }
    uint16_t offset = HEADER_SIZE + SLOT_SIZE * leaf->num_keys + leaf->free_space;

    for (int i = leaf->num_keys; i > insertion_index; i--) {
        leaf->slots[i].key = leaf->slots[i - 1].key;
        leaf->slots[i].size = leaf->slots[i - 1].size;
        leaf->slots[i].trx_id = leaf->slots[i - 1].trx_id;
        leaf->slots[i].offset = leaf->slots[i - 1].offset;
    }
    leaf->slots[insertion_index].key = key;
    leaf->slots[insertion_index].size = val_size;
    leaf->slots[insertion_index].trx_id = 0;
    leaf->slots[insertion_index].offset = offset - val_size;
    memcpy((char*)leaf + offset - val_size, value, val_size);

    leaf->num_keys++;
    leaf->free_space -= (SLOT_SIZE + val_size);

    buffer_write_page(table_id, leaf_pgnum);
}

void insert_into_leaf_split(int64_t table_id, pagenum_t leaf_pgnum,
                            int64_t key, char* value, uint16_t val_size) {
    pagenum_t new_pgnum;
    page_t *leaf, *new_leaf;
    slot_t temp_slots[65];
    char temp_page[4096];

    buffer_read_page(table_id, leaf_pgnum, &leaf);

    int insertion_index = 0;
    while (insertion_index < leaf->num_keys && leaf->slots[insertion_index].key < key) {
        insertion_index++;
    }
    uint16_t offset = HEADER_SIZE + SLOT_SIZE * leaf->num_keys + leaf->free_space;

    for (int i = 0, j = 0; i < leaf->num_keys; i++, j++) {
        if (j == insertion_index) j++;
        temp_slots[j].key = leaf->slots[i].key;
        temp_slots[j].size = leaf->slots[i].size;
        temp_slots[j].trx_id = leaf->slots[i].trx_id;
        temp_slots[j].offset = leaf->slots[i].offset;
    }
    temp_slots[insertion_index].key = key;
    temp_slots[insertion_index].size = val_size;
    temp_slots[insertion_index].trx_id = 0;
    temp_slots[insertion_index].offset = offset - val_size;
    memcpy(temp_page + offset, (char*)leaf + offset, PAGE_SIZE - offset);
    memcpy(temp_page + offset - val_size, value, val_size);

    int num_keys = leaf->num_keys;
    leaf->num_keys = 0;
    leaf->free_space = FREE_SPACE;

    int total_size = 0;
    int split;
    for (split = 0; split <= num_keys; split++) {
        total_size += (SLOT_SIZE + temp_slots[split].size);
        if (total_size >= FREE_SPACE / 2) break;
    }
    new_pgnum = make_leaf(table_id);

    buffer_read_page(table_id, new_pgnum, &new_leaf);

    offset = PAGE_SIZE;
    for (int i = 0; i < split; i++) {
        offset -= temp_slots[i].size;
        leaf->slots[i].key = temp_slots[i].key;
        leaf->slots[i].size = temp_slots[i].size;
        leaf->slots[i].trx_id = temp_slots[i].trx_id;
        leaf->slots[i].offset = offset;
        memcpy((char*)leaf + offset, temp_page + temp_slots[i].offset, temp_slots[i].size);
        leaf->num_keys++;
        leaf->free_space -= (SLOT_SIZE + temp_slots[i].size);
    }

    offset = PAGE_SIZE;
    for (int i = split, j = 0; i <= num_keys; i++, j++) {
        offset -= temp_slots[i].size;
        new_leaf->slots[j].key = temp_slots[i].key;
        new_leaf->slots[j].size = temp_slots[i].size;
        new_leaf->slots[j].trx_id = temp_slots[i].trx_id;
        new_leaf->slots[j].offset = offset;
        memcpy((char*)new_leaf + offset, temp_page + temp_slots[i].offset, temp_slots[i].size);
        new_leaf->num_keys++;
        new_leaf->free_space -= (SLOT_SIZE + temp_slots[i].size);
    }

    new_leaf->sibling = leaf->sibling;
    leaf->sibling = new_pgnum;

    new_leaf->parent = leaf->parent;
    int64_t new_key = new_leaf->slots[0].key;

    buffer_write_page(table_id, leaf_pgnum);
    buffer_write_page(table_id, new_pgnum);

    insert_into_parent(table_id, leaf_pgnum, new_key, new_pgnum);
}

void insert_into_parent(int64_t table_id,
                        pagenum_t left_pgnum, int64_t key, pagenum_t right_pgnum) {
    pagenum_t parent_pgnum;
    page_t *left, *parent;

    buffer_read_page(table_id, left_pgnum, &left);
    parent_pgnum = left->parent;
    buffer_unpin_page(table_id, left_pgnum);

    if (parent_pgnum == 0) {
        insert_into_new_root(table_id, left_pgnum, key, right_pgnum);
        return;
    }

    int left_index = get_left_index(table_id, parent_pgnum, left_pgnum);

    buffer_read_page(table_id, parent_pgnum, &parent);
    int num_keys = parent->num_keys;
    buffer_unpin_page(table_id, parent_pgnum);

    if (num_keys < ENTRY_ORDER - 1) {
        insert_into_page(table_id, parent_pgnum, left_index, key, right_pgnum);
    } else {
        insert_into_page_split(table_id, parent_pgnum, left_index, key, right_pgnum);
    }
}

void insert_into_page(int64_t table_id, pagenum_t p_pgnum,
                      int left_index, int64_t key, pagenum_t right_pgnum) {
    page_t* p;

    buffer_read_page(table_id, p_pgnum, &p);

    for (int i = p->num_keys; i > left_index; i--) {
        p->entries[i].key = p->entries[i - 1].key;
        p->entries[i].child = p->entries[i - 1].child;
    }
    p->entries[left_index].child = right_pgnum;
    p->entries[left_index].key = key;
    p->num_keys++;

    buffer_write_page(table_id, p_pgnum);
}

void insert_into_page_split(int64_t table_id, pagenum_t old_pgnum,
                            int left_index, int64_t key, pagenum_t right_pgnum) {
    pagenum_t new_pgnum;
    page_t *old_page, *new_page, *child;
    int i, j;
    pagenum_t temp_left_child;
    entry_t temp[ENTRY_ORDER];

    buffer_read_page(table_id, old_pgnum, &old_page);

    temp_left_child = old_page->left_child;
    for (i = 0, j = 0; i < old_page->num_keys; i++, j++) {
        if (j == left_index) j++;
        temp[j].child = old_page->entries[i].child;
    }
    for (i = 0, j = 0; i < old_page->num_keys; i++, j++) {
        if (j == left_index) j++;
        temp[j].key = old_page->entries[i].key;
    }
    temp[left_index].child = right_pgnum;
    temp[left_index].key = key;

    int split = ENTRY_ORDER / 2 + 1;
    new_pgnum = make_page(table_id);

    buffer_read_page(table_id, new_pgnum, &new_page);

    old_page->num_keys = 0;
    old_page->left_child = temp_left_child;
    for (i = 0; i < split - 1; i++) {
        old_page->entries[i].child = temp[i].child;
        old_page->entries[i].key = temp[i].key;
        old_page->num_keys++;
    }
    int64_t k_prime = temp[split - 1].key;
    new_page->left_child = temp[i].child;
    for (++i, j = 0; i < ENTRY_ORDER; i++, j++) {
        new_page->entries[j].child = temp[i].child;
        new_page->entries[j].key = temp[i].key;
        new_page->num_keys++;
    }
    new_page->parent = old_page->parent;

    buffer_read_page(table_id, new_page->left_child, &child);
    child->parent = new_pgnum;
    buffer_write_page(table_id, new_page->left_child);
    for (i = 0; i < new_page->num_keys; i++) {
        buffer_read_page(table_id, new_page->entries[i].child, &child);
        child->parent = new_pgnum;
        buffer_write_page(table_id, new_page->entries[i].child);
    }

    buffer_write_page(table_id, new_pgnum);
    buffer_write_page(table_id, old_pgnum);

    insert_into_parent(table_id, old_pgnum, k_prime, new_pgnum);
}

void start_tree(int64_t table_id, int64_t key, char* value, uint16_t val_size) {
    pagenum_t root_pgnum;
    page_t *root, *header;

    root_pgnum = make_leaf(table_id);

    buffer_read_page(table_id, root_pgnum, &root);
    buffer_read_page(table_id, 0, &header);

    uint16_t offset = PAGE_SIZE - val_size;
    root->slots[0].key = key;
    root->slots[0].size = val_size;
    root->slots[0].trx_id = 0;
    root->slots[0].offset = offset;
    memcpy((char*)root + offset, value, val_size);
    root->free_space -= (SLOT_SIZE + val_size);
    root->parent = 0;
    root->num_keys++;
    header->root_num = root_pgnum;

    buffer_write_page(table_id, root_pgnum);
    buffer_write_page(table_id, 0);
}

void insert_into_new_root(int64_t table_id,
                          pagenum_t left_pgnum, int64_t key, pagenum_t right_pgnum) {
    pagenum_t root_pgnum;
    page_t *left, *right, *root, *header;

    root_pgnum = make_page(table_id);

    buffer_read_page(table_id, left_pgnum, &left);
    buffer_read_page(table_id, right_pgnum, &right);
    buffer_read_page(table_id, root_pgnum, &root);
    buffer_read_page(table_id, 0, &header);

    root->left_child = left_pgnum;
    root->entries[0].key = key;
    root->entries[0].child = right_pgnum;
    root->num_keys++;
    root->parent = 0;
    left->parent = root_pgnum;
    right->parent = root_pgnum;
    header->root_num = root_pgnum;

    buffer_write_page(table_id, left_pgnum);
    buffer_write_page(table_id, right_pgnum);
    buffer_write_page(table_id, root_pgnum);
    buffer_write_page(table_id, 0);
}

pagenum_t make_leaf(int64_t table_id) {
    pagenum_t new_pgnum;
    page_t* new_leaf;
    new_pgnum = make_page(table_id);
    buffer_read_page(table_id, new_pgnum, &new_leaf);
    new_leaf->is_leaf = 1;
    new_leaf->free_space = FREE_SPACE;
    new_leaf->sibling = 0;
    buffer_write_page(table_id, new_pgnum);
    return new_pgnum;
}

pagenum_t make_page(int64_t table_id) {
    pagenum_t new_pgnum;
    page_t* new_page;
    new_pgnum = buffer_alloc_page(table_id);
    buffer_read_page(table_id, new_pgnum, &new_page);
    new_page->parent = 0;
    new_page->is_leaf = 0;
    new_page->num_keys = 0;
    buffer_write_page(table_id, new_pgnum);
    return new_pgnum;
}

int get_left_index(int64_t table_id, pagenum_t parent_pgnum, pagenum_t left_pgnum) {
    page_t* parent;
    buffer_read_page(table_id, parent_pgnum, &parent);
    int left_index = 0;
    if (parent->left_child == left_pgnum) {
        buffer_unpin_page(table_id, parent_pgnum);
        return left_index;
    }
    do {
        left_index++;
    } while (parent->entries[left_index - 1].child != left_pgnum);
    buffer_unpin_page(table_id, parent_pgnum);
    return left_index;
}

// Deletion

int db_delete(int64_t table_id, int64_t key) {
    pagenum_t leaf_pgnum, sibling_pgnum, parent_pgnum;
    page_t *leaf, *sibling, *parent;

    leaf_pgnum = find_leaf(table_id, key);

    buffer_read_page(table_id, leaf_pgnum, &leaf);
    int i;
    int num_keys = leaf->num_keys;
    for (i = 0; i < num_keys; i++) {
        if (leaf->slots[i].key == key) break;
    }
    buffer_unpin_page(table_id, leaf_pgnum);

    if (i == num_keys) return -1;

    delete_from_leaf(table_id, leaf_pgnum, key);

    buffer_read_page(table_id, leaf_pgnum, &leaf);
    parent_pgnum = leaf->parent;
    int leaf_num_keys = leaf->num_keys;
    int leaf_free_space = leaf->free_space;
    buffer_unpin_page(table_id, leaf_pgnum);

    if (parent_pgnum == 0) {
        if (leaf_num_keys > 0) return 0;
        end_tree(table_id, leaf_pgnum);
        return 0;
    }

    if (leaf_free_space < THRESHOLD) {
        return 0;
    }

    int sibling_index = get_sibling_index(table_id, parent_pgnum, leaf_pgnum);

    buffer_read_page(table_id, parent_pgnum, &parent);
    int k_prime_index = (sibling_index != -1) ? sibling_index : 0;
    int64_t k_prime = parent->entries[k_prime_index].key;
    if (sibling_index == -1) {
        sibling_pgnum = parent->entries[0].child;
    } else if (sibling_index == 0) {
        sibling_pgnum = parent->left_child;
    } else {
        sibling_pgnum = parent->entries[sibling_index - 1].child;
    }
    buffer_unpin_page(table_id, parent_pgnum);

    buffer_read_page(table_id, sibling_pgnum, &sibling);
    int sibling_free_space = sibling->free_space;
    buffer_unpin_page(table_id, sibling_pgnum);

    if (sibling_free_space + leaf_free_space >= FREE_SPACE) {
        merge_leaves(table_id, leaf_pgnum, sibling_pgnum, sibling_index, k_prime);
    } else {
        redistribute_leaves(table_id, leaf_pgnum, sibling_pgnum, sibling_index, k_prime_index);
    }

    return 0;
}

void delete_from_leaf(int64_t table_id, pagenum_t leaf_pgnum, int64_t key) {
    page_t* leaf;

    buffer_read_page(table_id, leaf_pgnum, &leaf);

    int key_index = 0;
    while (leaf->slots[key_index].key != key) key_index++;

    uint16_t val_size = leaf->slots[key_index].size;
    uint16_t deletion_offset = leaf->slots[key_index].offset;
    uint16_t insertion_offset = HEADER_SIZE + SLOT_SIZE * leaf->num_keys + leaf->free_space;

    for (int i = key_index + 1; i < leaf->num_keys; i++) {
        leaf->slots[i - 1].key = leaf->slots[i].key;
        leaf->slots[i - 1].size = leaf->slots[i].size;
        leaf->slots[i - 1].offset = leaf->slots[i].offset;
    }
    memmove((char*)leaf + insertion_offset + val_size,
            (char*)leaf + insertion_offset, deletion_offset - insertion_offset);

    leaf->num_keys--;
    leaf->free_space += (SLOT_SIZE + val_size);

    for (int i = 0; i < leaf->num_keys; i++) {
        if (leaf->slots[i].offset < deletion_offset) {
            leaf->slots[i].offset += val_size;
        }
    }

    buffer_write_page(table_id, leaf_pgnum);
}

void merge_leaves(int64_t table_id, pagenum_t leaf_pgnum,
                  pagenum_t sibling_pgnum, int sibling_index, int64_t k_prime) {
    pagenum_t parent_pgnum;
    page_t *leaf, *sibling;

    if (sibling_index != -1) {
        buffer_read_page(table_id, leaf_pgnum, &leaf);
        buffer_read_page(table_id, sibling_pgnum, &sibling);
    } else {
        buffer_read_page(table_id, sibling_pgnum, &leaf);
        buffer_read_page(table_id, leaf_pgnum, &sibling);
    }

    uint16_t sibling_offset = HEADER_SIZE + SLOT_SIZE * sibling->num_keys + sibling->free_space;
    uint16_t sibling_size = PAGE_SIZE - sibling_offset;
    uint16_t leaf_offset = HEADER_SIZE + SLOT_SIZE * leaf->num_keys + leaf->free_space;
    uint16_t leaf_size = PAGE_SIZE - leaf_offset;

    for (int i = 0, j = sibling->num_keys; i < leaf->num_keys; i++, j++) {
        sibling->slots[j].key = leaf->slots[i].key;
        sibling->slots[j].size = leaf->slots[i].size;
        sibling->slots[j].offset = leaf->slots[i].offset - sibling_size;
        sibling->num_keys++;
        sibling->free_space -= (leaf->slots[i].size + SLOT_SIZE);
    }
    memcpy((char*)sibling + sibling_offset - leaf_size, (char*)leaf + leaf_offset, leaf_size);
    sibling->sibling = leaf->sibling;

    parent_pgnum = leaf->parent;

    if (sibling_index != -1) {
        buffer_unpin_page(table_id, leaf_pgnum);
        buffer_write_page(table_id, sibling_pgnum);
        delete_from_child(table_id, parent_pgnum, k_prime, leaf_pgnum);
    } else {
        buffer_unpin_page(table_id, sibling_pgnum);
        buffer_write_page(table_id, leaf_pgnum);
        delete_from_child(table_id, parent_pgnum, k_prime, sibling_pgnum);
    }
}

void redistribute_leaves(int64_t table_id, pagenum_t leaf_pgnum,
                         pagenum_t sibling_pgnum, int sibling_index, int k_prime_index) {
    page_t *leaf, *sibling, *parent;

    buffer_read_page(table_id, leaf_pgnum, &leaf);
    buffer_read_page(table_id, sibling_pgnum, &sibling);

    while (leaf->free_space >= THRESHOLD) {
        int src_index = (sibling_index != -1) ? sibling->num_keys - 1 : 0;
        int dest_index = (sibling_index != -1) ? 0 : leaf->num_keys;
        int16_t src_size = sibling->slots[src_index].size;
        int16_t dest_offset = HEADER_SIZE + SLOT_SIZE * leaf->num_keys + leaf->free_space - src_size;

        if (sibling_index != -1) {
            for (int i = leaf->num_keys; i > 0; i--) {
                leaf->slots[i].key = leaf->slots[i - 1].key;
                leaf->slots[i].size = leaf->slots[i - 1].size;
                leaf->slots[i].offset = leaf->slots[i - 1].offset;
            }
        }
        leaf->slots[dest_index].key = sibling->slots[src_index].key;
        leaf->slots[dest_index].size = sibling->slots[src_index].size;
        leaf->slots[dest_index].offset = dest_offset;
        memcpy((char*)leaf + dest_offset,
               (char*)sibling + sibling->slots[src_index].offset, src_size);

        leaf->num_keys++;
        leaf->free_space -= (SLOT_SIZE + src_size);
        int64_t rotate_key = sibling->slots[src_index].key;

        buffer_unpin_page(table_id, sibling_pgnum);
        delete_from_leaf(table_id, sibling_pgnum, rotate_key);
        buffer_read_page(table_id, sibling_pgnum, &sibling);
    }

    buffer_read_page(table_id, leaf->parent, &parent);
    parent->entries[k_prime_index].key =
        (sibling_index != -1) ? leaf->slots[0].key : sibling->slots[0].key;
    buffer_write_page(table_id, leaf->parent);

    buffer_write_page(table_id, leaf_pgnum);
    buffer_write_page(table_id, sibling_pgnum);
}

void delete_from_child(int64_t table_id,
                       pagenum_t p_pgnum, int64_t key, pagenum_t child_pgnum) {
    pagenum_t sibling_pgnum, parent_pgnum;
    page_t *p, *sibling, *parent;

    delete_from_page(table_id, p_pgnum, key, child_pgnum);

    buffer_read_page(table_id, p_pgnum, &p);
    parent_pgnum = p->parent;
    int p_num_keys = p->num_keys;
    buffer_unpin_page(table_id, p_pgnum);

    if (parent_pgnum == 0) {
        if (p_num_keys > 0) return;
        adjust_root(table_id, p_pgnum);
        return;
    }

    if (p_num_keys >= ENTRY_ORDER / 2) {
        return;
    }

    int sibling_index = get_sibling_index(table_id, parent_pgnum, p_pgnum);

    buffer_read_page(table_id, parent_pgnum, &parent);
    int k_prime_index = (sibling_index != -1) ? sibling_index : 0;
    int64_t k_prime = parent->entries[k_prime_index].key;
    if (sibling_index == 0) {
        sibling_pgnum = parent->left_child;
    } else if (sibling_index == -1) {
        sibling_pgnum = parent->entries[0].child;
    } else {
        sibling_pgnum = parent->entries[sibling_index - 1].child;
    }
    buffer_unpin_page(table_id, parent_pgnum);

    buffer_read_page(table_id, sibling_pgnum, &sibling);
    int sibling_num_keys = sibling->num_keys;
    buffer_unpin_page(table_id, sibling_pgnum);

    if (sibling_num_keys + p_num_keys < ENTRY_ORDER - 1) {
        merge_pages(table_id, p_pgnum, sibling_pgnum, sibling_index, k_prime);
    } else {
        redistribute_pages(table_id, p_pgnum,
                           sibling_pgnum, sibling_index, k_prime_index, k_prime);
    }
}

void delete_from_page(int64_t table_id, pagenum_t p_pgnum,
                      int64_t key, pagenum_t child_pgnum) {
    page_t* p;

    buffer_read_page(table_id, p_pgnum, &p);

    int i = 0;
    while (p->entries[i].key != key) i++;
    for (++i; i < p->num_keys; i++) {
        p->entries[i - 1].key = p->entries[i].key;
    }

    i = 0;
    if (p->left_child != child_pgnum) {
        i++;
        while (p->entries[i - 1].child != child_pgnum) i++;
    }
    for (; i < p->num_keys; i++) {
        if (i == 0)
            p->left_child = p->entries[0].child;
        else
            p->entries[i - 1].child = p->entries[i].child;
    }

    p->num_keys--;

    buffer_write_page(table_id, p_pgnum);

    buffer_free_page(table_id, child_pgnum);
}

void merge_pages(int64_t table_id, pagenum_t p_pgnum,
                 pagenum_t sibling_pgnum, int sibling_index, int64_t k_prime) {
    pagenum_t parent_pgnum;
    page_t *p, *sibling, *nephew;

    if (sibling_index != -1) {
        buffer_read_page(table_id, p_pgnum, &p);
        buffer_read_page(table_id, sibling_pgnum, &sibling);
    } else {
        buffer_read_page(table_id, sibling_pgnum, &p);
        buffer_read_page(table_id, p_pgnum, &sibling);
    }

    int insertion_index = sibling->num_keys;
    sibling->entries[insertion_index].key = k_prime;
    sibling->num_keys++;
    int p_end = p->num_keys;

    sibling->entries[insertion_index].child = p->left_child;
    for (int i = insertion_index + 1, j = 0; j < p_end; i++, j++) {
        sibling->entries[i].key = p->entries[j].key;
        sibling->entries[i].child = p->entries[j].child;
        sibling->num_keys++;
    }

    buffer_read_page(table_id, sibling->left_child, &nephew);
    nephew->parent = (sibling_index != -1) ? sibling_pgnum : p_pgnum;
    buffer_write_page(table_id, sibling->left_child);
    for (int i = 0; i < sibling->num_keys; i++) {
        buffer_read_page(table_id, sibling->entries[i].child, &nephew);
        nephew->parent = (sibling_index != -1) ? sibling_pgnum : p_pgnum;
        buffer_write_page(table_id, sibling->entries[i].child);
    }

    parent_pgnum = p->parent;

    if (sibling_index != -1) {
        buffer_unpin_page(table_id, p_pgnum);
        buffer_write_page(table_id, sibling_pgnum);
        delete_from_child(table_id, parent_pgnum, k_prime, p_pgnum);
    } else {
        buffer_unpin_page(table_id, sibling_pgnum);
        buffer_write_page(table_id, p_pgnum);
        delete_from_child(table_id, parent_pgnum, k_prime, sibling_pgnum);
    }
}

void redistribute_pages(int64_t table_id, pagenum_t p_pgnum,
                        pagenum_t sibling_pgnum, int sibling_index,
                        int k_prime_index, int64_t k_prime) {
    page_t *p, *sibling, *parent, *child;

    buffer_read_page(table_id, p_pgnum, &p);
    buffer_read_page(table_id, sibling_pgnum, &sibling);

    if (sibling_index != -1) {
        for (int i = p->num_keys; i > 0; i--) {
            p->entries[i].key = p->entries[i - 1].key;
            p->entries[i].child = p->entries[i - 1].child;
        }
        p->entries[0].child = p->left_child;

        p->left_child = sibling->entries[sibling->num_keys - 1].child;
        buffer_read_page(table_id, p->left_child, &child);
        child->parent = p_pgnum;
        buffer_write_page(table_id, p->left_child);
        p->entries[0].key = k_prime;

        buffer_read_page(table_id, p->parent, &parent);
        parent->entries[k_prime_index].key = sibling->entries[sibling->num_keys - 1].key;
        buffer_write_page(table_id, p->parent);
    } else {
        p->entries[p->num_keys].key = k_prime;
        p->entries[p->num_keys].child = sibling->left_child;
        buffer_read_page(table_id, p->entries[p->num_keys].child, &child);
        child->parent = p_pgnum;
        buffer_write_page(table_id, p->entries[p->num_keys].child);

        buffer_read_page(table_id, p->parent, &parent);
        parent->entries[k_prime_index].key = sibling->entries[0].key;
        buffer_write_page(table_id, p->parent);

        sibling->left_child = sibling->entries[0].child;
        for (int i = 0; i < sibling->num_keys - 1; i++) {
            sibling->entries[i].key = sibling->entries[i + 1].key;
            sibling->entries[i].child = sibling->entries[i + 1].child;
        }
    }

    p->num_keys++;
    sibling->num_keys--;

    buffer_write_page(table_id, p_pgnum);
    buffer_write_page(table_id, sibling_pgnum);
}

void end_tree(int64_t table_id, pagenum_t root_pgnum) {
    page_t* header;
    
    buffer_read_page(table_id, 0, &header);

    header->root_num = 0;

    buffer_write_page(table_id, 0);

    buffer_free_page(table_id, root_pgnum);
}

void adjust_root(int64_t table_id, pagenum_t root_pgnum) {
    page_t *root, *new_root, *header;

    buffer_read_page(table_id, root_pgnum, &root);
    buffer_read_page(table_id, root->left_child, &new_root);
    buffer_read_page(table_id, 0, &header);

    new_root->parent = 0;
    header->root_num = root->left_child;

    buffer_write_page(table_id, 0);
    buffer_write_page(table_id, root->left_child);
    buffer_unpin_page(table_id, root_pgnum);

    buffer_free_page(table_id, root_pgnum);
}

int get_sibling_index(int64_t table_id, pagenum_t parent_pgnum, pagenum_t p_pgnum) {
    page_t* parent;
    buffer_read_page(table_id, parent_pgnum, &parent);
    int sibling_index = -1;
    if (parent->left_child == p_pgnum) {
        buffer_unpin_page(table_id, parent_pgnum);
        return sibling_index;
    }
    do {
        sibling_index++;
    } while (parent->entries[sibling_index].child != p_pgnum);
    buffer_unpin_page(table_id, parent_pgnum);
    return sibling_index;
}
