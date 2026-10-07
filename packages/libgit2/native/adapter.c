#include "bindings.h"

#include <stddef.h>

static git_repository *native_repository(struct GtiGitRepository *repository) {
  return (git_repository *)repository;
}

static const git_repository *
native_const_repository(const struct GtiGitRepository *repository) {
  return (const git_repository *)repository;
}

static git_commit *native_commit(struct GtiGitCommit *commit) {
  return (git_commit *)commit;
}

static const git_commit *
native_const_commit(const struct GtiGitCommit *commit) {
  return (const git_commit *)commit;
}

int32_t gti_git_init(void) { return (int32_t)git_libgit2_init(); }
int32_t gti_git_shutdown(void) { return (int32_t)git_libgit2_shutdown(); }

void gti_git_version(int32_t *major, int32_t *minor, int32_t *revision) {
  int native_major = 0;
  int native_minor = 0;
  int native_revision = 0;
  git_libgit2_version(&native_major, &native_minor, &native_revision);
  if (major != NULL) {
    *major = (int32_t)native_major;
  }
  if (minor != NULL) {
    *minor = (int32_t)native_minor;
  }
  if (revision != NULL) {
    *revision = (int32_t)native_revision;
  }
}

const char *gti_git_last_error_message(void) {
  const git_error *error = git_error_last();
  return error == NULL ? NULL : error->message;
}

int32_t gti_git_last_error_class(void) {
  const git_error *error = git_error_last();
  return error == NULL ? (int32_t)GIT_ERROR_NONE : (int32_t)error->klass;
}

int32_t gti_git_repository_init(struct GtiGitRepository **output,
                                const char *path, int32_t bare) {
  git_repository *repository = NULL;
  int result;
  if (output == NULL || path == NULL) {
    return (int32_t)GIT_EINVALID;
  }
  result = git_repository_init(&repository, path, bare != 0);
  if (result == GIT_OK) {
    *output = (struct GtiGitRepository *)repository;
  }
  return (int32_t)result;
}

int32_t gti_git_repository_open(struct GtiGitRepository **output,
                                const char *path) {
  git_repository *repository = NULL;
  int result;
  if (output == NULL || path == NULL) {
    return (int32_t)GIT_EINVALID;
  }
  result = git_repository_open(&repository, path);
  if (result == GIT_OK) {
    *output = (struct GtiGitRepository *)repository;
  }
  return (int32_t)result;
}

void gti_git_repository_free(struct GtiGitRepository *repository) {
  git_repository_free(native_repository(repository));
}

const char *gti_git_repository_path(const struct GtiGitRepository *repository) {
  return repository == NULL
             ? NULL
             : git_repository_path(native_const_repository(repository));
}

const char *
gti_git_repository_workdir(const struct GtiGitRepository *repository) {
  return repository == NULL
             ? NULL
             : git_repository_workdir(native_const_repository(repository));
}

int32_t gti_git_repository_is_bare(const struct GtiGitRepository *repository) {
  return repository == NULL ? (int32_t)GIT_EINVALID
                            : (int32_t)git_repository_is_bare(
                                  native_const_repository(repository));
}

int32_t gti_git_repository_is_empty(struct GtiGitRepository *repository) {
  return repository == NULL
             ? (int32_t)GIT_EINVALID
             : (int32_t)git_repository_is_empty(native_repository(repository));
}

int32_t gti_git_repository_head_detached(struct GtiGitRepository *repository) {
  return repository == NULL ? (int32_t)GIT_EINVALID
                            : (int32_t)git_repository_head_detached(
                                  native_repository(repository));
}

int32_t gti_git_repository_is_shallow(struct GtiGitRepository *repository) {
  return repository == NULL ? (int32_t)GIT_EINVALID
                            : (int32_t)git_repository_is_shallow(
                                  native_repository(repository));
}

int32_t gti_git_repository_state(struct GtiGitRepository *repository) {
  return repository == NULL
             ? (int32_t)GIT_EINVALID
             : (int32_t)git_repository_state(native_repository(repository));
}

int32_t gti_git_status_file(struct GtiGitRepository *repository,
                            const char *path, uint32_t *status) {
  unsigned int native_status = 0;
  int result;
  if (repository == NULL || path == NULL || status == NULL) {
    return (int32_t)GIT_EINVALID;
  }
  result = git_status_file(&native_status, native_repository(repository), path);
  if (result == GIT_OK) {
    *status = (uint32_t)native_status;
  }
  return (int32_t)result;
}

int32_t gti_git_head_commit(struct GtiGitRepository *repository,
                            struct GtiGitCommit **output) {
  git_reference *head = NULL;
  git_object *object = NULL;
  int result;
  if (repository == NULL || output == NULL) {
    return (int32_t)GIT_EINVALID;
  }
  result = git_repository_head(&head, native_repository(repository));
  if (result != GIT_OK) {
    return (int32_t)result;
  }
  result = git_reference_peel(&object, head, GIT_OBJECT_COMMIT);
  git_reference_free(head);
  if (result == GIT_OK) {
    *output = (struct GtiGitCommit *)object;
  }
  return (int32_t)result;
}

void gti_git_commit_free(struct GtiGitCommit *commit) {
  git_commit_free(native_commit(commit));
}

const char *gti_git_commit_summary(struct GtiGitCommit *commit) {
  return commit == NULL ? NULL : git_commit_summary(native_commit(commit));
}

const char *gti_git_commit_message(const struct GtiGitCommit *commit) {
  return commit == NULL ? NULL
                        : git_commit_message(native_const_commit(commit));
}

const char *gti_git_commit_author_name(const struct GtiGitCommit *commit) {
  const git_signature *author;
  if (commit == NULL) {
    return NULL;
  }
  author = git_commit_author(native_const_commit(commit));
  return author == NULL ? NULL : author->name;
}

const char *gti_git_commit_author_email(const struct GtiGitCommit *commit) {
  const git_signature *author;
  if (commit == NULL) {
    return NULL;
  }
  author = git_commit_author(native_const_commit(commit));
  return author == NULL ? NULL : author->email;
}

int64_t gti_git_commit_time(const struct GtiGitCommit *commit) {
  return commit == NULL ? 0
                        : (int64_t)git_commit_time(native_const_commit(commit));
}

int32_t gti_git_commit_time_offset(const struct GtiGitCommit *commit) {
  return commit == NULL
             ? 0
             : (int32_t)git_commit_time_offset(native_const_commit(commit));
}

uint32_t gti_git_commit_parent_count(const struct GtiGitCommit *commit) {
  return commit == NULL
             ? 0
             : (uint32_t)git_commit_parentcount(native_const_commit(commit));
}

int32_t gti_git_commit_id_hex(const struct GtiGitCommit *commit,
                              uint8_t *output, uint64_t capacity) {
  const git_oid *identifier;
  if (commit == NULL || output == NULL) {
    return (int32_t)GIT_EINVALID;
  }
  if (capacity < (uint64_t)GTI_GIT_OID_HEX_BUFFER_SIZE ||
      (uint64_t)(size_t)capacity != capacity) {
    return (int32_t)GIT_EBUFS;
  }
  identifier = git_commit_id(native_const_commit(commit));
  if (identifier == NULL || git_oid_tostr((char *)output, (size_t)capacity,
                                          identifier) != (char *)output) {
    return (int32_t)GIT_ERROR;
  }
  return (int32_t)GIT_OK;
}
