#ifndef GTI_PACKAGE_LIBGIT2_BINDINGS_H
#define GTI_PACKAGE_LIBGIT2_BINDINGS_H

#include <git2/commit.h>
#include <git2/errors.h>
#include <git2/global.h>
#include <git2/refs.h>
#include <git2/repository.h>
#include <git2/status.h>
#include <stdint.h>

struct GtiGitRepository;
struct GtiGitCommit;

enum GtiGitErrorCode {
  GTI_GIT_OK = GIT_OK,
  GTI_GIT_ERROR = GIT_ERROR,
  GTI_GIT_ENOTFOUND = GIT_ENOTFOUND,
  GTI_GIT_EEXISTS = GIT_EEXISTS,
  GTI_GIT_EAMBIGUOUS = GIT_EAMBIGUOUS,
  GTI_GIT_EBUFS = GIT_EBUFS,
  GTI_GIT_EUNBORNBRANCH = GIT_EUNBORNBRANCH,
  GTI_GIT_EINVALIDSPEC = GIT_EINVALIDSPEC,
  GTI_GIT_EINVALID = GIT_EINVALID,
  GTI_GIT_ENOTSUPPORTED = GIT_ENOTSUPPORTED
};

enum GtiGitRepositoryState {
  GTI_GIT_REPOSITORY_STATE_NONE = GIT_REPOSITORY_STATE_NONE,
  GTI_GIT_REPOSITORY_STATE_MERGE = GIT_REPOSITORY_STATE_MERGE,
  GTI_GIT_REPOSITORY_STATE_REVERT = GIT_REPOSITORY_STATE_REVERT,
  GTI_GIT_REPOSITORY_STATE_CHERRYPICK = GIT_REPOSITORY_STATE_CHERRYPICK,
  GTI_GIT_REPOSITORY_STATE_BISECT = GIT_REPOSITORY_STATE_BISECT,
  GTI_GIT_REPOSITORY_STATE_REBASE = GIT_REPOSITORY_STATE_REBASE,
  GTI_GIT_REPOSITORY_STATE_APPLY_MAILBOX = GIT_REPOSITORY_STATE_APPLY_MAILBOX
};

enum GtiGitStatusFlag {
  GTI_GIT_STATUS_CURRENT = GIT_STATUS_CURRENT,
  GTI_GIT_STATUS_INDEX_NEW = GIT_STATUS_INDEX_NEW,
  GTI_GIT_STATUS_INDEX_MODIFIED = GIT_STATUS_INDEX_MODIFIED,
  GTI_GIT_STATUS_INDEX_DELETED = GIT_STATUS_INDEX_DELETED,
  GTI_GIT_STATUS_INDEX_RENAMED = GIT_STATUS_INDEX_RENAMED,
  GTI_GIT_STATUS_INDEX_TYPECHANGE = GIT_STATUS_INDEX_TYPECHANGE,
  GTI_GIT_STATUS_WT_NEW = GIT_STATUS_WT_NEW,
  GTI_GIT_STATUS_WT_MODIFIED = GIT_STATUS_WT_MODIFIED,
  GTI_GIT_STATUS_WT_DELETED = GIT_STATUS_WT_DELETED,
  GTI_GIT_STATUS_WT_TYPECHANGE = GIT_STATUS_WT_TYPECHANGE,
  GTI_GIT_STATUS_WT_RENAMED = GIT_STATUS_WT_RENAMED,
  GTI_GIT_STATUS_WT_UNREADABLE = GIT_STATUS_WT_UNREADABLE,
  GTI_GIT_STATUS_IGNORED = GIT_STATUS_IGNORED,
  GTI_GIT_STATUS_CONFLICTED = GIT_STATUS_CONFLICTED
};

enum GtiGitLimits { GTI_GIT_OID_HEX_BUFFER_SIZE = GIT_OID_MAX_HEXSIZE + 1 };

int32_t gti_git_init(void);
int32_t gti_git_shutdown(void);
void gti_git_version(int32_t *major, int32_t *minor, int32_t *revision);
const char *gti_git_last_error_message(void);
int32_t gti_git_last_error_class(void);

int32_t gti_git_repository_init(struct GtiGitRepository **output,
                                const char *path, int32_t bare);
int32_t gti_git_repository_open(struct GtiGitRepository **output,
                                const char *path);
void gti_git_repository_free(struct GtiGitRepository *repository);
const char *gti_git_repository_path(const struct GtiGitRepository *repository);
const char *
gti_git_repository_workdir(const struct GtiGitRepository *repository);
int32_t gti_git_repository_is_bare(const struct GtiGitRepository *repository);
int32_t gti_git_repository_is_empty(struct GtiGitRepository *repository);
int32_t gti_git_repository_head_detached(struct GtiGitRepository *repository);
int32_t gti_git_repository_is_shallow(struct GtiGitRepository *repository);
int32_t gti_git_repository_state(struct GtiGitRepository *repository);
int32_t gti_git_status_file(struct GtiGitRepository *repository,
                            const char *path, uint32_t *status);

int32_t gti_git_head_commit(struct GtiGitRepository *repository,
                            struct GtiGitCommit **output);
void gti_git_commit_free(struct GtiGitCommit *commit);
const char *gti_git_commit_summary(struct GtiGitCommit *commit);
const char *gti_git_commit_message(const struct GtiGitCommit *commit);
const char *gti_git_commit_author_name(const struct GtiGitCommit *commit);
const char *gti_git_commit_author_email(const struct GtiGitCommit *commit);
int64_t gti_git_commit_time(const struct GtiGitCommit *commit);
int32_t gti_git_commit_time_offset(const struct GtiGitCommit *commit);
uint32_t gti_git_commit_parent_count(const struct GtiGitCommit *commit);
int32_t gti_git_commit_id_hex(const struct GtiGitCommit *commit,
                              uint8_t *output, uint64_t capacity);

#endif
