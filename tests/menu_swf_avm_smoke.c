/* Host test: bounded AVM1/AS2 inventory and safe frame control. */

#include "menu_swf_avm.h"
#include "bsa_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

static void check(int condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t put_tag(uint8_t *out, size_t cap, size_t at, unsigned code,
                      const uint8_t *payload, uint32_t len) {
    int long_form = len >= 0x3f;
    size_t need = 2 + (long_form ? 4 : 0) + len;
    if (at > cap || need > cap - at) return 0;
    put_u16(out + at, (uint16_t)((code << 6) | (long_form ? 0x3fu : len)));
    at += 2;
    if (long_form) {
        put_u32(out + at, len);
        at += 4;
    }
    if (len) memcpy(out + at, payload, len);
    return at + len;
}

static size_t high_action(uint8_t *out, size_t cap, size_t at, uint8_t op,
                          const uint8_t *data, uint16_t len) {
    if (at + 3 + len > cap) return 0;
    out[at++] = op;
    put_u16(out + at, len);
    at += 2;
    if (len) memcpy(out + at, data, len);
    return at + len;
}

static size_t make_sprite(uint8_t *out, size_t cap, uint16_t id,
                          const uint8_t *tags, size_t len) {
    if (cap < 4 + len) return 0;
    put_u16(out, id);
    put_u16(out + 2, 1);
    memcpy(out + 4, tags, len);
    return 4 + len;
}

static size_t make_fws(uint8_t *out, size_t cap, const uint8_t *body,
                       size_t body_len) {
    if (cap < 8 + body_len) return 0;
    memcpy(out, "FWS", 3);
    out[3] = 10;
    put_u32(out + 4, (uint32_t)(8 + body_len));
    memcpy(out + 8, body, body_len);
    return 8 + body_len;
}

static size_t frame_header(uint8_t *body, size_t cap) {
    if (cap < 5) return 0;
    body[0] = 0;
    put_u16(body + 1, 0x1e00);
    put_u16(body + 3, 1);
    return 5;
}

static size_t place2(uint8_t *out, size_t cap, uint16_t depth,
                     uint16_t character) {
    if (cap < 5) return 0;
    out[0] = 0x02; /* HasCharacter */
    put_u16(out + 1, depth);
    put_u16(out + 3, character);
    return 5;
}

static void test_parse_controls(void) {
    char reason[BSA_REASON_MAX];
    MenuSwfAvmRecord record;
    MenuSwfAvmControlState state;

    const uint8_t stop[] = {0x07, 0x00};
    check(menu_swf_avm_parse_record(stop, sizeof stop, 12, &record, reason,
                                   sizeof reason) == MENU_SWF_OK,
          "Stop record parses");
    check(record.opcode_count == 2 && record.stop_count == 1 &&
              record.control_count == 1 && record.end_seen,
          "Stop inventory counts");
    memset(&state, 0, sizeof state);
    check(menu_swf_avm_apply_record(&record, 1, &state, reason, sizeof reason) ==
              MENU_SWF_OK && state.stopped && state.applied == 1,
          "Stop safely stops its timeline");

    const uint8_t play[] = {0x06, 0x00};
    check(menu_swf_avm_parse_record(play, sizeof play, 12, &record, reason,
                                   sizeof reason) == MENU_SWF_OK,
          "Play record parses");
    check(menu_swf_avm_apply_record(&record, 1, &state, reason, sizeof reason) ==
              MENU_SWF_OK && !state.stopped && state.applied == 2,
          "Play safely resumes its timeline");

    const uint8_t goto_frame[] = {0x81, 0x02, 0x00, 0x01, 0x00, 0x00};
    check(menu_swf_avm_parse_record(goto_frame, sizeof goto_frame, 12, &record,
                                   reason, sizeof reason) == MENU_SWF_OK,
          "GotoFrame record parses");
    memset(&state, 0, sizeof state);
    check(menu_swf_avm_apply_record(&record, 2, &state, reason, sizeof reason) ==
              MENU_SWF_OK && state.frame == 1,
          "in-range GotoFrame updates only frame state");
    check(menu_swf_avm_apply_record(&record, 1, &state, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL && state.frame == 1 && state.refused == 1 &&
              strstr(reason, "outside 1 frames"),
          "out-of-range GotoFrame is named and not executed");

    const uint8_t label[] = {0x8c, 0x04, 0x00, 'f', 'o', 'o', 0, 0x00};
    check(menu_swf_avm_parse_record(label, sizeof label, 12, &record, reason,
                                   sizeof reason) == MENU_SWF_OK &&
              record.goto_label_count == 1,
          "GotoLabel is inventoried");
    memset(&state, 0, sizeof state);
    check(menu_swf_avm_apply_record(&record, 2, &state, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL && strstr(reason, "GotoLabel"),
          "GotoLabel is a named safe-execution refusal");

    const uint8_t wait[] = {0x8a, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00};
    check(menu_swf_avm_parse_record(wait, sizeof wait, 12, &record, reason,
                                   sizeof reason) == MENU_SWF_OK &&
              record.wait_count == 1,
          "WaitForFrame is inventoried");
    check(menu_swf_avm_apply_record(&record, 2, &state, reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL && strstr(reason, "WaitForFrame"),
          "WaitForFrame is not executed");

    const uint8_t side[] = {0x96, 0x04, 0x00, 0x00, 'A', 0, 0x02,
                            0x1c, 0x1d, 0x00};
    check(menu_swf_avm_parse_record(side, sizeof side, 12, &record, reason,
                                   sizeof reason) == MENU_SWF_OK &&
              record.push_count == 1 && record.get_variable_count == 1 &&
              record.set_variable_count == 1 && record.refused_count >= 3,
          "Push/GetVariable/SetVariable are named but not executed");
}

static void test_function_scope_unknown_and_errors(void) {
    char reason[BSA_REASON_MAX];
    MenuSwfAvmRecord record;
    MenuSwfAvmControlState state;

    /* DefineFunction("f", 0, body={Stop,End}); body Stop is inventoried but is
     * not a top-level executable frame-control operation. */
    const uint8_t function[] = {0x9b, 0x06, 0x00, 'f', 0, 0x00, 0x00,
                                0x02, 0x00, 0x07, 0x00};
    check(menu_swf_avm_parse_record(function, sizeof function, 12, &record,
                                   reason, sizeof reason) == MENU_SWF_OK &&
              record.function_count == 1 && record.stop_count == 1 &&
              record.control_count == 0,
          "function body opcodes are inventoried without executable controls");
    memset(&state, 0, sizeof state);
    check(menu_swf_avm_apply_record(&record, 1, &state, reason, sizeof reason) ==
              MENU_SWF_OK && !state.stopped && state.applied == 0,
          "function-body Stop is not executed");

    const uint8_t unknown[] = {0x97, 0x03, 0x00, 0xaa, 0xbb, 0xcc,
                               0x07, 0x00};
    check(menu_swf_avm_parse_record(unknown, sizeof unknown, 12, &record,
                                   reason, sizeof reason) ==
              MENU_SWF_ERR_NOTIMPL && record.unknown_count == 1 &&
              record.stop_count == 1 && strstr(reason, "unknown opcode 0x97"),
          "unknown length-prefixed opcode is named and skipped");

    const uint8_t truncated[] = {0x81, 0x02, 0x00, 0x01};
    check(menu_swf_avm_parse_record(truncated, sizeof truncated, 12, &record,
                                   reason, sizeof reason) ==
              MENU_SWF_ERR_TRUNCATED,
          "truncated action length is refused");
    const uint8_t bad_wait[] = {0x8a, 0x02, 0x00, 0x00, 0x00};
    check(menu_swf_avm_parse_record(bad_wait, sizeof bad_wait, 12, &record,
                                   reason, sizeof reason) == MENU_SWF_ERR_FORMAT,
          "wrong WaitForFrame length is refused");
    const uint8_t short_init[] = {0x01};
    check(menu_swf_avm_parse_record(short_init, sizeof short_init, 59, &record,
                                   reason, sizeof reason) ==
              MENU_SWF_ERR_TRUNCATED,
          "short DoInitAction header is refused");
}

static size_t build_control_movie(uint8_t *file, size_t cap) {
    uint8_t body[2048], sprite[512], tags[1024], payload[128];
    const uint8_t stop[] = {0x07, 0x00};
    const uint8_t stop_play[] = {0x07, 0x06, 0x00};
    size_t s = 0, t = 0, n;

    n = place2(payload, sizeof payload, 1, 100);
    s = put_tag(sprite, sizeof sprite, s, 26, payload, (uint32_t)n);
    s = put_tag(sprite, sizeof sprite, s, 12, stop, sizeof stop);
    s = put_tag(sprite, sizeof sprite, s, 1, NULL, 0);
    s = put_tag(sprite, sizeof sprite, s, 0, NULL, 0);
    n = make_sprite(payload, sizeof payload, 10, sprite, s);
    t = put_tag(tags, sizeof tags, t, 39, payload, (uint32_t)n);

    s = 0;
    n = place2(payload, sizeof payload, 1, 100);
    s = put_tag(sprite, sizeof sprite, s, 26, payload, (uint32_t)n);
    s = put_tag(sprite, sizeof sprite, s, 12, stop_play, sizeof stop_play);
    s = put_tag(sprite, sizeof sprite, s, 1, NULL, 0);
    s = put_tag(sprite, sizeof sprite, s, 0, NULL, 0);
    n = make_sprite(payload, sizeof payload, 11, sprite, s);
    t = put_tag(tags, sizeof tags, t, 39, payload, (uint32_t)n);

    n = place2(payload, sizeof payload, 1, 10);
    t = put_tag(tags, sizeof tags, t, 26, payload, (uint32_t)n);
    n = place2(payload, sizeof payload, 2, 11);
    t = put_tag(tags, sizeof tags, t, 26, payload, (uint32_t)n);

    put_u16(payload, 30);
    payload[2] = 0x07;
    payload[3] = 0x00;
    t = put_tag(tags, sizeof tags, t, 59, payload, 4);
    const uint8_t play[] = {0x06, 0x00};
    t = put_tag(tags, sizeof tags, t, 12, play, sizeof play);
    t = put_tag(tags, sizeof tags, t, 1, NULL, 0);
    t = put_tag(tags, sizeof tags, t, 0, NULL, 0);

    size_t b = frame_header(body, sizeof body);
    memcpy(body + b, tags, t);
    b += t;
    return make_fws(file, cap, body, b);
}

static void test_collect_and_display(void) {
    uint8_t file[4096];
    size_t flen = build_control_movie(file, sizeof file);
    check(flen > 0, "AVM control movie builds");
    MenuSwfAvmRecord items[8];
    MenuSwfAvmList list = {items, 8, 0, 0, 0};
    char reason[BSA_REASON_MAX];
    check(menu_swf_avm_collect(file, flen, &list, reason, sizeof reason) ==
              MENU_SWF_OK && list.count == 4 && list.do_action == 3 &&
              list.do_init_action == 1 && list.stop_count == 3 &&
              list.play_count == 2,
          "DoAction/DoInitAction inventory recurses into sprites");
    check(list.items[0].sprite_id == 10 && list.items[1].sprite_id == 11 &&
              list.items[2].sprite_id == 30 && list.items[3].sprite_id == 0,
          "action records retain their owning sprite");

    MenuSwfDisplayListRecord records[8];
    MenuSwfDisplayListResult built;
    check(menu_swf_display_list_build(file, flen, records, 8, &built, reason,
                                      sizeof reason) == MENU_SWF_OK &&
              built.record_count == 4,
          "control movie display list builds");
    MenuSwfAvmTimeline timelines[] = {{0, 1}, {10, 1}, {11, 1}};
    MenuSwfAvmDisplayResult applied;
    check(menu_swf_avm_apply_display_controls(
              &list, records, built.record_count, timelines, 3, &applied,
              reason, sizeof reason) == MENU_SWF_OK &&
              applied.stopped_records == 1 && applied.playing_records == 3 &&
              applied.refused_controls == 0,
          "safe Stop/Play state applies to display records");
    check(records[0].parent_sprite_id == 0 && !records[0].stopped &&
              records[1].parent_sprite_id == 10 && records[1].stopped &&
              records[3].parent_sprite_id == 11 && !records[3].stopped,
          "sprite Stop and Stop+Play annotate the correct records");

    MenuSwfAvmList no_capacity = {NULL, 0, 0, 0, 0};
    check(menu_swf_avm_collect(file, flen, &no_capacity, reason, sizeof reason) ==
              MENU_SWF_ERR_BUDGET && no_capacity.truncated,
          "AVM inventory capacity overflow is named");
}

typedef struct {
    uint8_t opcode;
    uint32_t count;
} ExpectedOpcode;

static const ExpectedOpcode expected_real[] = {
    {0x00,87},{0x07,53},{0x0b,79},{0x0c,21},{0x0d,17},{0x12,866},
    {0x17,1088},{0x1c,626},{0x1d,3},{0x26,3},{0x2b,2},{0x3a,16},
    {0x3c,12},{0x3d,47},{0x3e,224},{0x40,39},{0x42,59},{0x43,57},
    {0x44,2},{0x45,4},{0x47,98},{0x48,47},{0x49,377},{0x4c,151},
    {0x4e,2376},{0x4f,858},{0x50,34},{0x51,6},{0x52,892},{0x53,9},
    {0x54,14},{0x55,6},{0x60,23},{0x61,6},{0x62,1},{0x63,4},
    {0x64,6},{0x65,3},{0x66,62},{0x67,48},{0x69,13},{0x87,350},
    {0x88,25},{0x8e,328},{0x96,5503},{0x99,215},{0x9b,40},{0x9d,749}
};

static void test_real(const char *data_dir) {
    const char *archives[2] = {"Update.bsa", "Skyrim - Interface.bsa"};
    char path[1024];
    BsaArchive *archive = NULL;
    BsaMatch match;
    for (unsigned i = 0; i < 2; ++i) {
        int n = snprintf(path, sizeof path, "%s/%s", data_dir, archives[i]);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (bsa_open_path(path, NULL, &archive, NULL, 0) == BSA_OK) {
            if (bsa_find(archive, "interface/startmenu.swf", &match) == BSA_OK)
                break;
            bsa_close(archive);
            archive = NULL;
        }
    }
    if (!archive) {
        printf("real: skip (StartMenu.swf not found under %s)\n", data_dir);
        return;
    }
    uint32_t size = 0;
    char reason[BSA_REASON_MAX];
    check(bsa_unpacked_size(archive, &match.entry, &size, reason, sizeof reason) ==
              BSA_OK,
          "real StartMenu size");
    uint8_t *bytes = malloc(size ? size : 1);
    size_t got = 0;
    if (bytes)
        check(bsa_read(archive, &match.entry, bytes, size, &got, reason,
                       sizeof reason) == BSA_OK && got == size,
              "real StartMenu read");
    bsa_close(archive);
    if (!bytes) return;

    MenuSwfAvmRecord *items = calloc(MENU_SWF_AVM_MAX_RECORDS, sizeof *items);
    MenuSwfAvmList list = {items, MENU_SWF_AVM_MAX_RECORDS, 0, 0, 0};
    check(items != NULL, "real AVM inventory allocation");
    if (!items) { free(bytes); return; }
    MenuSwfStatus st = menu_swf_avm_collect(bytes, got, &list, reason,
                                            sizeof reason);
    check(st == MENU_SWF_OK, "real AVM inventory collect");
    if (st == MENU_SWF_OK) {
        printf("real: action_records=%u do_action=%u do_init_action=%u "
               "payload_bytes=%llu code_bytes=%llu opcodes=%llu unknown=%u\n",
               list.count, list.do_action, list.do_init_action,
               (unsigned long long)list.payload_bytes,
               (unsigned long long)list.code_bytes,
               (unsigned long long)list.opcode_count, list.unknown_opcodes);
        printf("real: controls stop=%u play=%u goto=%u goto_label=%u "
               "set_target=%u wait=%u push=%u get_variable=%u set_variable=%u "
               "functions=%u calls=%u not_executed=%u\n",
               list.stop_count, list.play_count, list.goto_count,
               list.goto_label_count, list.set_target_count, list.wait_count,
               list.push_count, list.get_variable_count,
               list.set_variable_count, list.function_count, list.call_count,
               list.not_executed_opcodes);
        check(list.count == 87 && list.do_action == 53 && list.do_init_action == 34 &&
                  list.payload_bytes == 85933 && list.code_bytes == 85865 &&
                  list.opcode_count == 15549 && list.unknown_opcodes == 0 &&
                  list.stop_count == 53 && list.play_count == 0 &&
                  list.goto_count == 0 && list.goto_label_count == 0 &&
                  list.set_target_count == 0 && list.wait_count == 0 &&
                  list.push_count == 5503 && list.get_variable_count == 626 &&
                  list.set_variable_count == 3 && list.function_count == 368 &&
                  list.call_count == 939,
              "exact real StartMenu AVM totals");
        int histogram_ok = 1;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t want = 0;
            for (size_t j = 0; j < sizeof expected_real / sizeof expected_real[0]; ++j)
                if (expected_real[j].opcode == i) want = expected_real[j].count;
            if (list.histogram[i] != want) histogram_ok = 0;
        }
        check(histogram_ok, "exact real StartMenu opcode histogram");
        for (size_t i = 0; i < sizeof expected_real / sizeof expected_real[0]; ++i) {
            uint8_t op = expected_real[i].opcode;
            printf("real: opcode 0x%02x %-18s count=%u\n", op,
                   menu_swf_avm_opcode_name(op), list.histogram[op]);
        }
        for (uint32_t i = 0; i < list.count; ++i) {
            for (uint32_t j = 0; j < list.items[i].control_count; ++j)
                printf("real: control sprite=%u action_frame=%u opcode=0x%02x %s frame=%u\n",
                       list.items[i].sprite_id, list.items[i].frame_index,
                       list.items[i].controls[j].opcode,
                       menu_swf_avm_opcode_name(list.items[i].controls[j].opcode),
                       list.items[i].controls[j].frame);
        }

        MenuSwfDisplaySummary *display = malloc(sizeof *display);
        MenuSwfDisplayListRecord *records = malloc(
            MENU_SWF_DISPLAY_LIST_MAX_RECORDS * sizeof *records);
        check(display && records, "real display-control allocation");
        if (display && records) {
            st = menu_swf_display_summary(bytes, got, display, reason,
                                          sizeof reason);
            MenuSwfDisplayListResult built;
            MenuSwfStatus ds = st == MENU_SWF_OK
                ? menu_swf_display_list_build(bytes, got, records,
                                              MENU_SWF_DISPLAY_LIST_MAX_RECORDS,
                                              &built, reason, sizeof reason)
                : st;
            check(ds == MENU_SWF_OK, "real display list for AVM controls");
            if (ds == MENU_SWF_OK) {
                MenuSwfAvmTimeline timelines[MENU_SWF_AVM_MAX_TIMELINES];
                uint32_t timeline_count = 1;
                timelines[0].sprite_id = 0;
                timelines[0].frame_count = display->top.frame_count;
                for (uint32_t i = 0; i < display->stored_sprites; ++i) {
                    timelines[timeline_count].sprite_id = display->sprites[i].sprite_id;
                    timelines[timeline_count].frame_count = display->sprites[i].declared_frames;
                    ++timeline_count;
                }
                MenuSwfAvmDisplayResult applied;
                st = menu_swf_avm_apply_display_controls(
                    &list, records, built.record_count, timelines,
                    timeline_count, &applied, reason, sizeof reason);
                printf("real: display_control stopped=%u playing=%u refused=%u\n",
                       applied.stopped_records, applied.playing_records,
                       applied.refused_controls);
                check(st == MENU_SWF_OK && applied.stopped_records > 0 &&
                          applied.refused_controls == 0,
                      "real Stop actions annotate display records safely");
            }
        }
        free(records);
        free(display);
    } else {
        printf("real: AVM collect failed: %s\n", reason);
    }
    free(items);
    free(bytes);
}

int main(int argc, char **argv) {
    printf("menu SWF AVM/AS2 inventory tests\n");
    test_parse_controls();
    test_function_scope_unknown_and_errors();
    test_collect_and_display();
    const char *real = argc > 1 ? argv[1] : getenv("SKYRIM_N3DS_REAL_DATA");
    if (real && real[0]) {
        if (!strcmp(real, "1")) real = "F:\\Skyrim\\Data";
        test_real(real);
    } else {
        printf("real: skip (pass a Data directory or SKYRIM_N3DS_REAL_DATA=1)\n");
    }
    printf("menu SWF AVM: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
