//
// clib-install.c
//
// Copyright (c) 2012-2020 clib authors
// MIT licensed
//

#include "commander/commander.h"
#include "common/clib-cache.h"
#include "common/clib-package.h"
#include "common/clib-settings.h"
#include "common/clib-validate.h"
#include "debug/debug.h"
#include "fs/fs.h"
#include "http-get/http-get.h"
#include "logger/logger.h"
#include "mkdirp/mkdirp.h"
#include "parson/parson.h"
#include "str-replace/str-replace.h"
#include "version.h"
#include <curl/curl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SX(s) #s
#define S(s) SX(s)

#if defined(_WIN32) || defined(WIN32) || defined(__MINGW32__) ||               \
    defined(__MINGW64__)
#define setenv(k, v, _) _putenv_s(k, v)
#define realpath(a, b) _fullpath(a, b, strlen(a))
#endif

extern CURLSH *clib_package_curl_share;

debug_t debugger = {0};

struct options {
  const char *dir;
  char *prefix;
  char *token;
  int verbose;
  int dev;
  int savedev;
  int nosave;
  int force;
  int global;
  int skip_cache;
#ifdef HAVE_PTHREADS
  unsigned int concurrency;
#endif
};

static struct options opts = {0};

static clib_package_opts_t package_opts = {0};
static clib_package_t *root_package = NULL;

/**
 * Option setters.
 */

static void setopt_dir(command_t *self) {
  opts.dir = (char *)self->arg;
  debug(&debugger, "set dir: %s", opts.dir);
}

static void setopt_prefix(command_t *self) {
  opts.prefix = (char *)self->arg;
  debug(&debugger, "set prefix: %s", opts.prefix);
}

static void setopt_token(command_t *self) {
  opts.token = (char *)self->arg;
  debug(&debugger, "set token: %s", opts.token);
}

static void setopt_quiet(command_t *self) {
  opts.verbose = 0;
  debug(&debugger, "set quiet flag");
}

static void setopt_dev(command_t *self) {
  opts.dev = 1;
  debug(&debugger, "set development flag");
}

static void setopt_save(command_t *self) {
  logger_warn("deprecated", "--save option is deprecated "
                            "(dependencies are now saved by default)");
}

static void setopt_savedev(command_t *self) {
  opts.savedev = 1;
  debug(&debugger, "set savedev flag");
}

static void setopt_nosave(command_t *self) {
  opts.nosave = 1;
  debug(&debugger, "set nosave flag");
}

static void setopt_force(command_t *self) {
  opts.force = 1;
  debug(&debugger, "set force flag");
}

static void setopt_global(command_t *self) {
  opts.global = 1;
  debug(&debugger, "set global flag");
}

#ifdef HAVE_PTHREADS
static void setopt_concurrency(command_t *self) {
  if (self->arg) {
    opts.concurrency = atol(self->arg);
    debug(&debugger, "set concurrency: %lu", opts.concurrency);
  }
}
#endif

static void setopt_skip_cache(command_t *self) {
  opts.skip_cache = 1;
  debug(&debugger, "set skip cache flag");
}

static int install_local_packages_with_package_name(const char *file) {
  if (0 != clib_validate(file)) {
    return 1;
  }

  debug(&debugger, "reading local clib.json or package.json");
  char *json = fs_read(file);
  if (NULL == json)
    return 1;

  clib_package_t *pkg = clib_package_new(json, opts.verbose);
  if (NULL == pkg)
    goto e1;

  if (opts.prefix) {
    setenv("PREFIX", opts.prefix, 1);
  } else if (root_package && root_package->prefix) {
    setenv("PREFIX", root_package->prefix, 1);
  } else if (pkg->prefix) {
    setenv("PREFIX", pkg->prefix, 1);
  }

  int rc = clib_package_install_dependencies(pkg, opts.dir, opts.verbose);
  if (-1 == rc)
    goto e2;

  if (opts.dev) {
    rc = clib_package_install_development(pkg, opts.dir, opts.verbose);
    if (-1 == rc)
      goto e2;
  }

  free(json);
  clib_package_free(pkg);
  return 0;

e2:
  clib_package_free(pkg);
e1:
  free(json);
  return 1;
}

/**
 * Install dependency packages at `pwd`.
 */
static int install_local_packages() {
  const char *name = NULL;
  unsigned int i = 0;
  int rc = 0;

  do {
    name = manifest_names[i];
    rc = install_local_packages_with_package_name(name);
  } while (NULL != manifest_names[++i] && 0 != rc);

  return rc;
}

static int write_dependency_with_package_name(clib_package_t *pkg, char *prefix,
                                              const char *file) {
  JSON_Value *packageJson = json_parse_file(file);
  JSON_Object *packageJsonObject = json_object(packageJson);
  JSON_Value *newDepSectionValue = NULL;

  if (NULL == packageJson || NULL == packageJsonObject)
    return 1;

  // If the dependency section doesn't exist then create it
  JSON_Object *depSection =
      json_object_dotget_object(packageJsonObject, prefix);
  if (NULL == depSection) {
    newDepSectionValue = json_value_init_object();
    depSection = json_value_get_object(newDepSectionValue);
    json_object_set_value(packageJsonObject, prefix, newDepSectionValue);
  }

  // Add the dependency to the dependency section
  json_object_set_string(depSection, pkg->repo, pkg->version);

  // Flush package.json
  int rc = json_serialize_to_file_pretty(packageJson, file);
  json_value_free(packageJson);
  return rc;
}

/**
 * Writes out a dependency to clib.json or package.json
 */
static int write_dependency(clib_package_t *pkg, char *prefix) {
  const char *name = NULL;
  unsigned int i = 0;
  int rc = 0;

  do {
    name = manifest_names[i];
    rc = write_dependency_with_package_name(pkg, prefix, name);
  } while (NULL != manifest_names[++i] && 0 != rc);

  return rc;
}

/**
 * Save a dependency to clib.json or package.json.
 */
static int save_dependency(clib_package_t *pkg) {
  debug(&debugger, "saving dependency %s at %s", pkg->name, pkg->version);
  return write_dependency(pkg, "dependencies");
}

/**
 * Save a development dependency to clib.json or package.json.
 */
static int save_dev_dependency(clib_package_t *pkg) {
  debug(&debugger, "saving dev dependency %s at %s", pkg->name, pkg->version);
  return write_dependency(pkg, "development");
}

/**
 * Create and install a package from `slug`.
 */

static int install_package(const char *slug) {
  clib_package_t *pkg = NULL;
  int rc;

#ifdef PATH_MAX
  long path_max = PATH_MAX;
#elif defined(_PC_PATH_MAX)
  long path_max = pathconf(slug, _PC_PATH_MAX);
#else
  long path_max = 4096;
#endif

  if ('.' == slug[0]) {
    if (1 == strlen(slug) || ('/' == slug[1] && 2 == strlen(slug))) {
      char dir[path_max];
      realpath(slug, dir);
      slug = dir;
      return install_local_packages();
    }
  }

  if (0 == fs_exists(slug)) {
    fs_stats *stats = fs_stat(slug);
    if (NULL != stats && (S_IFREG == (stats->st_mode & S_IFMT)
#if defined(__unix__) || defined(__linux__) || defined(_POSIX_VERSION)
                          || S_IFLNK == (stats->st_mode & S_IFMT)
#endif
                              )) {
      free(stats);
      return install_local_packages_with_package_name(slug);
    }

    if (stats) {
      free(stats);
    }
  }

  if (!pkg) {
    pkg = clib_package_new_from_slug(slug, opts.verbose);
  }

  if (NULL == pkg)
    return -1;

  rc = clib_package_install(pkg, opts.dir, opts.verbose);
  if (0 != rc) {
    goto cleanup;
  }

  if (0 == rc && opts.dev) {
    rc = clib_package_install_development(pkg, opts.dir, opts.verbose);
    if (0 != rc) {
      goto cleanup;
    }
  }

  if (0 == pkg->repo || 0 != strcmp(slug, pkg->repo)) {
    char* version_char = NULL;
    // NOTE: check if version was specified
    if ((version_char = strchr(slug, '@')) != NULL) {
      size_t length = version_char - slug;
      pkg->repo = malloc(sizeof(char) * length);
      memcpy(pkg->repo, slug, length);
    } else {
      pkg->repo = strdup(slug);
    }
  }

  if (!opts.nosave) {
    opts.savedev ? save_dev_dependency(pkg) : save_dependency(pkg);
  }

cleanup:
  clib_package_free(pkg);
  return rc;
}

/**
 * Install the given `pkgs`.
 */

static int install_packages(int n, char *pkgs[]) {
  for (int i = 0; i < n; i++) {
    debug(&debugger, "install %s (%d)", pkgs[i], i);
    if (-1 == install_package(pkgs[i])) {
      logger_error("error", "Unable to install package %s", pkgs[i]);
      return 1;
    }
  }
  return 0;
}

/**
 * Entry point.
 */

int main(int argc, char *argv[]) {
#ifdef _WIN32
  opts.dir = ".\\deps";
#else
  opts.dir = "./deps";
#endif
  opts.verbose = 1;
  opts.dev = 0;

#ifdef PATH_MAX
  long path_max = PATH_MAX;
#elif defined(_PC_PATH_MAX)
  long path_max = pathconf(opts.dir, _PC_PATH_MAX);
#else
  long path_max = 4096;
#endif

  debug_init(&debugger, "clib-install");

  // 30 days expiration
  clib_cache_init(CLIB_PACKAGE_CACHE_TIME);

  command_t program;

  command_init(&program, "clib-install", CLIB_VERSION);

  program.usage = "[options] [name ...]";

  command_option(&program, "-o", "--out <dir>",
                 "change the output directory [deps]", setopt_dir);
  command_option(&program, "-P", "--prefix <dir>",
                 "change the prefix directory (usually '/usr/local')",
                 setopt_prefix);
  command_option(&program, "-q", "--quiet", "disable verbose output",
                 setopt_quiet);
  command_option(&program, "-d", "--dev", "install development dependencies",
                 setopt_dev);
  command_option(&program, "-S", "--save",
                 "[DEPRECATED] save dependency in clib.json or package.json",
                 setopt_save);
  command_option(&program, "-D", "--save-dev",
                 "save development dependency in clib.json or package.json",
                 setopt_savedev);
  command_option(&program, "-N", "--no-save",
                 "don't save dependency in clib.json or package.json",
                 setopt_nosave);
  command_option(&program, "-f", "--force",
                 "force the action of something, like overwriting a file",
                 setopt_force);
  command_option(&program, "-c", "--skip-cache", "skip cache when installing",
                 setopt_skip_cache);
  command_option(&program, "-g", "--global",
                 "global install, don't write to output dir (default: deps/)",
                 setopt_global);
  command_option(&program, "-t", "--token <token>",
                 "Access token used to read private content", setopt_token);
#ifdef HAVE_PTHREADS
  command_option(&program, "-C", "--concurrency <number>",
                 "Set concurrency (default: " S(MAX_THREADS) ")",
                 setopt_concurrency);
#endif
  command_parse(&program, argc, argv);

  debug(&debugger, "%d arguments", program.argc);

  if (0 != curl_global_init(CURL_GLOBAL_ALL)) {
    logger_error("error", "Failed to initialize cURL");
  }

  root_package = clib_package_load_local_manifest(opts.verbose);

  if (root_package && root_package->prefix && !opts.prefix) {
    opts.prefix = root_package->prefix;
  }

  if (opts.prefix) {
    char prefix[path_max];

    mkdirp(opts.prefix, 0777);

    memset(prefix, 0, path_max);
    realpath(opts.prefix, prefix);

    unsigned long int size = strlen(prefix) + 1;
    opts.prefix = malloc(size);

    memset((void *)opts.prefix, 0, size);
    memcpy((void *)opts.prefix, prefix, size);
  }

  clib_cache_init(CLIB_PACKAGE_CACHE_TIME);

  package_opts.skip_cache = opts.skip_cache;
  package_opts.prefix = opts.prefix;
  package_opts.global = opts.global;
  package_opts.force = opts.force;
  package_opts.token = opts.token;

#ifdef HAVE_PTHREADS
  package_opts.concurrency = opts.concurrency;
#endif

  clib_package_set_opts(package_opts);

  int code = 0 == program.argc ? install_local_packages()
                               : install_packages(program.argc, program.argv);

  curl_global_cleanup();
  clib_package_cleanup();

  command_free(&program);
  return code;
}
