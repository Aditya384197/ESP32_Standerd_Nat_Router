#include "router_log.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MOUNT "/storage"
#define LOGFILE MOUNT "/router.log"
#define MAX_LOG (128*1024)

static SemaphoreHandle_t s_log_mutex;
static bool s_littlefs_mounted;

static void rotate_if_needed(void){
    struct stat st;
    if(stat(LOGFILE,&st)!=0 || st.st_size < MAX_LOG) return;
    remove(MOUNT "/router.2.log");
    rename(MOUNT "/router.1.log", MOUNT "/router.2.log");
    rename(LOGFILE, MOUNT "/router.1.log");
}

void router_log_init(void){
    if (!s_log_mutex) s_log_mutex = xSemaphoreCreateMutex();
    esp_vfs_littlefs_conf_t c={.base_path=MOUNT,.partition_label="storage",.format_if_mount_failed=true,.dont_mount=false};
    esp_err_t e = esp_vfs_littlefs_register(&c);
    s_littlefs_mounted = (e == ESP_OK);
    if (!s_littlefs_mounted) return;
    FILE *f = fopen(LOGFILE, "a");
    if (f) fclose(f);
}

esp_err_t router_log_write(const char *level,const char *message){
    if (!s_littlefs_mounted) return ESP_ERR_INVALID_STATE;
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    rotate_if_needed();
    FILE *f=fopen(LOGFILE,"a");
    if(!f){ xSemaphoreGive(s_log_mutex); return ESP_FAIL; }
    fprintf(f,"%llu %s %s\n",(unsigned long long)(esp_timer_get_time()/1000000ULL),level?level:"I",message?message:"");
    fclose(f);
    xSemaphoreGive(s_log_mutex);
    return ESP_OK;
}

size_t router_log_read(char *out,size_t cap){
    if(!out||cap<2)return 0;
    out[0]=0;
    if(!s_littlefs_mounted) return 0;
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;

    FILE *f=fopen(LOGFILE,"r");
    if(!f){ xSemaphoreGive(s_log_mutex); return 0; }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        xSemaphoreGive(s_log_mutex);
        return 0;
    }

    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        xSemaphoreGive(s_log_mutex);
        return 0;
    }

    size_t want = cap - 1;
    if ((unsigned long)end > want) {
        long start = end - (long)want;
        if (fseek(f, start, SEEK_SET) != 0) {
            fclose(f);
            xSemaphoreGive(s_log_mutex);
            return 0;
        }
        int ch;
        while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
    } else if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        xSemaphoreGive(s_log_mutex);
        return 0;
    }

    size_t n=fread(out,1,cap-1,f);
    fclose(f);
    out[n]=0;
    xSemaphoreGive(s_log_mutex);
    return n;
}

esp_err_t router_log_clear(void){
    if (!s_littlefs_mounted) return ESP_ERR_INVALID_STATE;
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int r = 0;
    if (remove(LOGFILE) != 0) {
        struct stat st;
        if (stat(LOGFILE, &st) == 0) r = -1;
    }
    if (remove(MOUNT "/router.1.log") != 0) {
        struct stat st;
        if (stat(MOUNT "/router.1.log", &st) == 0) r = -1;
    }
    if (remove(MOUNT "/router.2.log") != 0) {
        struct stat st;
        if (stat(MOUNT "/router.2.log", &st) == 0) r = -1;
    }
    FILE *f = fopen(LOGFILE, "a");
    if (!f) r = -1;
    else fclose(f);
    xSemaphoreGive(s_log_mutex);
    return r==0?ESP_OK:ESP_FAIL;
}
