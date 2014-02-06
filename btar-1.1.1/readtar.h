struct readtar_file
{
    const struct header_gnu_tar *header;
    char *name;
    char *linkname;
    unsigned long long size;
};

enum readtar_newfile_result {
    READTAR_NORMAL,
    READTAR_SKIPDATA
};

struct readtar_callbacks
{
    enum readtar_newfile_result 
        (*new_file)(const struct readtar_file *file, void *userdata);
    void (*new_data)(const char *data, size_t len, void *userdata);
    void *userdata;
};

struct readtar
{
    enum {
        IN_HEADER,
        IN_DATA,
        IN_EXTRA_DATA,
        IN_LONG_FILENAME,
        IN_LONG_LINKNAME
    } state;
    char *longfilename;
    char *longlinkname;
    struct header_gnu_tar header;
    unsigned long long filedata_left;
    unsigned long long until_header_left;
    unsigned long long data_read;
    unsigned long long total_data_read;
    struct readtar_callbacks cb;
    int good_header;
    int fd;
};

void init_readtar(struct readtar *readtar, const struct readtar_callbacks *cb);
void read_full_tar(int infd, struct readtar *readtar);
void process_this_tar_data(struct readtar *readtar, const char *data, size_t len);
size_t readtar_bytes_to_next_change(const struct readtar *readtar);
