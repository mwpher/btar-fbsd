#include <stdio.h>
#include <sys/stat.h>

struct header_gnu_tar {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag[1];
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char atime[12];
    char ctime[12];
    char offset[12];
    char longnames[4];
    char unused[1];
    struct {
        char offset[12];
        char numbytes[12];
    } sparse[4];
    char isextended[1];
    char realsize[12];
    char pad[17];
};

struct mytar
{
    struct header_gnu_tar header;
    char *longname;
    char *longlinkname;
    unsigned long long file_size;
    unsigned long long file_data_written;
    int fd;
    unsigned long long total_written;
};

struct mytar * mytar_new();
void mytar_open_fd(struct mytar *t, int fd);
void mytar_new_file(struct mytar *t);
void mytar_set_filename(struct mytar *t, char *name);
void mytar_set_uid(struct mytar *t, int uid);
void mytar_set_size(struct mytar *t, unsigned long long size);
void mytar_set_gid(struct mytar *t, int gid);
void mytar_set_mode(struct mytar *t, int mode);
void mytar_set_mtime(struct mytar *t, time_t time);
void mytar_set_atime(struct mytar *t, time_t time);
void mytar_set_from_stat(struct mytar *t, struct stat *s);
void mytar_set_linkname(struct mytar *t, const char *linkname);
int mytar_set_filetype(struct mytar *t, int type);
void mytar_set_uname(struct mytar *t, const char *uname);
void mytar_set_gname(struct mytar *t, const char *gname);
ssize_t mytar_write_header(struct mytar *t);
ssize_t mytar_write_data(struct mytar *t, const char *buffer, size_t n);
ssize_t mytar_write_end(struct mytar *t);
ssize_t mytar_write_archive_end(struct mytar *t);
int calc_checksum(const struct header_gnu_tar *h);

unsigned long long read_octal_number(const char *str, int len);
unsigned long long read_size(const char *str);
char *read_fixed_size_string(const char *str, int len);
char *read_filename(const struct header_gnu_tar *h);

ssize_t write_all(int fd, const void *buf, size_t n);
