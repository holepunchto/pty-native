#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

#ifdef _WIN32

static js_value_t *
pty_native__unsupported(js_env_t *env, js_callback_info_t *info) {
  js_throw_error(env, "ENOTSUP", "Pseudo-terminals are not supported on Windows");

  return NULL;
}

#define pty_native_spawn      pty_native__unsupported
#define pty_native_write      pty_native__unsupported
#define pty_native_close      pty_native__unsupported
#define pty_native_wait       pty_native__unsupported
#define pty_native_kill       pty_native__unsupported
#define pty_native_attributes pty_native__unsupported

#else

static void
pty_native__throw(js_env_t *env, int uv_err) {
  js_throw_error(env, uv_err_name(uv_err), uv_strerror(uv_err));
}

static char *
pty_native__string(js_env_t *env, js_value_t *value) {
  int err;

  size_t len;
  err = js_get_value_string_utf8(env, value, NULL, 0, &len);
  assert(err == 0);

  char *str = malloc(len + 1);
  assert(str != NULL);

  err = js_get_value_string_utf8(env, value, (utf8_t *) str, len + 1, NULL);
  assert(err == 0);

  return str;
}

static js_value_t *
pty_native_spawn(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 3);

  char *file = pty_native__string(env, argv[0]);

  uint32_t len;
  err = js_get_array_length(env, argv[1], &len);
  assert(err == 0);

  char **args = calloc(len + 2, sizeof(char *));
  assert(args != NULL);

  args[0] = file;

  for (uint32_t i = 0; i < len; i++) {
    js_value_t *item;
    err = js_get_element(env, argv[1], i, &item);
    assert(err == 0);

    args[i + 1] = pty_native__string(env, item);
  }

  char *cwd = NULL;

  bool has_cwd;
  err = js_is_string(env, argv[2], &has_cwd);
  assert(err == 0);

  if (has_cwd) cwd = pty_native__string(env, argv[2]);

  struct winsize size = {.ws_row = 24, .ws_col = 80};

  int master;
  pid_t pid = forkpty(&master, NULL, NULL, &size);

  if (pid == 0) {
    // Start from a clean slate, as uv_spawn() does: the runtime ignores and
    // blocks signals that a child would otherwise inherit across exec
    for (int sig = 1; sig < NSIG; sig++) {
      if (sig == SIGKILL || sig == SIGSTOP) continue;
      signal(sig, SIG_DFL);
    }

    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);

    if (cwd && chdir(cwd) != 0) _exit(127);

    execvp(file, args);

    _exit(127);
  }

  int saved = errno;

  for (uint32_t i = 0; i < len + 1; i++) free(args[i]);
  free(args);
  free(cwd);

  if (pid < 0) {
    pty_native__throw(env, uv_translate_sys_error(saved));
    return NULL;
  }

  fcntl(master, F_SETFD, FD_CLOEXEC);

  // A stream wrapped around the master closes it on hang-up, so hand out a
  // second descriptor for writing and reading the terminal settings that
  // stays valid until the caller is done
  int control = fcntl(master, F_DUPFD_CLOEXEC, 0);

  if (control < 0) {
    saved = errno;
    close(master);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    pty_native__throw(env, uv_translate_sys_error(saved));
    return NULL;
  }

  int32_t values[3] = {master, control, (int32_t) pid};

  js_value_t *result;
  err = js_create_array_with_length(env, 3, &result);
  assert(err == 0);

  for (uint32_t i = 0; i < 3; i++) {
    js_value_t *value;
    err = js_create_int32(env, values[i], &value);
    assert(err == 0);

    err = js_set_element(env, result, i, value);
    assert(err == 0);
  }

  return result;
}

static js_value_t *
pty_native_write(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 3);

  int32_t fd;
  err = js_get_value_int32(env, argv[0], &fd);
  assert(err == 0);

  uint8_t *data;
  size_t len;
  err = js_get_typedarray_info(env, argv[1], NULL, (void **) &data, &len, NULL, NULL);
  assert(err == 0);

  int64_t timeout;
  err = js_get_value_int64(env, argv[2], &timeout);
  assert(err == 0);

  // The stream wrapped around the master shares this descriptor's file
  // description and may have set it blocking, so make sure a write to a
  // terminal the child has stopped reading cannot hang the event loop
  int flags = fcntl(fd, F_GETFL);

  if (flags >= 0 && (flags & O_NONBLOCK) == 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  uint64_t deadline = uv_hrtime() / 1000000 + (uint64_t) timeout;

  size_t offset = 0;

  while (offset < len) {
    ssize_t n = write(fd, data + offset, len - offset);

    if (n > 0) {
      offset += (size_t) n;
      continue;
    }

    if (n < 0 && errno == EINTR) continue;

    if (n < 0 && errno != EAGAIN) {
      pty_native__throw(env, uv_translate_sys_error(errno));
      return NULL;
    }

    // The terminal's buffer is full, so wait for the child to drain it
    uint64_t now = uv_hrtime() / 1000000;

    if (now >= deadline) {
      js_throw_error(env, "ETIMEDOUT", "Terminal is not being read");
      return NULL;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLOUT};

    poll(&pfd, 1, (int) (deadline - now));
  }

  return NULL;
}

static js_value_t *
pty_native_close(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  int32_t fd;
  err = js_get_value_int32(env, argv[0], &fd);
  assert(err == 0);

  if (close(fd) != 0) pty_native__throw(env, uv_translate_sys_error(errno));

  return NULL;
}

static js_value_t *
pty_native_wait(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  int32_t pid;
  err = js_get_value_int32(env, argv[0], &pid);
  assert(err == 0);

  int status;
  pid_t res;

  // Never block the event loop: null while the child is still running, so
  // the caller polls
  do {
    res = waitpid((pid_t) pid, &status, WNOHANG);
  } while (res < 0 && errno == EINTR);

  if (res < 0) {
    pty_native__throw(env, uv_translate_sys_error(errno));
    return NULL;
  }

  if (res == 0) {
    js_value_t *result;
    err = js_get_null(env, &result);
    assert(err == 0);

    return result;
  }

  js_value_t *result;
  err = js_create_array_with_length(env, 2, &result);
  assert(err == 0);

  js_value_t *value;
  err = js_create_int32(env, WIFEXITED(status) ? WEXITSTATUS(status) : -1, &value);
  assert(err == 0);

  err = js_set_element(env, result, 0, value);
  assert(err == 0);

  err = js_create_int32(env, WIFSIGNALED(status) ? WTERMSIG(status) : 0, &value);
  assert(err == 0);

  err = js_set_element(env, result, 1, value);
  assert(err == 0);

  return result;
}

static js_value_t *
pty_native_kill(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  int32_t pid;
  err = js_get_value_int32(env, argv[0], &pid);
  assert(err == 0);

  int32_t signum;
  err = js_get_value_int32(env, argv[1], &signum);
  assert(err == 0);

  if (kill((pid_t) pid, signum) != 0) pty_native__throw(env, uv_translate_sys_error(errno));

  return NULL;
}

static js_value_t *
pty_native_attributes(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  int32_t fd;
  err = js_get_value_int32(env, argv[0], &fd);
  assert(err == 0);

  struct termios attributes;

  if (tcgetattr(fd, &attributes) != 0) {
    pty_native__throw(env, uv_translate_sys_error(errno));
    return NULL;
  }

  uint32_t flags[4] = {
    (uint32_t) attributes.c_iflag,
    (uint32_t) attributes.c_oflag,
    (uint32_t) attributes.c_cflag,
    (uint32_t) attributes.c_lflag,
  };

  js_value_t *result;
  err = js_create_array_with_length(env, 4, &result);
  assert(err == 0);

  for (uint32_t i = 0; i < 4; i++) {
    js_value_t *value;
    err = js_create_uint32(env, flags[i], &value);
    assert(err == 0);

    err = js_set_element(env, result, i, value);
    assert(err == 0);
  }

  return result;
}

#endif

static js_value_t *
pty_native_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("spawn", pty_native_spawn)
  V("write", pty_native_write)
  V("close", pty_native_close)
  V("wait", pty_native_wait)
  V("kill", pty_native_kill)
  V("attributes", pty_native_attributes)
#undef V

#define V(name, n) \
  { \
    js_value_t *val; \
    err = js_create_uint32(env, n, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

#ifndef _WIN32
  V("ECHO", ECHO)
  V("ICANON", ICANON)
  V("ISIG", ISIG)
  V("IEXTEN", IEXTEN)
  V("ICRNL", ICRNL)
  V("OPOST", OPOST)
  V("SIGTERM", SIGTERM)
  V("SIGKILL", SIGKILL)
  V("SIGHUP", SIGHUP)
#endif
#undef V

  return exports;
}

BARE_MODULE(pty_native, pty_native_exports)
