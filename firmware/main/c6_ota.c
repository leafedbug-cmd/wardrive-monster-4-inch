#include "c6_ota.h"

#include <stdio.h>
#include <string.h>

#include "bsp_pins.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "c6_ota";

/* ------------------------------------------------------------------ *
 *  Which OTA API is available?                                        *
 *                                                                     *
 *  esp_hosted 3.x exposes a chunked API -- begin / write / end /      *
 *  activate -- which lets us stream an image from anywhere, with no   *
 *  network involved. It declares those as macros.                     *
 *                                                                     *
 *  esp_hosted 2.x only has esp_hosted_slave_ota(url), which needs the *
 *  P4 joined to a network and an HTTP server hosting the image. This  *
 *  firmware never joins a network, so on 2.x the streaming path is    *
 *  compiled out and we say so plainly instead of failing at runtime.  *
 *                                                                     *
 *  See docs/C6-OTA.md.                                                *
 * ------------------------------------------------------------------ */
#ifdef esp_hosted_slave_ota_begin
#  define C6_OTA_CHUNKED 1
#else
#  define C6_OTA_CHUNKED 0
#endif

/* Matches the chunk size the upstream coprocessor_ota example uses. Larger
 * chunks do not help: the RPC layer fragments anyway. */
#define CHUNK_SIZE 1400

/* Partition in the P4's own flash holding a copy of the C6 image, written
 * there by `idf.py flash`. Lets the C6 be updated with no SD card at all. */
#define C6FW_PARTITION "c6fw"

static c6_ota_status_t s_st;
static volatile bool   s_busy;

static void set_state(c6_ota_state_t st, const char *msg)
{
    s_st.state = st;
    if (msg) {
        strlcpy(s_st.message, msg, sizeof(s_st.message));
    }
}

/* ------------------------------------------------------------------ *
 *  Image sources                                                      *
 *                                                                     *
 *  Two places a C6 image can live: a file on the SD card, or the      *
 *  c6fw partition in the P4's flash. Both reduce to "read N bytes at  *
 *  offset X", so the OTA loop is written once against this.           *
 * ------------------------------------------------------------------ */

typedef struct img_src {
    const char *label;
    void       *ctx;
    esp_err_t (*read)(struct img_src *s, uint32_t off, void *dst, size_t len);
    void      (*close)(struct img_src *s);
    uint32_t    size;        /* real image size, from the header walk */
} img_src_t;

static esp_err_t file_read(img_src_t *s, uint32_t off, void *dst, size_t len)
{
    FILE *f = (FILE *)s->ctx;
    if (fseek(f, off, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    return (fread(dst, 1, len, f) == len) ? ESP_OK : ESP_FAIL;
}

static void file_close(img_src_t *s)
{
    if (s->ctx) {
        fclose((FILE *)s->ctx);
        s->ctx = NULL;
    }
}

static esp_err_t part_read(img_src_t *s, uint32_t off, void *dst, size_t len)
{
    return esp_partition_read((const esp_partition_t *)s->ctx, off, dst, len);
}

static void part_close(img_src_t *s)
{
    s->ctx = NULL;
}

static esp_err_t open_file_src(const char *path, img_src_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = (img_src_t){ .label = path, .ctx = f,
                        .read = file_read, .close = file_close };
    return ESP_OK;
}

static esp_err_t open_part_src(img_src_t *out)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, C6FW_PARTITION);
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = (img_src_t){ .label = "c6fw partition", .ctx = (void *)p,
                        .read = part_read, .close = part_close };
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Image validation                                                   *
 *                                                                     *
 *  Pushing a non-image to the C6 wastes a minute and can leave the    *
 *  slave confused, so check the magic byte first. We also walk the    *
 *  segment table to learn the real image length -- a partition has no *
 *  file size to fall back on, and sending trailing erased flash would *
 *  fail verification on the C6.                                       *
 * ------------------------------------------------------------------ */

static esp_err_t img_probe(img_src_t *src, char *ver, size_t ver_len)
{
    esp_image_header_t hdr;
    ESP_RETURN_ON_ERROR(src->read(src, 0, &hdr, sizeof(hdr)), TAG, "read hdr");

    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "%s does not hold an ESP firmware image (magic 0x%02x)",
                 src->label, hdr.magic);
        return ESP_ERR_INVALID_ARG;
    }

    /* The app descriptor sits right after the first segment header. */
    esp_app_desc_t desc;
    if (ver && ver_len) {
        if (src->read(src, sizeof(esp_image_header_t) +
                           sizeof(esp_image_segment_header_t),
                      &desc, sizeof(desc)) == ESP_OK &&
            desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
            strlcpy(ver, desc.version, ver_len);
        } else {
            strlcpy(ver, "unknown", ver_len);
        }
    }

    /* Walk the segments to the true end of the image. */
    uint32_t off = sizeof(esp_image_header_t);
    uint32_t total = sizeof(esp_image_header_t);
    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        ESP_RETURN_ON_ERROR(src->read(src, off, &seg, sizeof(seg)), TAG, "read seg");
        if (seg.data_len > 16 * 1024 * 1024) {
            ESP_LOGE(TAG, "segment %d length implausible (%lu)",
                     i, (unsigned long)seg.data_len);
            return ESP_ERR_INVALID_SIZE;
        }
        total += sizeof(seg) + seg.data_len;
        off   += sizeof(seg) + seg.data_len;
    }

    total += (16 - (total % 16)) % 16;   /* 16-byte alignment padding */
    total += 1;                          /* checksum byte             */
    if (hdr.hash_appended == 1) {
        total += 32;                     /* appended SHA-256          */
    }

    src->size = total;
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 *  Remembering what we already pushed                                 *
 *                                                                     *
 *  The embedded image must flash exactly once per change, not on      *
 *  every boot. Record version+size after a success and compare.       *
 * ------------------------------------------------------------------ */

#define NVS_NS "wardrive"

static void record_flashed(const char *ver, uint32_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "c6ver", ver);
        nvs_set_u32(h, "c6size", size);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool differs_from_flashed(const char *ver, uint32_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return true;    /* never recorded -> treat as new */
    }

    char prev[32] = {0};
    size_t len = sizeof(prev);
    uint32_t prev_size = 0;

    esp_err_t a = nvs_get_str(h, "c6ver", prev, &len);
    esp_err_t b = nvs_get_u32(h, "c6size", &prev_size);
    nvs_close(h);

    if (a != ESP_OK || b != ESP_OK) {
        return true;
    }
    return (strcmp(prev, ver) != 0) || (prev_size != size);
}

/* ------------------------------------------------------------------ *
 *  Public inspection                                                  *
 * ------------------------------------------------------------------ */

static esp_err_t inspect_src(img_src_t *src, c6_ota_status_t *out)
{
    c6_ota_status_t tmp = { .state = C6_OTA_CHECKING };
    esp_err_t err = img_probe(src, tmp.image_version, sizeof(tmp.image_version));
    tmp.total = src->size;
    tmp.state = (err == ESP_OK) ? C6_OTA_IDLE : C6_OTA_FAILED;
    if (err != ESP_OK) {
        strlcpy(tmp.message, esp_err_to_name(err), sizeof(tmp.message));
    }
    if (out) {
        *out = tmp;
    }
    return err;
}

esp_err_t c6_ota_inspect(const char *path, c6_ota_status_t *out)
{
    img_src_t src;
    esp_err_t err = open_file_src(path ? path : BSP_C6_FIRMWARE_PATH, &src);
    if (err != ESP_OK) {
        if (out) {
            *out = (c6_ota_status_t){ .state = C6_OTA_FAILED };
            strlcpy(out->message, "no image on card", sizeof(out->message));
        }
        return err;
    }
    err = inspect_src(&src, out);
    src.close(&src);
    return err;
}

esp_err_t c6_ota_inspect_partition(c6_ota_status_t *out)
{
    img_src_t src;
    esp_err_t err = open_part_src(&src);
    if (err != ESP_OK) {
        if (out) {
            *out = (c6_ota_status_t){ .state = C6_OTA_FAILED };
            strlcpy(out->message, "no c6fw partition", sizeof(out->message));
        }
        return err;
    }
    err = inspect_src(&src, out);
    src.close(&src);
    return err;
}

bool c6_ota_partition_pending(void)
{
    c6_ota_status_t st;
    if (c6_ota_inspect_partition(&st) != ESP_OK) {
        return false;
    }
    /* An erased/blank partition probes as invalid and never gets here. */
    return differs_from_flashed(st.image_version, st.total);
}

/* ------------------------------------------------------------------ *
 *  The update itself                                                  *
 * ------------------------------------------------------------------ */

#if !C6_OTA_CHUNKED

static esp_err_t ota_stream(img_src_t *src)
{
    (void)src;
    set_state(C6_OTA_FAILED, "needs esp_hosted 3.x");
    ESP_LOGE(TAG,
        "Streaming OTA needs the chunked API from esp_hosted 3.x, but this "
        "build linked 2.x, which only offers esp_hosted_slave_ota(url). "
        "See docs/C6-OTA.md.");
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* C6_OTA_CHUNKED */

static esp_err_t ota_stream(img_src_t *src)
{
    uint8_t *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        set_state(C6_OTA_FAILED, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    /* Erases the C6's inactive OTA partition -- this is the slow step. */
    set_state(C6_OTA_ERASING, "erasing C6 partition");
    ESP_LOGI(TAG, "ota begin: %lu bytes from %s",
             (unsigned long)src->size, src->label);

    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        set_state(C6_OTA_FAILED, esp_err_to_name(err));
        goto out;
    }

    set_state(C6_OTA_WRITING, "streaming to C6");
    for (uint32_t sent = 0; sent < src->size; ) {
        size_t n = src->size - sent;
        if (n > CHUNK_SIZE) {
            n = CHUNK_SIZE;
        }
        err = src->read(src, sent, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "source read failed at %lu", (unsigned long)sent);
            set_state(C6_OTA_FAILED, "image read error");
            goto out;
        }

        err = esp_hosted_slave_ota_write(buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at %lu: %s",
                     (unsigned long)sent, esp_err_to_name(err));
            set_state(C6_OTA_FAILED, esp_err_to_name(err));
            goto out;
        }

        sent += n;
        s_st.written = sent;
        s_st.percent = (uint8_t)((uint64_t)sent * 100 / src->size);

        /* Let the transport drain and keep the watchdog fed. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    set_state(C6_OTA_FINALISING, "verifying on C6");
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        set_state(C6_OTA_FAILED, esp_err_to_name(err));
        goto out;
    }

    err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_activate failed: %s", esp_err_to_name(err));
        set_state(C6_OTA_FAILED, esp_err_to_name(err));
        goto out;
    }

    s_st.percent = 100;
    set_state(C6_OTA_DONE, "C6 rebooting into new image");

out:
    free(buf);
    return err;
}

#endif /* C6_OTA_CHUNKED */

static esp_err_t run_src(img_src_t *src)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;

    memset(&s_st, 0, sizeof(s_st));
    set_state(C6_OTA_CHECKING, "validating image");

    esp_err_t err = img_probe(src, s_st.image_version, sizeof(s_st.image_version));
    if (err != ESP_OK) {
        set_state(C6_OTA_FAILED, esp_err_to_name(err));
        s_busy = false;
        return err;
    }
    s_st.total = src->size;

    err = ota_stream(src);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "C6 updated to '%s' from %s",
                 s_st.image_version, src->label);
        record_flashed(s_st.image_version, s_st.total);
    }

    s_busy = false;
    return err;
}

esp_err_t c6_ota_run(const char *path)
{
    img_src_t src;
    esp_err_t err = open_file_src(path ? path : BSP_C6_FIRMWARE_PATH, &src);
    if (err != ESP_OK) {
        set_state(C6_OTA_FAILED, "no image on card");
        return err;
    }
    err = run_src(&src);
    src.close(&src);
    return err;
}

esp_err_t c6_ota_run_partition(void)
{
    img_src_t src;
    esp_err_t err = open_part_src(&src);
    if (err != ESP_OK) {
        set_state(C6_OTA_FAILED, "no c6fw partition");
        return err;
    }
    err = run_src(&src);
    src.close(&src);
    return err;
}

/* ------------------------------------------------------------------ */

static void ota_task(void *arg)
{
    char *path = (char *)arg;
    if (path) {
        c6_ota_run(path);
        free(path);
    } else {
        c6_ota_run_partition();
    }
    vTaskDelete(NULL);
}

esp_err_t c6_ota_start_async(const char *path)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = path ? strdup(path) : NULL;
    if (path && !copy) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ota_task, "c6_ota", 6144, copy, 5, NULL) != pdPASS) {
        free(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void c6_ota_status(c6_ota_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}

bool c6_ota_busy(void)
{
    return s_busy;
}
