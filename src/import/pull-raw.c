/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <curl/curl.h>
#include <linux/fs.h>
#include <sys/xattr.h>

#include "sd-daemon.h"

#include "btrfs-util.h"
#include "chattr-util.h"
#include "copy.h"
#include "curl-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "hostname-util.h"
#include "import-common.h"
#include "import-util.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "pull-common.h"
#include "pull-job.h"
#include "pull-raw.h"
#include "qcow2-util.h"
#include "rm-rf.h"
#include "string-util.h"
#include "strv.h"
#include "utf8.h"
#include "util.h"

typedef enum RawProgress {
        RAW_DOWNLOADING,
        RAW_VERIFYING,
        RAW_UNPACKING,
        RAW_FINALIZING,
        RAW_COPYING,
} RawProgress;

struct RawPull {
        sd_event *event;
        CurlGlue *glue;

        char *image_root;

        PullJob *raw_job;
        PullJob *settings_job;
        PullJob *checksum_job;
        PullJob *signature_job;

        RawPullFinished on_finished;
        void *userdata;

        char *local;
        bool force_local;
        bool grow_machine_directory;
        bool settings;

        char *final_path;
        char *temp_path;

        char *settings_path;
        char *settings_temp_path;

        ImportVerify verify;
};

RawPull* raw_pull_unref(RawPull *i) {
        if (!i)
                return NULL;

        pull_job_unref(i->raw_job);
        pull_job_unref(i->settings_job);
        pull_job_unref(i->checksum_job);
        pull_job_unref(i->signature_job);

        curl_glue_unref(i->glue);
        sd_event_unref(i->event);

        if (i->temp_path) {
                (void) unlink(i->temp_path);
                free(i->temp_path);
        }

        if (i->settings_temp_path) {
                (void) unlink(i->settings_temp_path);
                free(i->settings_temp_path);
        }

        free(i->final_path);
        free(i->settings_path);
        free(i->image_root);
        free(i->local);
        free(i);

        return NULL;
}

int raw_pull_new(
                RawPull **ret,
                sd_event *event,
                const char *image_root,
                RawPullFinished on_finished,
                void *userdata) {

        _cleanup_(raw_pull_unrefp) RawPull *i = NULL;
        int r;

        assert(ret);

        i = new0(RawPull, 1);
        if (!i)
                return -ENOMEM;

        i->on_finished = on_finished;
        i->userdata = userdata;

        i->image_root = strdup(image_root ?: "/var/lib/machines");
        if (!i->image_root)
                return -ENOMEM;

        i->grow_machine_directory = path_startswith(i->image_root, "/var/lib/machines");

        if (event)
                i->event = sd_event_ref(event);
        else {
                r = sd_event_default(&i->event);
                if (r < 0)
                        return r;
        }

        r = curl_glue_new(&i->glue, i->event);
        if (r < 0)
                return r;

        i->glue->on_finished = pull_job_curl_on_finished;
        i->glue->userdata = i;

        *ret = i;
        i = NULL;

        return 0;
}

static void raw_pull_report_progress(RawPull *i, RawProgress p) {
        unsigned percent;

        assert(i);

        switch (p) {

        case RAW_DOWNLOADING: {
                unsigned remain = 80;

                percent = 0;

                if (i->settings_job) {
                        percent += i->settings_job->progress_percent * 5 / 100;
                        remain -= 5;
                }

                if (i->checksum_job) {
                        percent += i->checksum_job->progress_percent * 5 / 100;
                        remain -= 5;
                }

                if (i->signature_job) {
                        percent += i->signature_job->progress_percent * 5 / 100;
                        remain -= 5;
                }

                if (i->raw_job)
                        percent += i->raw_job->progress_percent * remain / 100;
                break;
        }

        case RAW_VERIFYING:
                percent = 80;
                break;

        case RAW_UNPACKING:
                percent = 85;
                break;

        case RAW_FINALIZING:
                percent = 90;
                break;

        case RAW_COPYING:
                percent = 95;
                break;

        default:
                assert_not_reached("Unknown progress state");
        }

        sd_notifyf(false, "X_IMPORT_PROGRESS=%u", percent);
        log_debug("Combined progress %u%%", percent);
}

static int raw_pull_maybe_convert_qcow2(RawPull *i) {
        _cleanup_close_ int converted_fd = -1;
        _cleanup_free_ char *t = NULL;
        int r;

        assert(i);
        assert(i->raw_job);

        r = qcow2_detect(i->raw_job->disk_fd);
        if (r < 0)
                return log_error_errno(r, "Failed to detect whether this is a QCOW2 image: %m");
        if (r == 0)
                return 0;

        /* This is a QCOW2 image, let's convert it */
        r = tempfn_random(i->final_path, NULL, &t);
        if (r < 0)
                return log_oom();

        converted_fd = open(t, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (converted_fd < 0)
                return log_error_errno(errno, "Failed to create %s: %m", t);

        r = chattr_fd(converted_fd, FS_NOCOW_FL, FS_NOCOW_FL);
        if (r < 0)
                log_warning_errno(errno, "Failed to set file attributes on %s: %m", t);

        log_info("Unpacking QCOW2 file.");

        r = qcow2_convert(i->raw_job->disk_fd, converted_fd);
        if (r < 0) {
                unlink(t);
                return log_error_errno(r, "Failed to convert qcow2 image: %m");
        }

        (void) unlink(i->temp_path);
        free(i->temp_path);
        i->temp_path = t;
        t = NULL;

        safe_close(i->raw_job->disk_fd);
        i->raw_job->disk_fd = converted_fd;
        converted_fd = -1;

        return 1;
}

static int raw_pull_make_local_copy(RawPull *i) {
        _cleanup_free_ char *tp = NULL;
        _cleanup_close_ int dfd = -1;
        const char *p;
        int r;

        assert(i);
        assert(i->raw_job);

        if (!i->local)
                return 0;

        if (!i->final_path) {
                r = pull_make_path(i->raw_job->url, i->raw_job->etag, i->image_root, ".raw-", ".raw", &i->final_path);
                if (r < 0)
                        return log_oom();
        }

        if (i->raw_job->etag_exists) {
                /* We have downloaded this one previously, reopen it */

                assert(i->raw_job->disk_fd < 0);

                i->raw_job->disk_fd = open(i->final_path, O_RDONLY|O_NOCTTY|O_CLOEXEC);
                if (i->raw_job->disk_fd < 0)
                        return log_error_errno(errno, "Failed to open vendor image: %m");
        } else {
                /* We freshly downloaded the image, use it */

                assert(i->raw_job->disk_fd >= 0);

                if (lseek(i->raw_job->disk_fd, SEEK_SET, 0) == (off_t) -1)
                        return log_error_errno(errno, "Failed to seek to beginning of vendor image: %m");
        }

        p = strjoina(i->image_root, "/", i->local, ".raw");

        if (i->force_local)
                (void) rm_rf(p, REMOVE_ROOT|REMOVE_PHYSICAL|REMOVE_SUBVOLUME);

        r = tempfn_random(p, NULL, &tp);
        if (r < 0)
                return log_oom();

        dfd = open(tp, O_WRONLY|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (dfd < 0)
                return log_error_errno(errno, "Failed to create writable copy of image: %m");

        /* Turn off COW writing. This should greatly improve
         * performance on COW file systems like btrfs, since it
         * reduces fragmentation caused by not allowing in-place
         * writes. */
        r = chattr_fd(dfd, FS_NOCOW_FL, FS_NOCOW_FL);
        if (r < 0)
                log_warning_errno(errno, "Failed to set file attributes on %s: %m", tp);

        r = copy_bytes(i->raw_job->disk_fd, dfd, (uint64_t) -1, true);
        if (r < 0) {
                unlink(tp);
                return log_error_errno(r, "Failed to make writable copy of image: %m");
        }

        (void) copy_times(i->raw_job->disk_fd, dfd);
        (void) copy_xattr(i->raw_job->disk_fd, dfd);

        dfd = safe_close(dfd);

        r = rename(tp, p);
        if (r < 0)  {
                unlink(tp);
                return log_error_errno(errno, "Failed to move writable image into place: %m");
        }

        log_info("Created new local image '%s'.", i->local);

        if (i->settings) {
                const char *local_settings;
                assert(i->settings_job);

                if (!i->settings_path) {
                        r = pull_make_path(i->settings_job->url, i->settings_job->etag, i->image_root, ".settings-", NULL, &i->settings_path);
                        if (r < 0)
                                return log_oom();
                }

                local_settings = strjoina(i->image_root, "/", i->local, ".nspawn");

                r = copy_file_atomic(i->settings_path, local_settings, 0644, i->force_local, 0);
                if (r == -EEXIST)
                        log_warning_errno(r, "Settings file %s already exists, not replacing.", local_settings);
                else if (r < 0 && r != -ENOENT)
                        log_warning_errno(r, "Failed to copy settings files %s, ignoring: %m", local_settings);
                else
                        log_info("Created new settings file '%s.nspawn'", i->local);
        }

        return 0;
}

static bool raw_pull_is_done(RawPull *i) {
        assert(i);
        assert(i->raw_job);

        if (!PULL_JOB_IS_COMPLETE(i->raw_job))
                return false;
        if (i->settings_job && !PULL_JOB_IS_COMPLETE(i->settings_job))
                return false;
        if (i->checksum_job && !PULL_JOB_IS_COMPLETE(i->checksum_job))
                return false;
        if (i->signature_job && !PULL_JOB_IS_COMPLETE(i->signature_job))
                return false;

        return true;
}

static void raw_pull_job_on_finished(PullJob *j) {
        RawPull *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;
        if (j == i->settings_job) {
                if (j->error != 0)
                        log_info_errno(j->error, "Settings file could not be retrieved, proceeding without.");
        } else if (j->error != 0) {
                if (j == i->checksum_job)
                        log_error_errno(j->error, "Failed to retrieve SHA256 checksum, cannot verify. (Try --verify=no?)");
                else if (j == i->signature_job)
                        log_error_errno(j->error, "Failed to retrieve signature file, cannot verify. (Try --verify=no?)");
                else
                        log_error_errno(j->error, "Failed to retrieve image file. (Wrong URL?)");

                r = j->error;
                goto finish;
        }

        /* This is invoked if either the download completed
         * successfully, or the download was skipped because we
         * already have the etag. In this case ->etag_exists is
         * true.
         *
         * We only do something when we got all three files */

        if (!raw_pull_is_done(i))
                return;

        if (i->settings_job)
                i->settings_job->disk_fd = safe_close(i->settings_job->disk_fd);

        if (!i->raw_job->etag_exists) {
                /* This is a new download, verify it, and move it into place */
                assert(i->raw_job->disk_fd >= 0);

                raw_pull_report_progress(i, RAW_VERIFYING);

                r = pull_verify(i->raw_job, i->settings_job, i->checksum_job, i->signature_job);
                if (r < 0)
                        goto finish;

                raw_pull_report_progress(i, RAW_UNPACKING);

                r = raw_pull_maybe_convert_qcow2(i);
                if (r < 0)
                        goto finish;

                raw_pull_report_progress(i, RAW_FINALIZING);

                r = import_make_read_only_fd(i->raw_job->disk_fd);
                if (r < 0)
                        goto finish;

                r = rename_noreplace(AT_FDCWD, i->temp_path, AT_FDCWD, i->final_path);
                if (r < 0) {
                        log_error_errno(r, "Failed to move RAW file into place: %m");
                        goto finish;
                }

                i->temp_path = mfree(i->temp_path);

                if (i->settings_job &&
                    i->settings_job->error == 0 &&
                    !i->settings_job->etag_exists) {

                        assert(i->settings_temp_path);
                        assert(i->settings_path);

                        r = import_make_read_only(i->settings_temp_path);
                        if (r < 0)
                                goto finish;

                        r = rename_noreplace(AT_FDCWD, i->settings_temp_path, AT_FDCWD, i->settings_path);
                        if (r < 0) {
                                log_error_errno(r, "Failed to rename settings file: %m");
                                goto finish;
                        }

                        i->settings_temp_path = mfree(i->settings_temp_path);
                }
        }

        raw_pull_report_progress(i, RAW_COPYING);

        r = raw_pull_make_local_copy(i);
        if (r < 0)
                goto finish;

        r = 0;

finish:
        if (i->on_finished)
                i->on_finished(i, r, i->userdata);
        else
                sd_event_exit(i->event, r);
}

static int raw_pull_job_on_open_disk_raw(PullJob *j) {
        RawPull *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;
        assert(i->raw_job == j);
        assert(!i->final_path);
        assert(!i->temp_path);

        r = pull_make_path(j->url, j->etag, i->image_root, ".raw-", ".raw", &i->final_path);
        if (r < 0)
                return log_oom();

        r = tempfn_random(i->final_path, NULL, &i->temp_path);
        if (r < 0)
                return log_oom();

        (void) mkdir_parents_label(i->temp_path, 0700);

        j->disk_fd = open(i->temp_path, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (j->disk_fd < 0)
                return log_error_errno(errno, "Failed to create %s: %m", i->temp_path);

        r = chattr_fd(j->disk_fd, FS_NOCOW_FL, FS_NOCOW_FL);
        if (r < 0)
                log_warning_errno(errno, "Failed to set file attributes on %s: %m", i->temp_path);

        return 0;
}

static int raw_pull_job_on_open_disk_settings(PullJob *j) {
        RawPull *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;
        assert(i->settings_job == j);
        assert(!i->settings_path);
        assert(!i->settings_temp_path);

        r = pull_make_path(j->url, j->etag, i->image_root, ".settings-", NULL, &i->settings_path);
        if (r < 0)
                return log_oom();

        r = tempfn_random(i->settings_path, NULL, &i->settings_temp_path);
        if (r < 0)
                return log_oom();

        mkdir_parents_label(i->settings_temp_path, 0700);

        j->disk_fd = open(i->settings_temp_path, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (j->disk_fd < 0)
                return log_error_errno(errno, "Failed to create %s: %m", i->settings_temp_path);

        return 0;
}

static void raw_pull_job_on_progress(PullJob *j) {
        RawPull *i;

        assert(j);
        assert(j->userdata);

        i = j->userdata;

        raw_pull_report_progress(i, RAW_DOWNLOADING);
}

int raw_pull_start(
                RawPull *i,
                const char *url,
                const char *local,
                bool force_local,
                ImportVerify verify,
                bool settings) {

        int r;

        assert(i);
        assert(verify < _IMPORT_VERIFY_MAX);
        assert(verify >= 0);

        if (!http_url_is_valid(url))
                return -EINVAL;

        if (local && !machine_name_is_valid(local))
                return -EINVAL;

        if (i->raw_job)
                return -EBUSY;

        r = free_and_strdup(&i->local, local);
        if (r < 0)
                return r;

        i->force_local = force_local;
        i->verify = verify;
        i->settings = settings;

        /* Queue job for the image itself */
        r = pull_job_new(&i->raw_job, url, i->glue, i);
        if (r < 0)
                return r;

        i->raw_job->on_finished = raw_pull_job_on_finished;
        i->raw_job->on_open_disk = raw_pull_job_on_open_disk_raw;
        i->raw_job->on_progress = raw_pull_job_on_progress;
        i->raw_job->calc_checksum = verify != IMPORT_VERIFY_NO;
        i->raw_job->grow_machine_directory = i->grow_machine_directory;

        r = pull_find_old_etags(url, i->image_root, DT_REG, ".raw-", ".raw", &i->raw_job->old_etags);
        if (r < 0)
                return r;

        if (settings) {
                r = pull_make_settings_job(&i->settings_job, url, i->glue, raw_pull_job_on_finished, i);
                if (r < 0)
                        return r;

                i->settings_job->on_open_disk = raw_pull_job_on_open_disk_settings;
                i->settings_job->on_progress = raw_pull_job_on_progress;
                i->settings_job->calc_checksum = verify != IMPORT_VERIFY_NO;

                r = pull_find_old_etags(i->settings_job->url, i->image_root, DT_REG, ".settings-", NULL, &i->settings_job->old_etags);
                if (r < 0)
                        return r;
        }

        r = pull_make_verification_jobs(&i->checksum_job, &i->signature_job, verify, url, i->glue, raw_pull_job_on_finished, i);
        if (r < 0)
                return r;

        r = pull_job_begin(i->raw_job);
        if (r < 0)
                return r;

        if (i->settings_job) {
                r = pull_job_begin(i->settings_job);
                if (r < 0)
                        return r;
        }

        if (i->checksum_job) {
                i->checksum_job->on_progress = raw_pull_job_on_progress;

                r = pull_job_begin(i->checksum_job);
                if (r < 0)
                        return r;
        }

        if (i->signature_job) {
                i->signature_job->on_progress = raw_pull_job_on_progress;

                r = pull_job_begin(i->signature_job);
                if (r < 0)
                        return r;
        }

        return 0;
}
