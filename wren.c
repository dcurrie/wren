/** @file wren.h
*
* @brief Wren interpreter
*
* @par
* @copyright Copyright (c) 2007 Darius Bacon <darius@wry.me>
* @copyright Copyright (c) 2018 Doug Currie, Londonderry, NH, USA
* @note See LICENSE file for licensing terms.
*/

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "wren.h" /* Configuration & API */

/* More Configuration */

enum {
    /* Capacity in bytes. */
    store_capacity = 4096,

    /* True iff voluminous tracing is wanted. */
    loud = 0,
};

/* Accessors for unaligned storage in dictionary and code spaces */

#if WREN_UNALIGNED_ACCESS_OK

static inline uint16_t fetch_2u (const uint8_t *p)
{
    return *(uint16_t *)p;
}

static inline int16_t fetch_2i (const uint8_t *p)
{
    return *(int16_t *)p;
}

static inline wValue fetch_wV (const uint8_t *p)
{
    return *(wValue *)p;
}

static inline apply_t fetch_wP (const uint8_t *p)
{
    return *(apply_t *)p;
}

static inline void write_2i (uint8_t *p, const int16_t v)
{
    *(int16_t *)p = v;
}

static inline void write_2u (uint8_t *p, const uint16_t v)
{
    *(uint16_t *)p = v;
}

static inline void write_wV (uint8_t *p, const wValue v)
{
    *(wValue *)p = v;
}

static inline void write_wP (uint8_t *p, const apply_t v)
{
    *(apply_t *)p = v;
}

#else

// Unaligned access is not supported by hardware.
// It looks a little silly using memcpy() here, but it has a big advantage:
// with sufficient optimization, e.g., -Os, the compiler will expand it inline 
// using a simple (and hopfully optimal) instruction sequence. Together with
// the union, this code uses no Undefined Behavior as long as the data are 
// written and read with the matching pair write_XX and fetch_XX.

static inline uint16_t fetch_2u (const uint8_t *p)
{
    union { uint16_t v; uint8_t b[sizeof(uint16_t)]; } u;
    memcpy(u.b, p, sizeof(uint16_t));
    return u.v;
}

static inline int16_t fetch_2i (const uint8_t *p)
{
    union { int16_t v; uint8_t b[sizeof(int16_t)]; } u;
    memcpy(u.b, p, sizeof(int16_t));
    return u.v;
}

static inline wValue fetch_wV (const uint8_t *p)
{
    union { wValue v; uint8_t b[sizeof(wValue)]; } u;
    memcpy(u.b, p, sizeof(wValue));
    return u.v;
}

static inline apply_t fetch_wP (const uint8_t *p)
{
    union { apply_t v; uint8_t b[sizeof(apply_t)]; } u;
    memcpy(u.b, p, sizeof(apply_t));
    return u.v;
}

static inline void write_2u (uint8_t *p, const uint16_t v)
{
    union { uint16_t v; uint8_t b[sizeof(uint16_t)]; } u;
    u.v = v;
    memcpy(p, u.b, sizeof(uint16_t));
}

static inline void write_2i (uint8_t *p, const int16_t v)
{
    union { int16_t v; uint8_t b[sizeof(int16_t)]; } u;
    u.v = v;
    memcpy(p, u.b, sizeof(int16_t));
}

static inline void write_wV (uint8_t *p, const wValue v)
{
    union { wValue v; uint8_t b[sizeof(wValue)]; } u;
    u.v = v;
    memcpy(p, u.b, sizeof(wValue));
}

static inline void write_wP (uint8_t *p, const apply_t v)
{
    union { apply_t v; uint8_t b[sizeof(apply_t)]; } u;
    u.v = v;
    memcpy(p, u.b, sizeof(apply_t));
}

#endif


/* Error state */

static const char *complaint = NULL;

static void complain (const char *msg)
{
    if (!complaint)
        complaint = msg;
}

/* Main data store in RAM

   Most of the memory we use is doled out of one block.

   From the top, growing downwards, is a dictionary: a stack of 
   header/name pairs. The header distinguishes the kind of name and
   what address it denotes, along with the length of the name.

   From the bottom, growing upwards, are the bindings of the names:
   the code, for procedures, or the data cell, for globals. (Locals
   denote an offset in a transient stack frame. We'd have interleaved
   the dictionary headers with the values, like in Forth, except the
   entries for locals would get in the way while we're compiling the
   body of a procedure; moving all of the headers out of the way was
   the simplest solution.)

   At runtime, the stack grows down from the bottom of the dictionary
   (but wValue-aligned). 
   */

static uint8_t the_store[store_capacity];
#define store_end  (the_store + store_capacity)

typedef enum { a_primitive, a_procedure, a_global, a_local, a_cfunction } NameKind;
typedef struct Header Header;
struct Header {
    uint16_t binding;   // or for primintives, uint8_t arity; uint8_t opcode
    uint8_t  kind_lnm1; // (kind << 4) | (name_length - 1)
    uint8_t  name[0];
} __attribute__((packed));  /* XXX gcc dependency */

#define PRIM_HEADER(opcode, arity, name_length) \
    (uint8_t )(arity), (uint8_t )(opcode), \
    (uint8_t )(a_primitive << 4) | (((name_length) - 1u) & 0xfu)

static inline NameKind get_header_kind (const uint8_t *p_header)
{
    return (NameKind )((((Header *)p_header)->kind_lnm1) >> 4);
}

static inline uint8_t get_header_name_length (const uint8_t *p_header)
{
    return ((((Header *)p_header)->kind_lnm1) & 0xfu) + 1u;
}

static inline uint16_t get_header_binding (const uint8_t *p_header)
{
    return fetch_2u(p_header);
}

static inline uint8_t get_header_prim_arity (const uint8_t *p_header)
{
    return p_header[0];
}

static inline uint8_t get_header_prim_opcode (const uint8_t *p_header)
{
    return p_header[1];
}

static inline void set_header_kind_lnm1 (uint8_t *p_header, NameKind kind, int name_length)
{
    uint8_t k = (uint8_t )kind;
    uint8_t z = (uint8_t )(name_length - 1);
    ((Header *)p_header)->kind_lnm1 = (k << 4) | (z & 0xfu);
}

static inline void set_header_binding (uint8_t *p_header, const uint16_t binding)
{
    write_2u(p_header, binding);
}

// TODO: fix this comment...
/* We make compiler_ptr accessible as a global variable to Wren code;
   it's located in the first wValue cell of the_store. (See
   primitive_dictionary, below.) This requires that
   sizeof (uint8_t *) == sizeof (wValue). Sorry!
   (If you change wValue to a short type, then change compiler_ptr to a
   short offset from the_store instead of a pointer type.
   */
//#define compiler_ptr   (((uint8_t **)the_store)[0])
//#define dictionary_ptr (((uint8_t **)the_store)[1])

#define code_idx (((wValue *)the_store)[0])
#define dict_idx (((wValue *)the_store)[1])

#define code_ptr (&the_store[code_idx])
#define dict_ptr (&the_store[dict_idx])

static int available (unsigned amount)
{
    if (code_idx + amount <= dict_idx)
        return 1;
    complain("Store exhausted");
    return 0;
}

static const uint8_t *next_header (const uint8_t *header)
{
    const Header *h = (const Header *) header;
    return h->name + get_header_name_length(header);
}

static Header *bind (const char *name, unsigned length, NameKind kind, unsigned binding)
{
    assert(name);
    assert((length - 1u) < (1u << 4));
    assert(kind <= a_cfunction);
    assert(binding <= UINT16_MAX);
    
    if (available(sizeof(Header) + length))
    {
        dict_idx -= sizeof(Header) + length;
        {
            Header *h = (Header *)dict_ptr;
            set_header_kind_lnm1((uint8_t *)h, kind, (uint8_t )length);
            set_header_binding((uint8_t *)h, (uint16_t )binding);
            memcpy(h->name, name, length);
            return h;
        }
    }
    return NULL;
}

static const Header *lookup (const uint8_t *dict, const uint8_t *end,
                             const char *name, unsigned length)
{
    for (; dict < end; dict = next_header(dict))
    {
        const Header *h = (const Header *)dict;
        if (get_header_name_length(dict) == length && 0 == memcmp(h->name, name, length))
            return h;
    }
    return NULL;
}

static inline uint8_t get_proc_arity (uint16_t binding)
{
    // Procedures are compiled with the first byte holding the procedure's arity
    return the_store[binding];
}

#ifndef NDEBUG
#if 0
static void dump (const uint8_t *dict, const uint8_t *end)
{
    for (; dict < end; dict = next_header(dict))
    {
        const Header     *h = (const Header *)dict;
        const uint8_t  nlen = get_header_name_length(dict);
        const NameKind nknd = get_header_kind(dict);
        printf("  %*.*s\t%x %x %x\n", 
                nlen, nlen, h->name, nknd, h->binding, 
                (nknd == a_procedure || nknd == a_cfunction)
                    ? get_proc_arity(h->binding)
                    : nknd == a_primitive ? get_header_prim_arity(dict) : 0u);
    }
}
#endif
#endif


/* The virtual machine */

typedef uint8_t Instruc;

enum {
    HALT,
    PUSH, POP, PUSH_STRING,
    GLOBAL_FETCH, GLOBAL_STORE, LOCAL_FETCH,
    TCALL, CALL, RETURN,
    BRANCH, JUMP,
    ADD, SUB, MUL, DIV, MOD, UMUL, UDIV, UMOD, NEGATE,
    EQ, LT, ULT,
    AND, OR, XOR, SLA, SRA, SRL,
    GETC, PUTC,
    FETCH_BYTE, PEEK, POKE,
    LOCAL_FETCH_0, LOCAL_FETCH_1, PUSHW, PUSHB,
    CCALL, 
};

#ifndef NDEBUG
static const char *opcode_names[] = {
    "HALT",
    "PUSH", "POP", "PUSH_STRING",
    "GLOBAL_FETCH", "GLOBAL_STORE", "LOCAL_FETCH",
    "TCALL", "CALL", "RETURN",
    "BRANCH", "JUMP",
    "ADD", "SUB", "MUL", "DIV", "MOD", "UMUL", "UDIV", "UMOD", "NEGATE",
    "EQ", "LT", "ULT",
    "AND", "OR", "XOR", "SLA", "SRA", "SRL",
    "GETC", "PUTC",
    "FETCH_BYTE", "PEEK", "POKE",
    "LOCAL_FETCH_0", "LOCAL_FETCH_1", "PUSHW", "PUSHB",
    "CCALL", 
};
#endif

static const uint8_t primitive_dictionary[] = 
{
    PRIM_HEADER(UMUL, 2, 4), 'u', 'm', 'u', 'l',
    PRIM_HEADER(UDIV, 2, 4), 'u', 'd', 'i', 'v',
    PRIM_HEADER(UMOD, 2, 4), 'u', 'm', 'o', 'd',
    PRIM_HEADER(ULT,  2, 3), 'u', 'l', 't',
    PRIM_HEADER(SLA,  2, 3), 's', 'l', 'a',
    PRIM_HEADER(SRA,  2, 3), 's', 'r', 'a',
    PRIM_HEADER(SRL,  2, 3), 's', 'r', 'l',
    PRIM_HEADER(GETC, 0, 4), 'g', 'e', 't', 'c',
    PRIM_HEADER(PUTC, 1, 4), 'p', 'u', 't', 'c',
    PRIM_HEADER(PEEK, 1, 4), 'p', 'e', 'e', 'k',
    PRIM_HEADER(POKE, 2, 4), 'p', 'o', 'k', 'e',
};

#ifndef NDEBUG
#if 0
static void dump_dictionary (void)
{
    printf("dictionary:\n");
    dump(dict_ptr, store_end);
    dump(primitive_dictionary,
            primitive_dictionary + sizeof primitive_dictionary);
}
#endif
#endif

/* Call to C functions; see bind_c_function and CCALL prim */

static long ccall(apply_t fn, wValue *args, unsigned arity)
{
  switch (arity)
  {
#define A1 args[0]
#define A2 args[1],A1
#define A3 args[2],A2
#define A4 args[3],A3
#define A5 args[4],A4
#define A6 args[5],A5
#define A7 args[6],A6
    case 0: return (*fn)();
    case 1: return (*fn)(A1);
    case 2: return (*fn)(A2);
    case 3: return (*fn)(A3);
    case 4: return (*fn)(A4);
    case 5: return (*fn)(A5);
    case 6: return (*fn)(A6);
    case 7: return (*fn)(A7);
    default: return 0;
  }
}
#undef A1
#undef A2
#undef A3
#undef A4
#undef A5
#undef A6
#undef A7

/* Run VM code starting at 'pc', with the stack allocated the space between
   'end' and dict_ptr. Return the result on top of the stack. */
static wValue run (Instruc *pc, const Instruc *end)
{
    /* Stack pointer and base pointer 
       Initially just above the first free aligned wValue cell below
       the dictionary. */
    wValue *sp = (wValue *)(((uintptr_t )dict_ptr) & ~(sizeof(wValue) - 1));
    wValue *bp = sp;

#define need(n) \
    if (((uint8_t *)sp - ((n) * sizeof(wValue))) < end) goto stack_overflow; else 

    for (;;)
    {
#ifndef NDEBUG
        if (loud)
            printf("RUN: %"PRVAL"\t%s\n", pc - the_store, opcode_names[*pc]);
#endif

        switch (*pc++)
        {
            case HALT:
                return sp[0];
                break;

            case PUSH: 
                need(1);
                *--sp = fetch_wV(pc);
                pc += sizeof(wValue);
                break;
            case PUSHW:
                need(1);
                *--sp = fetch_2i(pc);
                pc += sizeof(int16_t);
                break;
            case PUSHB:
                need(1);
                *--sp = *(int8_t *)pc;
                pc += sizeof(int8_t);
                break;
            case POP:
                ++sp;
                break;

            case PUSH_STRING:
                need(1);
                *--sp = (wValue)(pc - the_store);
                /* N.B. this op is slower the longer the string is! */
                pc += strlen((const char *)pc) + 1;
                break;

            case GLOBAL_FETCH:
                need(1);
                *--sp = fetch_wV(the_store + fetch_2u(pc));
                pc += sizeof(uint16_t);
                break;

            case GLOBAL_STORE:
                write_wV(the_store + fetch_2u(pc), sp[0]);
                pc += sizeof(uint16_t);
                break;

            case LOCAL_FETCH_0:
                need(1);
                *--sp = bp[0];
                break;
            case LOCAL_FETCH_1:
                need(1);
                *--sp = bp[-1];
                break;
            case LOCAL_FETCH:
                need(1);
                *--sp = bp[-*pc++];
                break;

                /* A stack frame looks like this:
                   bp[0]: leftmost argument
                   (This is also where the return value will go.)
                   ...
                   bp[-(n-1)]: rightmost argument (where n is the number of arguments)
                   bp[-n]: pair of old bp and return address (in two half-words)
                   ...temporaries...
                   sp[0]: topmost temporary

                   The bp could be dispensed with, but it simplifies the compiler and VM
                   interpreter slightly, and ought to make basic debugging support
                   significantly simpler, and if we were going to make every stack slot be
                   32 bits wide then we don't even waste any extra space.

                   By the time we return, there's only one temporary in this frame:
                   the return value. Thus, &bp[-n] == &sp[1] at this time, and the 
                   RETURN instruction doesn't need to know the value of n. CALL,
                   otoh, does. It looks like <CALL> <n> <addr-byte-1> <addr-byte-2>.
                   */ 
            case TCALL: /* Known tail call. */
                {
                    uint16_t binding = fetch_2u(pc);
                    uint8_t n = get_proc_arity(binding);
                    /* XXX portability: this assumes two unsigned shorts fit in a wValue */
                    wValue frame_info = sp[n];
                    memmove((bp+1-n), sp, n * sizeof(wValue));
                    sp = bp - n;
                    sp[0] = frame_info;
                    pc = the_store + binding + 1u;
                }
                break;
            case CALL:
                {
                    /* Optimize tail calls.

                       Why doesn't the compiler emit a tail-call instruction instead
                       of us checking this at runtime? Because I don't see how it
                       could without some greater expense there: when we finish parsing
                       a function with lots of if-then-else branches, we may discover
                       only then that a bunch of calls we've compiled were in tail
                       position. 

                       (Maybe that expense would be worth incurring, though, for the
                       sake of smaller compiled code.)
                       */
                    const Instruc *cont = pc + sizeof(uint16_t);
                    while (*cont == JUMP)
                    {
                        ++cont;
                        cont += fetch_2u(cont);
                    }
                    if (*cont == RETURN)
                    {
                        /* This is a tail call. Replace opcode and re-run */
                        *--pc = TCALL;
                    }
                    else
                    {
                        uint16_t binding = fetch_2u(pc);
                        uint8_t n = get_proc_arity(binding);
                        /* This is a non-tail call. Build a new frame. */ 
                        need(1);
                        --sp;
                        {
                            /* XXX portability: this assumes two uint16_t fit in a wValue
                            ** and they the alignment is natural for both values; seems ok */
                            uint16_t *f = (uint16_t *)sp;
                            f[0] = (uint8_t *)bp - the_store;
                            f[1] = cont - the_store;
                            bp = sp + n;
                        }
                        pc = the_store + binding + 1u;
                    }
                }
                break;

            case CCALL:
                {
                    uint16_t binding = fetch_2u(pc);
                    uint8_t n = get_proc_arity(binding);
                    wValue result = ccall((apply_t )fetch_wP(the_store + binding + 1u), sp, n);
                    if (n == 0u)
                    {
                        need(1);
                        sp -= 1;
                    }
                    else
                    {
                        sp += n - 1;
                    }
                    sp[0] = result;
                    pc += sizeof(uint16_t);
                }
                break;

            case RETURN:
                {
                    wValue result = sp[0];
                    uint16_t *f = (uint16_t *)(sp + 1);
                    sp = bp;
                    bp = (wValue *)(the_store + f[0]);
                    pc = the_store + f[1];
                    sp[0] = result;
                }
                break;

            case BRANCH:
                if (0 == *sp++)
                    pc += fetch_2u(pc);
                else
                    pc += sizeof(uint16_t);
                break;

            case JUMP:
                pc += fetch_2u(pc);
                break;

            case ADD:  sp[1] += sp[0]; ++sp; break;
            case SUB:  sp[1] -= sp[0]; ++sp; break;
            case MUL:  sp[1] *= sp[0]; ++sp; break;
            case DIV:  sp[1] /= sp[0]; ++sp; break;
            case MOD:  sp[1] %= sp[0]; ++sp; break;
            case UMUL: sp[1] = (wUvalu )sp[1] * (wUvalu )sp[0]; ++sp; break;
            case UDIV: sp[1] = (wUvalu )sp[1] / (wUvalu )sp[0]; ++sp; break;
            case UMOD: sp[1] = (wUvalu )sp[1] % (wUvalu )sp[0]; ++sp; break;
            case NEGATE: sp[0] = -sp[0]; break;

            case EQ:   sp[1] = sp[1] == sp[0]; ++sp; break;
            case LT:   sp[1] = sp[1] < sp[0];  ++sp; break;
            case ULT:  sp[1] = (wUvalu )sp[1] < (wUvalu )sp[0]; ++sp; break;

            case AND:  sp[1] &= sp[0]; ++sp; break;
            case OR:   sp[1] |= sp[0]; ++sp; break;
            case XOR:  sp[1] ^= sp[0]; ++sp; break;

            case SLA:  sp[1] <<= sp[0]; ++sp; break;
            case SRA:  sp[1] >>= sp[0]; ++sp; break;
            case SRL:  sp[1] = (wUvalu )sp[1] >> (wUvalu )sp[0]; ++sp; break;

            case GETC:
                   need(1);
                   *--sp = getc(stdin);
                   break;

            case PUTC:
                   putc(sp[0], stdout);
                   break;

            case FETCH_BYTE:
                   /* XXX boundschecking */
                   sp[0] = the_store[sp[0]];
                   break;

            case PEEK:
                   sp[0] = fetch_wV(&the_store[sp[0]]);
                   break;

            case POKE:
                   write_wV(&the_store[sp[1]], sp[0]);
                   ++sp;                    // e: just one value popped
                   break;

            default: assert(0);
        }
    }

stack_overflow:
    complain("Stack overflow");
    return 0;
}


/* The 'assembler' */

static wIndex prev_instruc = 0u;

static void gen (Instruc opcode)
{
#ifndef NDEBUG
    if (loud)
        printf("ASM: %"PRVAL"\t%s\n", code_idx, opcode_names[opcode]);
#endif
    if (available(1))
    {
        prev_instruc = code_idx;
        the_store[code_idx++] = opcode;
    }
}

static void gen_ubyte (uint8_t b)
{
    if (loud)
        printf("ASM: %"PRVAL"\tubyte %u\n", code_idx, b);
    if (available(1))
        the_store[code_idx++] = b;
}

static void gen_sbyte (int8_t b)
{
    if (loud)
        printf("ASM: %"PRVAL"\tsbyte %d\n", code_idx, b);
    if (available(1))
        ((int8_t *)the_store)[code_idx++] = b;
}

static void gen_ushort (uint16_t u)
{
    if (loud)
        printf("ASM: %"PRVAL"\tushort %u\n", code_idx, u);
    if (available(sizeof u))
    {
        write_2u(code_ptr, u);
        code_idx += sizeof u;
    }
}

static void gen_sshort (int16_t u)
{
    if (loud)
        printf("ASM: %"PRVAL"\tsshort %d\n", code_idx, u);
    if (available(sizeof u))
    {
        write_2i(code_ptr, u);
        code_idx += sizeof u;
    }
}

static void gen_value (wValue v)
{
    if (loud)
        printf("ASM: %"PRVAL"\tvalue %"PRVAL"\n", code_idx, v);
    if (available(sizeof v))
    {
        write_wV(code_ptr, v);
        code_idx += sizeof v;
    }
}

static void gen_pointer (apply_t v)
{
    if (loud)
        printf("ASM: %"PRVAL"\tvalue %"PRPTR"\n", code_idx, v);
    if (available(sizeof v))
    {
        write_wP(code_ptr, v);
        code_idx += sizeof v;
    }
}

static wIndex forward_ref (void)
{
    wIndex ref = code_idx;
    code_idx += sizeof(uint16_t);
    return ref;
}

static void resolve (wIndex ref)
{
    if (loud)
        printf("ASM: %"PRIDX"\tresolved: %"PRVAL"\n", ref, code_idx - ref);
    write_2u(&the_store[ref], code_idx - ref);
}

static void block_prev (void)
{
    prev_instruc = 0u;    // The previous instruction isn't really known
}

/* Scanning */

enum { unread = EOF - 1 };
static int input_char = unread;
static int token;
static FILE *in_file;
static wValue token_value;
static char token_name[17]; // 16 + NUL

static int ch (void)
{
    if (input_char == unread)
        input_char = getc(in_file);
    return input_char;
}

static void next_char (void)
{
    if (input_char != EOF)
        input_char = unread;
}

static void skip_line (void)
{
    while (ch() != '\n' && ch() != EOF)
        next_char();
}

static unsigned hex_char_value (char c)
{
    return c <= '9' ? c - '0' : toupper(c) - ('A'-10);
}

static void next (void)
{
again:

    if (isdigit(ch()))
    {
        token = PUSH;
        token_value = 0;
        do {
            token_value = 10 * token_value + ch() - '0';
            next_char();
            if (ch() == 'x' && token_value == 0)
            {
                unsigned int digit_count = 0;
                /* Oh, it's a hex literal, not decimal as we presumed. */
                next_char();
                for (; isxdigit(ch()); next_char()) {
                    token_value = 16 * token_value + hex_char_value(ch());
                    digit_count++;
                }

                if (digit_count == 0)
                    complain("Invalid Hex Number");
                else if (digit_count > 2*sizeof(wValue)) {  // allow all bits used for hex entry
                    complain("Numeric overflow");
                }

                break;
            }

            if (token_value < 0)    // overflow
            {
                complain("Numeric overflow");
                break;
            }
        } while (isdigit(ch()));
    }
    else if (isalpha(ch()) || ch() == '_')
    {
        char *n = token_name;
        do {
            if (token_name + sizeof token_name == n + 1)
            {
                complain("Identifier too long");
                break;
            }
            *n++ = ch();
            next_char();
        } while (isalnum(ch()) || ch() == '_');
        *n++ = '\0';
        if (0 == strcmp(token_name, "then"))
            token = 't';
        else if (0 == strcmp(token_name, "forget"))
            token = 'o';
        else if (0 == strcmp(token_name, "let"))
            token = 'l';
        else if (0 == strcmp(token_name, "if"))
            token = 'i';
        else if (0 == strcmp(token_name, "fun"))
            token = 'f';
        else if (0 == strcmp(token_name, "else"))
            token = 'e';
        else
            token = 'a';
    }
    else
        switch (ch())
        {
            case '\'':
                next_char();
                {
                    /* We need to stick this string somewhere; after reaching
                       the parser, if successfully parsed, it would be compiled
                       into the instruction stream right after the next opcode.
                       So just put it there -- but don't yet update code_idx. */
                    uint8_t *s = code_ptr + 1;
                    for (; ch() != '\''; next_char())
                    {
                        if (ch() == EOF)
                        {
                            complain("Unterminated string");
                            token = EOF;
                            return;
                        }
                        if (!available(s + 2 - code_ptr))
                        {
                            token = '\n';  /* XXX need more for error recovery? */
                            return;
                        }
                        *s++ = ch();
                    }
                    next_char();
                    *s = '\0';
                    token = '\'';
                }
                break;

            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '<':
            case '&':
            case '|':
            case '^':
            case '(':
            case ')':
            case '=':
            case ':':
            case ';':
            case '\n':
            case EOF:
                token = ch();
                next_char();
                break;

            case ' ':
            case '\t':
            case '\r':
                next_char();
                goto again;

            case '#':
                skip_line();
                goto again;

            default:
                complain("Lexical error");
                token = '\n';  /* XXX need more for error recovery */
                break;
        }
}


/* Parsing and compiling */

static int expect (uint8_t expected, const char *plaint)
{
    if (token == expected)
        return 1;
    complain(plaint);
    return 0;
}

static void skip_newline (void)
{
    while (!complaint && token == '\n')
        next();
}

static void parse_expr (int precedence);

static void parse_arguments (unsigned arity)
{
    unsigned i;
    for (i = 0; i < arity; ++i)
        parse_expr(20); /* 20 is higher than any operator precedence */
}

static void parse_factor (void)
{
    skip_newline();
    switch (token)
    {
        case PUSH:
            if (token_value < 128 && token_value >= -128) {
                gen(PUSHB);
                gen_sbyte((int8_t )token_value);
            } else if (token_value < 32768 && token_value >= -32768) {
                gen(PUSHW);
                gen_sshort((int16_t )token_value);
            } else {
                gen(PUSH);
                gen_value(token_value);
            }
            next();
            break;

        case '\'':                  /* string constant */
            gen(PUSH_STRING);
            code_idx += strlen((const char *)code_ptr) + 1;
            next();
            break;

        case 'a':                   /* identifier */
            {
                const Header *h = lookup(dict_ptr, store_end, token_name, strlen(token_name));
                if (!h)
                    h = lookup(primitive_dictionary, 
                                primitive_dictionary + sizeof primitive_dictionary,
                                token_name, strlen(token_name));
                if (!h)
                    complain("Unknown identifier");
                else
                {
                    next();
                    switch (get_header_kind((uint8_t *)h))
                    {
                        case a_global:
                            gen(GLOBAL_FETCH);
                            gen_ushort(h->binding);
                            break;

                        case a_local:
                            if (h->binding == 0)
                                gen(LOCAL_FETCH_0);
                            else if (h->binding == 1)
                                gen(LOCAL_FETCH_1);
                            else {
                                gen(LOCAL_FETCH);
                                gen_ubyte(h->binding);
                            }
                            break;

                        case a_procedure:
                            {
                                uint16_t binding = get_header_binding((uint8_t *)h);
                                parse_arguments(get_proc_arity(binding));
                                gen(CALL);
                                gen_ushort(binding);
                            }
                            break;

                        case a_cfunction:
                            {
                                uint16_t binding = get_header_binding((uint8_t *)h);
                                parse_arguments(get_proc_arity(binding));
                                gen(CCALL);
                                gen_ushort(binding);
                            }
                            break;

                        case a_primitive:
                            {
                                uint8_t arity = get_header_prim_arity((uint8_t *)h);
                                parse_arguments(arity);
                                gen(get_header_prim_opcode((uint8_t *)h));
                            }
                            break;

                        default:
                            assert(0);
                    }
                }
            }
            break;

        case 'i':                   /* if-then-else */
            {
                wIndex branch, jump;
                next();
                parse_expr(0);
                gen(BRANCH);
                branch = forward_ref();
                skip_newline();
                if (expect('t', "Expected 'then'"))
                {
                    next();
                    parse_expr(3);
                    gen(JUMP);
                    jump = forward_ref();
                    skip_newline();
                    if (expect('e', "Expected 'else'"))
                    {
                        next();
                        resolve(branch);
                        parse_expr(3);
                        resolve(jump);
                        block_prev();  // We can't optimize the previous instruction here.
                    }
                }
            }
            break;

        case '*':                   /* character fetch */
            next();
            parse_factor();
            gen(FETCH_BYTE);
            break;

        case '-':                   /* unary minus */
            next();
            parse_factor();

            // If previous instruction is a value, then just negate it.
            if (prev_instruc) {
                if (the_store[prev_instruc] == PUSH)
                    write_wV(code_ptr - sizeof(wValue), -fetch_wV(code_ptr - sizeof(wValue)));
                else if (the_store[prev_instruc] == PUSHW)
                    write_2i(code_ptr - sizeof(int16_t), -fetch_2i(code_ptr - sizeof(int16_t)));
                else if (the_store[prev_instruc] == PUSHB)
                    ((signed char *)code_ptr)[-1] *= -1;
                else
                    gen(NEGATE);
            } else
                gen(NEGATE);
            break;

        case '(':
            next();
            parse_expr(0);
            if (expect(')', "Syntax error: expected ')'"))
                next();
            break;

        default:
            complain("Syntax error: expected a factor");
    }
}

static void parse_expr (int precedence) 
{
    if (complaint)
        return;
    parse_factor();
    while (!complaint)
    {
        int l, rator;   /* left precedence and operator */

        if (precedence == 0)
            skip_newline();

        switch (token) {
            case ';': l = 1; rator = POP; break;

            case ':': l = 3; rator = GLOBAL_STORE; break;

            case '&': l = 5; rator = AND; break;
            case '|': l = 5; rator = OR;  break;
            case '^': l = 5; rator = XOR; break;

            case '<': l = 7; rator = LT;  break;
            case '=': l = 7; rator = EQ;  break;

            case '+': l = 9; rator = ADD; break;
            case '-': l = 9; rator = SUB; break;

            case '*': l = 11; rator = MUL; break;
            case '/': l = 11; rator = DIV; break;
            case '%': l = 11; rator = MOD; break;

            default: return;
        }

        if (l < precedence || complaint)
            return;

        next();
        skip_newline();
        if (rator == POP)
            gen(rator);
        else if (rator == GLOBAL_STORE)
        {
            if (prev_instruc && the_store[prev_instruc] == GLOBAL_FETCH)
            {
                uint16_t addr = fetch_2u(&the_store[prev_instruc + 1]);
                code_idx = prev_instruc;
                parse_expr(l);
                gen(GLOBAL_STORE);
                gen_ushort(addr);
                continue;
            }
            else
            {
                complain("Not an l-value");
                break;
            }
        }
        parse_expr(l + 1);
        if (rator != POP)
            gen(rator);
    }
}

static void parse_done (void)
{
    if (token != EOF && token != '\n')
        complain("Syntax error: unexpected token");
}

static wValue scratch_expr (void)
{
    Instruc *start = code_ptr;
    parse_expr(-1);
    parse_done();
    gen(HALT);
    {
        Instruc *end = code_ptr;
        code_idx = start - the_store;
        return complaint ? 0 : run(start, end);
    }
}

static void run_expr (FILE *outp)
{
    wValue v = scratch_expr();
    if (!complaint && NULL != outp)
        fprintf(outp, "%"PRVAL"\n", v);
}

static void run_let (void)
{
    if (expect('a', "Expected identifier") && available(sizeof(wValue)))
    {
        uint8_t *cell = code_ptr;
        gen_value(0);
        bind(token_name, strlen(token_name), a_global, cell - the_store);
        next();
        if (expect('=', "Expected '='"))
        {
            next();
            write_wV(cell, scratch_expr());
        }
    }
}

static void run_forget (void)
{
    if (expect('a', "Expected identifier"))
    {
        const Header *h = lookup(dict_ptr, store_end, token_name, strlen(token_name));
        if (!h)
            complain("Unknown identifier");
        else
        {
            NameKind k = get_header_kind((uint8_t *)h);
            if (k != a_global && k != a_procedure && k != a_cfunction)
                complain("Not a definition");
        }
        next();
        parse_done();
        if (!complaint)
        {
            uint8_t *cp = the_store + h->binding;
            uint8_t *dp = (uint8_t *)next_header((const uint8_t *)h);
            if (the_store < cp && cp <= dp && dp <= store_end)
            {
                code_idx = cp - the_store;
                dict_idx = dp - the_store;
            }
            else
                complain("Dictionary corrupted");
        }
    }
}

static void run_fun (void)
{
    if (expect('a', "Expected identifier"))
    {
        wIndex di = dict_idx;
        wIndex ci = code_idx;
        Header *f = bind(token_name, strlen(token_name), a_procedure, code_idx);
        uint8_t arity = 0u;
        next();
        if (f)
        {
            wIndex di = dict_idx;
            while (token == 'a')
            {
                /* XXX check for too many parameters */
                bind(token_name, strlen(token_name), a_local, arity++);
                next();
            }
            if (expect('=', "Expected '='"))
            {
                next();
                gen_ubyte(arity);     // first "opcode" of function is arity
                parse_expr(-1);
                parse_done();
                gen(RETURN);
            }
            dict_idx = di;  /* forget parameter names */
        }
        if (complaint)
        {
            dict_idx = di;  /* forget function and code. */
            code_idx = ci;
        }
    }
}

static void run_command (FILE *outp)
{

    skip_newline();
    if (token == 'f')             /* 'fun' */
    {
        next();
        run_fun();
    }
    else if (token == 'l')        /* 'let' */
    {
        next();
        run_let();
    }
    else if (token == 'o')        /* 'forget' */
    {
        next();
        run_forget();
    }
    else
        run_expr(outp);

    if (complaint)
    {
        fprintf(stderr, "%s\n", complaint);
        skip_line();  /* i.e., flush any buffered input, sort of */
        next();
    }
}

/**
* @brief wren_load_file -- loads a file into wren interpreter
* ; similar to wren_read_eval_print_loop but without the prompts or print of expression results
*/
void wren_load_file (FILE *fp)
{
    FILE *saved_in_file = in_file;

    in_file = fp; // set the input source

    // just like wren_read_eval_print_loop but no prompts or printing results
    //
    next_char();
    complaint = NULL;
    next();
    while (token != EOF)
    {
        run_command(NULL);
        skip_newline();
        complaint = NULL;
    }

    input_char = unread;
    in_file = saved_in_file; // restore the input source
}

/**
* @brief wren_read_eval_print_loop - The top level
* ; does not return until stdin runs out of characters!
*/
void wren_read_eval_print_loop (void)
{
    printf("> ");
    next_char();
    complaint = NULL;
    next();
    while (token != EOF)
    {
        run_command(stdout);
        printf("> ");
        skip_newline();
        complaint = NULL;
    }
    printf("\n");
}

// Test CCALL C functions

static wValue tstfn2 (wValue a1, wValue a2)
{
    printf("tstfn2: %"PRVAL"\t%"PRVAL"\n", a1, a2);
    return a1 - a2;
}

static wValue tstfn0 (void)
{
    printf("tstfn0\n");
    return 13;
}

/**
* @brief wren_bind_c_function - create binding for C function callable by wren code
* 
* @param[in] name  - new wren function name
* @param[in] fn    - the C function of type apply_t
* @param[in] arity - number of arguments to the C funciton (must be < 8)
*/
void wren_bind_c_function (const char *name, apply_t fn, const uint8_t arity)
{
    (void )bind(name, strlen(name), a_cfunction, code_idx);
    gen_ubyte(arity);
    gen_pointer(fn);
}

/**
* @brief wren_initialize - create initial dictionary
* ; must be called before any other wren_functions
*/
void wren_initialize (void)
{
    ((wValue *)the_store)[2] = 0u;                      // TODO: scrap this
    ((wValue *)the_store)[3] = (wValue )store_capacity;
    dict_idx = (wValue )store_capacity;
    bind("cp", 2, a_global, 0 * sizeof(wValue));
    bind("dp", 2, a_global, 1 * sizeof(wValue));
    bind("c0", 2, a_global, 2 * sizeof(wValue));
    bind("d0", 2, a_global, 3 * sizeof(wValue));
    code_idx = 4 * sizeof(wValue);
    in_file = stdin;
}

#if WREN_STANDALONE

int main (int argc, char **argv)
{
    int i = 1;
    int skip_repl = 0;

    wren_initialize();

    wren_bind_c_function("tstfn2", tstfn2, 2);
    wren_bind_c_function("tstfn0", tstfn0, 0);

    while (i < argc)
    {
        if (strcmp(argv[i], "-f") == 0)
        {
            skip_repl = 1;
        }
        else
        {
            FILE *fp = fopen(argv[i], "r");
            if (NULL != fp)
            {
                wren_load_file(fp);
                fclose(fp);
            }
            else
            {
                //printf("Cannot open %s for reading, error: %d\n", argv[i], errno);
                printf("Cannot open arg %d %s for reading\n", i, argv[i]);
            }
        }
        i += 1;
    }

    if (!skip_repl) wren_read_eval_print_loop();

    return 0;
}

#endif
