#include <stdio.h>
#include <stdlib.h>

/* ==========================================
 * 1. 动态字符串缓冲区 (核心改进)
 * ========================================== */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynamicBuffer;

// 初始化缓冲区，初始分配 64 字节
void dbuf_init(DynamicBuffer *buf) {
    volatile asm ("nop" ::: "memory");
    buf->cap = 64;
    buf->len = 0;
    buf->data = (char *)malloc(buf->cap);
    if (!buf->data) {
        printf("内存分配失败！\n");
        exit(1);
    }
    buf->data[0] = '\0';
}

// 自动扩容并追加字符
void dbuf_append(DynamicBuffer *buf, char c) {
    if (buf->len + 1 >= buf->cap) {
        buf->cap *= 2; // 容量翻倍
        char *new_data = (char *)realloc(buf->data, buf->cap);
        if (!new_data) {
            printf("内存重分配失败！\n");
            free(buf->data);
            exit(1);
        }
        buf->data = new_data;
    }
    buf->data[buf->len++] = c;
    buf->data[buf->len] = '\0';
}

// 追加字符串
void dbuf_append_str(DynamicBuffer *buf, const char *str) {
    while (*str) {
        dbuf_append(buf, *str++);
    }
}

// 弹出最后一个字符 (用于撤销/回溯)
void dbuf_pop(DynamicBuffer *buf) {
    if (buf->len > 0) {
        buf->len--;
        buf->data[buf->len] = '\0';
    }
}

// 重置缓冲区 (不清空内存，只重置长度)
void dbuf_reset(DynamicBuffer *buf) {
    buf->len = 0;
    if (buf->data) buf->data[0] = '\0';
}

// 释放内存
void dbuf_free(DynamicBuffer *buf) {
    if (buf->data) free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

/* ==========================================
 * 2. 基础工具函数
 * ========================================== */
int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
int is_digit(char c) { return (c >= '0' && c <= '9'); }
int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
int str_cmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
int is_keyword(const char *str) {
    const char *keywords[] = {
        "int", "char", "float", "double", "void", "if", "else", 
        "while", "for", "return", "struct", "typedef", "static", 
        "const", "sizeof", "goto", "inline", NULL
    };
    for (int i = 0; keywords[i] != NULL; i++) {
        if (str_cmp(str, keywords[i]) == 0) return 1;
    }
    return 0;
}
int is_valid_extension(const char *filename) {
    int len = 0;
    while (filename[len] != '\0') len++;
    if (len >= 2 && filename[len-2] == '.') {
        char ext = filename[len-1];
        if (ext == 'c' || ext == 'C' || ext == 'h' || ext == 'H') return 1;
    }
    return 0;
}

void print_token(int *line_has_tokens, int line_num, const char *type, const char *val) {
    if (!(*line_has_tokens)) {
        printf("Line %04d: ", line_num);
        *line_has_tokens = 1;
    }
    printf("<%s: %s>  ", type, val);
}

/* ==========================================
 * 3. 高级括号匹配机 (支持无上限动态缓冲)
 * ========================================== */
int consume_balanced_block(FILE *fp, DynamicBuffer *buf, int *line_num) {
    int next, paren_count = 0, found_paren = 0;
    int in_str = 0, escape = 0, in_bc = 0, in_lc = 0;

    while ((next = fgetc(fp)) != EOF) {
        if (next == '\n') {
            (*line_num)++;
            in_lc = 0;
            dbuf_append(buf, ' '); // 换行替换为空格
            continue;
        }
        
        dbuf_append(buf, next);

        if (in_bc) {
            if (next == '*') {
                int peek = fgetc(fp);
                if (peek == '/') {
                    in_bc = 0;
                    dbuf_append(buf, peek);
                } else if (peek != EOF) ungetc(peek, fp);
            }
        } else if (in_lc) {
            // 等待换行符解除
        } else if (in_str) {
            if (next == '\\' && !escape) escape = 1;
            else if (next == '"' && !escape) in_str = 0;
            else escape = 0;
        } else {
            if (next == '/') {
                int peek = fgetc(fp);
                if (peek == '*') {
                    in_bc = 1;
                    dbuf_append(buf, peek);
                } else if (peek == '/') {
                    in_lc = 1;
                    dbuf_append(buf, peek);
                } else if (peek != EOF) ungetc(peek, fp);
            } else if (next == '"') {
                in_str = 1;
            } else if (next == '(') {
                paren_count++;
                found_paren = 1;
            } else if (next == ')') {
                if (found_paren) {
                    paren_count--;
                    if (paren_count == 0) return 1;
                }
            } else if (!found_paren && next == ';') {
                ungetc(';', fp);
                dbuf_pop(buf); // 撤销刚加进缓冲区的分号
                return 0;
            }
        }
    }
    return 0;
}

/* ==========================================
 * 4. 核心词法分析逻辑
 * ========================================== */
void tokenize_file(const char *filepath) {
    if (!is_valid_extension(filepath)) return;
    FILE *fp = fopen(filepath, "r");
    if (!fp) return;

    printf("\n-------------------- 正在分析: %s --------------------\n", filepath);

    int c;
    int in_block_comment = 0;
    int line_num = 1, line_has_tokens = 0;
    
    // 初始化动态缓冲区
    DynamicBuffer buf;
    dbuf_init(&buf);

    while ((c = fgetc(fp)) != EOF) {
        if (in_block_comment) {
            if (c == '*') {
                int next = fgetc(fp);
                if (next == '/') in_block_comment = 0;
                else ungetc(next, fp);
            } else if (c == '\n') {
                if (line_has_tokens) { printf("\n"); line_has_tokens = 0; }
                line_num++;
            }
            continue;
        }

        if (is_space(c)) { 
            if (c == '\n') { 
                if (line_has_tokens) { printf("\n"); line_has_tokens = 0; } 
                line_num++; 
            } 
            continue; 
        }

        if (c == '/') {
            int next = fgetc(fp);
            if (next == '/') {
                while ((c = fgetc(fp)) != EOF && c != '\n');
                if (c == '\n') { if (line_has_tokens) { printf("\n"); line_has_tokens = 0; } line_num++; }
                continue;
            } else if (next == '*') {
                in_block_comment = 1;
                continue;
            } else {
                ungetc(next, fp);
                print_token(&line_has_tokens, line_num, "OPERATOR", "/");
                continue;
            }
        }

        if (c == '#') {
            dbuf_reset(&buf);
            dbuf_append(&buf, '#');
            int next;
            while ((next = fgetc(fp)) != EOF && next != '\n' && is_space(next));
            if (next != EOF && is_alpha(next)) {
                dbuf_append(&buf, next);
                while ((next = fgetc(fp)) != EOF && is_alnum(next)) dbuf_append(&buf, next);
                if (next != EOF) ungetc(next, fp);
            } else if (next != EOF) ungetc(next, fp);
            print_token(&line_has_tokens, line_num, "PREPROCESSOR", buf.data);
            continue;
        }

        if (c == '"') {
            dbuf_reset(&buf);
            dbuf_append(&buf, '"');
            int escape = 0;
            while ((c = fgetc(fp)) != EOF) {
                dbuf_append(&buf, c);
                if (c == '\\' && !escape) escape = 1;
                else if (c == '"' && !escape) break;
                else escape = 0;
            }
            print_token(&line_has_tokens, line_num, "STRING", buf.data);
            continue;
        }

        if (c == '\'') {
            dbuf_reset(&buf);
            dbuf_append(&buf, '\'');
            int escape = 0;
            while ((c = fgetc(fp)) != EOF) {
                dbuf_append(&buf, c);
                if (c == '\\' && !escape) escape = 1;
                else if (c == '\'' && !escape) break;
                else escape = 0;
            }
            print_token(&line_has_tokens, line_num, "CHAR", buf.data);
            continue;
        }

        if (is_digit(c)) {
            dbuf_reset(&buf);
            dbuf_append(&buf, c);
            int is_hex = 0, next = fgetc(fp);
            if (c == '0' && (next == 'x' || next == 'X')) { is_hex = 1; dbuf_append(&buf, next); } 
            else ungetc(next, fp);

            while ((c = fgetc(fp)) != EOF) {
                if (is_hex) {
                    if (is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) dbuf_append(&buf, c);
                    else break;
                } else {
                    if (is_digit(c) || c == '.') dbuf_append(&buf, c);
                    else break;
                }
            }
            ungetc(c, fp);
            print_token(&line_has_tokens, line_num, "NUMBER", buf.data);
            continue;
        }

        if (is_alpha(c)) {
            dbuf_reset(&buf);
            dbuf_append(&buf, c);
            while ((c = fgetc(fp)) != EOF && is_alnum(c)) {
                dbuf_append(&buf, c);
            }
            ungetc(c, fp);

            // --------- 拦截：__attribute__ ---------
            if (str_cmp(buf.data, "__attribute__") == 0) {
                int next, peek_line_add = 0;
                while ((next = fgetc(fp)) != EOF && is_space(next)) {
                    if (next == '\n') peek_line_add++;
                }
                if (next == '(') {
                    line_num += peek_line_add;
                    ungetc('(', fp);
                    consume_balanced_block(fp, &buf, &line_num);
                    print_token(&line_has_tokens, line_num, "ATTRIBUTE", buf.data);
                    continue;
                } else {
                    if (next != EOF) ungetc(next, fp);
                    line_num += peek_line_add;
                }
            }

            // --------- 拦截：asm / __asm__ ---------
            if (str_cmp(buf.data, "asm") == 0 || str_cmp(buf.data, "__asm__") == 0) {
                consume_balanced_block(fp, &buf, &line_num);
                print_token(&line_has_tokens, line_num, "INLINE_ASM", buf.data);
                continue;
            }

            if (is_keyword(buf.data)) print_token(&line_has_tokens, line_num, "KEYWORD", buf.data);
            else print_token(&line_has_tokens, line_num, "IDENTIFIER", buf.data);
            continue;
        }

        // 标点与操作符
        char op[3] = { (char)c, '\0', '\0' };
        if (c == '{' || c == '}' || c == '(' || c == ')' || 
            c == '[' || c == ']' || c == ';' || c == ',' || c == '.') {
            print_token(&line_has_tokens, line_num, "PUNCTUATION", op);
        } else {
            int next = fgetc(fp);
            if ((c == '=' && next == '=') || (c == '!' && next == '=') || 
                (c == '<' && (next == '=' || next == '<')) || (c == '>' && (next == '=' || next == '>')) ||
                (c == '&' && next == '&') || (c == '|' && next == '|') ||
                (c == '+' && next == '+') || (c == '-' && (next == '-' || next == '>'))) {
                op[1] = next; 
            } else if (next != EOF) ungetc(next, fp);
            print_token(&line_has_tokens, line_num, "OPERATOR", op);
        }
    }

    if (line_has_tokens) printf("\n");
    
    // 安全释放动态内存
    dbuf_free(&buf);
    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    for (int i = 1; i < argc; i++) tokenize_file(argv[i]);
    return 0;
}