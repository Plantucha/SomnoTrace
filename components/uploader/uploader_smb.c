/*
 * SomnoTrace - SMB upload backend using libsmb2
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "uploader.h"
#include "uploader_state.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include <sys/time.h>
#include <sys/poll.h>

/* SD card paths — must match sd_storage.h */
#define SD_MOUNT_POINT      "/somnotrace"
#define SD_SDCARD_DIR       SD_MOUNT_POINT "/SDCARD"
#define SD_SDCARD_DATALOG   SD_SDCARD_DIR "/DATALOG"
#define SD_SDCARD_SETTINGS  SD_SDCARD_DIR "/SETTINGS"

static const char *TAG = "upload_smb";

/* Write buffer size — allocated in PSRAM */
#define SMB_BUF_SIZE  (32 * 1024)

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Sync callback for smb2_cmd_set_info_async */
struct set_info_sync {
    int is_finished;
    int status;
};

static void set_info_cb(struct smb2_context *smb2, int status,
                        void *command_data, void *private_data)
{
    struct set_info_sync *sync = private_data;
    sync->is_finished = 1;
    sync->status = status;
}

/* Set the last-write and change timestamps on an open SMB file handle
 * to match the local file's mtime.  Uses SMB2 SET_INFO with
 * FILE_BASIC_INFORMATION. */
static int smb_set_mtime(struct smb2_context *smb2, struct smb2fh *fh,
                         time_t mtime)
{
    struct smb2_file_basic_info bi;
    struct smb2_set_info_request si_req;
    struct set_info_sync sync = {0, 0};
    struct smb2_pdu *pdu;

    memset(&bi, 0, sizeof(bi));
    /* Setting times to 0 means "don't change" in SMB2 */
    bi.creation_time.tv_sec = 0;
    bi.creation_time.tv_usec = 0;
    bi.last_access_time.tv_sec = 0;
    bi.last_access_time.tv_usec = 0;
    bi.last_write_time.tv_sec = mtime;
    bi.last_write_time.tv_usec = 0;
    bi.change_time.tv_sec = mtime;
    bi.change_time.tv_usec = 0;
    bi.file_attributes = 0;  /* 0 = don't change */

    memset(&si_req, 0, sizeof(si_req));
    si_req.info_type = SMB2_0_INFO_FILE;
    si_req.file_info_class = SMB2_FILE_BASIC_INFORMATION;
    si_req.additional_information = 0;
    smb2_file_id *fid = smb2_get_file_id(fh);
    memcpy(si_req.file_id, fid, SMB2_FD_SIZE);
    si_req.input_data = &bi;

    pdu = smb2_cmd_set_info_async(smb2, &si_req, set_info_cb, &sync);
    if (!pdu) {
        ESP_LOGW(TAG, "  smb2_cmd_set_info_async failed: %s",
                 smb2_get_error(smb2));
        return -1;
    }
    smb2_queue_pdu(smb2, pdu);

    /* Poll loop — same pattern as libsmb2's sync.c wait_for_reply() */
    while (!sync.is_finished) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = smb2_get_fd(smb2);
        pfd.events = smb2_which_events(smb2);
        if (poll(&pfd, 1, 1000) < 0) {
            ESP_LOGW(TAG, "  set_info poll failed");
            return -1;
        }
        if (pfd.revents == 0) continue;
        if (smb2_service(smb2, pfd.revents) < 0) {
            ESP_LOGW(TAG, "  set_info smb2_service failed: %s",
                     smb2_get_error(smb2));
            return -1;
        }
    }

    return sync.status;
}

/* Upload a single local file to an SMB path. */
static upload_result_t smb_upload_file(struct smb2_context *smb2,
                                        const char *local_path,
                                        const char *remote_path)
{
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "  cannot open %s: %s", local_path, strerror(errno));
        return UPLOAD_FAILED;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Open remote file for writing */
    struct smb2fh *fh = smb2_open(smb2, remote_path, O_WRONLY | O_CREAT);
    if (!fh) {
        ESP_LOGW(TAG, "  smb2_open(%s) failed: %s", remote_path,
                 smb2_get_error(smb2));
        /* Try creating parent directory and retry */
        char dir_path[512];
        strlcpy(dir_path, remote_path, sizeof(dir_path));
        char *slash = strrchr(dir_path, '/');
        if (slash) {
            *slash = '\0';
            ESP_LOGI(TAG, "  trying mkdir %s", dir_path);
            smb2_mkdir(smb2, dir_path);
            *slash = '/';
        }
        fh = smb2_open(smb2, remote_path, O_WRONLY | O_CREAT);
        if (!fh) {
            ESP_LOGE(TAG, "  smb2_open retry failed: %s", smb2_get_error(smb2));
            fclose(f);
            return UPLOAD_FAILED;
        }
    }

    /* Allocate read buffer in PSRAM */
    uint8_t *buf = heap_caps_malloc(SMB_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = malloc(SMB_BUF_SIZE);
    }
    if (!buf) {
        ESP_LOGE(TAG, "  buffer alloc failed");
        smb2_close(smb2, fh);
        fclose(f);
        return UPLOAD_FAILED;
    }

    size_t total_written = 0;
    int last_pct = -1;

    while (total_written < file_size) {
        size_t to_read = SMB_BUF_SIZE;
        if (to_read > file_size - total_written)
            to_read = file_size - total_written;

        size_t rd = fread(buf, 1, to_read, f);
        if (rd == 0) break;

        int wr = smb2_pwrite(smb2, fh, buf, rd, total_written);
        if (wr < 0) {
            ESP_LOGE(TAG, "  smb2_pwrite failed at offset %u: %s",
                     (unsigned)total_written, smb2_get_error(smb2));
            free(buf);
            smb2_close(smb2, fh);
            fclose(f);
            return UPLOAD_FAILED;
        }

        total_written += wr;

        int pct = (int)(total_written * 100 / file_size);
        if (pct != last_pct && pct % 25 == 0) {
            ESP_LOGI(TAG, "  %s: %d%% (%u/%u)",
                     strrchr(local_path, '/') ? strrchr(local_path, '/') + 1 : local_path,
                     pct, (unsigned)total_written, (unsigned)file_size);
            last_pct = pct;
        }
    }

    free(buf);

    /* Set remote file timestamps to match local file mtime */
    struct stat local_st;
    if (stat(local_path, &local_st) == 0) {
        int rc = smb_set_mtime(smb2, fh, local_st.st_mtime);
        if (rc != 0) {
            ESP_LOGW(TAG, "  smb_set_mtime failed: %d (continuing)", rc);
        }
    }

    smb2_close(smb2, fh);
    fclose(f);

    ESP_LOGI(TAG, "  uploaded %s (%u bytes)", remote_path, (unsigned)total_written);
    return UPLOAD_OK;
}

/* Upload all .edf files from a DATALOG day folder. */
static int upload_day_folder(struct smb2_context *smb2,
                              const char *local_day_dir,
                              const char *remote_day_dir)
{
    DIR *d = opendir(local_day_dir);
    if (!d) {
        ESP_LOGW(TAG, "  cannot open %s", local_day_dir);
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Upload all .edf files in the day folder */
        if (strstr(ent->d_name, ".edf") == NULL) continue;

        char local_path[512];
        snprintf(local_path, sizeof(local_path), "%s/%s", local_day_dir, ent->d_name);

        char remote_path[512];
        snprintf(remote_path, sizeof(remote_path), "%s/%s", remote_day_dir, ent->d_name);

        if (smb_upload_file(smb2, local_path, remote_path) == UPLOAD_OK) {
            count++;
        } else {
            ESP_LOGW(TAG, "  failed to upload %s", ent->d_name);
        }
    }
    closedir(d);
    return count;
}

/* Upload mandatory root files (STR.edf, Identification.json, etc.). */
static int upload_mandatory_files(struct smb2_context *smb2,
                                   const char *remote_base)
{
    int count = 0;

    /* Root files */
    const char *root_files[] = {
        "/STR.edf",
        "/Identification.json",
        "/Identification.crc",
        NULL
    };

    for (int i = 0; root_files[i]; i++) {
        char local_path[300];
        snprintf(local_path, sizeof(local_path), "%s%s", SD_SDCARD_DIR, root_files[i]);

        struct stat st;
        if (stat(local_path, &st) != 0) continue;

        char remote_path[512];
        snprintf(remote_path, sizeof(remote_path), "%s%s", remote_base, root_files[i]);

        if (smb_upload_file(smb2, local_path, remote_path) == UPLOAD_OK) {
            count++;
        }
    }

    /* Settings directory */
    DIR *d = opendir(SD_SDCARD_SETTINGS);
    if (d) {
        char remote_settings[400];
        snprintf(remote_settings, sizeof(remote_settings), "%s/SETTINGS", remote_base);
        smb2_mkdir(smb2, remote_settings);

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            char local_path[400];
            snprintf(local_path, sizeof(local_path), "%s/%s", SD_SDCARD_SETTINGS, ent->d_name);

            char remote_path[1024];
            snprintf(remote_path, sizeof(remote_path), "%s/%s", remote_settings, ent->d_name);

            if (smb_upload_file(smb2, local_path, remote_path) == UPLOAD_OK) {
                count++;
            }
        }
        closedir(d);
    }

    return count;
}

/* ── Backend interface ──────────────────────────────────────────────── */

static bool smb_is_configured(void)
{
    return uploader_is_smb_configured();
}

static upload_result_t smb_upload_day(const char *day_folder)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);

    if (!cfg.smb_host[0] || !cfg.smb_share[0]) {
        return UPLOAD_NOT_CONFIGURED;
    }

    ESP_LOGI(TAG, "SMB upload: day %s -> %s/%s%s", day_folder,
             cfg.smb_host, cfg.smb_share, cfg.smb_path);

    /* Init SMB context */
    struct smb2_context *smb2 = smb2_init_context();
    if (!smb2) {
        ESP_LOGE(TAG, "smb2_init_context failed");
        return UPLOAD_FAILED;
    }

    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

    /* Set credentials */
    if (cfg.smb_user[0]) {
        smb2_set_user(smb2, cfg.smb_user);
    } else {
        smb2_set_user(smb2, "Guest");
    }
    if (cfg.smb_pass[0]) {
        smb2_set_password(smb2, cfg.smb_pass);
    }

    /* Connect to share */
    ESP_LOGI(TAG, "connecting to %s/%s as %s", cfg.smb_host, cfg.smb_share,
             cfg.smb_user[0] ? cfg.smb_user : "Guest");

    if (smb2_connect_share(smb2, cfg.smb_host, cfg.smb_share,
                           cfg.smb_user[0] ? cfg.smb_user : "Guest") != 0) {
        ESP_LOGE(TAG, "smb2_connect_share failed: %s", smb2_get_error(smb2));
        smb2_destroy_context(smb2);
        return UPLOAD_FAILED;
    }
    ESP_LOGI(TAG, "SMB connected");

    /* Build remote base path — SMB paths are relative to the share root,
     * so must NOT start with '/' or '\\'.  Windows returns
     * STATUS_INVALID_PARAMETER for absolute paths. */
    char remote_base[256];
    const char *p = cfg.smb_path;
    while (*p == '/' || *p == '\\') p++;
    snprintf(remote_base, sizeof(remote_base), "%s", p);

    /* Create remote directories */
    char remote_datalog[400];
    snprintf(remote_datalog, sizeof(remote_datalog), "%s/DATALOG", remote_base);
    smb2_mkdir(smb2, remote_datalog);

    char remote_day_dir[512];
    snprintf(remote_day_dir, sizeof(remote_day_dir), "%s/%s", remote_datalog, day_folder);
    smb2_mkdir(smb2, remote_day_dir);

    /* Build local day directory path */
    char local_day_dir[256];
    snprintf(local_day_dir, sizeof(local_day_dir), "%s/%s", SD_SDCARD_DATALOG, day_folder);

    /* Upload all EDF files in the day folder */
    int edf_count = upload_day_folder(smb2, local_day_dir, remote_day_dir);
    if (edf_count < 0) {
        ESP_LOGE(TAG, "failed to read day folder %s", local_day_dir);
        smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "uploaded %d EDF files for day %s", edf_count, day_folder);

    /* Upload mandatory root files */
    int mand_count = upload_mandatory_files(smb2, remote_base);
    ESP_LOGI(TAG, "uploaded %d mandatory files", mand_count);

    /* Cleanup */
    smb2_disconnect_share(smb2);
    smb2_destroy_context(smb2);

    int total = edf_count + mand_count;
    ESP_LOGI(TAG, "SMB upload complete for day %s (%d files)", day_folder, total);
    return total > 0 ? UPLOAD_OK : UPLOAD_FAILED;
}

const upload_backend_t smb_backend = {
    .name = "smb",
    .is_configured = smb_is_configured,
    .upload_day = smb_upload_day,
};
