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

#include <sys/xattr.h>
#include <linux/fs.h>
#include <curl/curl.h>
#include <lzma.h>

#include "hashmap.h"
#include "utf8.h"
#include "curl-util.h"
#include "qcow2-util.h"
#include "strv.h"
#include "copy.h"
#include "import-util.h"
#include "import-raw.h"

typedef struct RawImportFile RawImportFile;

struct RawImportFile {
        RawImport *import;

        char *url;
        char *local;

        CURL *curl;
        struct curl_slist *request_header;

        char *temp_path;
        char *final_path;
        char *etag;
        char **old_etags;

        uint64_t content_length;
        uint64_t written_compressed;
        uint64_t written_uncompressed;

        void *payload;
        size_t payload_size;

        usec_t mtime;

        bool force_local;
        bool done;

        int disk_fd;

        lzma_stream lzma;
        bool compressed;

        unsigned progress_percent;
        usec_t start_usec;
        usec_t last_status_usec;
};

struct RawImport {
        sd_event *event;
        CurlGlue *glue;

        char *image_root;
        Hashmap *files;

        raw_import_on_finished on_finished;
        void *userdata;

        bool finished;
};

#define FILENAME_ESCAPE "/.#\"\'"

#define RAW_MAX_SIZE (1024LLU*1024LLU*1024LLU*8) /* 8 GB */

static RawImportFile *raw_import_file_unref(RawImportFile *f) {
        if (!f)
                return NULL;

        if (f->import)
                curl_glue_remove_and_free(f->import->glue, f->curl);
        curl_slist_free_all(f->request_header);

        safe_close(f->disk_fd);

        free(f->final_path);

        if (f->temp_path) {
                unlink(f->temp_path);
                free(f->temp_path);
        }

        lzma_end(&f->lzma);
        free(f->url);
        free(f->local);
        free(f->etag);
        strv_free(f->old_etags);
        free(f->payload);
        free(f);

        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(RawImportFile*, raw_import_file_unref);

static void raw_import_finish(RawImport *import, int error) {
        assert(import);

        if (import->finished)
                return;

        import->finished = true;

        if (import->on_finished)
                import->on_finished(import, error, import->userdata);
        else
                sd_event_exit(import->event, error);
}

static int raw_import_file_make_final_path(RawImportFile *f) {
        _cleanup_free_ char *escaped_url = NULL, *escaped_etag = NULL;

        assert(f);

        if (f->final_path)
                return 0;

        escaped_url = xescape(f->url, FILENAME_ESCAPE);
        if (!escaped_url)
                return -ENOMEM;

        if (f->etag) {
                escaped_etag = xescape(f->etag, FILENAME_ESCAPE);
                if (!escaped_etag)
                        return -ENOMEM;

                f->final_path = strjoin(f->import->image_root, "/.raw-", escaped_url, ".", escaped_etag, ".raw", NULL);
        } else
                f->final_path = strjoin(f->import->image_root, "/.raw-", escaped_url, ".raw", NULL);
        if (!f->final_path)
                return -ENOMEM;

        return 0;
}

static int raw_import_file_make_local_copy(RawImportFile *f) {
        _cleanup_free_ char *tp = NULL;
        _cleanup_close_ int dfd = -1;
        const char *p;
        int r;

        assert(f);

        if (!f->local)
                return 0;

        if (f->disk_fd >= 0) {
                if (lseek(f->disk_fd, SEEK_SET, 0) == (off_t) -1)
                        return log_error_errno(errno, "Failed to seek to beginning of vendor image: %m");
        } else {
                r = raw_import_file_make_final_path(f);
                if (r < 0)
                        return log_oom();

                f->disk_fd = open(f->final_path, O_RDONLY|O_NOCTTY|O_CLOEXEC);
                if (f->disk_fd < 0)
                        return log_error_errno(errno, "Failed to open vendor image: %m");
        }

        p = strappenda(f->import->image_root, "/", f->local, ".raw");
        if (f->force_local)
                (void) rm_rf_dangerous(p, false, true, false);

        r = tempfn_random(p, &tp);
        if (r < 0)
                return log_oom();

        dfd = open(tp, O_WRONLY|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0664);
        if (dfd < 0)
                return log_error_errno(errno, "Failed to create writable copy of image: %m");

        /* Turn off COW writing. This should greatly improve
         * performance on COW file systems like btrfs, since it
         * reduces fragmentation caused by not allowing in-place
         * writes. */
        r = chattr_fd(dfd, true, FS_NOCOW_FL);
        if (r < 0)
                log_warning_errno(errno, "Failed to set file attributes on %s: %m", tp);

        r = copy_bytes(f->disk_fd, dfd, (off_t) -1, true);
        if (r < 0) {
                unlink(tp);
                return log_error_errno(r, "Failed to make writable copy of image: %m");
        }

        (void) copy_times(f->disk_fd, dfd);
        (void) copy_xattr(f->disk_fd, dfd);

        dfd = safe_close(dfd);

        r = rename(tp, p);
        if (r < 0)  {
                unlink(tp);
                return log_error_errno(errno, "Failed to move writable image into place: %m");
        }

        log_info("Created new local image %s.", p);
        return 0;
}

static void raw_import_file_success(RawImportFile *f) {
        int r;

        assert(f);

        f->done = true;

        r = raw_import_file_make_local_copy(f);
        if (r < 0)
                goto finish;

        f->disk_fd = safe_close(f->disk_fd);
        r = 0;

finish:
        raw_import_finish(f->import, r);
}

static int raw_import_maybe_convert_qcow2(RawImportFile *f) {
        _cleanup_close_ int converted_fd = -1;
        _cleanup_free_ char *t = NULL;
        int r;

        assert(f);
        assert(f->disk_fd);
        assert(f->temp_path);

        r = qcow2_detect(f->disk_fd);
        if (r < 0)
                return log_error_errno(r, "Failed to detect whether this is a QCOW2 image: %m");
        if (r == 0)
                return 0;

        /* This is a QCOW2 image, let's convert it */
        r = tempfn_random(f->final_path, &t);
        if (r < 0)
                return log_oom();

        converted_fd = open(t, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0644);
        if (converted_fd < 0)
                return log_error_errno(errno, "Failed to create %s: %m", t);

        log_info("Unpacking QCOW2 file.");

        r = qcow2_convert(f->disk_fd, converted_fd);
        if (r < 0) {
                unlink(t);
                return log_error_errno(r, "Failed to convert qcow2 image: %m");
        }

        unlink(f->temp_path);
        free(f->temp_path);

        f->temp_path = t;
        t = NULL;

        safe_close(f->disk_fd);
        f->disk_fd = converted_fd;
        converted_fd = -1;

        return 1;
}

static void raw_import_curl_on_finished(CurlGlue *g, CURL *curl, CURLcode result) {
        RawImportFile *f = NULL;
        struct stat st;
        CURLcode code;
        long status;
        int r;

        if (curl_easy_getinfo(curl, CURLINFO_PRIVATE, &f) != CURLE_OK)
                return;

        if (!f || f->done)
                return;

        f->done = true;

        if (result != CURLE_OK) {
                log_error("Transfer failed: %s", curl_easy_strerror(result));
                r = -EIO;
                goto fail;
        }

        code = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (code != CURLE_OK) {
                log_error("Failed to retrieve response code: %s", curl_easy_strerror(code));
                r = -EIO;
                goto fail;
        } else if (status == 304) {
                log_info("Image already downloaded. Skipping download.");
                raw_import_file_success(f);
                return;
        } else if (status >= 300) {
                log_error("HTTP request to %s failed with code %li.", f->url, status);
                r = -EIO;
                goto fail;
        } else if (status < 200) {
                log_error("HTTP request to %s finished with unexpected code %li.", f->url, status);
                r = -EIO;
                goto fail;
        }

        if (f->disk_fd < 0) {
                log_error("No data received.");
                r = -EIO;
                goto fail;
        }

        if (f->content_length != (uint64_t) -1 &&
            f->content_length != f->written_compressed) {
                log_error("Download truncated.");
                r = -EIO;
                goto fail;
        }

        /* Make sure the file size is right, in case the file was
         * sparse and we just seeked for the last part */
        if (ftruncate(f->disk_fd, f->written_uncompressed) < 0) {
                log_error_errno(errno, "Failed to truncate file: %m");
                r = -errno;
                goto fail;
        }

        r = raw_import_maybe_convert_qcow2(f);
        if (r < 0)
                goto fail;

        if (f->etag)
                (void) fsetxattr(f->disk_fd, "user.source_etag", f->etag, strlen(f->etag), 0);
        if (f->url)
                (void) fsetxattr(f->disk_fd, "user.source_url", f->url, strlen(f->url), 0);

        if (f->mtime != 0) {
                struct timespec ut[2];

                timespec_store(&ut[0], f->mtime);
                ut[1] = ut[0];
                (void) futimens(f->disk_fd, ut);

                fd_setcrtime(f->disk_fd, f->mtime);
        }

        if (fstat(f->disk_fd, &st) < 0) {
                r = log_error_errno(errno, "Failed to stat file: %m");
                goto fail;
        }

        /* Mark read-only */
        (void) fchmod(f->disk_fd, st.st_mode & 07444);

        assert(f->temp_path);
        assert(f->final_path);

        r = rename(f->temp_path, f->final_path);
        if (r < 0) {
                r = log_error_errno(errno, "Failed to move RAW file into place: %m");
                goto fail;
        }

        free(f->temp_path);
        f->temp_path = NULL;

        log_info("Completed writing vendor image %s.", f->final_path);

        raw_import_file_success(f);
        return;

fail:
        raw_import_finish(f->import, r);
}

static int raw_import_file_open_disk_for_write(RawImportFile *f) {
        int r;

        assert(f);

        if (f->disk_fd >= 0)
                return 0;

        r = raw_import_file_make_final_path(f);
        if (r < 0)
                return log_oom();

        if (!f->temp_path) {
                r = tempfn_random(f->final_path, &f->temp_path);
                if (r < 0)
                        return log_oom();
        }

        f->disk_fd = open(f->temp_path, O_RDWR|O_CREAT|O_EXCL|O_NOCTTY|O_CLOEXEC, 0644);
        if (f->disk_fd < 0)
                return log_error_errno(errno, "Failed to create %s: %m", f->temp_path);

        r = chattr_fd(f->disk_fd, true, FS_NOCOW_FL);
        if (r < 0)
                log_warning_errno(errno, "Failed to set file attributes on %s: %m", f->temp_path);

        return 0;
}

static int raw_import_file_write_uncompressed(RawImportFile *f, void *p, size_t sz) {
        ssize_t n;

        assert(f);
        assert(p);
        assert(sz > 0);
        assert(f->disk_fd >= 0);

        if (f->written_uncompressed + sz < f->written_uncompressed) {
                log_error("File too large, overflow");
                return -EOVERFLOW;
        }

        if (f->written_uncompressed + sz > RAW_MAX_SIZE) {
                log_error("File overly large, refusing");
                return -EFBIG;
        }

        n = sparse_write(f->disk_fd, p, sz, 64);
        if (n < 0) {
                log_error_errno(errno, "Failed to write file: %m");
                return -errno;
        }
        if ((size_t) n < sz) {
                log_error("Short write");
                return -EIO;
        }

        f->written_uncompressed += sz;

        return 0;
}

static int raw_import_file_write_compressed(RawImportFile *f, void *p, size_t sz) {
        int r;

        assert(f);
        assert(p);
        assert(sz > 0);
        assert(f->disk_fd >= 0);

        if (f->written_compressed + sz < f->written_compressed) {
                log_error("File too large, overflow");
                return -EOVERFLOW;
        }

        if (f->content_length != (uint64_t) -1 &&
            f->written_compressed + sz > f->content_length) {
                log_error("Content length incorrect.");
                return -EFBIG;
        }

        if (!f->compressed) {
                r = raw_import_file_write_uncompressed(f, p, sz);
                if (r < 0)
                        return r;
        } else {
                f->lzma.next_in = p;
                f->lzma.avail_in = sz;

                while (f->lzma.avail_in > 0) {
                        uint8_t buffer[16 * 1024];
                        lzma_ret lzr;

                        f->lzma.next_out = buffer;
                        f->lzma.avail_out = sizeof(buffer);

                        lzr = lzma_code(&f->lzma, LZMA_RUN);
                        if (lzr != LZMA_OK && lzr != LZMA_STREAM_END) {
                                log_error("Decompression error.");
                                return -EIO;
                        }

                        r = raw_import_file_write_uncompressed(f, buffer, sizeof(buffer) - f->lzma.avail_out);
                        if (r < 0)
                                return r;
                }
        }

        f->written_compressed += sz;

        return 0;
}

static int raw_import_file_detect_xz(RawImportFile *f) {
        static const uint8_t xz_signature[] = {
                '\xfd', '7', 'z', 'X', 'Z', '\x00'
        };
        lzma_ret lzr;
        int r;

        assert(f);

        if (f->payload_size < sizeof(xz_signature))
                return 0;

        f->compressed = memcmp(f->payload, xz_signature, sizeof(xz_signature)) == 0;
        log_debug("Stream is XZ compressed: %s", yes_no(f->compressed));

        if (f->compressed) {
                lzr = lzma_stream_decoder(&f->lzma, UINT64_MAX, LZMA_TELL_UNSUPPORTED_CHECK);
                if (lzr != LZMA_OK) {
                        log_error("Failed to initialize LZMA decoder.");
                        return -EIO;
                }
        }

        r = raw_import_file_open_disk_for_write(f);
        if (r < 0)
                return r;

        r = raw_import_file_write_compressed(f, f->payload, f->payload_size);
        if (r < 0)
                return r;

        free(f->payload);
        f->payload = NULL;
        f->payload_size = 0;

        return 0;
}

static size_t raw_import_file_write_callback(void *contents, size_t size, size_t nmemb, void *userdata) {
        RawImportFile *f = userdata;
        size_t sz = size * nmemb;
        int r;

        assert(contents);
        assert(f);

        if (f->done) {
                r = -ESTALE;
                goto fail;
        }

        if (f->disk_fd < 0) {
                uint8_t *p;

                /* We haven't opened the file yet, let's first check what it actually is */

                p = realloc(f->payload, f->payload_size + sz);
                if (!p) {
                        r = log_oom();
                        goto fail;
                }

                memcpy(p + f->payload_size, contents, sz);
                f->payload_size = sz;
                f->payload = p;

                r = raw_import_file_detect_xz(f);
                if (r < 0)
                        goto fail;

                return sz;
        }

        r = raw_import_file_write_compressed(f, contents, sz);
        if (r < 0)
                goto fail;

        return sz;

fail:
        raw_import_finish(f->import, r);
        return 0;
}

static size_t raw_import_file_header_callback(void *contents, size_t size, size_t nmemb, void *userdata) {
        RawImportFile *f = userdata;
        size_t sz = size * nmemb;
        _cleanup_free_ char *length = NULL, *last_modified = NULL;
        char *etag;
        int r;

        assert(contents);
        assert(f);

        if (f->done) {
                r = -ESTALE;
                goto fail;
        }

        r = curl_header_strdup(contents, sz, "ETag:", &etag);
        if (r < 0) {
                log_oom();
                goto fail;
        }
        if (r > 0) {
                free(f->etag);
                f->etag = etag;

                if (strv_contains(f->old_etags, f->etag)) {
                        log_info("Image already downloaded. Skipping download.");
                        raw_import_file_success(f);
                        return sz;
                }

                return sz;
        }

        r = curl_header_strdup(contents, sz, "Content-Length:", &length);
        if (r < 0) {
                log_oom();
                goto fail;
        }
        if (r > 0) {
                (void) safe_atou64(length, &f->content_length);

                if (f->content_length != (uint64_t) -1) {
                        char bytes[FORMAT_BYTES_MAX];
                        log_info("Downloading %s.", format_bytes(bytes, sizeof(bytes), f->content_length));
                }

                return sz;
        }

        r = curl_header_strdup(contents, sz, "Last-Modified:", &last_modified);
        if (r < 0) {
                log_oom();
                goto fail;
        }
        if (r > 0) {
                (void) curl_parse_http_time(last_modified, &f->mtime);
                return sz;
        }

        return sz;

fail:
        raw_import_finish(f->import, r);
        return 0;
}

static int raw_import_file_progress_callback(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
        RawImportFile *f = userdata;
        unsigned percent;
        usec_t n;

        assert(f);

        if (dltotal <= 0)
                return 0;

        percent = ((100 * dlnow) / dltotal);
        n = now(CLOCK_MONOTONIC);

        if (n > f->last_status_usec + USEC_PER_SEC &&
            percent != f->progress_percent) {
                char buf[FORMAT_TIMESPAN_MAX];

                if (n - f->start_usec > USEC_PER_SEC && dlnow > 0) {
                        usec_t left, done;

                        done = n - f->start_usec;
                        left = (usec_t) (((double) done * (double) dltotal) / dlnow) - done;

                        log_info("Got %u%%. %s left.", percent, format_timespan(buf, sizeof(buf), left, USEC_PER_SEC));
                } else
                        log_info("Got %u%%.", percent);

                f->progress_percent = percent;
                f->last_status_usec = n;
        }

        return 0;
}

static int raw_import_file_find_old_etags(RawImportFile *f) {
        _cleanup_free_ char *escaped_url = NULL;
        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        int r;

        escaped_url = xescape(f->url, FILENAME_ESCAPE);
        if (!escaped_url)
                return -ENOMEM;

        d = opendir(f->import->image_root);
        if (!d) {
                if (errno == ENOENT)
                        return 0;

                return -errno;
        }

        FOREACH_DIRENT_ALL(de, d, return -errno) {
                const char *a, *b;
                char *u;

                if (de->d_type != DT_UNKNOWN &&
                    de->d_type != DT_REG)
                        continue;

                a = startswith(de->d_name, ".raw-");
                if (!a)
                        continue;

                a = startswith(a, escaped_url);
                if (!a)
                        continue;

                a = startswith(a, ".");
                if (!a)
                        continue;

                b = endswith(de->d_name, ".raw");
                if (!b)
                        continue;

                if (a >= b)
                        continue;

                u = cunescape_length(a, b - a);
                if (!u)
                        return -ENOMEM;

                if (!http_etag_is_valid(u)) {
                        free(u);
                        continue;
                }

                r = strv_consume(&f->old_etags, u);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int raw_import_file_begin(RawImportFile *f) {
        int r;

        assert(f);
        assert(!f->curl);

        log_info("Getting %s.", f->url);

        r = raw_import_file_find_old_etags(f);
        if (r < 0)
                return r;

        r = curl_glue_make(&f->curl, f->url, f);
        if (r < 0)
                return r;

        if (!strv_isempty(f->old_etags)) {
                _cleanup_free_ char *cc = NULL, *hdr = NULL;

                cc = strv_join(f->old_etags, ", ");
                if (!cc)
                        return -ENOMEM;

                hdr = strappend("If-None-Match: ", cc);
                if (!hdr)
                        return -ENOMEM;

                f->request_header = curl_slist_new(hdr, NULL);
                if (!f->request_header)
                        return -ENOMEM;

                if (curl_easy_setopt(f->curl, CURLOPT_HTTPHEADER, f->request_header) != CURLE_OK)
                        return -EIO;
        }

        if (curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, raw_import_file_write_callback) != CURLE_OK)
                return -EIO;

        if (curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, f) != CURLE_OK)
                return -EIO;

        if (curl_easy_setopt(f->curl, CURLOPT_HEADERFUNCTION, raw_import_file_header_callback) != CURLE_OK)
                return -EIO;

        if (curl_easy_setopt(f->curl, CURLOPT_HEADERDATA, f) != CURLE_OK)
                return -EIO;

        if (curl_easy_setopt(f->curl, CURLOPT_XFERINFOFUNCTION, raw_import_file_progress_callback) != CURLE_OK)
                return -EIO;

        if (curl_easy_setopt(f->curl, CURLOPT_XFERINFODATA, f) != CURLE_OK)
                return -EIO;

        if (curl_easy_setopt(f->curl, CURLOPT_NOPROGRESS, 0) != CURLE_OK)
                return -EIO;

        r = curl_glue_add(f->import->glue, f->curl);
        if (r < 0)
                return r;

        return 0;
}

int raw_import_new(RawImport **import, sd_event *event, const char *image_root, raw_import_on_finished on_finished, void *userdata) {
        _cleanup_(raw_import_unrefp) RawImport *i = NULL;
        int r;

        assert(import);
        assert(image_root);

        i = new0(RawImport, 1);
        if (!i)
                return -ENOMEM;

        i->on_finished = on_finished;
        i->userdata = userdata;

        i->image_root = strdup(image_root);
        if (!i->image_root)
                return -ENOMEM;

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

        i->glue->on_finished = raw_import_curl_on_finished;
        i->glue->userdata = i;

        *import = i;
        i = NULL;

        return 0;
}

RawImport* raw_import_unref(RawImport *import) {
        RawImportFile *f;

        if (!import)
                return NULL;

        while ((f = hashmap_steal_first(import->files)))
                raw_import_file_unref(f);
        hashmap_free(import->files);

        curl_glue_unref(import->glue);
        sd_event_unref(import->event);

        free(import->image_root);
        free(import);

        return NULL;
}

int raw_import_cancel(RawImport *import, const char *url) {
        RawImportFile *f;

        assert(import);
        assert(url);

        f = hashmap_remove(import->files, url);
        if (!f)
                return 0;

        raw_import_file_unref(f);
        return 1;
}

int raw_import_pull(RawImport *import, const char *url, const char *local, bool force_local) {
        _cleanup_(raw_import_file_unrefp) RawImportFile *f = NULL;
        int r;

        assert(import);
        assert(http_url_is_valid(url));
        assert(!local || machine_name_is_valid(local));

        if (hashmap_get(import->files, url))
                return -EEXIST;

        r = hashmap_ensure_allocated(&import->files, &string_hash_ops);
        if (r < 0)
                return r;

        f = new0(RawImportFile, 1);
        if (!f)
                return -ENOMEM;

        f->import = import;
        f->disk_fd = -1;
        f->content_length = (uint64_t) -1;
        f->start_usec = now(CLOCK_MONOTONIC);

        f->url = strdup(url);
        if (!f->url)
                return -ENOMEM;

        if (local) {
                f->local = strdup(local);
                if (!f->local)
                        return -ENOMEM;

                f->force_local = force_local;
        }

        r = hashmap_put(import->files, f->url, f);
        if (r < 0)
                return r;

        r = raw_import_file_begin(f);
        if (r < 0) {
                raw_import_cancel(import, f->url);
                f = NULL;
                return r;
        }

        f = NULL;
        return 0;
}
