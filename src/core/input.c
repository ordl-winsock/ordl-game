/*
 * src/core/input.c — Timestamped input frames for real-time netcode
 */

#include "forge/input.h"
#include "forge/memory.h"

void fge_input_history_init(fge_input_history_t *h) {
    if (!h) return;
    fge_memzero(h, sizeof(*h));
    h->next_sequence = 1;
}

uint32_t fge_input_capture(fge_input_history_t *h,
                            float move_x, float move_y,
                            float aim_x, float aim_y,
                            uint32_t buttons, uint32_t modifiers,
                            uint16_t dt_ms) {
    if (!h) return 0;
    fge_input_frame_t *f = &h->frames[h->head];
    f->tick = 0; /* caller sets authoritative tick if needed */
    f->sequence = h->next_sequence++;
    f->move_x = move_x;
    f->move_y = move_y;
    f->aim_x = aim_x;
    f->aim_y = aim_y;
    f->buttons = buttons;
    f->modifiers = modifiers;
    f->dt_ms = dt_ms;

    h->head = (h->head + 1) & (FGE_INPUT_HISTORY - 1);
    if (h->count < FGE_INPUT_HISTORY) h->count++;
    return f->sequence;
}

const fge_input_frame_t *fge_input_get(const fge_input_history_t *h, uint32_t sequence) {
    if (!h || h->count == 0) return NULL;
    /* Search from newest to oldest */
    for (uint32_t i = 0; i < h->count; i++) {
        uint32_t idx = (h->head - 1 - i + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
        if (h->frames[idx].sequence == sequence) return &h->frames[idx];
    }
    return NULL;
}

const fge_input_frame_t *fge_input_latest(const fge_input_history_t *h) {
    if (!h || h->count == 0) return NULL;
    uint32_t idx = (h->head - 1 + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
    return &h->frames[idx];
}

void fge_input_acknowledge(fge_input_history_t *h, uint32_t sequence) {
    if (!h || h->count == 0) return;
    /* Drop frames up to and including the acknowledged sequence */
    while (h->count > 0) {
        uint32_t tail_idx = (h->head - h->count + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
        if (h->frames[tail_idx].sequence > sequence) break;
        h->count--;
    }
}

void fge_input_history_clear(fge_input_history_t *h) {
    if (!h) return;
    h->head = 0;
    h->count = 0;
}

size_t fge_input_snapshot_pack(const fge_input_history_t *h,
                                fge_input_snapshot_t *out,
                                uint32_t last_acked_sequence,
                                uint32_t max_frames) {
    if (!h || !out || max_frames == 0) return 0;
    uint32_t to_send = h->count;
    if (to_send > max_frames) to_send = max_frames;
    if (to_send > FGE_INPUT_SNAPSHOT_MAX) to_send = FGE_INPUT_SNAPSHOT_MAX;

    /* Find first unacked frame */
    uint32_t start = 0;
    for (uint32_t i = 0; i < h->count; i++) {
        uint32_t idx = (h->head - h->count + i + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
        if (h->frames[idx].sequence > last_acked_sequence) {
            start = i;
            break;
        }
        if (i == h->count - 1) return 0; /* all acked */
    }

    out->start_sequence = h->frames[(h->head - h->count + start + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1)].sequence;
    out->count = 0;

    for (uint32_t i = start; i < h->count && out->count < to_send; i++) {
        uint32_t idx = (h->head - h->count + i + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
        out->frames[out->count++] = h->frames[idx];
    }
    return out->count;
}

size_t fge_input_snapshot_serialize(const fge_input_snapshot_t *snap,
                                     uint8_t *buf, size_t buf_len) {
    if (!snap || !buf || buf_len < 8) return 0;
    uint8_t *p = buf;
    *(uint32_t *)p = snap->start_sequence; p += 4;
    *(uint32_t *)p = snap->count; p += 4;
    for (uint32_t i = 0; i < snap->count; i++) {
        const fge_input_frame_t *f = &snap->frames[i];
        if ((size_t)(p - buf) + 28 > buf_len) return 0;
        *(uint64_t *)p = f->tick; p += 8;
        *(uint32_t *)p = f->sequence; p += 4;
        *(float *)p = f->move_x; p += 4;
        *(float *)p = f->move_y; p += 4;
        *(float *)p = f->aim_x; p += 4;
        *(float *)p = f->aim_y; p += 4;
        *(uint32_t *)p = f->buttons; p += 4;
        *(uint32_t *)p = f->modifiers; p += 4;
        *(uint16_t *)p = f->dt_ms; p += 2;
    }
    return (size_t)(p - buf);
}

size_t fge_input_snapshot_deserialize(fge_input_snapshot_t *snap,
                                       const uint8_t *buf, size_t buf_len) {
    if (!snap || !buf || buf_len < 8) return 0;
    const uint8_t *p = buf;
    snap->start_sequence = *(const uint32_t *)p; p += 4;
    snap->count = *(const uint32_t *)p; p += 4;
    if (snap->count > FGE_INPUT_SNAPSHOT_MAX) return 0;
    for (uint32_t i = 0; i < snap->count; i++) {
        fge_input_frame_t *f = &snap->frames[i];
        if ((size_t)(p - buf) + 28 > buf_len) return 0;
        f->tick = *(const uint64_t *)p; p += 8;
        f->sequence = *(const uint32_t *)p; p += 4;
        f->move_x = *(const float *)p; p += 4;
        f->move_y = *(const float *)p; p += 4;
        f->aim_x = *(const float *)p; p += 4;
        f->aim_y = *(const float *)p; p += 4;
        f->buttons = *(const uint32_t *)p; p += 4;
        f->modifiers = *(const uint32_t *)p; p += 4;
        f->dt_ms = *(const uint16_t *)p; p += 2;
    }
    return (size_t)(p - buf);
}
