/**
 * @file hash_table.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief libyang dictionary for storing strings and generic hash table
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#include "common.h"
#include "context.h"
#include "hash_table.h"

void
lydict_init(struct dict_table *dict)
{
    if (!dict) {
        LOGARG;
        return;
    }

    dict->hash_mask = DICT_SIZE - 1;
    pthread_mutex_init(&dict->lock, NULL);
}

void
lydict_clean(struct dict_table *dict)
{
    int i;
    struct dict_rec *chain, *rec;

    if (!dict) {
        LOGARG;
        return;
    }

    for (i = 0; i < DICT_SIZE; i++) {
        rec = &dict->recs[i];
        chain = rec->next;

        free(rec->value);
        while (chain) {
            rec = chain;
            chain = rec->next;

            free(rec->value);
            free(rec);
        }
    }

    pthread_mutex_destroy(&dict->lock);
}

/*
 * Bob Jenkin's one-at-a-time hash
 * http://www.burtleburtle.net/bob/hash/doobs.html
 *
 * Spooky hash is faster, but it works only for little endian architectures.
 */
static uint32_t
dict_hash(const char *key, size_t len)
{
    uint32_t hash, i;

    for (hash = i = 0; i < len; ++i) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

/*
 * Usage:
 * - init hash to 0
 * - repeatedly call dict_hash_multi(), provide hash from the last call
 * - call dict_hash_multi() with key_part = NULL to finish the hash
 */
uint32_t
dict_hash_multi(uint32_t hash, const char *key_part, size_t len)
{
    uint32_t i;

    if (key_part) {
        for (i = 0; i < len; ++i) {
            hash += key_part[i];
            hash += (hash << 10);
            hash ^= (hash >> 6);
        }
    } else {
        hash += (hash << 3);
        hash ^= (hash >> 11);
        hash += (hash << 15);
    }

    return hash;
}

API void
lydict_remove(struct ly_ctx *ctx, const char *value)
{
    size_t len;
    uint32_t index;
    struct dict_rec *record, *prev = NULL;

    if (!value || !ctx) {
        return;
    }

    len = strlen(value);

    pthread_mutex_lock(&ctx->dict.lock);

    if (!ctx->dict.used) {
        pthread_mutex_unlock(&ctx->dict.lock);
        return;
    }

    index = dict_hash(value, len) & ctx->dict.hash_mask;
    record = &ctx->dict.recs[index];

    while (record && record->value != value) {
        prev = record;
        record = record->next;
    }

    if (!record) {
        /* record not found */
        pthread_mutex_unlock(&ctx->dict.lock);
        return;
    }

    record->refcount--;
    if (!record->refcount) {
        free(record->value);
        if (record->next) {
            if (prev) {
                /* change in dynamically allocated chain */
                prev->next = record->next;
                free(record);
            } else {
                /* move dynamically allocated record into the static array */
                prev = record->next;    /* temporary storage */
                memcpy(record, record->next, sizeof *record);
                free(prev);
            }
        } else if (prev) {
            /* removing last record from the dynamically allocated chain */
            prev->next = NULL;
            free(record);
        } else {
            /* clean the static record content */
            memset(record, 0, sizeof *record);
        }
        ctx->dict.used--;
    }

    pthread_mutex_unlock(&ctx->dict.lock);
}

static char *
dict_insert(struct ly_ctx *ctx, char *value, size_t len, int zerocopy)
{
    uint32_t index;
    int match = 0;
    struct dict_rec *record, *new;

    index = dict_hash(value, len) & ctx->dict.hash_mask;
    record = &ctx->dict.recs[index];

    if (!record->value) {
        /* first record with this hash */
        if (zerocopy) {
            record->value = value;
        } else {
            record->value = malloc((len + 1) * sizeof *record->value);
            LY_CHECK_ERR_RETURN(!record->value, LOGMEM(ctx), NULL);
            memcpy(record->value, value, len);
            record->value[len] = '\0';
        }
        record->refcount = 1;
        if (len > DICT_REC_MAXLEN) {
            record->len = 0;
        } else {
            record->len = len;
        }
        record->next = NULL;

        ctx->dict.used++;

        LOGDBG(LY_LDGDICT, "inserting \"%s\"", record->value);
        return record->value;
    }

    /* collision, search if the value is already in dict */
    while (record) {
        if (record->len) {
            /* for strings shorter than DICT_REC_MAXLEN we are able to speed up
             * recognition of varying strings according to their lengths, and
             * for strings with the same length it is safe to use faster memcmp()
             * instead of strncmp() */
            if ((record->len == len) && !memcmp(value, record->value, len)) {
                match = 1;
            }
        } else {
            if (!strncmp(value, record->value, len) && record->value[len] == '\0') {
                match = 1;
            }
        }
        if (match) {
            /* record found */
            if (record->refcount == DICT_REC_MAXCOUNT) {
                LOGWRN(ctx, "Refcount overflow detected, duplicating dictionary record");
                break;
            }
            record->refcount++;

            if (zerocopy) {
                free(value);
            }

            LOGDBG(LY_LDGDICT, "inserting (refcount) \"%s\"", record->value);
            return record->value;
        }

        if (!record->next) {
            /* not present, add as a new record in chain */
            break;
        }

        record = record->next;
    }

    /* create new record and add it behind the last record */
    new = malloc(sizeof *record);
    LY_CHECK_ERR_RETURN(!new, LOGMEM(ctx), NULL);
    if (zerocopy) {
        new->value = value;
    } else {
        new->value = malloc((len + 1) * sizeof *record->value);
        LY_CHECK_ERR_RETURN(!new->value, LOGMEM(ctx); free(new), NULL);
        memcpy(new->value, value, len);
        new->value[len] = '\0';
    }
    new->refcount = 1;
    if (len > DICT_REC_MAXLEN) {
        new->len = 0;
    } else {
        new->len = len;
    }
    new->next = record->next; /* in case of refcount overflow, we are not at the end of chain */
    record->next = new;

    ctx->dict.used++;

    LOGDBG(LY_LDGDICT, "inserting \"%s\" with collision ", new->value);
    return new->value;
}

API const char *
lydict_insert(struct ly_ctx *ctx, const char *value, size_t len)
{
    const char *result;

    if (value && !len) {
        len = strlen(value);
    }

    if (!value) {
        return NULL;
    }

    pthread_mutex_lock(&ctx->dict.lock);
    result = dict_insert(ctx, (char *)value, len, 0);
    pthread_mutex_unlock(&ctx->dict.lock);

    return result;
}

API const char *
lydict_insert_zc(struct ly_ctx *ctx, char *value)
{
    const char *result;

    if (!value) {
        return NULL;
    }

    pthread_mutex_lock(&ctx->dict.lock);
    result = dict_insert(ctx, value, strlen(value), 1);
    pthread_mutex_unlock(&ctx->dict.lock);

    return result;
}

struct hash_table *
lyht_new(uint32_t size, values_equal_cb val_equal, void *cb_data, int resize)
{
    struct hash_table *ht;

    /* check that 2^x == size (power of 2) */
    assert(size && !(size & (size - 1)));
    assert(val_equal);
    assert(resize == 0 || resize == 1);

    if (size < LYHT_MIN_SIZE) {
        size = LYHT_MIN_SIZE;
    }

    ht = malloc(sizeof *ht);
    LY_CHECK_ERR_RETURN(!ht, LOGMEM(NULL), NULL);

    ht->recs = calloc(size, sizeof *ht->recs);
    LY_CHECK_ERR_RETURN(!ht->recs, free(ht); LOGMEM(NULL), NULL);

    ht->used = 0;
    ht->size = size;
    ht->val_equal = val_equal;
    ht->cb_data = cb_data;
    ht->resize = resize;
    return ht;
}

void
lyht_free(struct hash_table *ht)
{
    if (ht) {
        free(ht->recs);
        free(ht);
    }
}

static int
lyht_resize(struct hash_table *ht, int enlarge)
{
    struct ht_rec *old_recs;
    uint32_t i, new_size;

    old_recs = ht->recs;

    if (enlarge) {
        /* double the size */
        new_size = ht->size << 1;
    } else {
        /* half the size */
        new_size = ht->size >> 1;
    }

    ht->recs = calloc(new_size, sizeof *ht->recs);
    LY_CHECK_ERR_RETURN(!ht->recs, LOGMEM(NULL); ht->recs = old_recs, -1);

    /* reset used, it will increase again */
    ht->used = 0;

    /* add all the old records into the new records array */
    for (i = 0; i < ht->size; ++i) {
        if (old_recs[i].value) {
            lyht_insert(ht, old_recs[i].value, old_recs[i].hash);
        }
    }

    /* final touches */
    ht->size = new_size;
    free(old_recs);
    return 0;
}

int
lyht_insert(struct hash_table *ht, void *value, uint32_t hash)
{
    uint32_t i, idx, c;
    int ret = 0;

    idx = hash & (ht->size - 1);

    if (ht->recs[idx].value) {
        /* is it collision or is the cell just filled by an overflow item? */
        for (i = idx; ht->recs[i].value && (ht->recs[i].hash != hash); i = (i + 1) % ht->size);
        if (!ht->recs[i].value) {
            goto first;
        }

        /* collision or instance duplication */
        c = ht->recs[i].hits;
        do {
            if (ht->recs[i].hash != hash) {
                i = (i + 1) % ht->size;
                continue;
            }

            /* compare nodes */
            if (ht->val_equal(value, ht->recs[i].value, ht->cb_data)) {
                /* instance duplication */
                return 1;
            }
        } while (c--);

        /* collision, insert item into next free cell */
        if (ht->recs[idx].hits == UINT8_MAX) {
            LOGINT(NULL);
        }
        ++ht->recs[idx].hits;
        for (i = (i + 1) % ht->size; ht->recs[i].value; i = (i + 1) % ht->size);
        ht->recs[i].hash = hash;
        ht->recs[i].value = value;
    } else {
first:
        /* first hash instance */
        ht->recs[idx].value = value;
        ht->recs[idx].hash = hash;
    }

    ++ht->used;
    if (ht->resize) {
        c = (ht->used * 100) / ht->size;
        if ((ht->resize == 1) && (c >= LYHT_FIRST_SHRINK_PERCENTAGE)) {
            /* enable shrinking */
            ht->resize = 2;
        } else if ((ht->resize == 2) && (c >= LYHT_ENLARGE_PERCENTAGE)) {
            /* enlarge */
            ret = lyht_resize(ht, 1);
        }
    }
    return ret;
}

int
lyht_remove(struct hash_table *ht, void *value, uint32_t hash)
{
    uint32_t i, idx, c;
    int ret = 0;

    idx = hash & (ht->size - 1);

    for (i = idx; ht->recs[i].value && (ht->recs[i].hash != hash); i = (i + 1) % ht->size);
    if (!ht->recs[i].value) {
        /* we could not find the value */
        return 1;
    }

    /* collision or instance duplication */
    c = ht->recs[i].hits;
    do {
        if (ht->recs[i].hash != hash) {
            i = (i + 1) % ht->size;
            continue;
        }

        /* compare nodes */
        if (ht->val_equal(value, ht->recs[i].value, ht->cb_data)) {
            /* instance found, remove it */
            memset(&(ht->recs[i]), 0, sizeof *ht->recs);
            if (ht->recs[idx].hits) {
                assert(idx != i);
                --ht->recs[idx].hits;
            }

            --ht->used;
            if (ht->resize == 2) {
                c = (ht->used * 100) / ht->size;
                if ((c < LYHT_SHRINK_PERCENTAGE) && (ht->size > LYHT_MIN_SIZE)) {
                    /* shrink */
                    ret = lyht_resize(ht, 0);
                }
            }
            return ret;
        }
    } while (c--);

    /* value not found */
    return 1;
}
