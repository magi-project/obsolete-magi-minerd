
/*
 * Copyright 2010 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "cpuminer-config.h"
#define _GNU_SOURCE

#include <iostream>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#ifndef WIN32
#include <sys/resource.h>
#endif
#include <getopt.h>
#include <jansson.h>
#include <curl/curl.h>
#include <openssl/sha.h>
extern "C" {
#include "compat.h"
#include "miner.h"
}
#include "uint256.h"
#include "hashblock.h"

#define PROGRAM_NAME		"minerd"
#define DEF_RPC_URL		"http://127.0.0.1:8232/"
#define DEF_RPC_USERNAME	"rpcuser"
#define DEF_RPC_PASSWORD	"rpcpass"
#define DEF_RPC_USERPASS	DEF_RPC_USERNAME ":" DEF_RPC_PASSWORD

#define BEGIN(a)            ((char*)&(a))
#define END(a)              ((char*)&((&(a))[1]))


const signed char p_util_hexdigit[256] =
{ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0,1,2,3,4,5,6,7,8,9,-1,-1,-1,-1,-1,-1,
  -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, };

#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void)
{
	struct sched_param param;

#ifdef SCHED_IDLE
	if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
#ifdef SCHED_BATCH
		sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(&set), &set);
	applog(LOG_INFO, "Binding thread %d to cpu %d", id, cpu);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
}
#endif
		
enum workio_commands {
	WC_GET_WORK,
	WC_SUBMIT_WORK,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct work	*work;
	} u;
};

bool opt_debug = false;
bool opt_protocol = false;
bool want_longpoll = true;
bool have_longpoll = false;
bool use_syslog = false;
static bool opt_quiet = false;
static int opt_retries = INT_MAX; // default: almost infinite retries
static int opt_fail_pause = 10;
int opt_scantime = 1;
uint32_t opt_extranonce = 0;
static json_t *opt_config;
static const bool opt_time = true;
static int opt_n_threads;
static int num_processors;
static char *rpc_url;
static char *rpc_userpass;
static char *rpc_user, *rpc_pass;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id;
struct work_restart *work_restart = NULL;
pthread_mutex_t time_lock;
uint32_t en_work[32];

char *scratchpad;

struct option_help {
	const char	*name;
	const char	*helptext;
};

static struct option_help options_help[] = {
	{ "help",
	  "(-h) Display this help text" },

	{ "config FILE",
	  "(-c FILE) JSON-format configuration file (default: none)\n"
	  "See example-cfg.json for an example configuration." },

	{ "quiet",
	  "(-q) Disable per-thread hashmeter output (default: off)" },

	{ "debug",
	  "(-D) Enable debug output (default: off)" },

	{ "no-longpoll",
	  "Disable X-Long-Polling support (default: enabled)" },

	{ "protocol-dump",
	  "(-P) Verbose dump of protocol-level activities (default: off)" },

	{ "retries N",
	  "(-r N) Number of times to retry, if JSON-RPC call fails\n"
	  "\t(default: 10; use -1 for \"never\")" },

	{ "retry-pause N",
	  "(-R N) Number of seconds to pause, between retries\n"
	  "\t(default: 10)" },

	{ "scantime N",
	  "(-s N) Upper bound on time spent scanning current work,\n"
	  "\tin seconds. (default: 120)" },

#ifdef HAVE_SYSLOG_H
	{ "syslog",
	  "Use system log for output messages (default: standard error)" },
#endif

	{ "threads N",
	  "(-t N) Number of miner threads (default: 1, valid: 1,2,4,8,16..256)" },

	{ "extranonce",
	   "(-e N) Extranonce to displace miners using the same getwork server" },

	{ "url URL",
	  "URL for bitcoin JSON-RPC server "
	  "(default: " DEF_RPC_URL ")" },

	{ "userpass USERNAME:PASSWORD",
	  "Username:Password pair for bitcoin JSON-RPC server "
	  "(default: " DEF_RPC_USERPASS ")" },

	{ "user USERNAME",
	  "(-u USERNAME) Username for bitcoin JSON-RPC server "
	  "(default: " DEF_RPC_USERNAME ")" },

	{ "pass PASSWORD",
	  "(-p PASSWORD) Password for bitcoin JSON-RPC server "
	  "(default: " DEF_RPC_PASSWORD ")" },
};

static struct option options[] = {
	{ "config", 1, NULL, 'c' },
	{ "debug", 0, NULL, 'D' },
	{ "help", 0, NULL, 'h' },
	{ "no-longpoll", 0, NULL, 1003 },
	{ "pass", 1, NULL, 'p' },
	{ "protocol-dump", 0, NULL, 'P' },
	{ "quiet", 0, NULL, 'q' },
	{ "threads", 1, NULL, 't' },
	{ "retries", 1, NULL, 'r' },
	{ "retry-pause", 1, NULL, 'R' },
	{ "scantime", 1, NULL, 's' },
        { "extranonce", 1, NULL, 'e'},
#ifdef HAVE_SYSLOG_H
	{ "syslog", 0, NULL, 1004 },
#endif
	{ "url", 1, NULL, 1001 },
	{ "user", 1, NULL, 'u' },
	{ "userpass", 1, NULL, 1002 },

	{ }
};

#pragma pack(push,1)
class CBlockHeader
{
public:
    //!!!!!!!!!!! struct must be in packed order even though serialize order is version first
    //or else we can't use hash macros, could also use #pragma pack but that has 
    //terrible implicatation on non-x86
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
	    unsigned char Padding[48];
};
#pragma pack(pop)

struct work {
	CBlockHeader 	data;
	uint256	target,hash;
};

struct cwork {
	uint32_t data[32];
	uint32_t target[8];
};


inline uint32_t ByteReverse(uint32_t value)
{
    value = ((value & 0xFF00FF00) >> 8) | ((value & 0x00FF00FF) << 8);
    return (value<<16) | (value>>16);
}


template<typename T>
std::string HexStr(const T itbegin, const T itend, bool fSpaces=false)
{
    std::string rv;
    static const char hexmap[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    rv.reserve((itend-itbegin)*3);
    for(T it = itbegin; it < itend; ++it)
    {
        unsigned char val = (unsigned char)(*it);
        if(fSpaces && it != itbegin)
            rv.push_back(' ');
        rv.push_back(hexmap[val>>4]);
        rv.push_back(hexmap[val&15]);
    }

    return rv;
}

void print_work(struct work *work, char * s)
{
printf("********************************** %s ***********************************\n", s);
printf("nVersion:       %i\n",  work->data.nVersion);
printf("hashPrevBlock:  %s\n",  work->data.hashPrevBlock.GetHex().c_str());
printf("hashMerkleRoot: %s\n",  work->data.hashMerkleRoot.GetHex().c_str());
printf("nTime:          %i\n",  work->data.nTime);
printf("nBits:          %i\n",  work->data.nBits);
printf("nNonce:         %i\n",  work->data.nNonce);
printf("hashtarget:     %s\n",  work->target.GetHex().c_str());
printf("--------------------------------------------------------------------------------\n");
}

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
//	unsigned char cbuf[128];
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		applog(LOG_ERR, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin((unsigned char *)buf, hexstr, buflen))
		return false;

	return true;
}

inline void get_work_en(struct work *work, struct cwork *cwork)
{
    for (int i=0; i < 32; i++)
	be32enc(&en_work[i], ((uint32_t*)cwork->data)[i]);
    memcpy(&work->data.nVersion, en_work, 4);	// nversion
    memcpy(&work->data.nVersion+1, en_work+1, 32);	// hashPrevBlock
    memcpy(&work->data.nVersion+9, en_work+9, 32);	// hashMerkleRoot
    memcpy(&work->data.nVersion+17, en_work+17, 4);	// nTime
    memcpy(&work->data.nVersion+18, en_work+18, 4);	// nBits
    memcpy(&work->data.nVersion+19, en_work+19, 4);	// nNonce
}

inline void submit_work_en(struct work *work)
{
    for (int i=0; i < 32; i++)
	be32enc(&en_work[i], en_work[i]);
    memcpy(&work->data.nVersion, en_work, 4);	// nversion
    memcpy(&work->data.nVersion+1, en_work+1, 32);	// hashPrevBlock
    memcpy(&work->data.nVersion+9, en_work+9, 32);	// hashMerkleRoot
    memcpy(&work->data.nVersion+17, en_work+17, 4);	// nTime
    memcpy(&work->data.nVersion+18, en_work+18, 4);	// nBits
    memcpy(&work->data.nVersion+19, en_work+19, 4);	// nNonce
}

static bool work_decode(const json_t *val, struct work *work)
{
  struct cwork b_work;
  struct cwork *cwork=&b_work;
    
	if (unlikely(!jobj_binary(val, "data", &cwork->data, sizeof(cwork->data)))) {
//	if (unlikely(!jobj_binary(val, "data", &work->data, sizeof(work->data)))) {
		printf("%ld\n", sizeof(work->data));
		applog(LOG_ERR, "JSON inval data");
		goto err_out;
	}

	if (unlikely(!jobj_binary(val, "target", &work->target, sizeof(work->target)))) {
		applog(LOG_ERR, "JSON inval target");
		goto err_out;
	}

	get_work_en(work, cwork);
	if (opt_debug) print_work(work, "Get work");

	work->hash = 0;
	return true;

err_out:
	return false;
}

static bool submit_upstream_work(CURL *curl, struct work *work)
{
	char *hexstr = NULL;
	json_t *val, *res, *err;
	char s[345];
	bool rc = false;

	if (opt_debug) print_work(work, "Submit work");
	submit_work_en(work);

        //work->data.nHeight++;

	/* build hex string */
	hexstr = bin2hex((unsigned char*)&work->data, sizeof(work->data));
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "submit_upstream_work OOM");
		goto out;
	}

	/* build JSON-RPC request */
	sprintf(s,
	      "{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
		hexstr);

	if (opt_debug)
		applog(LOG_DEBUG, "DBG: sending RPC call: %s", s);

	/* issue JSON-RPC request */
	val = json_rpc_call(curl, rpc_url, rpc_userpass, s, false, false);
	if (unlikely(!val)) {
		applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
		goto out;
	}

	res = json_object_get(val, "result");
	err = json_object_get(val, "error");

	if (json_is_true(res)) {
		applog(LOG_INFO, "PROOF OF WORK RESULT: Accepted");
	} else if (json_is_array(err) && json_array_size(err) >= 1 && json_is_string(json_array_get(err, 0))) {
		applog(LOG_INFO, "PROOF OF WORK RESULT: Rejected (%s)", json_string_value(json_array_get(err, 0)));
	} else {
		applog(LOG_INFO, "PROOF OF WORK RESULT: Rejected");
	}

	json_decref(val);

	rc = true;

out:
	free(hexstr);
	return rc;
}

static const char *rpc_req =
	"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	json_t *val;
	bool rc;

	val = json_rpc_call(curl, rpc_url, rpc_userpass, rpc_req,
			    want_longpoll, false);
	if (!val)
		return false;

	rc = work_decode(json_object_get(val, "result"), work);
    
        //printf("%ld %ld %s\n", work->data.nHeight, work->data.nTime, work->target.GetHex().c_str());

	json_decref(val);

	return rc;
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		free(wc->u.work);
		break;
	default: /* do nothing */
		break;
	}

	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl)
{
	struct work *ret_work;
	int failures = 0;

	ret_work = (struct work *)calloc(1, sizeof(*ret_work));
	if (!ret_work)
		return false;

	/* obtain new work from bitcoin via JSON-RPC */
	while (!get_upstream_work(curl, ret_work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
			free(ret_work);
			return false;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "json_rpc_call failed, retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	/* send work to requesting thread */
	if (!tq_push(wc->thr->q, ret_work))
		free(ret_work);

	return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;

	/* submit solution to bitcoin via JSON-RPC */
	while (!submit_upstream_work(curl, wc->u.work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "...terminating workio thread");
			return false;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "...retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	return true;
}

static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	CURL *curl;
	bool ok = true;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		return NULL;
	}

	while (ok) {
		struct workio_cmd *wc;

		/* wait for workio_cmd sent to us, on our queue */
		wc = (workio_cmd*)tq_pop(mythr->q, NULL);
		if (!wc) {
			ok = false;
			break;
		}

		/* process workio_cmd */
		switch (wc->cmd) {
		case WC_GET_WORK:
			ok = workio_get_work(wc, curl);
			break;
		case WC_SUBMIT_WORK:
			ok = workio_submit_work(wc, curl);
			break;

		default:		/* should never happen */
			ok = false;
			break;
		}

		workio_cmd_free(wc);
	}

	tq_freeze(mythr->q);
	curl_easy_cleanup(curl);

	return NULL;
}

static void hashmeter(int thr_id, const struct timeval *diff, unsigned long hashes_done)
{
	double hashes, secs;
	hashes = hashes_done / 65536.0;
	secs = (double)diff->tv_sec + ((double)diff->tv_usec / 1000000.0);
	if (!opt_quiet)
		// applog(LOG_INFO, "thread %d: %.3f hash/min", thr_id, hashes * 60 / secs);
		applog(LOG_INFO, "thread %d: %.2f khash/s", thr_id, hashes_done / secs / 1000);
}

static bool get_work(struct thr_info *thr, struct work *work)
{
	struct workio_cmd *wc;
	struct work *work_heap;

	/* fill out work request message */
	wc = (struct workio_cmd*)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->cmd = WC_GET_WORK;
	wc->thr = thr;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
		return false;
	}

	/* wait for response, a unit of work */
	work_heap = (struct work*)tq_pop(thr->q, NULL);
	if (!work_heap)
		return false;

	/* copy returned work into storage provided by caller */
	memcpy(work, work_heap, sizeof(*work));
	free(work_heap);

	return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;

	/* fill out work request message */
	wc = (struct workio_cmd*)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = (work*)malloc(sizeof(*work_in));
	if (!wc->u.work)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(*work_in));

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}

bool scanhash(int thr_id, CBlockHeader *header, uint256 target, uint32_t max_nonce, uint64_t *hashes_done)
{
	int i;
	uint64_t n = 0;
	uint64_t original_nonce=header->nNonce;
	uint64_t stat_ctr = 0;

	work_restart[thr_id].restart = 0;

        struct hash_context ctx;
        HashInit(ctx);

	while (1) {
		header->nNonce = original_nonce + (((uint64_t)thr_id) << 24) + ((uint64_t)opt_extranonce << 32) + n++;
		//printf("%d %lX\n", opt_extranonce, header->nNonce);
		uint256 hash = Hash7(ctx,BEGIN(header->nVersion), END(header->nNonce));

		stat_ctr += 1;
		bool found = hash < target;
		if (found) {
		memcpy(en_work+19, &header->nNonce, 4);	// nNonce
		applog(LOG_INFO, "Found share  ...%s: submitting", hash.GetHex().c_str());
		printf("           Target hash: ...%s\n", target.GetHex().c_str());
			*hashes_done = stat_ctr;
			return true;
		} 

		// MMC scans only once
		if ((n >= max_nonce) || work_restart[thr_id].restart) {
		 	*hashes_done = n;
			return false;
		}
	}
}

static void *miner_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	int thr_id = mythr->id;
	uint32_t max_nonce = 0xfffff ;

	/* Set worker threads to nice 19 and then preferentially to SCHED_IDLE
	 * and if that fails, then SCHED_BATCH. No need for this to be an
	 * error if it fails */
	setpriority(PRIO_PROCESS, 0, 19);
	drop_policy();

	/* Cpu affinity only makes sense if the number of threads is a multiple
	 * of the number of CPUs */
	if (!(opt_n_threads % num_processors))
		affine_to_cpu(mythr->id, mythr->id % num_processors);

	while (1) {
		struct work work __attribute__((aligned(128)));
		uint64_t hashes_done;
		struct timeval tv_start, tv_end, diff;
		uint64_t max64;
		bool rc;

		/* obtain new work from internal workio thread */
		if (unlikely(!get_work(mythr, &work))) {
			applog(LOG_ERR, "work retrieval failed, exiting "
				"mining thread %d", mythr->id);
			goto out;
		}

		//printf("Scan\n");

		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		rc = scanhash(thr_id, &work.data, work.target, max_nonce, &hashes_done);


		/* record scanhash elapsed time */
		gettimeofday(&tv_end, NULL);
		timeval_subtract(&diff, &tv_end, &tv_start);

		hashmeter(thr_id, &diff, hashes_done);

		/* adjust max_nonce to meet target scan time */
		if (diff.tv_usec > 500000)
			diff.tv_sec++;
		if (diff.tv_sec > 0) {
			max64 =
			   (hashes_done * opt_scantime) / diff.tv_sec;
			if (max64 > 0xfffffffaULL)
				max64 = 0xfffffffaULL;
			max_nonce = max64;
		}
		//printf("Scan2\n");

		/* if nonce found, submit work */
		if (rc && !submit_work(mythr, &work))
			break;
	}

out:
	tq_freeze(mythr->q);

	return NULL;
}

static void restart_threads(void)
{
	int i;

	for (i = 0; i < opt_n_threads; i++)
		work_restart[i].restart = 1;
}

static void *longpoll_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	CURL *curl = NULL;
	char *copy_start, *hdr_path, *lp_url = NULL;
	bool need_slash = false;
	int failures = 0;

	hdr_path = (char*)tq_pop(mythr->q, NULL);
	if (!hdr_path)
		goto out;

	/* full URL */
	if (strstr(hdr_path, "://")) {
		lp_url = hdr_path;
		hdr_path = NULL;
	}
	
	/* absolute path, on current server */
	else {
		copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
		if (rpc_url[strlen(rpc_url) - 1] != '/')
			need_slash = true;

		lp_url = (char*)malloc(strlen(rpc_url) + strlen(copy_start) + 2);
		if (!lp_url)
			goto out;

	}
		sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);

	applog(LOG_INFO, "Long-polling activated for %s", lp_url);

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		goto out;
	}

	while (1) {
		json_t *val;

		val = json_rpc_call(curl, lp_url, rpc_userpass, rpc_req,
				    false, true);
		if (likely(val)) {
			failures = 0;
			json_decref(val);

			applog(LOG_INFO, "LONGPOLL detected new block");
			restart_threads();
		} else {
			if (failures++ < 10) {
				sleep(10);
				applog(LOG_ERR,
					"longpoll failed, sleeping for 10s");
			} else {
				applog(LOG_ERR,
					"longpoll failed, ending thread");
				goto out;
			}
		}
	}

out:
	free(hdr_path);
	free(lp_url);
	tq_freeze(mythr->q);
	if (curl)
		curl_easy_cleanup(curl);

	return NULL;
}

static void show_usage(void)
{
	int i;

	printf("minerd version %s\n\n", VERSION);
	printf("Usage:\tminerd [options]\n\nSupported options:\n");
	for (i = 0; i < ARRAY_SIZE(options_help); i++) {
		struct option_help *h;

		h = &options_help[i];
		printf("--%s\n%s\n\n", h->name, h->helptext);
	}

	exit(1);
}

static void parse_arg (int key, char *arg)
{
	int v, i;

	switch(key) {
	case 'c': {
		json_error_t err;
		if (opt_config)
			json_decref(opt_config);
#if JANSSON_MAJOR_VERSION >= 2
		opt_config = json_load_file(arg, 0, &err);
#else
		opt_config = json_load_file(arg, &err);
#endif
		if (!json_is_object(opt_config)) {
			applog(LOG_ERR, "JSON decode of '%s' failed(%d): %s", arg, err.line, err.text);
			show_usage();
		}
		break;
	}
	case 'q':
		opt_quiet = true;
		break;
        case 'e' :
		opt_extranonce = atoi(arg);
	case 'D':
		opt_debug = true;
		break;
	case 'p':
		free(rpc_pass);
		rpc_pass = strdup(arg);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999)	/* sanity check */
			show_usage();

		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage();

		opt_fail_pause = v;
		break;
	case 's':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage();

		opt_scantime = v;
		break;
	case 't':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage();

//		if (v & (v - 1))
//			show_usage();

		opt_n_threads = v;
		break;
	case 'u':
		free(rpc_user);
		rpc_user = strdup(arg);
		break;
	case 1001:			/* --url */
		if (strncmp(arg, "http://", 7) &&
		    strncmp(arg, "https://", 8))
			show_usage();

		free(rpc_url);
		rpc_url = strdup(arg);
		break;
	case 1002:			/* --userpass */
		if (!strchr(arg, ':'))
			show_usage();

		free(rpc_userpass);
		rpc_userpass = strdup(arg);
		break;
	case 1003:
		want_longpoll = false;
		break;
	case 1004:
		use_syslog = true;
		break;
	default:
		show_usage();
	}

#ifdef WIN32
	if (!opt_n_threads)
		opt_n_threads = 1;
#else
	num_processors = sysconf(_SC_NPROCESSORS_ONLN);
	if (!opt_n_threads)
		opt_n_threads = num_processors;
#endif /* !WIN32 */
    
}

static void parse_config(void)
{
	int i;
	json_t *val;

	if (!json_is_object(opt_config))
		return;

	for (i = 0; i < ARRAY_SIZE(options); i++) {
		if (!options[i].name)
			break;
		if (!strcmp(options[i].name, "config"))
			continue;

		val = json_object_get(opt_config, options[i].name);
		if (!val)
			continue;

		if (options[i].has_arg && json_is_string(val)) {
			char *s = strdup(json_string_value(val));
			if (!s)
				break;
			parse_arg(options[i].val, s);
			free(s);
		} else if (!options[i].has_arg && json_is_true(val))
			parse_arg(options[i].val, "");
		else
			applog(LOG_ERR, "JSON option %s invalid",
				options[i].name);
	}
}

static void parse_cmdline(int argc, char *argv[])
{
	int key;

	while (1) {
		key = getopt_long(argc, argv, "a:c:qDPr:s:t:h?", options, NULL);
		if (key < 0)
			break;

		parse_arg(key, optarg);
	}

	parse_config();
}

int main (int argc, char *argv[])
{
	struct thr_info *thr;
	int i;

	rpc_url = strdup(DEF_RPC_URL);

	/* parse command line */
	parse_cmdline(argc, argv);

	pthread_mutex_init(&time_lock, NULL);

	if (!rpc_userpass) {
		if (!rpc_user) {
			applog(LOG_ERR, "No login credentials supplied");
			return 1;
		}
		if (!rpc_pass) {
			rpc_pass = strdup("x");
		}
		rpc_userpass = (char*)malloc(strlen(rpc_user) + strlen(rpc_pass) + 2);
		if (!rpc_userpass)
			return 1;
		sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	}

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog("cpuminer", LOG_PID, LOG_USER);
#endif

	work_restart = (struct work_restart*)calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return 1;

	thr_info = (struct thr_info*)calloc(opt_n_threads + 2, sizeof(*thr));
	if (!thr_info)
		return 1;

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return 1;

	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		applog(LOG_ERR, "workio thread create failed");
		return 1;
	}

	/* init longpoll thread info */
	if (want_longpoll) {
		longpoll_thr_id = opt_n_threads + 1;
		thr = &thr_info[longpoll_thr_id];
		thr->id = longpoll_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start longpoll thread */
		if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
			applog(LOG_ERR, "longpoll thread create failed");
			return 1;
		}
	} else
		longpoll_thr_id = -1;

	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", i);
			return 1;
		}

		sleep(1);	/* don't pound RPC server all at once */
	}

	applog(LOG_INFO, "%d miner threads started, "
		"using M7 Proof-of-Work algorithm.",
		opt_n_threads);

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL);

	free(scratchpad);

	applog(LOG_INFO, "workio thread dead, exiting.");

	return 0;
}

