/*
 * lane.arcutil (M5-02): the shared stack helpers. SHA-256 against the FIPS 180-4 / NIST CAVP example
 * vectors (empty, "abc", the 448- and 896-bit messages, one million 'a'), fed whole and in odd-sized
 * pieces; directory creation; durable write (new file, replaced file, no temporary left); recursive
 * removal (nested tree, a missing path); total RAM. Symlinks under arcutil_remove_tree are checked
 * end to end by lane.arcpkg_install (the syscall inventory has no symlink for a target program).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../lane_check.h"
#include "arcutil.h"

/* sha_hex: SHA-256 of data[0..len) fed in pieces of `step` bytes (0: all at once), as hex. */
static void sha_hex(const void *data, size_t len, size_t step, char out[ARCUTIL_SHA256_HEX]) {
    arcutil_sha256 s;
    unsigned char digest[ARCUTIL_SHA256_SIZE];
    arcutil_sha256_init(&s);
    for (size_t at = 0; at < len || (len == 0 && at == 0); at += step ? step : len + 1) {
        size_t n = step && len - at > step ? step : len - at;
        arcutil_sha256_update(&s, (const char *)data + at, n);
        if (len == 0) {
            break;
        }
    }
    arcutil_sha256_final(&s, digest);
    arcutil_sha256_hex(digest, out);
}

static void test_sha256(void) {
    static const struct {
        const char *msg;
        const char *want;
    } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqr"
         "stu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    char hex[ARCUTIL_SHA256_HEX];
    char what[96];
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        size_t len = strlen(vectors[i].msg);
        snprintf(what, sizeof(what), "SHA-256 FIPS vector %zu (%zu bytes) whole", i + 1, len);
        sha_hex(vectors[i].msg, len, 0, hex);
        lane_check(strcmp(hex, vectors[i].want) == 0, what);
        snprintf(what, sizeof(what), "SHA-256 FIPS vector %zu (%zu bytes) byte by byte", i + 1, len);
        sha_hex(vectors[i].msg, len, 1, hex);
        lane_check(strcmp(hex, vectors[i].want) == 0, what);
    }
    size_t mlen = 1000000;
    char *million = malloc(mlen);
    lane_check(million != NULL, "allocate one million bytes");
    if (million == NULL) {
        return;
    }
    memset(million, 'a', mlen);
    const char *want = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    sha_hex(million, mlen, 0, hex);
    lane_check(strcmp(hex, want) == 0, "SHA-256 of one million 'a', whole");
    sha_hex(million, mlen, 997, hex);
    lane_check(strcmp(hex, want) == 0, "SHA-256 of one million 'a', in 997-byte pieces");
    sha_hex(million, mlen, 64, hex);
    lane_check(strcmp(hex, want) == 0, "SHA-256 of one million 'a', in 64-byte blocks");
    free(million);
    unsigned char bytes[1027];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = i < 1024 ? (unsigned char)i : (unsigned char)"xyz"[i - 1024];
    }
    sha_hex(bytes, sizeof(bytes), 55, hex);
    lane_check(strcmp(hex, "32b377390e072c37cfeb9bb327d8825616819a76b0ad3749e16fe22e53afbdfc") == 0,
               "SHA-256 of all byte values (hashlib), in 55-byte pieces");
}

/* read_all: the file at path as a NUL-terminated string in buf; false if missing or too long. */
static bool read_all(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) {
        return false;
    }
    buf[n] = '\0';
    return true;
}

static void test_files(const char *dir) {
    char path[512];
    char other[640]; /* path plus a suffix */
    char buf[64];
    struct stat st;
    snprintf(path, sizeof(path), "%s/a/b/c", dir);
    lane_check(arcutil_make_dirs(path) && stat(path, &st) == 0 && S_ISDIR(st.st_mode), "make_dirs creates a/b/c");
    lane_check(arcutil_make_dirs(path) && arcutil_make_dir(path),
               "make_dirs and make_dir accept an existing directory");
    snprintf(path, sizeof(path), "%s/a/file", dir);
    lane_check(arcutil_write_durable(path, "one", 3) && read_all(path, buf, sizeof(buf)) && strcmp(buf, "one") == 0,
               "write_durable creates a file");
    lane_check(arcutil_write_durable(path, "two!", 4) && read_all(path, buf, sizeof(buf)) && strcmp(buf, "two!") == 0,
               "write_durable replaces it whole");
    snprintf(other, sizeof(other), "%s.tmp", path);
    lane_check(access(other, F_OK) != 0, "no temporary file left behind");
    snprintf(other, sizeof(other), "%s/sub", path);
    lane_check(!arcutil_make_dirs(other) && errno == ENOTDIR, "make_dirs through a file fails with ENOTDIR");
    lane_check(!arcutil_make_dir(path) && errno == ENOTDIR, "make_dir on a file fails with ENOTDIR");
    snprintf(path, sizeof(path), "%s/a", dir);
    lane_check(arcutil_sync_dir(path), "sync_dir");
    snprintf(other, sizeof(other), "%s/a/b/c/deep", dir);
    lane_check(arcutil_write_durable(other, "x", 1), "a file deep in the tree");
    lane_check(arcutil_remove_tree(path) && access(path, F_OK) != 0, "remove_tree removes the whole tree");
    lane_check(arcutil_remove_tree(path), "remove_tree of a missing path succeeds");
    snprintf(path, sizeof(path), "%s/single", dir);
    lane_check(arcutil_write_durable(path, "x", 1) && arcutil_remove_tree(path) && access(path, F_OK) != 0,
               "remove_tree removes a plain file");
}

int main(void) {
    test_sha256();
    char dir[] = "arcutil-test-XXXXXX";
    lane_check(mkdtemp(dir) != NULL, "temporary directory");
    test_files(dir);
    lane_check(arcutil_remove_tree(dir), "clean up");
    lane_check(arcutil_ram_mib() > 0, "ram_mib reports the machine's RAM");
    return lane_check_done("arcutil");
}
