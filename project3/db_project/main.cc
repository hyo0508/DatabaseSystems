#include "bpt.h"

#include <queue>
#include <vector>
#include <iterator>
#include <algorithm>
#include <random>

#define NUM_KEYS 1000
#define NUM_BUFS 3
#define NUM_REPS 5

using namespace std;

void print_page(pagenum_t page_num, page_t page);
void print_pgnum(int64_t table_id, pagenum_t page_num); 
void print_pgnum_from_disk(int64_t table_id, pagenum_t page_num); 
void print_all(int64_t table_id);
void print_all_from_disk(int64_t table_id);
int path_to_root(int64_t table_id, pagenum_t child_num);
void print_tree(int64_t table_id);
int path_to_root_from_disk(int64_t table_id, pagenum_t child_num);
void print_tree_from_disk(int64_t table_id);
void print_LRUs();
void print_buffers();
void print_freepg_list(int64_t table_id);

char* val(int key);
int size();
int size(int key);

#if 1
int main(int argc, char** argv) {
	vector<int> keys;
	for (int i = 0; i < NUM_KEYS; i++) {
		keys.push_back(i);
	}
	random_device rd;
	mt19937 gen(rd());
	default_random_engine rng(rd());
	char value[112];
	uint16_t val_size;

	printf("[TEST START]\n\n");
    printf("[NUM_KEYS : %7d] \n", NUM_KEYS);
    printf("[NUM_BUFS : %7d] \n", NUM_BUFS);
    printf("[NUM_REPS : %7d] \n", NUM_REPS);
    printf("\n");

    printf("[INIT START]\n");
	init_db(NUM_BUFS);
    printf("[INIT END]\n\n");

    printf("[OPEN START]\n");
	int64_t table_id = open_table((char*)"table0");
    printf("[OPEN END]\n\n");

    for (int j = 0; j < NUM_REPS; j++) {
        shuffle(keys.begin(), keys.end(), rng);

        printf("[REP%2d] ", j);

        printf("[INSERT START]\n");
        for (const auto& i : keys) {
            // printf("insert %4d\n", i);
            if (db_insert(table_id, i, val(i), size(i)) != 0) goto func_exit;
        }
        printf("\t[INSERT END]\n");

        printf("\t[FIND START]\n");
        for (const auto& i : keys) {
            memset(value, 0x00, 112);
            val_size = 0;
            // printf("find %4d\n", i);
            if(db_find(table_id, i, value, &val_size) != 0) goto func_exit;
            // else if (size(i) != val_size ||
            // 		 val(i) != std::string(value, val_size)) {
            // 	printf("value dismatched\n");
            // 	goto func_exit;
            // }
        }
        printf("\t[FIND END]\n");

        // print_tree(table_id);
        // printf("\n");

        printf("\t[DELETE START]\n");
        for (const auto& i : keys) {
            // printf("delete %4d\n", i);
            if (db_delete(table_id, i) != 0) goto func_exit;
        }
        printf("\t[DELETE END]\n");

        printf("\t[FIND START AGAIN]\n");
        for (const auto& i : keys) {
            memset(value, 0x00, 112);
            val_size = 0;
            if (db_find(table_id, i, value, &val_size) == 0) goto func_exit;
        }
        printf("\t[FIND END AGAIN]\n");

        // print_tree(table_id);
        // printf("\n");
    }

    printf("\n[TEST END]\n\n");    

	func_exit:
	printf("[SHUTDOWN START]\n");
    // print_freepg_list(table_id);
    // printf("\n");
	// print_tree(table_id);
    // printf("\n");
	if (shutdown_db() != 0) {
		return 0;
	}
	printf("[SHUTDOWN END]\n");

    // print_tree_from_disk(table_id);

	return 0;
}
#endif


#if 0
void test1_read_page(int64_t table_id, pagenum_t page_num, page_t** dest_page) {
    page_t * page;
    page = (page_t *)malloc(sizeof(page_t));
    file_read_page(table_id, page_num, page);
    *dest_page = page;
}
void test1_write_page(int64_t table_id, pagenum_t page_num, page_t* const* src_page) {
    file_write_page(table_id, page_num, *src_page);
}
void test2_read_page(int64_t table_id, pagenum_t page_num, page_t** dest_page) {
	*dest_page = &(buffers[0]->frame);
}
int main(int argc, char** argv) {
	init_db(200);
	int64_t table_id = open_table((char*)"test_table");

	page_t* header;
    page_t* temp;
	
	test1_read_page(table_id, 0, &header);
	printf("header->next_frpg: %ld\n", header->next_frpg);
	printf("header->num_pages: %ld\n", header->num_pages);
	printf("header->root_num: %ld\n", header->root_num);
    header->next_frpg = 1252;

    // test1_read_page(table_id, 1, &temp);
    
    test1_write_page(table_id, 0, &header);

    header->next_frpg = 1111;

    // test1_read_page(table_id, 0, &header);

	printf("header->next_frpg: %ld\n", header->next_frpg);
	printf("header->num_pages: %ld\n", header->num_pages);
	printf("header->root_num: %ld\n", header->root_num);

	// buffers[0] = (buffer_t*)malloc(sizeof(buffer_t));
	// file_read_page(table_id, 0, &(buffers[0]->frame));

	// page_t* leaf;
	// leaf = (page_t*)malloc(sizeof(page_t));
	// test2_read_page(table_id, 0, &leaf);
	

	// printf("leaf->next_frpg: %ld\n", leaf->next_frpg);
	// printf("leaf->num_pages: %ld\n", leaf->num_pages);
	// printf("leaf->root_num: %ld\n", leaf->root_num);

	// leaf->next_frpg = 1252;

	// printf("\n");
	// printf("buffers[0]->next_frpg: %ld\n", buffers[0]->frame.next_frpg);
		
    return 0;
}
#endif

void print_page(pagenum_t page_num, page_t page) {
	if (page_num == 0) {
		printf("----header information----\n");
        printf("number: %ld\n", page_num);
		printf("next_frpg: %ld\n", page.next_frpg);
		printf("num_pages: %ld\n", page.num_pages);
		printf("root_num: %ld\n", page.root_num);
		printf("\n");
		return;
	}
	if (page.is_leaf) {
        if (page.parent)
		    printf("-----leaf information-----\n");
        else
            printf("-----root information-----\n");
        printf("number: %ld\n", page_num);
		printf("parent: %ld\n", page.parent);
		printf("is_leaf: %d\n", page.is_leaf);
		printf("num_keys: %d\n", page.num_keys);
		printf("free_space: %ld\n", page.free_space);
		printf("sibling: %ld\n", page.sibling);

		for (int i = 0; i < page.num_keys; i++) {
			printf("key: %3ld, size: %3d, offset: %4d, value: %s\n", page.slots[i].key, page.slots[i].size, page.slots[i].offset, page.values + page.slots[i].offset - HEADER_SIZE);
		}
	}
	else {
        if (page.parent)
		    printf("-----page information-----\n");
        else
            printf("-----root information-----\n");
        printf("number: %ld\n", page_num);
		printf("parent: %ld\n", page.parent);
		printf("is_leaf: %d\n", page.is_leaf);
		printf("num_keys: %d\n", page.num_keys);

		printf("left_child: %ld\n", page.left_child);
		for (int i = 0; i < page.num_keys; i++) {
			printf("key: %3ld, child: %ld\n", page.entries[i].key, page.entries[i].child);
		}
	}
    printf("\n");
}

void print_pgnum(int64_t table_id, pagenum_t page_num) {
	page_t* page;
	int idx = buffer_read_page(table_id, page_num, &page);
	print_page(page_num, *page);
	if (idx != -1) buffers[idx]->is_pinned--;
}

void print_pgnum_from_disk(int64_t table_id, pagenum_t page_num) {
    page_t page;
    file_read_page(table_id, page_num, &page);
    print_page(page_num, page);
}

void print_all(int64_t table_id) {
    pagenum_t root_num, temp_num;
    page_t* root, * page;

    root_num = get_root_num(table_id);
	printf("-----header information-----\n");
	printf("root_num: %ld\n", root_num);
	printf("\n");
	if (!root_num) return;

    int root_idx = buffer_read_page(table_id, root_num, &root);
    print_page(root_num, *root);
    if (root->is_leaf) {
        if (root_idx != -1) buffers[root_idx]->is_pinned--;
        return;
    }

	int temp_idx;

    temp_num = root->left_child;
    temp_idx = buffer_read_page(table_id, temp_num, &page);
    print_page(temp_num, *page); 
    for (int i = 0; i < root->num_keys; i++) {
        temp_num = root->entries[i].child;
		if (temp_idx != -1) buffers[temp_idx]->is_pinned--;
        temp_idx = buffer_read_page(table_id, temp_num, &page);
        print_page(temp_num, *page);
    }
	if (root_idx != -1) buffers[root_idx]->is_pinned--;
	if (temp_idx != -1) buffers[temp_idx]->is_pinned--;
}

void print_all_from_disk(int64_t table_id) {
    pagenum_t root_num, temp_num;
    page_t root, page;

    root_num = get_root_num(table_id);
	printf("-----header information-----\n");
	printf("root_num: %ld\n", root_num);
	printf("\n");
	if (!root_num) return;

    file_read_page(table_id, root_num, &root);
    print_page(root_num, root);
    if (root.is_leaf) return;

    temp_num = root.left_child;
    file_read_page(table_id, temp_num, &page);
    print_page(temp_num, page);
    for (int i = 0; i < root.num_keys; i++) {
        temp_num = root.entries[i].child;
        file_read_page(table_id, temp_num, &page);
        print_page(temp_num, page);
    }
}

char* val(int key) {
	static char value[3];
	sprintf(value, "%02d", key % 100);
	return value;
}

int size() {
	return rand() % 63 + 50;
}
int size(int key) {
	return key % 63 + 50;
}

int path_to_root(int64_t table_id, pagenum_t child_num) {
	int length = 0;
	pagenum_t c_num = child_num;
	page_t* c;
	int c_idx = buffer_read_page(table_id, c_num, &c);
	while (c->parent != 0) {
		c_num = c->parent;
		if (c_idx != -1) buffers[c_idx]->is_pinned--;
		c_idx = buffer_read_page(table_id, c_num, &c);
		length++;
	}
	if (c_idx != -1) buffers[c_idx]->is_pinned--;
	return length;
}

void print_tree(int64_t table_id) {
	pagenum_t root_pgnum, p_pgnum;
	page_t *p, *parent;
	int i = 0, rank = 0, new_rank = 0;
	int p_idx, parent_idx;
	
	root_pgnum = get_root_num(table_id);
	if (root_pgnum == 0) {
		printf("Empty tree.\n");
		return;
	}

	std::queue<pagenum_t> queue;
	queue.push(root_pgnum);
	while (!queue.empty()) {
		p_pgnum = queue.front();
		queue.pop();
		p_idx = buffer_read_page(table_id, p_pgnum, &p);
		parent_idx = buffer_read_page(table_id, p->parent, &parent);

		if (p->parent != 0 && p_pgnum == parent->left_child) {
			new_rank = path_to_root(table_id, p_pgnum);
			if (new_rank != rank) {
				rank = new_rank;
				printf("\n");
			}
		}
		if (p->is_leaf) printf("%ld ~ %ld ", p->slots[0].key, p->slots[p->num_keys - 1].key);
		else for (i = 0; i < p->num_keys; i++) printf("%ld ", p->entries[i].key);
		if (!p->is_leaf) {
			queue.push(p->left_child);
			for (i = 0; i < p->num_keys; i++)
				queue.push(p->entries[i].child);
		}
		printf("| ");

		if (p_idx != -1) buffers[p_idx]->is_pinned--;
		if (parent_idx != -1) buffers[parent_idx]->is_pinned--;
	}
	printf("\n");

}

int path_to_root_from_disk(int64_t table_id, pagenum_t child_num) {
	int length = 0;
	pagenum_t c_num = child_num;
	page_t c;
	file_read_page(table_id, c_num, &c);
	while (c.parent != 0) {
		c_num = c.parent;
		file_read_page(table_id, c_num, &c);
		length++;
	}
	return length;
}

void print_tree_from_disk(int64_t table_id) {
	pagenum_t root_pgnum, p_pgnum;
	page_t p, parent;
	int i = 0, rank = 0, new_rank = 0;
	
	root_pgnum = get_root_num(table_id);
	if (root_pgnum == 0) {
		printf("Empty tree.\n");
		return;
	}

	std::queue<pagenum_t> queue;
	queue.push(root_pgnum);
	while (!queue.empty()) {
		p_pgnum = queue.front();
		queue.pop();
		file_read_page(table_id, p_pgnum, &p);
		file_read_page(table_id, p.parent, &parent);
		if (p.parent != 0 && p_pgnum == parent.left_child) {
			new_rank = path_to_root(table_id, p_pgnum);
			if (new_rank != rank) {
				rank = new_rank;
				printf("\n");
			}
		}
		if (p.is_leaf) printf("%ld ~ %ld ", p.slots[0].key, p.slots[p.num_keys - 1].key);
		else for (i = 0; i < p.num_keys; i++) printf("%ld ", p.entries[i].key);
		if (!p.is_leaf) {
			queue.push(p.left_child);
			for (i = 0; i < p.num_keys; i++)
				queue.push(p.entries[i].child);
		}
		printf("| ");
	}
	printf("\n");
}

void print_LRUs() {
    int first_LRU_idx;
    int buffer_idx;
    buffer_t * buffer;
    pagenum_t page_num;

    first_LRU_idx = get_first_LRU_idx();
    buffer = buffers[first_LRU_idx];

    int i = 0;
    while (buffer) {
        page_num = buffer->page_num;
        buffer_idx = get_buffer_idx(buffer->table_id, page_num);
        printf("[%2d] buffer_idx : %2d, page_num : %4ld\n", i, buffer_idx, page_num);
        i++;
        buffer = buffer->next_LRU;
    }
    printf("\n");
}

void print_buffers() {
    int i;
    for (i = 0; i < buffer_size; i++) {
        printf("\n\n----------buffer [%2d]----------\n", i);
        print_page(buffers[i]->page_num, buffers[i]->frame);
        printf("\n");
        printf("pagenum: %ld\n", buffers[i]->page_num);
        printf("is_pinned: %d\n", buffers[i]->is_pinned);
        printf("\n");
    }
}

void print_freepg_list(int64_t table_id) {
    page_t * page;
    pagenum_t page_num;
    pagenum_t temp;
    buffer_read_page(table_id, 0, &page);
    page_num = page->next_frpg;
    buffer_write_page(table_id, 0, &page);
    printf("   0");
    while (page_num) {
        printf("->%4ld", page_num);
        buffer_read_page(table_id, page_num, &page);
        temp = page_num;
        page_num = page->next_frpg;
        buffer_write_page(table_id, temp, &page);
    }
    printf("\n");
}