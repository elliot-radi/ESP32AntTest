#include "buffer.h"
#include "config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Store up to ANT_MOB_BUFFER_MAX / sizeof(entry) entries (~200+). */
#define BUF_CAP  ((ANT_MOB_BUFFER_MAX) > 0 \
    ? ((ANT_MOB_BUFFER_MAX) / (int)sizeof(buf_entry_t)) : 64)

static buf_entry_t s_q[BUF_CAP > 0 ? BUF_CAP : 64];
static int s_head = 0;   /* next pop */
static int s_tail = 0;   /* next push */
static int s_count = 0;
static SemaphoreHandle_t s_mu;

void buffer_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_head = s_tail = s_count = 0;
}

bool buffer_push(const buf_entry_t *e)
{
    if (!e) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_count >= (int)(sizeof(s_q) / sizeof(s_q[0]))) {
        /* drop oldest */
        s_head = (s_head + 1) % (int)(sizeof(s_q) / sizeof(s_q[0]));
        s_count--;
    }
    s_q[s_tail] = *e;
    s_tail = (s_tail + 1) % (int)(sizeof(s_q) / sizeof(s_q[0]));
    s_count++;
    xSemaphoreGive(s_mu);
    return true;
}

bool buffer_pop(buf_entry_t *out)
{
    if (!out) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_mu);
        return false;
    }
    *out = s_q[s_head];
    s_head = (s_head + 1) % (int)(sizeof(s_q) / sizeof(s_q[0]));
    s_count--;
    xSemaphoreGive(s_mu);
    return true;
}

int buffer_count(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int c = s_count;
    xSemaphoreGive(s_mu);
    return c;
}

void buffer_clear(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_head = s_tail = s_count = 0;
    xSemaphoreGive(s_mu);
}
