#pragma once
// Conversation model (both modes, PSRAM) and the message widgets shared by H and HIST (docs/ui.md 3.3,
// 3.4, 3.14, 5.4). A message widget is a full-width wrapper whose user data is the entry id.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum { CM_USER, CM_BOT, CM_TURN, CM_CARD, CM_DIVIDER } cm_kind_t;

typedef struct {
    uint32_t id;            // unique over both modes, never 0
    uint8_t kind;           // cm_kind_t
    int8_t side;            // CM_TURN: UI_SIDE_A / UI_SIDE_B, UI_SIDE_AUTO = centred until known
    uint8_t card;           // CM_CARD: ui_card_t
    bool pending;           // waveform placeholder, transcript not known yet
    bool tts_failed;        // reply shown with the "голос недоступен" note
    bool new_turn;          // first message of a turn: 14 px above instead of 6
    bool has_env;
    char *text;             // transcript / reply / card body (sanitised, PSRAM)
    char *text2;            // CM_TURN translation (sanitised, PSRAM)
    char src[4], dst[4];    // CM_TURN language codes
    uint8_t env[32];        // RMS envelope of the clip, 0..255
} cm_entry_t;

#define CM_MAX      40      // stored per mode (HIST); H shows the last 20
#define CM_LIVE_MAX 20

// Allocates the PSRAM rings; call once.
void cm_init(void);
// New entry at the end (drops the oldest beyond CM_MAX); never NULL after cm_init().
cm_entry_t *cm_add(int mode, cm_kind_t kind);
// Entry by id in either mode, or NULL.
cm_entry_t *cm_find(uint32_t id);
// Mode that owns entry id (0/1), -1 if unknown.
int cm_mode_of(uint32_t id);
// idx-th entry, 0 = oldest.
cm_entry_t *cm_at(int mode, int idx);
int cm_count(int mode);
// Last entry of kind k (NULL if none); last pending user/turn entry.
cm_entry_t *cm_last_of(int mode, cm_kind_t k);
cm_entry_t *cm_last_pending(int mode);
void cm_clear(int mode);
// Sanitised PSRAM copy / append (ui_str_sanitize with ui_20 glyph coverage).
void cm_set(char **dst, const char *text);
void cm_append(char **dst, const char *chunk);

// Builds the widget of e at the end of list. animate: A16 appear (delay_ms later).
lv_obj_t *ui_msg_create(lv_obj_t *list, const cm_entry_t *e, bool animate, uint32_t delay_ms,
                        lv_event_cb_t on_click);
// Re-reads e after its text / side changed; animate: A17 crossfade and A18 slide.
void ui_msg_refresh(lv_obj_t *wrap, const cm_entry_t *e, bool animate);
// Wrapper of entry id inside list, or NULL.
lv_obj_t *ui_msg_find(lv_obj_t *list, uint32_t id);
// Entry id of a wrapper (or of any object inside it).
uint32_t ui_msg_id(lv_obj_t *obj);
