//
// clib-package.cpp
//
// Copyright (c) 2014 Stephen Mathieson
// MIT license
//

#include <stdlib.h>
#include <libgen.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
extern "C" {
    #include "strdup/strdup.h"
    #include "str-concat/str-concat.h"
    #include "str-replace/str-replace.h"
    #include "parson/parson.h"
    #include "substr/substr.h"
    #include "http-get/http-get.h"
    #include "mkdirp/mkdirp.h"
    #include "fs/fs.h"
    #include "path-join/path-join.h"
    #include "logger/logger.h"
    #include "parse-repo/parse-repo.h"
    #include "debug/debug.h"
    #include "semver/semver.h"
}
#include <pthread.h>
#include <string>

#include "clib-package.h"
#include "config.h"

#ifndef DEFAULT_REPO_VERSION
#define DEFAULT_REPO_VERSION "master"
#endif

#ifndef DEFAULT_REPO_OWNER
#define DEFAULT_REPO_OWNER "clibs"
#endif

#define GITHUB_CONTENT_URL "https://raw.githubusercontent.com/"

debug_t _debugger;

#define _debug(...) ({                                         \
  if (!(_debugger.name)) debug_init(&_debugger, "clib-package"); \
  debug(&_debugger, __VA_ARGS__);                               \
})

/**
 * Pre-declare prototypes.
 */

static inline char *
json_object_get_string_safe(JSON_Object *, const char *);

static inline char *
json_array_get_string_safe(JSON_Array *, int);

static inline char *
clib_package_file_url(const char *, const char *);

static inline char *
clib_package_slug(const char *, const char *, const char *);

static inline char *
clib_package_repo(const char *, const char *);

static inline list_t *
parse_package_deps(JSON_Object *);

static inline int
install_packages(list_t *, const char *, int, const char *);


/**
 * Create a copy of the result of a `json_object_get_string`
 * invocation.  This allows us to `json_value_free()` the
 * parent `JSON_Value` without destroying the string.
 */

static inline char *
json_object_get_string_safe(JSON_Object *obj, const char *key) {
  const char *val = json_object_get_string(obj, key);
  if (!val) return NULL;
  return strdup(val);
}

/**
 * Create a copy of the result of a `json_array_get_string`
 * invocation.  This allows us to `json_value_free()` the
 * parent `JSON_Value` without destroying the string.
 */

static inline char *
json_array_get_string_safe(JSON_Array *array, int index) {
  const char *val = json_array_get_string(array, index);
  if (!val) return NULL;
  return strdup(val);
}

/**
 * Build a URL for `file` of the package belonging to `url`
 */

static inline char *
clib_package_file_url(const char *url, const char *file) {
  if (!url || !file) return NULL;

  int size =
      strlen(url)
    + 1  // /
    + strlen(file)
    + 1  // \0
    ;

  char *res = (char *)malloc(size);
  if (res) {
    memset(res, '\0', size);
    sprintf(res, "%s/%s", url, file[0] == '@' ? &file[1] : file);
  }
  return res;
}

/**
 * Build a slug
 */

static inline char *
clib_package_slug(const char *author, const char *name, const char *version) {
  int size =
      strlen(author)
    + 1 // /
    + strlen(name)
    + 1 // @
    + strlen(version)
    + 1 // \0
    ;

  char *slug = (char*)malloc(size);
  if (slug) {
    memset(slug, '\0', size);
    sprintf(slug, "%s/%s@%s", author, name, version);
  }
  return slug;
}

/**
 * Build a repo
 */

static inline char *
clib_package_repo(const char *author, const char *name) {
  int size =
      strlen(author)
    + 1 // /
    + strlen(name)
    + 1 // \0
    ;

  char *repo = (char*)malloc(size);
  if (repo) {
    memset(repo, '\0', size);
    sprintf(repo, "%s/%s", author, name);
  }
  return repo;
}

/**
 * Parse the dependencies in the given `obj` into a `list_t`
 */

static inline list_t *
parse_package_deps(JSON_Object *obj) {
  list_t *list = NULL;

  if (!obj) goto done;
  if (!(list = list_new())) goto done;
  list->free = clib_package_dependency_free;

  for (unsigned int i = 0; i < json_object_get_count(obj); i++) {
    const char *name = NULL;
    char *version = NULL;
    clib_package_dependency_t *dep = NULL;
    int error = 1;

    if (!(name = json_object_get_name(obj, i))) goto loop_cleanup;
    if (!(version = json_object_get_string_safe(obj, name))) goto loop_cleanup;
    if (!(dep = clib_package_dependency_new(name, version))) goto loop_cleanup;
    if (!(list_rpush(list, list_node_new(dep)))) goto loop_cleanup;

    error = 0;

  loop_cleanup:
    if (version) free(version);
    if (error) {
      list_destroy(list);
      list = NULL;
      break;
    }
  }

done:
  return list;
}

struct dependency {
    pthread_t threadid;
    char * slug;
    clib_package_t * pkg;
    int verbose;
    const char * cfg;
};

struct file_info {
    clib_package_t * pkg;
    const char * dir;
    char * file;
    int verbose;
};

static int
fetch_package_file(
    clib_package_t *pkg
    , const char *dir
    , char *file
    , int verbose
    );

void * fetch_package_file_threaded(void * param)
{
    struct file_info * finfo = (struct file_info *)param;
    fetch_package_file(finfo->pkg, finfo->dir, finfo->file, finfo->verbose);
    return NULL;

}
pthread_t create_fetch_package_file_threaded(clib_package_t * pkg, const char * dir, char * file, int verbose)
{
    struct file_info * finfo = (struct file_info*)malloc(sizeof(struct file_info));
    finfo->pkg = pkg;
    finfo->dir = dir;
    finfo->file = file;
    finfo->verbose = verbose;
    pthread_t id;
    pthread_create(&id, NULL, fetch_package_file_threaded, finfo);
    return id;
}

void * clib_package_new_from_slug_threaded(void * param)
{
    struct dependency * depend = (struct dependency *)param;
    depend->pkg = clib_package_new_from_slug(depend->slug, depend->verbose, depend->cfg);
    return NULL;
}

static inline int
install_packages(list_t *list, const char *dir, int verbose, const char * cfg) {
  list_node_t *node = NULL;
  list_iterator_t *iterator = NULL;
  int rc = -1;

  int depcount = 0;
  struct dependency dependencies[255];

  if (!list || !dir) goto cleanup;

  iterator = list_iterator_new(list, LIST_HEAD);
  if (NULL == iterator) goto cleanup;

  depcount = 0;
  while ((node = list_iterator_next(iterator))) {
      clib_package_dependency_t *dep = NULL;
      char *slug = NULL;
      int error = 1;
      struct dependency * depend = &dependencies[depcount];

      dep = (clib_package_dependency_t *)node->val;
      slug = clib_package_slug(dep->author, dep->name, dep->version);
      printf("installing slug: %s\n", slug);

      depend->slug = slug;
      depend->verbose = verbose;
      depend->cfg = cfg;
      pthread_create(&depend->threadid, NULL, clib_package_new_from_slug_threaded, depend);
      depcount++;
  }
  for (int i = 0; i < depcount; i++)
  {
      clib_package_t *pkg = NULL;
      struct dependency * depend = &dependencies[i];
      pthread_join(depend->threadid, NULL);
      pkg = depend->pkg;
      if (NULL == pkg)
      {
          printf("failed pkg\n");
      }
      else
      {
          if (-1 == clib_package_install(pkg, dir, verbose))
              printf("failed\n");
      }

      if (depend->slug) free(depend->slug);
      if (depend->pkg) clib_package_free(depend->pkg);
  }

  rc = 0;

cleanup:
  if (iterator) list_iterator_destroy(iterator);
  return rc;
}

/**
 * Create a new clib package from the given `json`
 */

clib_package_t *
clib_package_new(const char *json, int verbose, const char * cfg) {
  clib_package_t *pkg = NULL;
  JSON_Value *root = NULL, *cfg_root = NULL;
  JSON_Object *json_object = NULL, *cfg_object = NULL;
  JSON_Array *src = NULL;
  JSON_Object *deps = NULL;
  JSON_Object *devs = NULL;
  int error = 1;

  if (!json) goto cleanup;
  if (!(root = json_parse_string(json))) {
    logger_error("error", "unable to parse json");
    goto cleanup;
  }
  if (!(json_object = json_value_get_object(root))) {
    logger_error("error", "invalid package.json");
    goto cleanup;
  }
  if (!(pkg = (clib_package_t*)malloc(sizeof(clib_package_t)))) goto cleanup;

  memset(pkg, '\0', sizeof(clib_package_t));

  pkg->json = strdup(json);
  pkg->name = json_object_get_string_safe(json_object, "name");
  pkg->repo = json_object_get_string_safe(json_object, "repo");
  pkg->version = json_object_get_string_safe(json_object, "version");
  pkg->license = json_object_get_string_safe(json_object, "license");
  pkg->description = json_object_get_string_safe(json_object, "description");
  pkg->install = json_object_get_string_safe(json_object, "install");
  pkg->makefile = json_object_get_string_safe(json_object, "makefile");
  pkg->cfg = cfg;

  _debug("creating package: %s", pkg->repo);

  // TODO npm-style "repository" (thlorenz/gumbo-parser.c#1)
  if (pkg->repo) {
    pkg->author = parse_repo_owner(pkg->repo, DEFAULT_REPO_OWNER);
    // repo name may not be package name (thing.c -> thing)
    pkg->repo_name = parse_repo_name(pkg->repo);
  } else {
    if (verbose) logger_warn("warning", "missing repo in package.json");
    pkg->author = NULL;
    pkg->repo_name = NULL;
  }

  if ((src = json_object_get_array(json_object, "src"))) {
    if (!(pkg->src = list_new())) goto cleanup;
    pkg->src->free = free;
    for (unsigned int i = 0; i < json_array_get_count(src); i++) {
      char *file = json_array_get_string_safe(src, i);
      _debug("file: %s", file);
      if (!file) goto cleanup;
      if (!(list_rpush(pkg->src, list_node_new(file)))) goto cleanup;
    }
  } else {
    _debug("no src files listed in package.json");
    pkg->src = NULL;
  }

  if ((deps = json_object_get_object(json_object, "dependencies"))) {
    if (!(pkg->dependencies = parse_package_deps(deps))) {
      goto cleanup;
    }
  } else {
    _debug("no dependencies listed in package.json");
    pkg->dependencies = NULL;
  }

  if ((devs = json_object_get_object(json_object, "development"))) {
    if (!(pkg->development = parse_package_deps(devs))) {
      goto cleanup;
    }
  } else {
    _debug("no development dependencies listed in package.json");
    pkg->development = NULL;
  }

  error = 0;

cleanup:
  if (root) json_value_free(root);
  if (error && pkg) {
    clib_package_free(pkg);
    pkg = NULL;
  }
  return pkg;
}

static const char *
clib_package_find_api_endpoint(const char * author, const char * name, const char *cfg)
{
  http_get_response_t *res = NULL;
  if(!cfg) {
    return NULL;
  }

  JSON_Value *cfg_root = NULL;
  JSON_Object *cfg_object = NULL;
  if(!(cfg_root = json_parse_string(cfg))) {
    logger_error("error", "unable to parse config.json file");
    return NULL;
  }
  if(!(cfg_object = json_value_get_object(cfg_root))) {
    logger_error("error", "invalid config.json file");
    return NULL;
  }

  JSON_Array *src = json_object_get_array(cfg_object, "api_endpoints");
  if(src)
  {
    for (unsigned int i = 0; i < json_array_get_count(src); i++) {
      char *url = json_array_get_string_safe(src, i);
      std::string try_url = url;
      try_url += std::string("repos/");
      try_url += std::string(author);
      try_url += std::string("/");
      try_url += std::string(name);

//      printf("trying API base at %s\n", try_url.c_str());
      res = http_get(try_url.c_str());
      if( res && res->ok) {
        return url;
      }
    }
  }

  return NULL;
}

/**
 * Create a package from the given repo `slug`
 */
clib_package_t *
clib_package_new_from_slug(const char *slug, int verbose, const char * cfg) {
  char *author = NULL;
  char *name = NULL;
  char *version = NULL;
  char *url = NULL;
  char *json_url = NULL;
  char *repo = NULL;
  http_get_response_t *res = NULL;
  clib_package_t *pkg = NULL;
  JSON_Value * root  = NULL;
  JSON_Object * obj = NULL;
  char * download_url = NULL;
  const char * api_endpoint = NULL;

  // parse chunks
  if (!slug) goto error;
  _debug("creating package: %s", slug);
  if (!(author = parse_repo_owner(slug, DEFAULT_REPO_OWNER))) goto error;
  if (!(name = parse_repo_name(slug))) goto error;
  if (!(version = parse_repo_version(slug, DEFAULT_REPO_VERSION))) goto error;

  // given an author and name, attempt to find the api endpoint
  api_endpoint = clib_package_find_api_endpoint(author, name, cfg);
  if(!api_endpoint) {
    logger_error("error", "failed to find api endpoint");
    goto error;
  }

  // if not master then Query the API for tags
  if(std::string(version) != "master") {

  }

  printf("%s:%s:%s\n", author, name, version);
  {
    std::string try_url = api_endpoint;
    try_url += std::string("repos/");
    try_url += std::string(author);
    try_url += std::string("/");
    try_url += std::string(name);
    try_url += std::string("/contents/package.json?");
    try_url += std::string(version);

    res = http_get(try_url.c_str());
  }
  if(!res || !res->ok) {
    logger_error("error", "unable to fetch %s/%s:package.json", author, name);
    goto error;
  }

  // Parse the API response
  root = json_parse_string(res->data);
  obj = json_value_get_object(root);
  download_url = json_object_get_string_safe(obj, "download_url");
  json_value_free(root);

  http_get_free(res);
  res = http_get(download_url);
  if (!res || !res->ok) {
    logger_error("error", "unable to fetch %s/%s:package.json", author, name);
    goto error;
  }

  free(name);
  name = NULL;

  // build package
  pkg = clib_package_new(res->data, verbose, cfg);
  pkg->api_endpoint = api_endpoint;
  http_get_free(res);
  res = NULL;
  if (!pkg) goto error;

  // force version number
  if (pkg->version) {
    if (version) {
      if (0 != strcmp(version, DEFAULT_REPO_VERSION)) {
        _debug("forcing version number: %s (%s)", version, pkg->version);
        free(pkg->version);
        pkg->version = version;
      } else {
        free(version);
      }
    }
  } else {
    pkg->version = version;
  }

  // force package author (don't know how this could fail)
  if (pkg->author) {
    if (0 != strcmp(author, pkg->author)) {
      free(pkg->author);
      pkg->author = author;
    } else {
      free(author);
    }
  } else {
    pkg->author = author;
  }

  if (!(repo = clib_package_repo(pkg->author, pkg->name))) goto error;

  if (pkg->repo) {
    if (0 != strcmp(repo, pkg->repo)) {
      free(url);
      if (!(url = clib_package_url_from_repo(pkg->repo, pkg->version)))
        goto error;
    }
    free(repo);
    repo = NULL;
  } else {
    pkg->repo = repo;
  }

  pkg->url = url;
  return pkg;

error:
  free(author);
  free(name);
  free(version);
  free(url);
  free(json_url);
  free(repo);
  http_get_free(res);
  if (pkg) clib_package_free(pkg);
  return NULL;
}

/**
 * Get a slug for the package `author/name@version`
 */

char *
clib_package_url(const char *author, const char *name, const char *version) {
  if (!author || !name || !version) return NULL;
  int size =
      strlen(GITHUB_CONTENT_URL)
    + strlen(author)
    + 1 // /
    + strlen(name)
    + 1 // /
    + strlen(version)
    + 1 // \0
    ;

  char *slug = (char*)malloc(size);
  if (slug) {
    memset(slug, '\0', size);
    sprintf(slug
      , GITHUB_CONTENT_URL "%s/%s/%s"
      , author
      , name
      , version);
    if (!strncmp(version, "https", 5)) {
        sprintf(slug, "%s", version);
    }
  }
  return slug;
}

char *
clib_package_url_from_repo(const char *repo, const char *version) {
  if (!repo || !version) return NULL;
  int size =
      strlen(GITHUB_CONTENT_URL)
    + strlen(repo)
    + 1 // /
    + strlen(version)
    + 1 // \0
    ;

  char *slug = (char *)malloc(size);
  if (slug) {
    memset(slug, '\0', size);
    sprintf(slug
      , GITHUB_CONTENT_URL "%s/%s"
      , repo
      , version);
  }
  return slug;
}

/**
 * Parse the package author from the given `slug`
 */

char *
clib_package_parse_author(const char *slug) {
  return parse_repo_owner(slug, DEFAULT_REPO_OWNER);
}

/**
 * Parse the package version from the given `slug`
 */

char *
clib_package_parse_version(const char *slug) {
  return parse_repo_version(slug, DEFAULT_REPO_VERSION);
}

/**
 * Parse the package name from the given `slug`
 */
char *
clib_package_parse_name(const char *slug) {
  return parse_repo_name(slug);
}

/**
 * Create a new package dependency from the given `repo` and `version`
 */

clib_package_dependency_t *
clib_package_dependency_new(const char *repo, const char *version) {
  if (!repo || !version) return NULL;

  clib_package_dependency_t *dep = (clib_package_dependency_t*)malloc(sizeof(clib_package_dependency_t));
  if (!dep) {
    return NULL;
  }

  dep->version = 0 == strcmp("*", version)
    ? strdup(DEFAULT_REPO_VERSION)
    : strdup(version);
  dep->name = clib_package_parse_name(repo);
  dep->author = clib_package_parse_author(repo);

  _debug("dependency: %s/%s@%s", dep->author, dep->name, dep->version);
  return dep;
}

/**
 * Fetch a file associated with the given `pkg`.
 *
 * Returns 0 on success.
 */

static int
fetch_package_file(
      clib_package_t *pkg
    , const char *dir
    , char *file
    , int verbose
  ) {
  char *url = NULL;
  char *path = NULL;
  int rc = 0;
  http_get_response_t *res = NULL;

  _debug("fetch file: %s/%s", pkg->repo, file);

  std::string try_url = pkg->api_endpoint;
  try_url += std::string("repos/");
  try_url += std::string(pkg->author);
  try_url += std::string("/");
  try_url += std::string(pkg->name);
  try_url += std::string("/contents/");
  try_url += std::string(file[0] == '@' ? &file[1] : file);
  try_url += std::string("?ref=master");
  //try_url += std::string(pkg->version);
  printf("Making API call at %s\n", try_url.c_str());

  res = http_get(try_url.c_str());
  if( res && res->ok) {
    JSON_Value * root = json_parse_string(res->data);
    JSON_Object * obj = json_value_get_object(root);
    char * download_url = json_object_get_string_safe(obj, "download_url");
    json_value_free(root);
    http_get_free(res);

    char * dpart = strdup(file);
    char * pkg_dir = path_join(dir, dirname(dpart) + (file[0] == '@' ? 1 : 0));
    mkdirp(pkg_dir, 0777);
    free(pkg_dir);

    file = (file[0] == '@') ? &file[1] : basename(file);
    if (!(path = path_join(dir, file))) {
      rc = 1;
      goto cleanup;
    }

    if(verbose) logger_info("fetch", "%s -> %s(%s)", url, path, file);

    if (-1 == http_get_file(download_url, path)) {
      logger_error("error", "unable to fetch %s:%s", pkg->repo, file);
      rc = 1;
      goto cleanup;
    }

    if (verbose) logger_info("save", path);
  }

cleanup:
  free(path);
  return rc;
}

/**
 * Install the given `pkg` in `dir`
 */

int
clib_package_install(clib_package_t *pkg, const char *dir, int verbose) {
  char *pkg_dir = NULL;
  char *package_json = NULL;
  list_iterator_t *iterator = NULL;
  int rc = -1;
  pthread_t threadids[255];
  int i = 0;
  list_node_t *source;
  char * fname, *mkfile;
  FILE * it;
  char * cmdline;
  struct stat finfo;
  char * localjson = NULL;

  if (!pkg || !dir) goto cleanup;
  if (!(pkg_dir = path_join(dir, pkg->name))) goto cleanup;

  _debug("mkdir -p %s", pkg_dir);
  // create directory for pkg
  if (-1 == mkdirp(pkg_dir, 0777)) goto cleanup;

  if (NULL == pkg->url) {
    pkg->url = clib_package_url(pkg->author
      , pkg->repo_name
      , pkg->version);
    if (NULL == pkg->url) goto cleanup;
  }

  // write package.json
  if (!(package_json = path_join(pkg_dir, "package.json"))) goto cleanup;

  _debug("reading local package.json");
  localjson = fs_read(package_json);
  if (NULL != localjson)
  {
      clib_package_t *localpkg = clib_package_new(localjson, verbose, pkg->cfg);

      semver_t current_version = {};
      semver_t compare_version = {};
      semver_parse(localpkg->version, &current_version);
      semver_parse(pkg->version, &compare_version);

      int resolution = semver_compare(compare_version, current_version);
      semver_free(&current_version);
      semver_free(&compare_version);

      if (resolution == 0 || resolution == -1) {
          if (verbose) logger_info("skipping", "new v%s is equal or lower than installed v%s for %s", pkg->version, localpkg->version, pkg->repo);
          rc = 0;
          clib_package_free(localpkg);
          goto cleanup;
      }
      clib_package_free(localpkg);
  }

  _debug("write: %s", package_json);
  if (-1 == fs_write(package_json, pkg->json)) {
    logger_error("error", "Failed to write %s", package_json);
    goto cleanup;
  }

  // fetch makefile
  if (pkg->makefile) {
    _debug("fetch: %s/%s", pkg->repo, pkg->makefile);
    if (0 != fetch_package_file(pkg, pkg_dir, pkg->makefile, verbose)) {
      goto cleanup;
    }
  }

  // if no sources are listed, just install
  if (NULL == pkg->src) goto install;

  iterator = list_iterator_new(pkg->src, LIST_HEAD);

  i = 0;
  while ((source = list_iterator_next(iterator))) {
      threadids[i++] = create_fetch_package_file_threaded(pkg, pkg_dir, (char *)source->val, verbose);
  }
  for (int j = 0; j < i; j++)
  {
      pthread_join(threadids[j], NULL);
  }

  /* Create a .mk file for the project */
  fname = concat(pkg->name, ".mk");
  mkfile = path_join(pkg_dir, fname);
  it = fopen(mkfile, "w+");
  fputs("deps_", it);
  fputs("_a_SOURCES += ", it);
  iterator = list_iterator_new(pkg->src, LIST_HEAD);
  while ((source = list_iterator_next(iterator))) {
      char * fname = strdup((char *)source->val);
      char * fmod = (fname[0] == '@') ? &fname[1] : basename(fname);
      fputs("deps/", it);
      fputs(pkg->name, it);
      fputs("/", it);
      fputs(fmod, it);
      fputs(" ", it);
      free(fname);
  }
  fputs("\n", it);
  fclose(it);
  free(fname);
  free(mkfile);

  cmdline = (char*)malloc(4096);
  sprintf(cmdline, "sed -i '/include $(top_srcdir)\\/deps\\/%s\\/%s.mk/d' deps.mk; echo 'include $(top_srcdir)/deps/%s/%s.mk' >> deps.mk",
      pkg->name, pkg->name, pkg->name, pkg->name);
  rc = system(cmdline);
  free(cmdline);

install:
  rc = clib_package_install_dependencies(pkg, dir, verbose);

cleanup:
  if (pkg_dir) free(pkg_dir);
  if (package_json) free(package_json);
  if (iterator) list_iterator_destroy(iterator);
  return rc;
}

/**
 * Install the given `pkg`'s dependencies in `dir`
 */

int
clib_package_install_dependencies(clib_package_t *pkg
    , const char *dir
    , int verbose) {
  if (!pkg || !dir) return -1;
  if (NULL == pkg->dependencies) return 0;

  return install_packages(pkg->dependencies, dir, verbose, pkg->cfg);
}

/**
 * Install the given `pkg`'s development dependencies in `dir`
 */

int
clib_package_install_development(clib_package_t *pkg
    , const char *dir
    , int verbose) {
  if (!pkg || !dir) return -1;
  if (NULL == pkg->development) return 0;

  return install_packages(pkg->development, dir, verbose, pkg->cfg);
}

/**
 * Free a clib package
 */

void
clib_package_free(clib_package_t *pkg) {
  free(pkg->author);
  free(pkg->description);
  free(pkg->install);
  free(pkg->json);
  free(pkg->license);
  free(pkg->name);
  free(pkg->makefile);
  free(pkg->repo);
  free(pkg->repo_name);
  free(pkg->url);
  free(pkg->version);
  if (pkg->src) list_destroy(pkg->src);
  if (pkg->dependencies) list_destroy(pkg->dependencies);
  if (pkg->development) list_destroy(pkg->development);
  free(pkg);
}

void
clib_package_dependency_free(void *_dep) {
  clib_package_dependency_t *dep = (clib_package_dependency_t *) _dep;
  free(dep->name);
  free(dep->author);
  free(dep->version);
  free(dep);
}
