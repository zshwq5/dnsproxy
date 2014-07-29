/*
 * Copyright 2014, Vietor Liu <vietor.liu at gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version. For full terms that can be
 * found in the LICENSE file.
 */

#include "dnsproxy.h"

static struct {
	unsigned int count;
	unsigned int wcount;
	struct rbtree rb_name;
	struct rbtree rb_wname;
	struct rbtree rb_expire;
} g_cache;

typedef struct {
	char* domain;
	int length;
	DOMAIN_CACHE* candidate;
} SEARCH_CONTEXT;

static int name_search(const void* c, const struct rbnode* r)
{
	DOMAIN_CACHE *right;
	SEARCH_CONTEXT *ctx = (SEARCH_CONTEXT*)c;
	right = rbtree_entry(r, DOMAIN_CACHE, rb_name);
	return strcmp(ctx->domain, right->domain);
}

static int name_compare(const struct rbnode* l, const struct rbnode* r)
{
	DOMAIN_CACHE *left, *right;
	left = rbtree_entry(l, DOMAIN_CACHE, rb_name);
	right = rbtree_entry(r, DOMAIN_CACHE, rb_name);
	return strcmp(left->domain, right->domain);
}

static int wname_search(const void* c, const struct rbnode* r)
{
	int ret, pos;
	DOMAIN_CACHE *right;
	SEARCH_CONTEXT *ctx = (SEARCH_CONTEXT*)c;
	right = rbtree_entry(r, DOMAIN_CACHE, rb_name);
	pos = ctx->length - right->d_length;
	if(pos < 0)
		return -1;
	ret = strcmp(ctx->domain + pos, right->domain);
	if(ret == 0) {
		if(pos == 0)
			ret = 0;
		else if(right->p_length == 0) {
			ret = 1;
			ctx->candidate = right;
		}
		else {
			ret = strncmp(ctx->domain, right->prefix, right->p_length);
			if(ret == 0) {
				ctx->candidate = right;
				if(pos > right->p_length)
					ret = 1;
			}
		}
	}
	return ret;
}

static int wname_compare(const struct rbnode* l, const struct rbnode* r)
{
	int ret, pos;
	DOMAIN_CACHE *left, *right;
	left = rbtree_entry(l, DOMAIN_CACHE, rb_name);
	right = rbtree_entry(r, DOMAIN_CACHE, rb_name);
	pos = left->d_length - right->d_length;
	if(pos < 0)
		return -1;
	ret = strcmp(left->domain + pos, right->domain);
	if(ret == 0)
		ret = strcmp(left->prefix, right->prefix);
	return ret;
}

static int expire_compare(const struct rbnode* l, const struct rbnode* r)
{
	DOMAIN_CACHE *left, *right;
	left = rbtree_entry(l, DOMAIN_CACHE, rb_expire);
	right = rbtree_entry(r, DOMAIN_CACHE, rb_expire);
	return (int)(left->expire - right->expire);
}

static inline int is_space(char c)
{
	return c == '\t' || c == ' ' || c == '\n' || c == '\r';
}

static char* skip_space(char* p)
{
	while(*p && is_space(*p))
		++p;
	return p;
}

static char* skip_to_space(char* p)
{
	while(*p && !is_space(*p))
		++p;
	return p;
}

static void name_append(char* domain, int d_length, unsigned short an_length, char *answer)
{
	char *pos;
	struct rbnode *node;
	DOMAIN_CACHE *cache = (DOMAIN_CACHE*)calloc(1, sizeof(DOMAIN_CACHE) + d_length + 1 + an_length);
	if(cache) {
		time(&cache->timestamp);
		pos = cache->buffer;
		cache->domain = pos;
		cache->d_length = d_length;
		memcpy(cache->domain, domain, d_length);
		pos += d_length + 1;
		cache->answer = pos;
		cache->an_count = 1;
		cache->an_length = an_length;
		memcpy(cache->answer, answer, an_length);
		node = rbtree_insert_broken(&g_cache.rb_name, &cache->rb_name);
		if(node != RBNODE_NULL)
			free(cache);
		else
			++g_cache.count;
	}
}

static void wname_append(char* prefix, int p_length, char* domain, int d_length, unsigned short an_length, char *answer)
{
	char *pos;
	struct rbnode *node;
	DOMAIN_CACHE *cache = (DOMAIN_CACHE*)calloc(1, sizeof(DOMAIN_CACHE) + p_length + d_length + 2 + an_length);
	if(cache) {
		pos = cache->buffer;
		cache->prefix = pos;
		cache->p_length = p_length;
		if(p_length > 0)
			memcpy(cache->prefix, prefix, p_length);
		pos += p_length + 1;
		cache->domain = pos;
		cache->d_length = d_length;
		if(d_length > 0)
			memcpy(cache->domain, domain, d_length);
		pos += d_length + 1;
		cache->answer = pos;
		cache->an_count = 1;
		cache->an_length = an_length;
		memcpy(cache->answer, answer, an_length);
		node = rbtree_insert_broken(&g_cache.rb_wname, &cache->rb_name);
		if(node != RBNODE_NULL)
			free(cache);
		else
			++g_cache.wcount;
	}
}

void domain_cache_init(const char* hosts_file)
{
	FILE *fp;
	DNS_QDS *qds;
	struct in_addr addr;
	unsigned short d_length, an_length;
	char line[8192], answer[4096];
	char *rear, *rlimit, *ip, *domain, *pos, *wildcard;

	g_cache.count = 0;
	rbtree_init(&g_cache.rb_name, name_search, name_compare);
	rbtree_init(&g_cache.rb_wname, wname_search, wname_compare);
	rbtree_init(&g_cache.rb_expire, NULL, expire_compare);

	if(hosts_file == NULL)
		return;

	memset(line, 0, sizeof(line));
	fp = fopen(hosts_file, "r");
	if(fp) {
		while(fgets(line, sizeof(line) - 1, fp)) {
			rlimit = line + strlen(line);

			ip = skip_space(line);
			if(is_space(*ip) || *ip == '#')
				continue;
			rear = skip_to_space(ip);
			if(!is_space(*rear))
				continue;
			*rear = '\0';
			addr.s_addr = inet_addr(ip);
			if(addr.s_addr == INADDR_NONE || addr.s_addr == INADDR_ANY)
				continue;

			pos = answer;
			*pos++ = 0xc0;
			*pos++ = 0x0c;
			qds = (DNS_QDS*)pos;
			qds->type = htons(0x01);
			qds->classes = htons(0x01);
			pos += sizeof(DNS_QDS);
			*(unsigned int*)pos = htonl(MAX_TTL);
			pos += sizeof(unsigned int);
			*(unsigned short*)pos = htons(sizeof(addr));
			pos += sizeof(unsigned short);
			memcpy(pos, &addr, sizeof(addr));
			pos += sizeof(addr);
			an_length = pos - answer;

			while(rear + 1 < rlimit) {
				domain = skip_space(rear + 1);
				if(is_space(*domain))
					break;
				rear = skip_to_space(domain);
				if(*rear && is_space(*rear))
					*rear = '\0';

				pos = domain;
				while(*pos) {
					*pos = (char)tolower(*pos);
					++ pos;
				}
				wildcard = strchr(domain, '*');
				if(wildcard) {
					*wildcard++ = '\0';
					wname_append(domain, wildcard - domain - 1, wildcard, rear - wildcard, an_length, answer);
				}
				else {
					d_length = rear - domain;
					name_append(domain, d_length, an_length, answer);
				}
			}
		}
		fclose(fp);
	}
}

DOMAIN_CACHE* domain_cache_search(char* domain)
{
	SEARCH_CONTEXT ctx;
	const struct rbnode *node;
	ctx.domain = domain;
	ctx.length = strlen(domain);
	ctx.candidate = NULL;
	node = rbtree_search(&g_cache.rb_name, &ctx);
	if(node == RBNODE_NULL) {
		if(g_cache.wcount < 1)
			return NULL;
		node = rbtree_search(&g_cache.rb_wname, &ctx);
		if(node == RBNODE_NULL) {
			if(!ctx.candidate)
				return NULL;
			node = &ctx.candidate->rb_name;
		}
	}
	return rbtree_entry(node, DOMAIN_CACHE, rb_name);
}

void domain_cache_append(char* domain, int d_length, unsigned int ttl, unsigned short an_count, unsigned short an_length, char *answer)
{
	struct rbnode *node;
	DOMAIN_CACHE *cache = (DOMAIN_CACHE*)calloc(1, sizeof(DOMAIN_CACHE) + d_length + 1 + an_length);
	if(cache) {
		time(&cache->timestamp);
		cache->expire = cache->timestamp + ttl;
		cache->domain = cache->buffer;
		cache->answer = cache->buffer + d_length + 1;
		cache->d_length = d_length;
		memcpy(cache->domain, domain, d_length);
		cache->an_count = an_count;
		cache->an_length = an_length;
		memcpy(cache->answer, answer, an_length);
		node = rbtree_insert_broken(&g_cache.rb_name, &cache->rb_name);
		if(node != RBNODE_NULL)
			free(cache);
		else
		{
			++g_cache.count;
			rbtree_insert(&g_cache.rb_expire, &cache->rb_expire);
		}
	}
}

void domain_cache_clean(time_t current)
{
	DOMAIN_CACHE* cache;
	struct rbnode *node;

	while(!rbtree_empty(&g_cache.rb_expire)) {
		node = rbtree_first(&g_cache.rb_expire);
		cache = rbtree_entry(node, DOMAIN_CACHE, rb_expire);
		if(cache->expire > current)
			break;
		--g_cache.count;
		rbtree_delete(&g_cache.rb_name, &cache->rb_name);
		rbtree_delete(&g_cache.rb_expire, &cache->rb_expire);
		free(cache);
	}
}
