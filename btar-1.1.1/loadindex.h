#include <sys/stat.h>

struct IndexElem {
    char *name;
    time_t mtime;
    int block;
    int nblocks;
    char *signature;
    int signaturelen;
    char seen;
    char is_dir;
} *ptr;

void index_load_from_fd(int fd);
struct IndexElem * index_find_element(const char *name);
const struct IndexElem * index_get_elements(size_t *nelems);
void send_index_to_fd(int fd);
void recv_index_from_fd(int fd);
char * index_find_in_tar(int fd, unsigned long long *size);
int block_name_to_int(const char *str);
void free_index();
void index_sort();
void index_sort_inverse();
