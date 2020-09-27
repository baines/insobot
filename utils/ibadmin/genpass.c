#include <crypt.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

static void gensalt(char out[static 16]) {
    int fd = open("/dev/urandom", O_RDONLY);
    assert(fd != -1);

    ssize_t n = read(fd, out, 16);
    assert(n == 16);
    close(fd);

    static const char syms[64] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";

    for(int i = 0; i < 16; ++i) {
        out[i] = syms[out[i] & 63];
    }
}

int main(int argc, char** argv) {

    if(argc < 2) {
        fprintf(stderr, "Usage: %s <password>\n", argv[0]);
        return 1;
    }

    char salt[3+16+1] = "$5$";
    gensalt(salt+3);
    salt[3+16] = '\0';

    char* pwd = crypt(argv[1], salt);
    puts(pwd);
}
