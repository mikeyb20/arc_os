/* arc_os — ext2 images for the write suite: mkfs.ext2, e2fsck and debugfs
 * over sparse temporary files (ext2_image.h). Host headers only. */

#define _GNU_SOURCE
#include "ext2_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *tmp_dir(void) {
    const char *t = getenv("TMPDIR");
    return t != NULL && t[0] != '\0' ? t : "/tmp";
}

/* Images made so far, removed when the program ends (the volumes stay
 * mounted, and debugfs reads them after shutdown) */
#define MAX_IMAGES 16
static char made[MAX_IMAGES][256];
static int nmade;

static void remove_images(void) {
    for (int i = 0; i < nmade; i++) unlink(made[i]);
}

/* Run a shell command; its exit status, or -1 */
static int run(const char *cmd) {
    int st = system(cmd);
    if (st == -1 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

int himg_make(HostImage *im, uint64_t mib, const char *opts, const char *tree) {
    memset(im, 0, sizeof(*im));
    snprintf(im->path, sizeof(im->path), "%s/arc_ext2w_XXXXXX", tmp_dir());
    int fd = mkstemp(im->path);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    close(fd);
    if (nmade == 0) atexit(remove_images);
    if (nmade < MAX_IMAGES) snprintf(made[nmade++], sizeof(made[0]), "%s", im->path);
    char dir[256] = "";
    char cmd[4096];
    if (tree != NULL) {
        snprintf(dir, sizeof(dir), "%s/arc_ext2w_tree_XXXXXX", tmp_dir());
        if (mkdtemp(dir) == NULL) {
            perror("mkdtemp");
            return -1;
        }
        snprintf(cmd, sizeof(cmd), "cd '%s' && (%s)", dir, tree);
        if (run(cmd) != 0) {
            fprintf(stderr, "tree script failed: %s\n", tree);
            return -1;
        }
    }
    /* The features arc_os volumes have (ADR-0008), whatever the host's
     * mke2fs.conf says */
    snprintf(cmd, sizeof(cmd),
             "mkfs.ext2 -q -F -t ext2 -b 4096 -I 256 -O none,filetype,sparse_super,large_file,resize_inode,"
             "dir_index,ext_attr %s %s%s%s '%s' %lluM",
             opts != NULL ? opts : "", tree != NULL ? "-d '" : "", tree != NULL ? dir : "", tree != NULL ? "'" : "",
             im->path, (unsigned long long)mib);
    int st = run(cmd);
    if (st == 0 && tree != NULL) {
        snprintf(cmd, sizeof(cmd), "e2fsck -fyD '%s' >/dev/null 2>&1", im->path);
        st = run(cmd) <= 1 ? 0 : -1;   /* 1: the indexes were made */
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        run(cmd);
    }
    if (st != 0) {
        fprintf(stderr, "mkfs failed: %s\n", cmd);
        unlink(im->path);
        return -1;
    }
    fd = open(im->path, O_RDWR);
    if (fd < 0) return -1;
    im->size = (uint64_t)lseek(fd, 0, SEEK_END);
    im->data = mmap(NULL, im->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (im->data == MAP_FAILED) {
        perror("mmap");
        im->data = NULL;
        return -1;
    }
    return 0;
}

static void sync_image(const HostImage *im) {
    if (msync(im->data, im->size, MS_SYNC) != 0) perror("msync");
}

int himg_fsck(const HostImage *im) {
    sync_image(im);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "e2fsck -fn '%s' >'%s.fsck' 2>&1", im->path, im->path);
    int st = run(cmd);
    if (st != 0) {
        printf("  e2fsck -fn exit %d:\n", st);
        snprintf(cmd, sizeof(cmd), "head -60 '%s.fsck'", im->path);
        fflush(stdout);
        run(cmd);
    }
    snprintf(cmd, sizeof(cmd), "%s.fsck", im->path);
    unlink(cmd);
    return st;
}

long himg_debugfs(const HostImage *im, const char *cmd, char *out, size_t size) {
    sync_image(im);
    char line[1024];
    snprintf(line, sizeof(line), "debugfs -R '%s' '%s' 2>/dev/null", cmd, im->path);
    FILE *p = popen(line, "r");
    if (p == NULL) return -1;
    size_t n = fread(out, 1, size - 1, p);
    out[n] = '\0';
    int st = pclose(p);
    return st == -1 ? -1 : (long)n;
}

uint8_t *himg_dump(const HostImage *im, const char *path, uint64_t *len) {
    sync_image(im);
    char out[300], cmd[1024];
    snprintf(out, sizeof(out), "%s.dump", im->path);
    unlink(out);
    snprintf(cmd, sizeof(cmd), "debugfs -R 'dump %s %s' '%s' >/dev/null 2>&1", path, out, im->path);
    if (run(cmd) != 0) return NULL;
    FILE *f = fopen(out, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    unlink(out);
    *len = (uint64_t)n;
    return buf;
}
