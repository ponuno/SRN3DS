#pragma once
/* Bounded AVM1/AS2 action inventory and side-effect-free frame control.
 *
 * DoAction (12) and DoInitAction (59) payloads are decoded structurally.  The
 * inventory records opcode histograms, byte/instruction counts, unknown
 * opcodes and named non-executed action classes.  No arbitrary ActionScript is
 * evaluated.  Only top-level Stop/Play and in-range GotoFrame operations may
 * update a caller-owned frame-control state; GotoLabel, SetTarget,
 * WaitForFrame, variables, calls and function definitions are never executed.
 */

#include <stddef.h>
#include <stdint.h>

#include "menu_swf.h"
#include "menu_swf_display_list.h"

#define MENU_SWF_AVM_MAX_RECORDS 256u
#define MENU_SWF_AVM_MAX_CONTROLS 32u
#define MENU_SWF_AVM_MAX_INSTRUCTIONS 65536u
#define MENU_SWF_AVM_MAX_NESTING 16u
#define MENU_SWF_AVM_MAX_TIMELINES 256u

typedef struct {
    uint8_t opcode;
    uint16_t frame;
} MenuSwfAvmControlOp;

typedef struct {
    uint16_t sprite_id; /* 0 for the top-level timeline */
    uint8_t tag_code;   /* 12 = DoAction, 59 = DoInitAction */
    uint32_t frame_index;
    uint32_t payload_bytes;
    uint32_t code_bytes; /* excludes the DoInitAction sprite id */
    uint32_t opcode_count;
    uint32_t unknown_count;
    uint32_t refused_count; /* named classes that are never executed */
    uint32_t stop_count;
    uint32_t play_count;
    uint32_t goto_frame_count;
    uint32_t goto_frame2_count;
    uint32_t goto_label_count;
    uint32_t set_target_count;
    uint32_t wait_count;
    uint32_t push_count;
    uint32_t get_variable_count;
    uint32_t set_variable_count;
    uint32_t function_count;
    uint32_t call_count;
    uint32_t control_count;
    uint32_t trailing_bytes;
    uint8_t end_seen;
    MenuSwfAvmControlOp controls[MENU_SWF_AVM_MAX_CONTROLS];
    char first_refusal[BSA_REASON_MAX];
} MenuSwfAvmRecord;

typedef struct {
    MenuSwfAvmRecord *items; /* caller-owned */
    uint32_t capacity;
    uint32_t count;
    uint32_t total;
    int truncated;

    uint32_t histogram[256];
    uint64_t payload_bytes;
    uint64_t code_bytes;
    uint64_t opcode_count;
    uint32_t do_action;
    uint32_t do_init_action;
    uint32_t unknown_opcodes;
    uint32_t not_executed_opcodes;
    uint32_t stop_count;
    uint32_t play_count;
    uint32_t goto_count;
    uint32_t goto_label_count;
    uint32_t set_target_count;
    uint32_t wait_count;
    uint32_t push_count;
    uint32_t get_variable_count;
    uint32_t set_variable_count;
    uint32_t function_count;
    uint32_t call_count;
    int has_refusal;
    char first_refusal[BSA_REASON_MAX];
} MenuSwfAvmList;

typedef struct {
    uint16_t frame;
    uint8_t stopped;
    uint32_t applied;
    uint32_t refused;
} MenuSwfAvmControlState;

typedef struct {
    uint16_t sprite_id; /* 0 = top-level timeline */
    uint32_t frame_count;
} MenuSwfAvmTimeline;

typedef struct {
    uint32_t stopped_records;
    uint32_t playing_records;
    uint32_t refused_controls;
} MenuSwfAvmDisplayResult;

const char *menu_swf_avm_opcode_name(uint8_t opcode);

/* Parse one complete DoAction/DoInitAction tag payload.  Unknown opcodes with
 * a length are inventoried and skipped, but the returned status is NOTIMPL and
 * reason names the first unknown opcode.  Truncated/malformed records are
 * named TRUNCATED/FORMAT errors. */
MenuSwfStatus menu_swf_avm_parse_record(
    const uint8_t *payload, size_t len, uint8_t tag_code,
    MenuSwfAvmRecord *out, char *reason, size_t reason_cap);

/* Walk top-level and nested DefineSprite tags, collecting DoAction and
 * DoInitAction records.  Unknown opcodes are retained as named refusals while
 * collection continues; malformed tags and caller capacity overflow are fatal
 * named errors. */
MenuSwfStatus menu_swf_avm_collect(
    const uint8_t *data, size_t size, MenuSwfAvmList *list,
    char *reason, size_t reason_cap);

/* Apply only top-level Stop/Play and in-range GotoFrame operations from one
 * record.  GotoLabel/SetTarget/WaitForFrame and out-of-range GotoFrame are not
 * executed and return NOTIMPL with a named reason. */
MenuSwfStatus menu_swf_avm_apply_record(
    const MenuSwfAvmRecord *record, uint32_t frame_count,
    MenuSwfAvmControlState *state, char *reason, size_t reason_cap);

/* Annotate flattened display-list records with the safe Stop/Play/frame state
 * for their parent timeline.  `timelines` supplies 0 for the top timeline and
 * each sprite's declared frame count.  No AVM side effect is executed. */
MenuSwfStatus menu_swf_avm_apply_display_controls(
    const MenuSwfAvmList *list,
    MenuSwfDisplayListRecord *records, uint32_t record_count,
    const MenuSwfAvmTimeline *timelines, uint32_t timeline_count,
    MenuSwfAvmDisplayResult *out, char *reason, size_t reason_cap);
