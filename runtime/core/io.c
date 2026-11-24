// io.c — Dao runtime IO and process-argument hooks.
//
// Authority: docs/contracts/CONTRACT_RUNTIME_ABI.md

#include "dao_abi.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

// Convert a dao_string to a null-terminated C string on the heap.
// Caller must free() the returned pointer. Returns NULL on allocation
// failure or if the dao_string contains an embedded NUL byte.
static char *dao_string_to_cstr(const struct dao_string *str, bool *has_nul) {
  if (has_nul != NULL) {
    *has_nul = false;
  }
  if (str == NULL || str->len < 0) {
    return NULL;
  }
  if (str->len > 0 && str->ptr == NULL) {
    return NULL;
  }
  if (str->len > 0 && memchr(str->ptr, '\0', (size_t)str->len) != NULL) {
    if (has_nul != NULL) {
      *has_nul = true;
    }
    return NULL;
  }
  char *cstr = (char *)malloc((size_t)str->len + 1);
  if (cstr == NULL) {
    return NULL;
  }
  if (str->len > 0) {
    memcpy(cstr, str->ptr, (size_t)str->len);
  }
  cstr[str->len] = '\0';
  return cstr;
}

static struct dao_string dao_copy_cstr_to_domain(const char *msg) {
  if (msg == NULL) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  size_t len = strlen(msg);
  if (len == 0) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  char *buf = (char *)__dao_mem_alloc((int64_t)len, 1);
  memcpy(buf, msg, len);
  return (struct dao_string){.ptr = buf, .len = (int64_t)len};
}

void __dao_io_write_stdout(const struct dao_string *msg) {
  if (msg == NULL || msg->ptr == NULL || msg->len <= 0) {
    fputc('\n', stdout);
    return;
  }

  fwrite(msg->ptr, 1, (size_t)msg->len, stdout);
  fputc('\n', stdout);
}

void __dao_io_write_stderr(const struct dao_string *msg) {
  if (msg == NULL || msg->ptr == NULL || msg->len <= 0) {
    fputc('\n', stderr);
    return;
  }

  fwrite(msg->ptr, 1, (size_t)msg->len, stderr);
  fputc('\n', stderr);
}

struct dao_string __dao_io_try_read_file(const struct dao_string *path,
                                         int32_t *ok_out) {
  if (ok_out == NULL) {
    fprintf(stderr, "dao: try_read_file: null status pointer\n");
    abort();
  }
  *ok_out = 0;

  if (path == NULL || path->ptr == NULL || path->len <= 0) {
    return dao_copy_cstr_to_domain("empty path");
  }

  bool has_nul = false;
  char *cpath = dao_string_to_cstr(path, &has_nul);
  if (cpath == NULL) {
    return dao_copy_cstr_to_domain(
        has_nul ? "path contains an embedded NUL byte" : "allocation failed");
  }

  FILE *file = fopen(cpath, "rb");
  if (file == NULL) {
    int err = errno;
    free(cpath);
    return dao_copy_cstr_to_domain(strerror(err));
  }

  // Reject directories explicitly, use fstat for regular files with known
  // byte counts, and read streams incrementally until EOF.
  struct stat file_stat;
  int have_stat = (fstat(fileno(file), &file_stat) == 0);
  if (have_stat && S_ISDIR(file_stat.st_mode)) {
    fclose(file);
    free(cpath);
    return dao_copy_cstr_to_domain("path names a directory, not a regular file");
  }
  int is_regular = have_stat && S_ISREG(file_stat.st_mode);

  if (is_regular && file_stat.st_size > 0) {
    int64_t size = (int64_t)file_stat.st_size;
    char *buf = (char *)__dao_mem_alloc(size, 1);

    size_t read_bytes = fread(buf, 1, (size_t)size, file);
    int read_err = ferror(file);
    fclose(file);
    free(cpath);

    if (read_err || read_bytes != (size_t)size) {
      __dao_mem_free(buf);
      return dao_copy_cstr_to_domain("short or failed file read");
    }

    *ok_out = 1;
    return (struct dao_string){.ptr = buf, .len = (int64_t)read_bytes};
  }

  size_t capacity = 4096;
  size_t length = 0;
  char *buf = (char *)__dao_mem_alloc((int64_t)capacity, 1);

  while (!feof(file)) {
    if (length + 4096 > capacity) {
      size_t grown = capacity * 2;
      buf = (char *)__dao_mem_realloc(buf, (int64_t)capacity, (int64_t)grown, 1);
      capacity = grown;
    }
    size_t n = fread(buf + length, 1, 4096, file);
    if (ferror(file)) {
      __dao_mem_free(buf);
      fclose(file);
      free(cpath);
      return dao_copy_cstr_to_domain("stream read error");
    }
    length += n;
  }

  fclose(file);
  free(cpath);

  *ok_out = 1;
  if (length == 0) {
    __dao_mem_free(buf);
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  return (struct dao_string){.ptr = buf, .len = (int64_t)length};
}

// Read an entire file into a string owned by the current domain.
// Traps on error.
struct dao_string __dao_io_read_file(const struct dao_string *path) {
  int32_t ok = 0;
  struct dao_string result = __dao_io_try_read_file(path, &ok);
  if (ok == 0) {
    if (path != NULL && path->ptr != NULL && path->len > 0) {
      fprintf(stderr, "dao: read_file: cannot read '%.*s': %.*s\n",
              (int)path->len, path->ptr, (int)result.len,
              result.ptr != NULL ? result.ptr : "error");
    } else {
      fprintf(stderr, "dao: read_file: %.*s\n", (int)result.len,
              result.ptr != NULL ? result.ptr : "empty path");
    }
    abort();
  }
  return result;
}

// Write a string to a file. Returns true on success, false on failure.
bool __dao_io_write_file(const struct dao_string *path,
                          const struct dao_string *content) {
  if (path == NULL || path->ptr == NULL || path->len <= 0) {
    return false;
  }

  char *cpath = dao_string_to_cstr(path, NULL);
  if (cpath == NULL) {
    return false;
  }

  FILE *file = fopen(cpath, "wb");
  if (file == NULL) {
    free(cpath);
    return false;
  }

  size_t written = 0;
  if (content != NULL && content->ptr != NULL && content->len > 0) {
    written = fwrite(content->ptr, 1, (size_t)content->len, file);
  }

  // fclose flushes buffered data — check its return value to catch
  // disk-full or deferred write errors that fwrite did not report.
  int close_err = fclose(file);
  free(cpath);

  if (close_err != 0) {
    return false;
  }
  return content == NULL || content->len <= 0 ||
         written == (size_t)content->len;
}

// Check if a file exists.
bool __dao_io_file_exists(const struct dao_string *path) {
  if (path == NULL || path->ptr == NULL || path->len <= 0) {
    return false;
  }

  char *cpath = dao_string_to_cstr(path, NULL);
  if (cpath == NULL) {
    return false;
  }

  struct stat st;
  bool exists = (stat(cpath, &st) == 0);
  free(cpath);
  return exists;
}

#if defined(__linux__) && defined(__GLIBC__)
static int64_t dao_linux_argc = -1;
static char **dao_linux_argv = NULL;

// glibc passes (argc, argv, envp) to .init_array constructors via _dl_init.
// Guarded with __GLIBC__ because musl invokes .init_array functions with no
// arguments, which would otherwise read uninitialized registers.
__attribute__((constructor)) static void dao_capture_linux_args(int argc,
                                                                char **argv) {
  if (argc >= 0 && argv != NULL) {
    dao_linux_argc = (int64_t)argc;
    dao_linux_argv = argv;
  }
}
#endif

int64_t __dao_io_arg_count(void) {
#ifdef __APPLE__
  int *argc_ptr = _NSGetArgc();
  if (argc_ptr == NULL || *argc_ptr < 0) {
    fprintf(stderr, "dao: arg_count: Darwin process arguments unavailable\n");
    abort();
  }
  return (int64_t)(*argc_ptr);
#elif defined(__linux__) && defined(__GLIBC__)
  if (dao_linux_argc < 0 || (dao_linux_argc > 0 && dao_linux_argv == NULL)) {
    fprintf(stderr, "dao: arg_count: Linux process arguments unavailable\n");
    abort();
  }
  return dao_linux_argc;
#else
  fprintf(stderr, "dao: arg_count: unsupported host platform or C runtime\n");
  abort();
#endif
}

struct dao_string __dao_io_arg_at(int64_t index) {
  int64_t count = __dao_io_arg_count();
  if (index < 0 || index >= count) {
    fprintf(stderr,
            "dao: arg_at: index %" PRId64 " out of bounds (argc = %" PRId64
            ")\n",
            index, count);
    abort();
  }
#ifdef __APPLE__
  char ***argv_ptr = _NSGetArgv();
  if (argv_ptr == NULL || *argv_ptr == NULL || (*argv_ptr)[index] == NULL) {
    fprintf(stderr, "dao: arg_at: null Darwin argument pointer\n");
    abort();
  }
  return dao_copy_cstr_to_domain((*argv_ptr)[index]);
#elif defined(__linux__) && defined(__GLIBC__)
  if (dao_linux_argv == NULL || dao_linux_argv[index] == NULL) {
    fprintf(stderr, "dao: arg_at: null Linux argument pointer\n");
    abort();
  }
  return dao_copy_cstr_to_domain(dao_linux_argv[index]);
#else
  abort();
#endif
}
