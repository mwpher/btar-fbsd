struct block_reader;

struct block
{
    char *data;
    struct block_reader **readers;
    size_t nreaders;
    size_t writer_pos;
    size_t allocated;
    size_t total_written;
    struct block *nextblock;
    struct block *lastblock;
    int go_back;
};

struct block_reader
{
    struct block *b;
    size_t pos;
};

struct block * block_new(size_t allocate);
void block_free(struct block *b);
struct block * block_new_never_back(size_t allocate);
size_t block_fill_from_fd(struct block *b, int fd, size_t maxbytes);
size_t block_fill_from_memory(struct block *b, const char *data, size_t len);
ssize_t block_fill_from_fd_multi(struct block *b, int fd, size_t maxbytes);
int block_can_accept(struct block *b);
struct block_reader * block_reader_new(struct block *b);
int block_reader_can_read(struct block_reader *r);
void block_reset_pos_if_possible(struct block *b);
int block_reader_to_fd(struct block_reader *r, int fd);
int block_are_readers_done(struct block *b);
void block_reader_free(struct block_reader *br);
void block_realloc_set(struct block *b, size_t len, char c);
