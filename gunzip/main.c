
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef i8 b8;
typedef i32 b32;

typedef struct {
    u8* str;
    u64 size;
} string8;

string8 read_file(const char* path);
string8 gunzip(string8 input);
u64 inflate(string8 input, string8 out);
u32 crc32(const u8* data, u64 size);

void fatal(const char* msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

// read a little-endian value without relying on unaligned loads
u16 read_le16(const u8* p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
u32 read_le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

u32 crc32_table[256];

void build_crc32_table(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (u32 k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
        }
        crc32_table[i] = c;
    }
}

u32 crc32(const u8* data, u64 size) {
    u32 c = 0xFFFFFFFF;

    for (u64 i = 0; i < size; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }

    return c ^ 0xFFFFFFFF;
}


string8 read_file(const char* path) {
    string8 out = { 0 };

    FILE* f = fopen(path, "rb");
    if (f == NULL) { fatal("could not open file"); }

    fseek(f, 0, SEEK_END);
    u64 size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // +4 zero padding: the bit readers load 4 bytes at a time and may
    // read up to 3 bytes past the last byte of real data
    out.size = size;
    out.str = malloc(size + 4);
    if (out.str == NULL) { fatal("out of memory"); }
    memset(out.str, 0, size + 4);

    if (size > 0 && fread(out.str, 1, size, f) != size) {
        fatal("could not read file");
    }

    fclose(f);

    return out;
}

#define FTEXT    0b00001
#define FHCRC    0b00010
#define FEXTRA   0b00100
#define FNAME    0b01000
#define FCOMMENT 0b10000
 
string8 gunzip(string8 input) {
    if (input.size < 18) { fatal("input too small to be gzip"); }
    if (input.str[0] != 0x1F || input.str[1] != 0x8B) { fatal("bad gzip magic"); }
    if (input.str[2] != 8) { fatal("unsupported gzip compression method"); }

    u8 flags = input.str[3];
    if (flags & 0xE0) { fatal("reserved gzip flag bits set"); }

    u32 block_offset = 10;

    if (flags & FEXTRA) {
        u16 xlen = read_le16(input.str + block_offset);
        if (block_offset + 2 + xlen + 8 > input.size) { fatal("extra field too long"); }
        block_offset += 2 + xlen;
    }

    if (flags & FNAME) {
        while (block_offset < input.size && input.str[block_offset]) { block_offset++; }
        if (block_offset >= input.size) { fatal("unterminated file name"); }
        block_offset++;
    }

    if (flags & FCOMMENT) {
        while (block_offset < input.size && input.str[block_offset]) { block_offset++; }
        if (block_offset >= input.size) { fatal("unterminated comment"); }
        block_offset++;
    }

    if (flags & FHCRC) { block_offset += 2; }

    if (block_offset + 8 > input.size) { fatal("no deflate data or trailer"); }

    u32 isize = read_le32(input.str + input.size - 4);
    u32 crc = read_le32(input.str + input.size - 8);

    string8 out = {
        .size = isize,
        .str = malloc(isize ? isize : 1)
    };
    if (out.str == NULL) { fatal("out of memory"); }

    string8 deflate_block = {
        .size = input.size - block_offset - 8,
        .str = input.str + block_offset,
    };

    u64 pos = inflate(deflate_block, out);

    if (pos != isize) { fatal("decompressed size does not match header"); }
    if (crc32(out.str, pos) != crc) { fatal("crc32 mismatch"); }

    return out;
}

u16 reverse_u16(u16 n) {
    u16 o = n;
    o = ((o & 0xAAAA) >> 1) | ((o & 0x5555) << 1);
    o = ((o & 0xCCCC) >> 2) | ((o & 0x3333) << 2);
    o = ((o & 0xF0F0) >> 4) | ((o & 0x0F0F) << 4);
    o = ((o & 0xFF00) >> 8) | ((o & 0x00FF) << 8);
    return o;
}

#define reverse_bits(n, bits) (reverse_u16(n) >> (16 - bits))

typedef struct {
    string8 backing;
    u64 bit_pos;
    u64 bit_size;
} bitstream;

bitstream bs_init(string8 backing) {
    return (bitstream){ 
        .backing = backing,
        .bit_pos = 0,
        .bit_size = backing.size * 8
    };
}

u32 bit_masks[] = {
    0b0000000000000000,
    0b0000000000000001,
    0b0000000000000011,
    0b0000000000000111,
    0b0000000000001111,
    0b0000000000011111,
    0b0000000000111111,
    0b0000000001111111,
    0b0000000011111111,
    0b0000000111111111,
    0b0000001111111111,
    0b0000011111111111,
    0b0000111111111111,
    0b0001111111111111,
    0b0011111111111111,
    0b0111111111111111,
    0b1111111111111111,
};

// nbits <= 16
u32 bs_peek(bitstream* bs, u8 nbits) {
    u64 byte_pos = bs->bit_pos / 8;
    u32 bits;
    memcpy(&bits, bs->backing.str + byte_pos, 4);

    bits >>= bs->bit_pos % 8;

    return bits & bit_masks[nbits];
}

// nbits <= 16
u32 bs_take(bitstream* bs, u8 nbits) {
    u64 byte_pos = bs->bit_pos / 8;
    u32 bits;
    memcpy(&bits, bs->backing.str + byte_pos, 4);

    bits >>= bs->bit_pos % 8;
    bs->bit_pos += nbits;

    return bits & bit_masks[nbits];
}

typedef struct {
    u16 len;
    u16 code;
} generic_huff_node;

#define MAX_BITS 15

void build_huff_tree(generic_huff_node* nodes, u32 node_count) {
    u32 bl_count[MAX_BITS + 1] = { 0 };

    for (u32 i = 0; i < node_count; i++) {
        bl_count[nodes[i].len]++;
    }

    u32 next_code[MAX_BITS + 1] = { 0 };

    u32 code = 0;
    bl_count[0] = 0;
    for (u32 bits = 1; bits <= MAX_BITS; bits++) {
        code = (code + bl_count[bits-1]) << 1;
        next_code[bits] = code;
    }

    for (u32 i = 0; i < node_count; i++) {
        if (nodes[i].len != 0) {
            nodes[i].code = next_code[nodes[i].len]++;
        }
    }
}

typedef struct {
    u16 code;
    u8 bits_used;
    u8 extra_bits;
    u16 length_base;
} ll_lut_node;

typedef struct {
    u8 bits_used;
    u8 extra_bits;
    u16 dist_base;
} dist_lut_node;

u8 ll_extras[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 0,
};

u16 ll_bases[] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
    83, 99, 115, 131, 163, 195, 227, 258,
};

u8 dist_extras[] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13,
};

u16 dist_bases[] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};

u64 _huff_impl(
    bitstream* bs,
    u8* out,
    u64 pos,
    u64 cap,
    generic_huff_node* ll_nodes,
    generic_huff_node* dist_nodes
) {
    u32 max_ll_bits = 0;
    u32 max_dist_bits = 0;

    for (u32 i = 0; i < 286; i++) {
        if (ll_nodes[i].len > max_ll_bits) {
            max_ll_bits = ll_nodes[i].len;
        }
    }

    for (u32 i = 0; i < 30; i++) {
        if (dist_nodes[i].len > max_dist_bits) {
            max_dist_bits = dist_nodes[i].len;
        }
    }

    u32 ll_lut_size = sizeof(ll_lut_node) * (1 << max_ll_bits);
    u32 dist_lut_size = sizeof(dist_lut_node) * (1 << max_dist_bits);

    ll_lut_node* ll_lut = malloc(ll_lut_size);
    dist_lut_node* dist_lut = malloc(dist_lut_size);

    memset(ll_lut, 0, ll_lut_size);
    memset(dist_lut, 0, dist_lut_size);

    for (u32 i = 0; i < 286; i++) {
        u32 len = ll_nodes[i].len;
        u32 code = ll_nodes[i].code;

        if (len == 0) { continue; }

        ll_lut_node lut_node = {
            .code = i,
            .bits_used = len,
            .extra_bits = i < 257 ? 0 : ll_extras[i-257],
            .length_base = i < 257 ? 0 : ll_bases[i-257],
        };

        u32 leftover_bits = max_ll_bits - len;
        u32 leftover_max = (1 << leftover_bits) - 1;
        for (u32 leftover = 0; leftover <= leftover_max; leftover++) {
            u32 index = reverse_bits(
                (code << leftover_bits) | leftover,
                max_ll_bits
            );

            ll_lut[index] = lut_node;
        }
    }

    for (u32 i = 0; i < 30; i++) {
        u32 len = dist_nodes[i].len;
        u32 code = dist_nodes[i].code;

        if (len == 0) { continue; }

        dist_lut_node lut_node = {
            .bits_used = len,
            .extra_bits = dist_extras[i],
            .dist_base = dist_bases[i],
        };

        u32 leftover_bits = max_dist_bits - len;
        u32 leftover_max = (1 << leftover_bits) - 1;
        for (u32 leftover = 0; leftover <= leftover_max; leftover++) {
            u32 index = reverse_bits(
                (code << leftover_bits) | leftover,
                max_dist_bits
            );

            dist_lut[index] = lut_node;
        }
    }

    while (bs->bit_pos < bs->bit_size) {
        u32 ll_peek_bits = bs_peek(bs, max_ll_bits);
        ll_lut_node ll_node = ll_lut[ll_peek_bits];
        if (ll_node.bits_used == 0) { fatal("invalid huffman code"); }
        bs->bit_pos += ll_node.bits_used;

        u32 code = ll_node.code;

        if (code < 256) {
            if (pos >= cap) { fatal("decompressed data overflow"); }
            out[pos++] = code;
        } else if (code == 256) {
            break;
        } else {
            u32 length = ll_node.length_base + bs_take(bs, ll_node.extra_bits);

            u32 dist_peek_bits = bs_peek(bs, max_dist_bits);
            dist_lut_node dist_node = dist_lut[dist_peek_bits];
            if (dist_node.bits_used == 0) { fatal("invalid distance code"); }
            bs->bit_pos += dist_node.bits_used;

            u32 dist = dist_node.dist_base + bs_take(bs, dist_node.extra_bits);

            if (dist > pos) { fatal("match distance before start of output"); }
            if (pos + length > cap) { fatal("decompressed data overflow"); }
            while (length--) {
                out[pos] = out[pos - dist];
                pos++;
            }
        }
    }

    free(ll_lut);
    free(dist_lut);

    return pos;
}

typedef struct {
    u8 code_len;
    u8 bits_used;
    u8 extra_bits;
    u8 repeat_base;
} cl_lut_node;

u8 cl_extras[19] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7
};

u8 cl_bases[19] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 11
};

u8 cl_order[] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

u8 _dyn_next_cl(
    bitstream* bs,
    generic_huff_node* nodes,
    u32* node_pos,
    u32 max_count,
    cl_lut_node* cl_lut,
    u8 prev_codelen
) {
    u32 peek_bits = bs_peek(bs, 7);
    cl_lut_node cl_node = cl_lut[peek_bits];
    if (cl_node.bits_used == 0) { fatal("invalid code length code"); }
    bs->bit_pos += cl_node.bits_used;

    u32 cl = cl_node.code_len;

    if (cl <= 15) {
        if (*node_pos >= max_count) { fatal("too many code lengths"); }
        nodes[(*node_pos)++].len = cl;
        return cl;
    }

    u32 repeat_value = cl == 16 ? prev_codelen : 0;
    u32 repeat_count = cl_node.repeat_base + bs_take(bs, cl_node.extra_bits);

    while (repeat_count--) {
        if (*node_pos >= max_count) { fatal("too many code lengths"); }
        nodes[(*node_pos)++].len = repeat_value;
    }

    return repeat_value;
}

u64 _dynamic_block(bitstream* bs, u8* out, u64 pos, u64 cap) {
    u32 num_ll_codes = bs_take(bs, 5) + 257;
    u32 num_dist_codes = bs_take(bs, 5) + 1;
    u32 num_cl_codes = bs_take(bs, 4) + 4;

    generic_huff_node cl_nodes[19] = { 0 };

    for (u32 i = 0; i < num_cl_codes; i++) {
        cl_nodes[cl_order[i]].len = bs_take(bs, 3);
    }

    build_huff_tree(cl_nodes, 19);

    cl_lut_node cl_lut[256] = { 0 };
    for (u32 i = 0; i < 19; i++) {
        u32 len = cl_nodes[i].len;
        u32 code = cl_nodes[i].code;

        if (len == 0) { continue; }

        cl_lut_node lut_node = {
            .code_len = i,
            .bits_used = len,
            .extra_bits = cl_extras[i],
            .repeat_base = cl_bases[i],
        };

        u32 leftover_bits = 7 - len;
        u32 leftover_max = (1 << leftover_bits) - 1;
        for (u32 leftover = 0; leftover <= leftover_max; leftover++) {
            u32 index = reverse_bits(
                (code << leftover_bits) | leftover,
                7
            );

            cl_lut[index] = lut_node;
        }
    }

    // Note(Ian) I have updated this logic from the video to be more spec
    // compliant
    generic_huff_node combined_nodes[286 + 30] = { 0 };

    u8 prev_codelen = 0;
    u32 node_pos = 0;
    while (node_pos < num_ll_codes + num_dist_codes) {
        prev_codelen = _dyn_next_cl(
            bs, combined_nodes, &node_pos, num_ll_codes + num_dist_codes,
            cl_lut, prev_codelen
        );
    }

    generic_huff_node ll_nodes[286] = { 0 };
    generic_huff_node dist_nodes[30] = { 0 };

    memcpy(ll_nodes, combined_nodes, num_ll_codes * sizeof(generic_huff_node));
    memcpy(
        dist_nodes,
        combined_nodes + num_ll_codes,
        num_dist_codes * sizeof(generic_huff_node)
    );

    build_huff_tree(ll_nodes, 286);
    build_huff_tree(dist_nodes, 30);

    return _huff_impl(bs, out, pos, cap, ll_nodes, dist_nodes);
}

u64 _fixed_block(bitstream* bs, u8* out, u64 pos, u64 cap) {
    generic_huff_node ll_nodes[288] = { 0 };
    generic_huff_node dist_nodes[32] = { 0 };

    for (u32 i = 0; i <= 143; i++)   { ll_nodes[i].len = 8; }
    for (u32 i = 144; i <= 255; i++) { ll_nodes[i].len = 9; }
    for (u32 i = 256; i <= 279; i++) { ll_nodes[i].len = 7; }
    for (u32 i = 280; i <= 287; i++) { ll_nodes[i].len = 8; }

    for (u32 i = 0; i < 32; i++) { dist_nodes[i].len = 5; }

    build_huff_tree(ll_nodes, 288);
    build_huff_tree(dist_nodes, 32);

    return _huff_impl(bs, out, pos, cap, ll_nodes, dist_nodes);
}

u64 inflate(string8 input, string8 out) {
    bitstream bs = bs_init(input);

    u64 pos = 0;

    b32 last_block = false;
    while (!last_block) {
        if (bs.bit_pos + 3 > bs.bit_size) { fatal("truncated block header"); }

        last_block = bs_take(&bs, 1);
        u32 block_type = bs_take(&bs, 2);

        switch (block_type) {
            case 0b00: {
                // stored block: byte aligned, then LEN/NLEN and raw bytes
                u64 b = (bs.bit_pos + 7) / 8; // byte aligned offset
                u64 data_size = bs.bit_size / 8;

                if (b + 4 > data_size) { fatal("stored block truncated"); }
                u16 len = read_le16(input.str + b);
                u16 nlen = read_le16(input.str + b + 2);
                if ((u16)~len != nlen) { fatal("stored block LEN/NLEN mismatch"); }
                if (b + 4 + len > data_size) { fatal("stored block data truncated"); }
                if (pos + len > out.size) { fatal("decompressed data overflow"); }

                memcpy(out.str + pos, input.str + b + 4, len);
                pos += len;
                bs.bit_pos = (b + 4 + len) * 8;
            } break;
            case 0b01: {
                pos = _fixed_block(&bs, out.str, pos, out.size);
            } break;
            case 0b10: {
                pos = _dynamic_block(&bs, out.str, pos, out.size);
            } break;
            default: fatal("reserved block type"); // 0b11
        }
    }

    return pos;
}

// ==============================================================
// deflate encoder
// ==============================================================

// growable bit writer that mirrors the LSB-first format of the decoder;
// .size holds the allocation size, the used size is bit_pos / 8
typedef struct {
    string8 data;
    u64 bit_pos;
} bit_writer;

bit_writer bw_init(void) {
    bit_writer bw = { 0 };
    bw.data.size = 256;
    bw.data.str = malloc(bw.data.size);
    memset(bw.data.str, 0, bw.data.size);
    return bw;
}

// nbits <= 16, written least-significant bit first
void bw_write(bit_writer* bw, u32 bits, u8 nbits) {
    if (nbits == 0) { return; }

    u64 byte_pos = bw->bit_pos / 8;
    u8 shift = (u8)(bw->bit_pos % 8);
    u8 need = (u8)((shift + nbits + 7) / 8);

    while (bw->data.size < byte_pos + need) {
        u64 new_size = bw->data.size * 2;
        bw->data.str = realloc(bw->data.str, new_size);
        memset(bw->data.str + bw->data.size, 0, new_size - bw->data.size);
        bw->data.size = new_size;
    }

    u64 chunk = (u64)bits << shift;
    for (u8 i = 0; i < need; i++) {
        bw->data.str[byte_pos + i] |= (u8)(chunk >> (8 * i));
    }
    bw->bit_pos += nbits;
}

// pad with zero bits up to the next byte boundary
void bw_align(bit_writer* bw) {
    bw->bit_pos = (bw->bit_pos + 7) & ~7ULL;
}

// the number of bytes the bits written so far fill
u64 bw_byte_size(bit_writer* bw) {
    return (bw->bit_pos + 7) / 8;
}

// ---- LZ77 ----

typedef struct {
    u32 length; // match length, or 0 for a literal
    u32 value;  // literal byte, or match distance
} lz_token;

typedef struct {
    lz_token* items;
    u64 count;
    u64 cap;
} token_list;

void token_push(token_list* list, u32 length, u32 value) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 1024;
        list->items = realloc(list->items, list->cap * sizeof(lz_token));
    }
    list->items[list->count++] = (lz_token){ .length = length, .value = value };
}

#define MAX_MATCH 258
#define MIN_MATCH 3
#define MAX_DIST 32768
#define MAX_CHAIN 128

u32 lz_hash(const u8* p) {
    return ((u32)p[0] * 65599u + (u32)p[1] * 31u + p[2]) & 0xFFFF;
}

// greedy LZ77 parse of the whole input into a token list. prev[] is one
// entry per input byte, which is simple and fine for the file sizes this
// tool targets
token_list lz_parse(const u8* data, u64 n) {
    token_list list = { 0 };

    i64* head = malloc(65536 * sizeof(i64));
    i64* prev = malloc(n * sizeof(i64));
    for (u32 i = 0; i < 65536; i++) { head[i] = -1; }

    u64 i = 0;
    while (i < n) {
        u32 best_len = 0;
        u32 best_dist = 0;

        if (i + MIN_MATCH <= n) {
            i64 cand = head[lz_hash(data + i)];
            u32 chain = 0;
            u32 max_len = (u32)(n - i < MAX_MATCH ? n - i : MAX_MATCH);

            while (cand >= 0 && chain < MAX_CHAIN) {
                u64 dist = i - (u64)cand;
                if (dist > MAX_DIST) { break; }

                u32 len = 0;
                while (len < max_len && data[cand + len] == data[i + len]) { len++; }

                if (len > best_len) { best_len = len; best_dist = (u32)dist; }
                if (len == max_len) { break; }

                cand = prev[cand];
                chain++;
            }
        }

        if (best_len >= MIN_MATCH) {
            token_push(&list, best_len, best_dist);
        } else {
            token_push(&list, 0, data[i]);
        }

        // add every byte we just passed to the hash chain so later
        // positions can match against it
        u64 end = best_len >= MIN_MATCH ? i + best_len : i + 1;
        for (; i < end; i++) {
            if (i + 2 < n) {
                u32 h = lz_hash(data + i);
                prev[i] = head[h];
                head[h] = (i64)i;
            }
        }
    }

    free(head);
    free(prev);
    return list;
}

// ---- huffman code lengths for the encoder ----

typedef struct {
    u64 freq;
    i64 left, right; // child node indices, -1 for a leaf
    u32 sym;         // symbol of a leaf
} huff_node;

b32 _huff_less(huff_node* nodes, i64 a, i64 b) {
    if (nodes[a].freq != nodes[b].freq) { return nodes[a].freq < nodes[b].freq; }
    return a < b;
}

void _huff_push(huff_node* nodes, i64* heap, u64* heap_n, i64 id) {
    u64 i = (*heap_n)++;
    heap[i] = id;
    while (i > 0) {
        u64 parent = (i - 1) / 2;
        if (_huff_less(nodes, heap[parent], heap[i])) { break; }
        i64 tmp = heap[i]; heap[i] = heap[parent]; heap[parent] = tmp;
        i = parent;
    }
}

i64 _huff_pop(huff_node* nodes, i64* heap, u64* heap_n) {
    i64 top = heap[0];
    heap[0] = heap[--(*heap_n)];

    u64 i = 0;
    for (;;) {
        u64 l = i * 2 + 1;
        u64 r = l + 1;
        if (l >= *heap_n) { break; }

        u64 best = l;
        if (r < *heap_n && _huff_less(nodes, heap[r], heap[l])) { best = r; }
        if (_huff_less(nodes, heap[i], heap[best])) { break; }

        i64 tmp = heap[i]; heap[i] = heap[best]; heap[best] = tmp;
        i = best;
    }

    return top;
}

b32 _huff_walk(huff_node* nodes, i64 id, u32 depth, u8 max_len, u8* out_len) {
    if (depth > max_len) { return false; }

    if (nodes[id].left == -1) {
        out_len[nodes[id].sym] = (u8)depth;
        return true;
    }

    return _huff_walk(nodes, nodes[id].left, depth + 1, max_len, out_len)
        && _huff_walk(nodes, nodes[id].right, depth + 1, max_len, out_len);
}

// build a huffman tree from symbol frequencies. Fills out_len[sym] for
// every symbol with freq > 0 and returns false if the tree would be deeper
// than max_len. The tree is always a complete prefix code: if only one
// symbol was used, a zero-frequency dummy sibling of length 1 is added and
// reported through *out_dummy (deflate needs two leaves to make a code).
b32 huff_build_lengths(u64* freq, u32 n_syms, u8 max_len, u8* out_len, u32* out_dummy) {
    huff_node* nodes = malloc(n_syms * 2 * sizeof(huff_node));
    i64* heap = malloc(n_syms * 2 * sizeof(i64));
    u64 heap_n = 0;

    *out_dummy = UINT32_MAX;

    u32 m = 0;
    for (u32 s = 0; s < n_syms; s++) {
        if (freq[s] == 0) { continue; }
        nodes[m] = (huff_node){ .freq = freq[s], .left = -1, .right = -1, .sym = s };
        _huff_push(nodes, heap, &heap_n, m);
        m++;
    }

    if (m == 1) {
        // one symbol on its own cannot form a prefix code, add a dummy
        u32 dummy = nodes[0].sym == 0 ? 1 : 0;
        out_len[nodes[0].sym] = 1;
        out_len[dummy] = 1;
        *out_dummy = dummy;
        free(nodes);
        free(heap);
        return true;
    }

    while (heap_n > 1) {
        i64 a = _huff_pop(nodes, heap, &heap_n);
        i64 b = _huff_pop(nodes, heap, &heap_n);
        nodes[m] = (huff_node){
            .freq = nodes[a].freq + nodes[b].freq,
            .left = a,
            .right = b,
        };
        _huff_push(nodes, heap, &heap_n, m);
        m++;
    }

    b32 ok = _huff_walk(nodes, heap[0], 0, max_len, out_len);

    free(nodes);
    free(heap);
    return ok;
}

// ---- encoding tables and block writers ----

u16 len_sym[259];  // ll symbol (257..285) for each match length 3..258
u8 len_extra[259];
u16 len_base[259];

void build_len_tables(void) {
    for (u32 c = 0; c < 29; c++) {
        u32 base = ll_bases[c];
        u32 extra = ll_extras[c];
        u32 end = base + ((u32)1 << extra);
        for (u32 l = base; l < end && l <= 258; l++) {
            len_sym[l] = (u16)(257 + c);
            len_extra[l] = extra;
            len_base[l] = (u16)base;
        }
    }
}

// symbol and extra-bit info for a match distance 1..32768
void dist_encode(u32 d, u8* sym, u8* extra_bits, u16* base) {
    for (u32 c = 0; c < 30; c++) {
        u32 b = dist_bases[c];
        if (d >= b && d < b + ((u32)1 << dist_extras[c])) {
            *sym = (u8)c;
            *extra_bits = dist_extras[c];
            *base = (u16)b;
            return;
        }
    }
}

// fixed huffman code lengths from RFC 1951 3.2.6; arrays are 288/32
// symbols wide like the decoder's, so the canonical codes match exactly
u8 fixed_ll_len[288];
u8 fixed_dist_len[32];

void build_fixed_len_tables(void) {
    for (u32 i = 0; i < 288; i++) {
        fixed_ll_len[i] = i <= 143 ? 8 : i <= 255 ? 9 : i <= 279 ? 7 : 8;
    }
    for (u32 i = 0; i < 32; i++) { fixed_dist_len[i] = 5; }
}

// number of bits the tokens need with the given code lengths
u64 tokens_bits(const lz_token* tokens, u64 count, const u8* ll_len, const u8* dist_len) {
    u64 bits = 0;

    for (u64 i = 0; i < count; i++) {
        const lz_token* t = &tokens[i];

        if (t->length == 0) {
            bits += ll_len[t->value];
        } else {
            u32 s = len_sym[t->length];
            bits += ll_len[s] + len_extra[t->length];

            u8 ds = 0, dex = 0;
            u16 db = 0;
            dist_encode(t->value, &ds, &dex, &db);
            bits += dist_len[ds] + dex;
        }
    }

    return bits;
}

// write the tokens and end-of-block code using canonical codes built from
// the given lengths; codes go out most-significant bit first like the
// decoder expects
void tokens_write(bit_writer* bw, const lz_token* tokens, u64 count,
                  generic_huff_node* ll, generic_huff_node* dist) {
    for (u64 i = 0; i < count; i++) {
        const lz_token* t = &tokens[i];

        if (t->length == 0) {
            u32 s = t->value;
            bw_write(bw, reverse_bits(ll[s].code, ll[s].len), ll[s].len);
        } else {
            u32 s = len_sym[t->length];
            bw_write(bw, reverse_bits(ll[s].code, ll[s].len), ll[s].len);
            bw_write(bw, t->length - len_base[t->length], len_extra[t->length]);

            u8 ds = 0, dex = 0;
            u16 db = 0;
            dist_encode(t->value, &ds, &dex, &db);
            bw_write(bw, reverse_bits(dist[ds].code, dist[ds].len), dist[ds].len);
            bw_write(bw, t->value - db, dex);
        }
    }

    // end of block
    bw_write(bw, reverse_bits(ll[256].code, ll[256].len), ll[256].len);
}

// one stored block (stored blocks are limited to 65535 bytes of payload)
void stored_block_write(bit_writer* bw, const u8* data, u64 n, b32 final) {
    bw_write(bw, final, 1); // BFINAL
    bw_write(bw, 0, 2);     // BTYPE 00
    bw_align(bw);

    u16 len = (u16)n;
    bw_write(bw, len, 16);
    bw_write(bw, (u16)~len, 16);

    for (u64 i = 0; i < n; i++) { bw_write(bw, data[i], 8); }
}

// run-length encoding of the code-length stream, in the exact format the
// decoder's _dynamic_block reads back (16/17/18 repeat symbols)
typedef struct {
    u8 sym;
    u8 extra_bits;
    u16 extra;
} cl_run;

#define MAX_CL_RUNS 340

u32 cl_build_runs(const u8* ll_len, u32 hlit, const u8* dist_len, u32 hdist,
                  cl_run* runs, u64* cl_freq) {
    u32 n_runs = 0;

    u32 pos = 0;
    while (pos < hlit + hdist) {
        u8 v = pos < hlit ? ll_len[pos] : dist_len[pos - hlit];

        u32 run = 1;
        while (pos + run < hlit + hdist) {
            u8 w = pos + run < hlit ? ll_len[pos + run] : dist_len[pos + run - hlit];
            if (w != v) { break; }
            run++;
        }
        pos += run;

        if (v == 0) {
            while (run >= 11) {
                u32 chunk = run > 138 ? 138 : run;
                runs[n_runs++] = (cl_run){ .sym = 18, .extra_bits = 7, .extra = (u16)(chunk - 11) };
                cl_freq[18]++;
                run -= chunk;
            }
            if (run >= 3) {
                runs[n_runs++] = (cl_run){ .sym = 17, .extra_bits = 3, .extra = (u16)(run - 3) };
                cl_freq[17]++;
                run = 0;
            }
            while (run > 0) {
                runs[n_runs++] = (cl_run){ .sym = 0, .extra_bits = 0, .extra = 0 };
                cl_freq[0]++;
                run--;
            }
        } else {
            runs[n_runs++] = (cl_run){ .sym = v, .extra_bits = 0, .extra = 0 };
            cl_freq[v]++;
            run--;

            while (run >= 3) {
                u32 chunk = run > 6 ? 6 : run;
                runs[n_runs++] = (cl_run){ .sym = 16, .extra_bits = 2, .extra = (u16)(chunk - 3) };
                cl_freq[16]++;
                run -= chunk;
            }
            while (run > 0) {
                runs[n_runs++] = (cl_run){ .sym = v, .extra_bits = 0, .extra = 0 };
                cl_freq[v]++;
                run--;
            }
        }
    }

    return n_runs;
}

// build canonical codes from code lengths (reuses the decoder's function)
void canonical_codes(u8* lens, u32 count, generic_huff_node* out) {
    for (u32 i = 0; i < count; i++) {
        out[i].len = lens[i];
        out[i].code = 0;
    }
    build_huff_tree(out, count);
}

// ---- deflate and gzip writers ----

// deflate the input as one final block, choosing the smallest of stored,
// fixed huffman and dynamic huffman
string8 deflate(string8 input) {
    if (input.size == 0) {
        bit_writer bw = bw_init();
        stored_block_write(&bw, input.str, 0, true);
        bw.data.size = bw_byte_size(&bw);
        return bw.data;
    }

    token_list tokens = lz_parse(input.str, input.size);

    // symbol frequencies
    build_len_tables();
    build_fixed_len_tables();

    u64 ll_freq[286] = { 0 };
    u64 dist_freq[30] = { 0 };
    u32 max_ll_sym = 256; // end of block
    u64 n_matches = 0;

    for (u64 i = 0; i < tokens.count; i++) {
        const lz_token* t = &tokens.items[i];

        if (t->length == 0) {
            ll_freq[t->value]++;
            if (t->value > max_ll_sym) { max_ll_sym = t->value; }
        } else {
            u32 s = len_sym[t->length];
            ll_freq[s]++;
            if (s > max_ll_sym) { max_ll_sym = s; }

            u8 ds = 0, dex = 0;
            u16 db = 0;
            dist_encode(t->value, &ds, &dex, &db);
            dist_freq[ds]++;
            n_matches++;
        }
    }
    ll_freq[256]++;

    u8 ll_len[286] = { 0 };
    u8 dist_len[30] = { 0 };
    u8 cl_len[19] = { 0 };
    generic_huff_node dyn_ll[286];
    generic_huff_node dyn_dist[30];
    generic_huff_node dyn_cl[19];
    cl_run runs[MAX_CL_RUNS];
    u32 n_runs = 0;
    u32 hlit = 0;
    u32 hdist = 0;
    u32 hclen = 0;

    // a dynamic block with no distance codes at all would pay the header
    // cost for nothing, so only try it when the input produced matches
    b32 dyn_ok = false;
    if (n_matches > 0) {
        u32 dummy;
        if (huff_build_lengths(ll_freq, 286, 15, ll_len, &dummy)
            && huff_build_lengths(dist_freq, 30, 15, dist_len, &dummy)) {
            for (u32 s = 0; s < 286; s++) { if (ll_len[s]) { hlit = s + 1; } }
            for (u32 s = 0; s < 30; s++) { if (dist_len[s]) { hdist = s + 1; } }
            if (hlit < 257) { hlit = 257; }
            if (hdist < 1) { hdist = 1; }

            u64 cl_freq[19] = { 0 };
            n_runs = cl_build_runs(ll_len, hlit, dist_len, hdist, runs, cl_freq);

            if (huff_build_lengths(cl_freq, 19, 7, cl_len, &dummy)) {
                for (u32 i = 0; i < 19; i++) {
                    if (cl_len[cl_order[i]]) { hclen = i + 1; }
                }
                if (hclen < 4) { hclen = 4; }

                canonical_codes(ll_len, 286, dyn_ll);
                canonical_codes(dist_len, 30, dyn_dist);
                canonical_codes(cl_len, 19, dyn_cl);
                dyn_ok = true;
            }
        }
    }

    u64 fixed_bits = 3 + tokens_bits(tokens.items, tokens.count, fixed_ll_len, fixed_dist_len)
                     + fixed_ll_len[256];

    u64 dyn_bits = 0;
    if (dyn_ok) {
        dyn_bits = 3 + 5 + 5 + 4 + (u64)3 * hclen;
        for (u32 i = 0; i < n_runs; i++) {
            dyn_bits += cl_len[runs[i].sym] + runs[i].extra_bits;
        }
        dyn_bits += tokens_bits(tokens.items, tokens.count, ll_len, dist_len) + ll_len[256];
    }

    u64 stored_bytes = input.size + 5 * ((input.size + 65535 - 1) / 65535);

    enum { BLOCK_STORED, BLOCK_FIXED, BLOCK_DYNAMIC } block = BLOCK_STORED;
    u64 fixed_bytes = (fixed_bits + 7) / 8;
    u64 dyn_bytes = (dyn_bits + 7) / 8;

    if (dyn_ok && dyn_bytes < stored_bytes && dyn_bytes <= fixed_bytes) {
        block = BLOCK_DYNAMIC;
    } else if (fixed_bytes < stored_bytes) {
        block = BLOCK_FIXED;
    }

    bit_writer bw = bw_init();

    if (block == BLOCK_STORED) {
        u64 left = input.size;
        u64 off = 0;
        while (left > 0) {
            u64 n = left > 65535 ? 65535 : left;
            stored_block_write(&bw, input.str + off, n, left == n);
            off += n;
            left -= n;
        }
    } else if (block == BLOCK_FIXED) {
        generic_huff_node ll[288];
        generic_huff_node dist[32];
        canonical_codes(fixed_ll_len, 288, ll);
        canonical_codes(fixed_dist_len, 32, dist);

        bw_write(&bw, 1, 1); // BFINAL
        bw_write(&bw, 1, 2); // BTYPE 01 (fixed huffman)
        tokens_write(&bw, tokens.items, tokens.count, ll, dist);
    } else {
        bw_write(&bw, 1, 1); // BFINAL
        bw_write(&bw, 2, 2); // BTYPE 10 (dynamic huffman)
        bw_write(&bw, hlit - 257, 5);
        bw_write(&bw, hdist - 1, 5);
        bw_write(&bw, hclen - 4, 4);
        for (u32 i = 0; i < hclen; i++) {
            bw_write(&bw, cl_len[cl_order[i]], 3);
        }
        for (u32 i = 0; i < n_runs; i++) {
            u8 s = runs[i].sym;
            bw_write(&bw, reverse_bits(dyn_cl[s].code, dyn_cl[s].len), dyn_cl[s].len);
            bw_write(&bw, runs[i].extra, runs[i].extra_bits);
        }
        tokens_write(&bw, tokens.items, tokens.count, dyn_ll, dyn_dist);
    }

    free(tokens.items);

    bw.data.size = bw_byte_size(&bw);
    return bw.data;
}

// wrap the input in a gzip member: header + deflate + crc32/isize trailer
string8 gzip(string8 input) {
    string8 deflated = deflate(input);

    string8 out = {
        .size = 10 + deflated.size + 8,
        .str = malloc(10 + deflated.size + 8)
    };
    if (out.str == NULL) { fatal("out of memory"); }

    out.str[0] = 0x1F;
    out.str[1] = 0x8B;
    out.str[2] = 8;
    out.str[3] = 0; // no flags
    for (u32 i = 4; i < 8; i++) { out.str[i] = 0; } // mtime
    out.str[8] = 0; // xfl
    out.str[9] = 3; // os = unix

    memcpy(out.str + 10, deflated.str, deflated.size);

    u32 crc = crc32(input.str, input.size);
    for (u32 i = 0; i < 4; i++) {
        out.str[10 + deflated.size + i] = (u8)(crc >> (8 * i));
    }
    for (u32 i = 0; i < 4; i++) {
        out.str[10 + deflated.size + 4 + i] = (u8)((u32)input.size >> (8 * i));
    }

    free(deflated.str);
    return out;
}

void usage(void) {
    fprintf(stderr,
        "usage: gunzip                demo: decompress + recompress test files\n"
        "       gunzip -c <file>      compress <file> into <file>.gz\n"
        "       gunzip -d <file.gz>   decompress into <file> (needs a .gz name)\n"
        "       gunzip -d <file.gz> -o <out>\n");
}

void write_file(const char* path, string8 data) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) { fatal("could not write output file"); }
    if (data.size > 0 && fwrite(data.str, 1, data.size, f) != data.size) {
        fatal("could not write output file");
    }
    fclose(f);
}

char* strip_gz_suffix(const char* path) {
    u64 n = strlen(path);
    if (n > 3 && strcmp(path + n - 3, ".gz") == 0) {
        char* out = malloc(n - 2);
        memcpy(out, path, n - 3);
        out[n - 3] = '\0';
        return out;
    }
    return NULL;
}

void run_demo(void) {
    string8 test0_gz = read_file("test0.gz");
    string8 test1_gz = read_file("test1.gz");

    string8 test0 = gunzip(test0_gz);
    string8 test1 = gunzip(test1_gz);

    printf("%.*s\n\n", (int)test0.size, test0.str);
    printf("%.*s\n\n", (int)test1.size, test1.str);

    // compress both back and check the round trip
    string8 test0_out = gzip(test0);
    string8 test1_out = gzip(test1);

    string8 test0_check = gunzip(test0_out);
    string8 test1_check = gunzip(test1_out);

    b32 ok0 = test0_check.size == test0.size && !memcmp(test0_check.str, test0.str, test0.size);
    b32 ok1 = test1_check.size == test1.size && !memcmp(test1_check.str, test1.str, test1.size);

    printf("test0.gz %llu -> %llu bytes  roundtrip %s\n",
           (unsigned long long)test0_gz.size, (unsigned long long)test0_out.size,
           ok0 ? "ok" : "FAILED");
    printf("test1.gz %llu -> %llu bytes  roundtrip %s\n",
           (unsigned long long)test1_gz.size, (unsigned long long)test1_out.size,
           ok1 ? "ok" : "FAILED");
}

int main(int argc, char** argv) {
    build_crc32_table();

    if (argc == 1) {
        run_demo();
        return 0;
    }

    b32 compress = false;
    b32 decompress = false;
    const char* in_path = NULL;
    const char* out_path = NULL;

    for (u32 i = 1; i < (u32)argc; i++) {
        const char* arg = argv[i];

        if (!strcmp(arg, "-c")) {
            compress = true;
        } else if (!strcmp(arg, "-d")) {
            decompress = true;
        } else if (!strcmp(arg, "-o")) {
            if (i + 1 >= (u32)argc) { fatal("missing file after -o"); }
            out_path = argv[++i];
        } else if (arg[0] == '-') {
            usage();
            return 1;
        } else {
            if (in_path != NULL) { usage(); return 1; }
            in_path = arg;
        }
    }

    if (in_path == NULL || compress == decompress) {
        usage();
        return 1;
    }

    if (compress) {
        string8 in = read_file(in_path);
        string8 gz = gzip(in);

        char* default_out = NULL;
        if (out_path == NULL) {
            default_out = malloc(strlen(in_path) + 4);
            sprintf(default_out, "%s.gz", in_path);
            out_path = default_out;
        }

        write_file(out_path, gz);

        printf("%llu bytes -> %llu bytes: %s\n",
               (unsigned long long)in.size, (unsigned long long)gz.size, out_path);

        free(default_out);
    } else {
        string8 gz = read_file(in_path);
        string8 out = gunzip(gz);

        char* default_out = NULL;
        if (out_path == NULL) {
            default_out = strip_gz_suffix(in_path);
            if (default_out == NULL) {
                fatal("input name does not end in .gz, use -o to choose the output");
            }
            out_path = default_out;
        }

        write_file(out_path, out);

        printf("%llu bytes -> %llu bytes: %s\n",
               (unsigned long long)gz.size, (unsigned long long)out.size, out_path);

        free(default_out);
    }

    return 0;
}
