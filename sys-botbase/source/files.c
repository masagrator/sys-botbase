// SD card file commands (xc2fsr fork). Paths are sdmc:/... or /... (no spaces). Replies:
//   fileList <dir>               -> one line per entry "D <name>" or "F <size> <name>", then an empty line
//   fileSize <path>              -> "<size>" or "-1"
//   fileRead <path> [off] [len]  -> "<n>\n" then n raw bytes ("-1\n" on error); len 0 / omitted = to end of file
//   fileWrite <path> <size>      -> then send <size> raw bytes; reply "ok <n>" or "err <n>" (file truncated / created)
//   fileAppend <path> <size>     -> like fileWrite, appends
//   fileDelete <path>, fileMkdir <path>, fileRename <from> <to> -> "ok" or "err"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <switch.h>
#include "util.h"
#include "files.h"

#define CHUNK 0x10000

static bool sdReady = false;

static bool sdMount(void)
{
    if (sdReady)
        return true;
    if (R_FAILED(fsInitialize()))
        return false;
    if (R_FAILED(fsdevMountSdmc()))
        return false;
    sdReady = true;
    return true;
}

static const char* sdPath(const char* p, char* out, size_t n)
{
    if (!strncmp(p, "sdmc:", 5))
        snprintf(out, n, "%s", p);
    else
        snprintf(out, n, "sdmc:%s%s", p[0] == '/' ? "" : "/", p);
    return out;
}

static bool sendAll(int fd, const void* buf, size_t n)
{
    const u8* b = buf;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0)
            return false;
        b += w;
        n -= w;
    }
    return true;
}

static void reply(int fd, const char* s)
{
    sendAll(fd, s, strlen(s));
}

static u64 parseNum(const char* s)
{
    return parseStringToInt((char*)s);
}

static void cmdList(int fd, const char* path)
{
    DIR* d = opendir(path);
    char line[FS_MAX_PATH + 64], full[FS_MAX_PATH * 2];
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) {
                snprintf(line, sizeof line, "D %s\n", e->d_name);
            } else {
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", path, e->d_name);
                long long sz = stat(full, &st) == 0 ? (long long)st.st_size : -1;
                snprintf(line, sizeof line, "F %lld %s\n", sz, e->d_name);
            }
            reply(fd, line);
        }
        closedir(d);
    }
    reply(fd, "\n");
}

static void cmdRead(int fd, const char* path, u64 off, u64 len)
{
    char line[32];
    FILE* f = fopen(path, "rb");
    if (!f) {
        reply(fd, "-1\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    u64 size = ftell(f);
    if (off > size)
        off = size;
    if (len == 0 || len > size - off)
        len = size - off;
    fseek(f, off, SEEK_SET);
    snprintf(line, sizeof line, "%lu\n", len);
    reply(fd, line);
    u8* buf = malloc(CHUNK);
    u64 left = len;
    while (left && buf) {
        size_t want = left < CHUNK ? left : CHUNK;
        size_t got = fread(buf, 1, want, f);
        if (got < want)
            memset(buf + got, 0, want - got);  // the length was announced: pad if the file shrank meanwhile
        if (!sendAll(fd, buf, want))
            break;
        left -= want;
    }
    free(buf);
    fclose(f);
}

static void cmdWrite(int fd, const char* path, u64 size, bool append)
{
    char line[48];
    FILE* f = fopen(path, append ? "ab" : "wb");
    u8* buf = malloc(CHUNK);
    u64 left = size, written = 0;
    // always drain the announced bytes, so the stream stays in sync even when the file can't be opened
    while (left && buf) {
        size_t want = left < CHUNK ? left : CHUNK;
        ssize_t r = recv(fd, buf, want, 0);
        if (r <= 0)
            break;
        if (f)
            written += fwrite(buf, 1, r, f);
        left -= r;
    }
    free(buf);
    if (f)
        fclose(f);
    snprintf(line, sizeof line, "%s %lu\n", (f && written == size) ? "ok" : "err", written);
    reply(fd, line);
}

bool fileCommand(int argc, char** argv, int fd)
{
    if (argc < 2 || strncmp(argv[0], "file", 4))
        return false;
    const char* cmd = argv[0];
    char path[FS_MAX_PATH * 2], path2[FS_MAX_PATH * 2];

    fflush(stdout);  // keep printf replies of earlier commands ahead of ours
    if (!sdMount()) {
        reply(fd, "err sd\n");
        return true;
    }
    sdPath(argv[1], path, sizeof path);

    if (!strcmp(cmd, "fileList")) {
        cmdList(fd, path);
    } else if (!strcmp(cmd, "fileSize")) {
        struct stat st;
        char line[32];
        snprintf(line, sizeof line, "%lld\n", stat(path, &st) == 0 ? (long long)st.st_size : -1LL);
        reply(fd, line);
    } else if (!strcmp(cmd, "fileRead")) {
        cmdRead(fd, path, argc > 2 ? parseNum(argv[2]) : 0, argc > 3 ? parseNum(argv[3]) : 0);
    } else if (!strcmp(cmd, "fileWrite") || !strcmp(cmd, "fileAppend")) {
        if (argc != 3) {
            reply(fd, "err args\n");
            return true;
        }
        cmdWrite(fd, path, parseNum(argv[2]), !strcmp(cmd, "fileAppend"));
    } else if (!strcmp(cmd, "fileDelete")) {
        reply(fd, remove(path) == 0 ? "ok\n" : "err\n");
    } else if (!strcmp(cmd, "fileMkdir")) {
        reply(fd, mkdir(path, 0777) == 0 ? "ok\n" : "err\n");
    } else if (!strcmp(cmd, "fileRename")) {
        if (argc != 3) {
            reply(fd, "err args\n");
            return true;
        }
        sdPath(argv[2], path2, sizeof path2);
        reply(fd, rename(path, path2) == 0 ? "ok\n" : "err\n");
    } else {
        reply(fd, "err unknown\n");
    }
    return true;
}
