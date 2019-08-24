//
// clib-build.c
//
// Copyright (c) 2012-2019 clib authors
// MIT licensed
//

#include <curl/curl.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
#include <unistd.h>
#endif

#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#include <clib-package/clib-package.h>
#include <clib-cache/cache.h>

#include <commander/commander.h>
#include <path-join/path-join.h>
#include <asprintf/asprintf.h>
#include <logger/logger.h>
#include <debug/debug.h>
#include <hash/hash.h>
#include <trim/trim.h>
#include <list/list.h>
#include <fs/fs.h>

#include "version.h"

#define CLIB_SEARCH_CACHE_TIME 1 * 24 * 60 * 60
#define PROGRAM_NAME "clib-build"

#define S(s) #s

#ifdef HAVE_PTHREADS
#define MAX_THREADS 4
#endif

#ifndef DEFAULT_MAKE_CLEAN_TARGET
#define DEFAULT_MAKE_CLEAN_TARGET "clean"
#endif

#ifndef DEFAULT_MAKE_CHECK_TARGET
#define DEFAULT_MAKE_CHECK_TARGET "test"
#endif

typedef struct options options_t;
struct options {
  const char *dir;
  char *prefix;
  int force;
  int verbose;
  int dev;
  int skip_cache;
  int global;
  char *clean;
  char *test;
#ifdef HAVE_PTHREADS
  unsigned int concurrency;
#endif
};

command_t program = { 0 };
debug_t debugger = { 0 };
hash_t *built = 0;

options_t opts = {
  .skip_cache = 0,
  .verbose = 1,
  .force = 0,
  .dev = 0,
#ifdef HAVE_PTHREADS
  .concurrency = MAX_THREADS,
#endif

#ifdef _WIN32
  .dir = ".\\deps"
#else
  .dir = "./deps"
#endif

};

int
build_package(const char *dir);

#ifdef HAVE_PTHREADS
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct clib_package_thread clib_package_thread_t;
struct clib_package_thread {
  const char *dir;
};

void *
build_package_with_package_name_thread(void *arg) {
  clib_package_thread_t *wrap = arg;
  const char *dir = wrap->dir;
  return (void *) build_package(dir);
}
#endif

int
build_package_with_package_name(const char *dir, const char *file) {
  clib_package_t *package = 0;
  char *json = 0;
  int ok = 0;
  int rc = 0;

  char *path = path_join(dir, file);

  if (0 == path) {
    return -ENOMEM;
  }

#ifdef HAVE_PTHREADS
  pthread_mutex_lock(&mutex);
#endif

  if (hash_has(built, path)) {
#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&mutex);
#endif
    goto cleanup;
  }

#ifdef HAVE_PTHREADS
  pthread_mutex_unlock(&mutex);
#endif

  if (-1 == fs_exists(path)) {
    rc = -ENOENT;
    goto cleanup;
  }

  debug(&debugger, "read %s", path);
  json = fs_read(path);

  if (0 != json) {
#ifdef DEBUG
    package = clib_package_new(json, 1);
#else
    package = clib_package_new(json, 0);
#endif
  } else {
#ifdef DEBUG
    package = clib_package_new_from_slug(dir, 1);
#else
    package = clib_package_new_from_slug(dir, 0);
#endif
  }

  if (0 == package) {
    rc = -ENOMEM;
    goto cleanup;
  }

  if (0 != package->makefile) {
    char *command = 0;
    asprintf(&command,
        "cd %s && %s %s %s && make -f %s %s %s",
        dir,
        0 != opts.clean ? "make -f" : ":",
        0 != opts.clean ? package->makefile : ":",
        0 != opts.clean ? opts.clean : ":",
        package->makefile,
        opts.force ? "-B" : "",
        opts.test ? opts.test : "");

    if (0 != opts.verbose) {
      logger_warn("build", "%s: %s", package->name, package->makefile);
    }

    rc = system(command);
    free(command);
    command = 0;
#ifdef HAVE_PTHREADS
    rc = pthread_mutex_lock(&mutex);
#endif

    hash_set(built, path, "t");
    ok = 1;
  } else {
#ifdef HAVE_PTHREADS
    rc = pthread_mutex_lock(&mutex);
#endif

    hash_set(built, path, "f");
    ok = 1;
  }

  if (0 != rc) {
    goto cleanup;
  }


#ifdef HAVE_PTHREADS
  pthread_mutex_unlock(&mutex);
#endif

  if (0 != package->dependencies) {
    list_iterator_t *iterator = 0;
    list_node_t *node = 0;

#ifdef HAVE_PTHREADS
    clib_package_thread_t wraps[opts.concurrency];
    pthread_t threads[opts.concurrency];
    unsigned int i = 0;
#endif

    iterator = list_iterator_new(package->dependencies, LIST_HEAD);

    while ((node = list_iterator_next(iterator))) {
      clib_package_dependency_t *dep = node->val;
      char *slug = 0;
      asprintf(&slug, "%s/%s@%s", dep->author, dep->name, dep->version);

      clib_package_t *dependency = clib_package_new_from_slug(slug, 0);
      char *dep_dir = path_join(opts.dir, dependency->name);

      free(slug);
      clib_package_free(dependency);

#ifdef HAVE_PTHREADS
      clib_package_thread_t *wrap = &wraps[i];
      pthread_t *thread = &threads[i];
      wrap->dir = dep_dir;
      rc = pthread_create(
            thread,
            0,
            build_package_with_package_name_thread,
            wrap);

      if (++i >= opts.concurrency) {
        for (int j = 0; j < i; ++j) {
          pthread_join(threads[j], 0);
          free((void *) wraps[j].dir);
        }

        i = 0;
      }
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
      usleep(1024 * 10);
#endif
#else
      if (0 == dep_dir) {
        rc = -ENOMEM;
        goto cleanup;
      }

      rc = build_package(dep_dir);

      free((void *) dep_dir);

      if (0 != rc) {
        goto cleanup;
      }
#endif
    }

#ifdef HAVE_PTHREADS
    for (int j = 0; j < i; ++j) {
      pthread_join(threads[j], 0);
      free((void *) wraps[j].dir);
    }
#endif

    if (0 != iterator) { list_iterator_destroy(iterator); }
  }

  if (opts.dev && 0 != package->development) {
    list_iterator_t *iterator = 0;
    list_node_t *node = 0;

#ifdef HAVE_PTHREADS
    clib_package_thread_t wraps[opts.concurrency];
    pthread_t threads[opts.concurrency];
    unsigned int i = 0;
#endif

    iterator = list_iterator_new(package->development, LIST_HEAD);

    while ((node = list_iterator_next(iterator))) {
      clib_package_dependency_t *dep = node->val;
      char *slug = 0;
      asprintf(&slug, "%s/%s@%s", dep->author, dep->name, dep->version);

      clib_package_t *dependency = clib_package_new_from_slug(slug, 0);
      char *dep_dir = path_join(opts.dir, dependency->name);

      free(slug);
      clib_package_free(dependency);

#ifdef HAVE_PTHREADS
      clib_package_thread_t *wrap = &wraps[i];
      pthread_t *thread = &threads[i];
      wrap->dir = dep_dir;
      rc = pthread_create(
            thread,
            0,
            build_package_with_package_name_thread,
            wrap);

      if (++i >= opts.concurrency) {
        for (int j = 0; j < i; ++j) {
          pthread_join(threads[j], 0);
          free((void *) wraps[j].dir);
        }

        i = 0;
      }
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
      usleep(1024 * 10);
#endif
#else
      if (0 == dep_dir) {
        rc = -ENOMEM;
        goto cleanup;
      }

      rc = build_package(dep_dir);

      free((void *) dep_dir);

      if (0 != rc) {
        goto cleanup;
      }
#endif
    }

#ifdef HAVE_PTHREADS
    for (int j = 0; j < i; ++j) {
      pthread_join(threads[j], 0);
      free((void *) wraps[j].dir);
    }
#endif

    if (0 != iterator) { list_iterator_destroy(iterator); }
  }

cleanup:
  if (0 != package) { clib_package_free(package); }
  if (0 != json) { free(json); }
  if (0 == ok) {
    if (0 != path) { free(path); }
  }
  return rc;
}

int
build_package(const char *dir) {
  static const char *package_names[] = { "clib.json", "package.json", 0 };
  const char *name = NULL;
  unsigned int i = 0;
  int rc = 0;

  do {
    name = package_names[i];
    rc = build_package_with_package_name(dir, name);
  } while (NULL != package_names[++i] && 0 != rc);

  return rc;
}

static void
setopt_skip_cache(command_t *self) {
  opts.skip_cache = 1;
  debug(&debugger, "set skip cache flag");
}

static void
setopt_dev(command_t *self) {
  opts.dev = 1;
  debug(&debugger, "set dev flag");
}

static void
setopt_force(command_t *self) {
  opts.force = 1;
  debug(&debugger, "set force flag");
}

static void
setopt_global(command_t *self) {
  opts.global = 1;
  debug(&debugger, "set global flag");
}

static void
setopt_clean(command_t *self) {
  if (self->arg && '-' != self->arg[0]) {
    opts.clean = (char *) self->arg;
  } else {
    opts.clean = DEFAULT_MAKE_CLEAN_TARGET;
  }

  debug(&debugger, "set clean flag");
}

static void
setopt_test(command_t *self) {
  if (self->arg && '-' != self->arg[0]) {
    opts.test = (char *) self->arg;
  } else {
    opts.test = DEFAULT_MAKE_CHECK_TARGET;
  }

  debug(&debugger, "set test flag");
}

static void
setopt_prefix(command_t *self) {
  if (self->arg && '-' != self->arg[0]) {
    opts.prefix = (char *) self->arg;
  }

  debug(&debugger, "set prefix: %s", opts.prefix);
}

static void
setopt_dir(command_t *self) {
  opts.dir = (char *) self->arg;
  debug(&debugger, "set dir: %s", opts.dir);
}

static void
setopt_quiet(command_t *self) {
  opts.verbose = 0;
  debug(&debugger, "set quiet flag");
}

#ifdef HAVE_PTHREADS
static void
setopt_concurrency(command_t *self) {
  if (self->arg) {
    opts.concurrency = atol(self->arg);
    debug(&debugger, "set concurrency: %lu", opts.concurrency);
  }
}
#endif

int
main(int argc, char **argv) {
  int rc = 0;

#ifdef PATH_MAX
  long path_max = PATH_MAX;
#elif defined(_PC_PATH_MAX)
  long path_max = pathconf(dir, _PC_PATH_MAX);
#else
  long path_max = 4096;
#endif

  char CWD[path_max];

  memset(CWD, 0, path_max);

  if (0 == getcwd(CWD, path_max)) {
    return -errno;
  }

  built = hash_new();
  hash_set(built, strdup("__" PROGRAM_NAME "__"), CLIB_VERSION);

  command_init(&program , PROGRAM_NAME, CLIB_VERSION);
  debug_init(&debugger, PROGRAM_NAME);

  program.usage = "[options] [name ...]";

  command_option(&program,
    "-o",
    "--out <dir>",
    "change the output directory [deps]",
    setopt_dir);

  command_option(&program,
    "-P",
    "--prefix <dir>",
    "change the prefix directory (usually '/usr/local')",
    setopt_prefix);

  command_option(&program,
    "-q",
    "--quiet",
    "disable verbose output",
    setopt_quiet);

  command_option(&program,
    "-g",
    "--global",
    "use global target",
    setopt_global);

  command_option(&program,
    "-C",
    "--clean [clean_target] (default: " DEFAULT_MAKE_CLEAN_TARGET ")",
    "clean target before building",
    setopt_clean);

  command_option(&program,
    "-T",
    "--test [test_target] (default: " DEFAULT_MAKE_CHECK_TARGET ")",
    "test target instead of building",
    setopt_test);

  command_option(&program,
    "-d",
    "--dev",
    "build development dependencies",
    setopt_dev);

  command_option(&program,
    "-f",
    "--force",
    "force the action of something, like overwriting a file",
    setopt_force);

  command_option(&program,
     "-c",
     "--skip-cache",
     "skip cache when configuring",
     setopt_skip_cache);

#ifdef HAVE_PTHREADS
  command_option(&program,
     "-C",
     "--concurrency <concurrency>",
     "Set concurrency (default: " S(MAX_THREADS) ")",
     setopt_concurrency);
#endif

  command_parse(&program, argc, argv);

  char dir[path_max];
  memset(dir, 0, path_max);
  opts.dir = realpath(opts.dir, dir);

  if (0 != curl_global_init(CURL_GLOBAL_ALL)) {
    logger_error("error", "Failed to initialize cURL");
    return 1;
  }

  clib_cache_init(CLIB_SEARCH_CACHE_TIME);
  clib_package_set_opts((clib_package_opts_t) {
    .skip_cache = opts.skip_cache,
    .prefix = opts.prefix,
    .global = opts.global,
    .force = opts.force
  });

  if (0 == program.argc) {
    rc = build_package(strdup(CWD));
  } else {
    for (int i = 0; i < program.argc; ++i) {
      const char *dep = 0;
      if ('.' == program.argv[i][0]) {
        char dir[path_max];
        memset(dir, 0, path_max);
        dep = realpath(program.argv[i], dir);
      } else {
        dep = path_join(opts.dir, program.argv[i]);
      }

      rc = build_package(dep);

      // try with slug
      if (0 != rc) {
        rc = build_package(program.argv[i]);
      }
    }
  }

  int total_built = 0;
  hash_each(built, {
    if (0 == strncmp("t", val, 1)) {
      (void) total_built++;
    }
    if (0 != key) {
      free((void *) key);
    }
  });

  hash_free(built);
  command_free(&program);
  curl_global_cleanup();
  clib_package_cleanup();

  if (0 == rc) {
    if (total_built > 0) {
      printf("\n");
    }

    if (opts.verbose) {
      if (total_built > 1){
        logger_info("info", "built %d packages", total_built);
      } else if (1 == total_built) {
        logger_info("info", "built 1 package");
      } else {
        logger_info("info", "built 0 packages");
      }
    }
  }

  return rc;
}
