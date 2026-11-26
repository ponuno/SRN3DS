#include "menu_swf_avm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void avm_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static void first_refusal(MenuSwfAvmRecord *record, char *reason, size_t cap,
                          const char *fmt, ...) {
    if (!record->first_refusal[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(record->first_refusal, sizeof record->first_refusal, fmt, ap);
        va_end(ap);
        record->first_refusal[sizeof record->first_refusal - 1] = 0;
    }
    if (reason && cap && !reason[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(reason, cap, fmt, ap);
        va_end(ap);
        reason[cap - 1] = 0;
    }
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int skip_string(const uint8_t *p, size_t len, size_t *off) {
    size_t at = *off;
    while (at < len && p[at]) ++at;
    if (at >= len) return -1;
    *off = at + 1;
    return 0;
}

static int known_low(uint8_t op) {
    return (op >= 0x04 && op <= 0x15) || op == 0x17 || op == 0x18 ||
           op == 0x1c || op == 0x1d ||
           (op >= 0x20 && op <= 0x2d) ||
           (op >= 0x30 && op <= 0x37) ||
           (op >= 0x3a && op <= 0x55) ||
           (op >= 0x60 && op <= 0x69);
}

static int known_high(uint8_t op) {
    switch (op) {
    case 0x81: case 0x83: case 0x87: case 0x88: case 0x89:
    case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e:
    case 0x8f: case 0x94: case 0x96: case 0x99: case 0x9a:
    case 0x9b: case 0x9d: case 0x9e: case 0x9f:
        return 1;
    default:
        return 0;
    }
}

const char *menu_swf_avm_opcode_name(uint8_t opcode) {
    switch (opcode) {
    case 0x00: return "End";
    case 0x04: return "NextFrame";
    case 0x05: return "PreviousFrame";
    case 0x06: return "Play";
    case 0x07: return "Stop";
    case 0x08: return "ToggleQuality";
    case 0x09: return "GetURL";
    case 0x0a: return "Add";
    case 0x0b: return "Subtract";
    case 0x0c: return "Multiply";
    case 0x0d: return "Divide";
    case 0x0e: return "Equals";
    case 0x0f: return "Less";
    case 0x10: return "And";
    case 0x11: return "Or";
    case 0x12: return "Not";
    case 0x13: return "StringEquals";
    case 0x14: return "StringLength";
    case 0x15: return "StringExtract";
    case 0x17: return "Pop";
    case 0x18: return "ToInteger";
    case 0x1c: return "GetVariable";
    case 0x1d: return "SetVariable";
    case 0x20: return "SetTarget2";
    case 0x21: return "StringAdd";
    case 0x22: return "GetProperty";
    case 0x23: return "SetProperty";
    case 0x24: return "CloneSprite";
    case 0x25: return "RemoveSprite";
    case 0x26: return "Trace";
    case 0x27: return "StartDrag";
    case 0x28: return "EndDrag";
    case 0x29: return "StringLess";
    case 0x2a: return "Throw";
    case 0x2b: return "CastOp";
    case 0x2c: return "ImplementsOp";
    case 0x2d: return "FsCommand2";
    case 0x30: return "RandomNumber";
    case 0x31: return "MbStringLength";
    case 0x32: return "CharToAscii";
    case 0x33: return "AsciiToChar";
    case 0x34: return "GetTime";
    case 0x35: return "MbStringExtract";
    case 0x36: return "MbCharToAscii";
    case 0x37: return "MbAsciiToChar";
    case 0x3a: return "Delete";
    case 0x3b: return "Delete2";
    case 0x3c: return "DefineLocal";
    case 0x3d: return "CallFunction";
    case 0x3e: return "Return";
    case 0x3f: return "Modulo";
    case 0x40: return "NewObject";
    case 0x41: return "DefineLocal2";
    case 0x42: return "InitArray";
    case 0x43: return "InitObject";
    case 0x44: return "TypeOf";
    case 0x45: return "TargetPath";
    case 0x46: return "Enumerate";
    case 0x47: return "Add2";
    case 0x48: return "Less2";
    case 0x49: return "Equals2";
    case 0x4a: return "ToNumber";
    case 0x4b: return "ToString";
    case 0x4c: return "PushDuplicate";
    case 0x4d: return "StackSwap";
    case 0x4e: return "GetMember";
    case 0x4f: return "SetMember";
    case 0x50: return "Increment";
    case 0x51: return "Decrement";
    case 0x52: return "CallMethod";
    case 0x53: return "NewMethod";
    case 0x54: return "InstanceOf";
    case 0x55: return "Enumerate2";
    case 0x60: return "BitAnd";
    case 0x61: return "BitOr";
    case 0x62: return "BitXor";
    case 0x63: return "BitLShift";
    case 0x64: return "BitRShift";
    case 0x65: return "BitURShift";
    case 0x66: return "StrictEquals";
    case 0x67: return "Greater";
    case 0x68: return "StringGreater";
    case 0x69: return "Extends";
    case 0x81: return "GotoFrame";
    case 0x83: return "GetURL";
    case 0x87: return "StoreRegister";
    case 0x88: return "ConstantPool";
    case 0x89: return "StrictMode";
    case 0x8a: return "WaitForFrame";
    case 0x8b: return "SetTarget";
    case 0x8c: return "GotoLabel";
    case 0x8d: return "WaitForFrame2";
    case 0x8e: return "DefineFunction2";
    case 0x8f: return "Try";
    case 0x94: return "With";
    case 0x96: return "Push";
    case 0x99: return "Jump";
    case 0x9a: return "GetURL2";
    case 0x9b: return "DefineFunction";
    case 0x9d: return "If";
    case 0x9e: return "Call";
    case 0x9f: return "GotoFrame2";
    default: return "Unknown";
    }
}

typedef struct {
    MenuSwfAvmRecord *record;
    uint32_t *histogram;
    uint32_t instructions;
    char *reason;
    size_t reason_cap;
} DecodeContext;

static MenuSwfStatus decode_fail(DecodeContext *ctx, MenuSwfStatus status,
                                 const char *fmt, ...) {
    char text[BSA_REASON_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    text[sizeof text - 1] = 0;
    first_refusal(ctx->record, ctx->reason, ctx->reason_cap, "%s", text);
    return status;
}

static MenuSwfStatus count_opcode(DecodeContext *ctx, uint8_t op) {
    if (ctx->histogram) ++ctx->histogram[op];
    ++ctx->record->opcode_count;
    if (++ctx->instructions > MENU_SWF_AVM_MAX_INSTRUCTIONS)
        return decode_fail(ctx, MENU_SWF_ERR_BUDGET,
                           "swf avm: instruction limit %u exceeded",
                           MENU_SWF_AVM_MAX_INSTRUCTIONS);
    return MENU_SWF_OK;
}

static MenuSwfStatus add_control(DecodeContext *ctx, uint8_t op, uint16_t frame) {
    MenuSwfAvmRecord *r = ctx->record;
    if (r->control_count >= MENU_SWF_AVM_MAX_CONTROLS)
        return decode_fail(ctx, MENU_SWF_ERR_BUDGET,
                           "swf avm: frame-control limit %u exceeded",
                           MENU_SWF_AVM_MAX_CONTROLS);
    r->controls[r->control_count].opcode = op;
    r->controls[r->control_count].frame = frame;
    ++r->control_count;
    return MENU_SWF_OK;
}

static void note_not_executed(DecodeContext *ctx, const char *what) {
    ++ctx->record->refused_count;
    first_refusal(ctx->record, ctx->reason, ctx->reason_cap,
                  "swf avm: %s is inventoried but not executed", what);
}

static void note_low_class(DecodeContext *ctx, uint8_t op) {
    MenuSwfAvmRecord *r = ctx->record;
    switch (op) {
    case 0x06: ++r->play_count; break;
    case 0x07: ++r->stop_count; break;
    case 0x09: note_not_executed(ctx, "GetURL"); break;
    case 0x1c:
        ++r->get_variable_count;
        note_not_executed(ctx, "GetVariable");
        break;
    case 0x1d:
        ++r->set_variable_count;
        note_not_executed(ctx, "SetVariable");
        break;
    case 0x20:
        ++r->set_target_count;
        note_not_executed(ctx, "SetTarget2");
        break;
    case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
    case 0x27: case 0x28: case 0x2a: case 0x2d:
    case 0x3a: case 0x3b: case 0x3c: case 0x40: case 0x41:
    case 0x42: case 0x43: case 0x4e: case 0x4f:
        note_not_executed(ctx, menu_swf_avm_opcode_name(op));
        break;
    case 0x3d: case 0x52:
        ++r->call_count;
        note_not_executed(ctx, menu_swf_avm_opcode_name(op));
        break;
    default:
        break;
    }
}

/* MenuSwfAvmList is defined below the decoder helpers. */
typedef struct {
    MenuSwfAvmList *list;
    uint32_t tags_seen;
} CollectContext;

static MenuSwfStatus decode_stream(DecodeContext *ctx, const uint8_t *p,
                                   size_t len, unsigned depth,
                                   int allow_controls, size_t *consumed);

static MenuSwfStatus validate_push(DecodeContext *ctx, const uint8_t *p,
                                   size_t len) {
    size_t off = 0;
    while (off < len) {
        uint8_t type = p[off++];
        size_t need = 0;
        switch (type) {
        case 0: if (skip_string(p, len, &off)) return -1; continue;
        case 1: need = 4; break;
        case 2: case 3: need = 0; break;
        case 4: case 5: case 8: need = 1; break;
        case 6: need = 8; break;
        case 7: need = 4; break;
        case 9: need = 2; break;
        default:
            return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                               "swf avm: unknown Push value type %u", type);
        }
        if (need > len - off)
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: truncated Push value type %u", type);
        off += need;
    }
    return MENU_SWF_OK;
}

static MenuSwfStatus validate_constant_pool(DecodeContext *ctx,
                                            const uint8_t *p, size_t len) {
    if (len < 2)
        return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                           "swf avm: truncated ConstantPool");
    size_t off = 2;
    uint16_t count = get_u16(p);
    for (uint32_t i = 0; i < count; ++i)
        if (skip_string(p, len, &off))
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: truncated ConstantPool string %u", i);
    if (off != len)
        return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                           "swf avm: ConstantPool has %zu trailing bytes",
                           len - off);
    return MENU_SWF_OK;
}

static MenuSwfStatus parse_function_header(DecodeContext *ctx,
                                           const uint8_t *p, size_t len,
                                           int version2, size_t *header_len,
                                           uint16_t *code_size) {
    size_t off = 0;
    if (skip_string(p, len, &off) || len - off < 2)
        return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                           "swf avm: truncated DefineFunction header");
    uint16_t params = get_u16(p + off);
    off += 2;
    if (version2) {
        if (len - off < 3)
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: truncated DefineFunction2 flags");
        off += 3; /* register count + two flag bytes */
    }
    for (uint32_t i = 0; i < params; ++i) {
        if (version2) {
            if (off >= len)
                return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                                   "swf avm: truncated function register");
            ++off;
        }
        if (skip_string(p, len, &off))
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: truncated function parameter %u", i);
    }
    if (len - off < 2)
        return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                           "swf avm: truncated function code size");
    *code_size = get_u16(p + off);
    off += 2;
    if (off != len)
        return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                           "swf avm: DefineFunction header has %zu trailing bytes",
                           len - off);
    *header_len = off;
    return MENU_SWF_OK;
}

static MenuSwfStatus decode_try(DecodeContext *ctx, const uint8_t *p,
                                size_t len, unsigned depth) {
    if (len < 8)
        return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                           "swf avm: truncated Try header");
    uint8_t flags = p[0];
    uint16_t try_size = get_u16(p + 1);
    uint16_t catch_size = get_u16(p + 3);
    uint16_t finally_size = get_u16(p + 5);
    size_t off = 8;
    if (flags & 0x04) {
        if (off >= len)
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: truncated Try register");
        ++off;
    } else if (skip_string(p, len, &off)) {
        return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                           "swf avm: truncated Try catch name");
    }
    uint64_t total = (uint64_t)off + try_size + catch_size + finally_size;
    if (total != len)
        return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                           "swf avm: Try block sizes do not match action length");
    note_not_executed(ctx, "Try");
    size_t used = 0;
    MenuSwfStatus st = decode_stream(ctx, p + off, try_size, depth + 1, 0, &used);
    if (st != MENU_SWF_OK && st != MENU_SWF_ERR_NOTIMPL) return st;
    st = decode_stream(ctx, p + off + try_size, catch_size, depth + 1, 0, &used);
    if (st != MENU_SWF_OK && st != MENU_SWF_ERR_NOTIMPL) return st;
    st = decode_stream(ctx, p + off + try_size + catch_size, finally_size,
                       depth + 1, 0, &used);
    if (st != MENU_SWF_OK && st != MENU_SWF_ERR_NOTIMPL) return st;
    return MENU_SWF_OK;
}

static MenuSwfStatus decode_stream(DecodeContext *ctx, const uint8_t *p,
                                   size_t len, unsigned depth,
                                   int allow_controls, size_t *consumed) {
    if (depth > MENU_SWF_AVM_MAX_NESTING)
        return decode_fail(ctx, MENU_SWF_ERR_TAG,
                           "swf avm: nesting exceeds %u",
                           MENU_SWF_AVM_MAX_NESTING);
    size_t off = 0;
    while (off < len) {
        uint8_t op = p[off++];
        MenuSwfStatus st = count_opcode(ctx, op);
        if (st != MENU_SWF_OK) return st;
        if (op == 0) {
            *consumed = off;
            ctx->record->end_seen = 1;
            return MENU_SWF_OK;
        }
        if (op < 0x80) {
            if (!known_low(op)) {
                ++ctx->record->unknown_count;
                first_refusal(ctx->record, ctx->reason, ctx->reason_cap,
                              "swf avm: unknown opcode 0x%02x has no length field",
                              op);
                *consumed = off;
                return MENU_SWF_ERR_NOTIMPL;
            }
            note_low_class(ctx, op);
            if (op == 0x06) {
                if (allow_controls && add_control(ctx, op, 0)) return MENU_SWF_ERR_BUDGET;
            } else if (op == 0x07) {
                if (allow_controls && add_control(ctx, op, 0)) return MENU_SWF_ERR_BUDGET;
            } else if (op == 0x09) {
                if (skip_string(p, len, &off) || skip_string(p, len, &off))
                    return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                                       "swf avm: truncated GetURL");
            } else if (op == 0x20 && allow_controls) {
                if (add_control(ctx, op, 0)) return MENU_SWF_ERR_BUDGET;
            }
            continue;
        }

        if (len - off < 2)
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: truncated length for opcode 0x%02x", op);
        uint16_t n = get_u16(p + off);
        off += 2;
        if (n > len - off)
            return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                               "swf avm: opcode 0x%02x length %u exceeds stream",
                               op, n);
        const uint8_t *data = p + off;
        size_t end = off + n;
        if (!known_high(op)) {
            ++ctx->record->unknown_count;
            first_refusal(ctx->record, ctx->reason, ctx->reason_cap,
                          "swf avm: unknown opcode 0x%02x length %u skipped",
                          op, n);
            off = end;
            continue;
        }

        switch (op) {
        case 0x81:
            if (n != 2)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: GotoFrame length is not 2");
            ++ctx->record->goto_frame_count;
            if (allow_controls && add_control(ctx, op, get_u16(data)))
                return MENU_SWF_ERR_BUDGET;
            break;
        case 0x83: {
            size_t q = 0;
            if (skip_string(data, n, &q) || skip_string(data, n, &q) || q != n)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: malformed GetURL");
            note_not_executed(ctx, "GetURL");
            break;
        }
        case 0x87:
            if (n != 1)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: StoreRegister length is not 1");
            note_not_executed(ctx, "StoreRegister");
            break;
        case 0x88:
            st = validate_constant_pool(ctx, data, n);
            if (st != MENU_SWF_OK) return st;
            break;
        case 0x89:
            if (n != 0)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: StrictMode has payload bytes");
            break;
        case 0x8a:
            if (n != 3)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: WaitForFrame length is not 3");
            ++ctx->record->wait_count;
            note_not_executed(ctx, "WaitForFrame");
            if (allow_controls && add_control(ctx, op, get_u16(data)))
                return MENU_SWF_ERR_BUDGET;
            break;
        case 0x8b:
        case 0x8c: {
            size_t q = 0;
            if (skip_string(data, n, &q) || q != n)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: malformed target/label string");
            if (op == 0x8b) {
                ++ctx->record->set_target_count;
                note_not_executed(ctx, "SetTarget");
            } else {
                ++ctx->record->goto_label_count;
                note_not_executed(ctx, "GotoLabel");
            }
            if (allow_controls && add_control(ctx, op, 0))
                return MENU_SWF_ERR_BUDGET;
            break;
        }
        case 0x8d:
            if (n != 1)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: WaitForFrame2 length is not 1");
            ++ctx->record->wait_count;
            note_not_executed(ctx, "WaitForFrame2");
            if (allow_controls && add_control(ctx, op, 0))
                return MENU_SWF_ERR_BUDGET;
            break;
        case 0x8e:
        case 0x9b: {
            size_t header = 0;
            uint16_t code_size = 0;
            ++ctx->record->function_count;
            note_not_executed(ctx, op == 0x8e ? "DefineFunction2" : "DefineFunction");
            st = parse_function_header(ctx, data, n, op == 0x8e, &header,
                                       &code_size);
            if (st != MENU_SWF_OK) return st;
            if (code_size > len - end)
                return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                                   "swf avm: function body exceeds stream");
            size_t used = 0;
            st = decode_stream(ctx, p + end, code_size, depth + 1, 0, &used);
            if (st != MENU_SWF_OK && st != MENU_SWF_ERR_NOTIMPL) return st;
            end += code_size;
            break;
        }
        case 0x8f:
            st = decode_try(ctx, data, n, depth);
            if (st != MENU_SWF_OK) return st;
            break;
        case 0x94:
            if (n != 2)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: With length is not 2");
            note_not_executed(ctx, "With");
            {
                uint16_t code_size = get_u16(data);
                if (code_size > len - end)
                    return decode_fail(ctx, MENU_SWF_ERR_TRUNCATED,
                                       "swf avm: With body exceeds stream");
                size_t used = 0;
                st = decode_stream(ctx, p + end, code_size, depth + 1, 0, &used);
                if (st != MENU_SWF_OK && st != MENU_SWF_ERR_NOTIMPL) return st;
                end += code_size;
            }
            break;
        case 0x96:
            ++ctx->record->push_count;
            note_not_executed(ctx, "Push");
            st = validate_push(ctx, data, n);
            if (st != MENU_SWF_OK) return st;
            break;
        case 0x99:
            if (n != 2)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: Jump length is not 2");
            note_not_executed(ctx, "Jump");
            break;
        case 0x9a:
            if (n != 1)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: GetURL2 length is not 1");
            note_not_executed(ctx, "GetURL2");
            break;
        case 0x9d:
            if (n != 2)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: If length is not 2");
            note_not_executed(ctx, "If");
            break;
        case 0x9e:
            if (n != 0)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: Call has payload bytes");
            ++ctx->record->call_count;
            note_not_executed(ctx, "Call");
            break;
        case 0x9f:
            if (n != 1)
                return decode_fail(ctx, MENU_SWF_ERR_FORMAT,
                                   "swf avm: GotoFrame2 length is not 1");
            ++ctx->record->goto_frame2_count;
            note_not_executed(ctx, "GotoFrame2");
            if (allow_controls && add_control(ctx, op, data[0]))
                return MENU_SWF_ERR_BUDGET;
            break;
        default:
            break;
        }
        off = end;
    }
    *consumed = off;
    return MENU_SWF_OK;
}

static MenuSwfStatus parse_record_hist(
    const uint8_t *payload, size_t len, uint8_t tag_code,
    MenuSwfAvmRecord *out, uint32_t *histogram,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!payload || !out || (tag_code != 12 && tag_code != 59))
        return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->tag_code = tag_code;
    out->payload_bytes = (uint32_t)len;
    const uint8_t *code = payload;
    size_t code_len = len;
    if (tag_code == 59) {
        if (len < 2) {
            avm_reason(reason, reason_cap, "swf avm: truncated DoInitAction sprite id");
            return MENU_SWF_ERR_TRUNCATED;
        }
        out->sprite_id = get_u16(payload);
        code += 2;
        code_len -= 2;
    }
    out->code_bytes = (uint32_t)code_len;
    DecodeContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.record = out;
    ctx.histogram = histogram;
    ctx.reason = reason;
    ctx.reason_cap = reason_cap;
    size_t consumed = 0;
    MenuSwfStatus st = decode_stream(&ctx, code, code_len, 0, 1, &consumed);
    out->trailing_bytes = (uint32_t)(code_len - consumed);
    if (st == MENU_SWF_OK && out->unknown_count)
        st = MENU_SWF_ERR_NOTIMPL;
    if (st == MENU_SWF_ERR_NOTIMPL && reason && reason_cap && !reason[0])
        avm_reason(reason, reason_cap, "swf avm: %s",
                   out->first_refusal[0] ? out->first_refusal : "unknown opcode");
    return st;
}

MenuSwfStatus menu_swf_avm_parse_record(
    const uint8_t *payload, size_t len, uint8_t tag_code,
    MenuSwfAvmRecord *out, char *reason, size_t reason_cap) {
    return parse_record_hist(payload, len, tag_code, out, NULL, reason,
                             reason_cap);
}

static MenuSwfStatus collect_tags(CollectContext *ctx, const uint8_t *p,
                                  size_t len, uint16_t owner,
                                  uint32_t frame_index, unsigned depth) {
    if (depth > MENU_SWF_MAX_SPRITE_DEPTH) {
        avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                   "swf avm: sprite depth exceeds %u", MENU_SWF_MAX_SPRITE_DEPTH);
        return MENU_SWF_ERR_TAG;
    }
    size_t off = 0;
    while (off < len) {
        if (len - off < 2) {
            avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                       "swf avm: truncated tag header");
            return MENU_SWF_ERR_TRUNCATED;
        }
        uint16_t header = get_u16(p + off);
        off += 2;
        unsigned code = header >> 6;
        uint32_t n = header & 0x3fu;
        if (n == 0x3fu) {
            if (len - off < 4) {
                avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                           "swf avm: truncated long tag header");
                return MENU_SWF_ERR_TRUNCATED;
            }
            n = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
                ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
            off += 4;
        }
        if (n > len - off) {
            avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                       "swf avm: tag %u length exceeds stream", code);
            return MENU_SWF_ERR_TAG;
        }
        const uint8_t *payload = p + off;
        off += n;
        if (++ctx->tags_seen > MENU_SWF_MAX_TAGS) {
            avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                       "swf avm: tag limit %u exceeded", MENU_SWF_MAX_TAGS);
            return MENU_SWF_ERR_BUDGET;
        }
        if (code == 0) {
            if (n) {
                avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                           "swf avm: End tag has payload bytes");
                return MENU_SWF_ERR_FORMAT;
            }
            return MENU_SWF_OK;
        }
        if (code == 1) {
            ++frame_index;
        } else if (code == 39) {
            if (n < 4) {
                avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
                           "swf avm: short DefineSprite");
                return MENU_SWF_ERR_FORMAT;
            }
            uint16_t sprite_id = get_u16(payload);
            MenuSwfStatus st = collect_tags(ctx, payload + 4, n - 4, sprite_id,
                                            0, depth + 1);
            if (st != MENU_SWF_OK) return st;
        } else if (code == 12 || code == 59) {
            MenuSwfAvmList *list = ctx->list;
            ++list->total;
            if (list->count >= list->capacity) {
                list->truncated = 1;
                avm_reason(list->first_refusal, sizeof list->first_refusal,
                           "swf avm: record capacity %u exceeded", list->capacity);
                return MENU_SWF_ERR_BUDGET;
            }
            MenuSwfAvmRecord record;
            char reason[BSA_REASON_MAX] = "";
            MenuSwfStatus st = parse_record_hist(
                payload, n, (uint8_t)code, &record, list->histogram, reason,
                sizeof reason);
            if (st != MENU_SWF_OK && st != MENU_SWF_ERR_NOTIMPL) {
                snprintf(list->first_refusal, sizeof list->first_refusal,
                         "%s", reason);
                return st;
            }
            if (code == 12) record.sprite_id = owner;
            record.frame_index = code == 59 ? 0 : frame_index;
            if (st == MENU_SWF_ERR_NOTIMPL || record.unknown_count) {
                list->unknown_opcodes += record.unknown_count;
                list->has_refusal = 1;
                if (!list->first_refusal[0])
                    snprintf(list->first_refusal, sizeof list->first_refusal,
                             "%s", reason);
            }
            list->items[list->count++] = record;
            if (code == 12) ++list->do_action;
            else ++list->do_init_action;
            list->payload_bytes += record.payload_bytes;
            list->code_bytes += record.code_bytes;
            list->opcode_count += record.opcode_count;
            list->not_executed_opcodes += record.refused_count;
            list->stop_count += record.stop_count;
            list->play_count += record.play_count;
            list->goto_count += record.goto_frame_count + record.goto_frame2_count;
            list->goto_label_count += record.goto_label_count;
            list->set_target_count += record.set_target_count;
            list->wait_count += record.wait_count;
            list->push_count += record.push_count;
            list->get_variable_count += record.get_variable_count;
            list->set_variable_count += record.set_variable_count;
            list->function_count += record.function_count;
            list->call_count += record.call_count;
            if (record.refused_count) list->has_refusal = 1;
            if (!list->first_refusal[0] && record.first_refusal[0])
                snprintf(list->first_refusal, sizeof list->first_refusal,
                         "%s", record.first_refusal);
        }
    }
    avm_reason(ctx->list->first_refusal, sizeof ctx->list->first_refusal,
               "swf avm: tag stream has no End tag");
    return MENU_SWF_ERR_END;
}

MenuSwfStatus menu_swf_avm_collect(
    const uint8_t *data, size_t size, MenuSwfAvmList *list,
    char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!list || (list->capacity && !list->items))
        return MENU_SWF_ERR_ARGUMENT;
    MenuSwfAvmRecord *items = list->items;
    uint32_t capacity = list->capacity;
    memset(list, 0, sizeof *list);
    list->items = items;
    list->capacity = capacity;
    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;
    CollectContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.list = list;
    st = collect_tags(&ctx, view.body + view.tag_offset,
                      view.body_len - view.tag_offset, 0, 0, 0);
    menu_swf_view_close(&view);
    if (st != MENU_SWF_OK && reason && reason_cap && !reason[0])
        avm_reason(reason, reason_cap, "%s",
                   list->first_refusal[0] ? list->first_refusal : "swf avm: collect failed");
    return st;
}

MenuSwfStatus menu_swf_avm_apply_record(
    const MenuSwfAvmRecord *record, uint32_t frame_count,
    MenuSwfAvmControlState *state, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!record || !state) return MENU_SWF_ERR_ARGUMENT;
    MenuSwfStatus status = MENU_SWF_OK;
    for (uint32_t i = 0; i < record->control_count; ++i) {
        const MenuSwfAvmControlOp *op = &record->controls[i];
        switch (op->opcode) {
        case 0x07:
            state->stopped = 1;
            ++state->applied;
            break;
        case 0x06:
            state->stopped = 0;
            ++state->applied;
            break;
        case 0x81:
            if (op->frame < frame_count) {
                state->frame = op->frame;
                ++state->applied;
            } else {
                ++state->refused;
                status = MENU_SWF_ERR_NOTIMPL;
                if (reason && reason_cap && !reason[0])
                    avm_reason(reason, reason_cap,
                               "swf avm: GotoFrame %u is outside %u frames",
                               op->frame, frame_count);
            }
            break;
        default:
            ++state->refused;
            status = MENU_SWF_ERR_NOTIMPL;
            if (reason && reason_cap && !reason[0])
                avm_reason(reason, reason_cap,
                           "swf avm: %s is not executed by safe frame control",
                           menu_swf_avm_opcode_name(op->opcode));
            break;
        }
    }
    return status;
}

MenuSwfStatus menu_swf_avm_apply_display_controls(
    const MenuSwfAvmList *list,
    MenuSwfDisplayListRecord *records, uint32_t record_count,
    const MenuSwfAvmTimeline *timelines, uint32_t timeline_count,
    MenuSwfAvmDisplayResult *out, char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!list || (record_count && !records) ||
        (timeline_count && !timelines) || !out ||
        timeline_count > MENU_SWF_AVM_MAX_TIMELINES)
        return MENU_SWF_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    MenuSwfAvmControlState states[MENU_SWF_AVM_MAX_TIMELINES];
    memset(states, 0, sizeof states);
    for (uint32_t i = 0; i < list->count; ++i) {
        const MenuSwfAvmRecord *record = &list->items[i];
        for (uint32_t t = 0; t < timeline_count; ++t) {
            if (timelines[t].sprite_id != record->sprite_id) continue;
            if (record->frame_index != states[t].frame) continue;
            char local_reason[BSA_REASON_MAX] = "";
            MenuSwfStatus st = menu_swf_avm_apply_record(
                record, timelines[t].frame_count, &states[t], local_reason,
                sizeof local_reason);
            if (st == MENU_SWF_ERR_NOTIMPL) {
                ++out->refused_controls;
                if (reason && reason_cap && !reason[0])
                    avm_reason(reason, reason_cap, "%s", local_reason);
            } else if (st != MENU_SWF_OK) {
                return st;
            }
        }
    }
    for (uint32_t i = 0; i < record_count; ++i) {
        uint16_t owner = records[i].parent_sprite_id;
        uint32_t t;
        for (t = 0; t < timeline_count; ++t)
            if (timelines[t].sprite_id == owner) break;
        if (t == timeline_count) {
            ++out->refused_controls;
            records[i].stopped = 0;
            records[i].frame_index = 0;
            if (reason && reason_cap && !reason[0])
                avm_reason(reason, reason_cap,
                           "swf avm: no frame count for display timeline %u", owner);
            continue;
        }
        records[i].stopped = states[t].stopped;
        records[i].frame_index = states[t].frame;
        if (records[i].stopped) ++out->stopped_records;
        else ++out->playing_records;
    }
    return MENU_SWF_OK;
}
