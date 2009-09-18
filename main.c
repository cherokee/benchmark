/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Cherokee Benchmark
 *
 * Authors:
 *      Alvaro Lopez Ortega <alvaro@alobbs.com>
 *
 * Copyright (C) 2001-2009 Alvaro Lopez Ortega
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */ 

#include <unistd.h>
#include <time.h>
#include <sys/resource.h>

#include <pthread.h>

#include <cherokee/buffer.h>
#include <cherokee/list.h>

#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h> 

#define EXIT_OK             0
#define EXIT_ERROR          1
#define THREAD_NUM_DEFAULT  10
#define REQUEST_NUM_DEFAULT 10000
#define KEEPALIVE_DEFAULT   0
#define RESPONSES_COUNT_LEN 10
#define THREAD_STACK_SIZE   80 * 1024

#define APP_VERSION  "0.1"
#define APP_NAME     "Cherokee Benchmark"

#define APP_COPY_NOTICE \
	"Written by Alvaro Lopez Ortega <alvaro@alobbs.com>\n\n"		       \
	"Copyright (C) 2009 Alvaro Lopez Ortega.\n"		                       \
	"This is free software; see the source for copying conditions.  There is NO\n" \
	"warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"


#define ALLOCATE(v,t)				\
	v = (t *) malloc(sizeof(t));		\
	if (unlikely (v == NULL)) {		\
		return ret_nomem;		\
	}

typedef struct {
	cherokee_list_t  entry;
	pthread_t        pthread;
	pthread_mutex_t  start_mutex;
	CURL            *curl;	
} cb_thread_t;
#define THREAD(t) ((cb_thread_t *)(t))

typedef struct {
	cherokee_list_t   entry;
	cherokee_buffer_t url;
} cb_url_t;
#define URL(u) ((cb_url_t *)(u))

typedef struct {
	int  http_code;
	long count;
} cb_response_t;


typedef unsigned long long time_msec_t;

/* Globals
 */
static cherokee_list_t        urls;
static int                    keepalive     = KEEPALIVE_DEFAULT;
static int                    thread_num    = THREAD_NUM_DEFAULT;
static long                   request_num   = REQUEST_NUM_DEFAULT;
static int                    verbose       = 0;
static int                    finished      = 0;
static int                    resp_check    = 1;
static int                    resp_size     = -1;
static volatile long          request_done  = 0;
static volatile long          request_fails = 0;
static volatile time_msec_t   time_start    = 0;
static volatile off_t         tx_total      = 0;
static volatile cb_response_t responses[RESPONSES_COUNT_LEN];

static time_msec_t
get_time_msecs (void)
{
	struct timeval tv;

	gettimeofday (&tv, NULL);
	return ((tv.tv_sec * 1000) + (tv.tv_usec) / 1000);
}

static void
print_update (void)
{
	int         elapse;
	time_msec_t time_now;
	int         reqs_sec = 0;
	int         tx_sec   = 0;

	time_now = get_time_msecs();
	elapse = time_now - time_start;

	if ((elapse == 0) ||
	    ((request_done == 0) && (request_fails == 0)))
	{
		return;
	}

	reqs_sec = (int)(request_done / (elapse/1000.0f));
	tx_sec   = (int)(tx_total     / (elapse/1000.0f));

	printf ("threads %d, reqs %li (%d reqs/s avg), TX %llu (%d bytes/s avg), fails %li, %.2f secs\n",
		thread_num, request_done, reqs_sec,
		(long long unsigned) tx_total, tx_sec,
		request_fails, elapse/1000.0f);
}

static void
print_error_codes (void)
{
	int i;

	printf ("\nHTTP responses:\n");
	for (i=0; i<RESPONSES_COUNT_LEN; i++) {
		if (responses[i].http_code == 0) {
			return;
		}
		printf ("  HTTP %d: %lu (%.2f%%)\n", 
			responses[i].http_code, responses[i].count,
			((responses[i].count / (float)request_done) * 100.0f));
	}
}


static int
count_response (int    http_code, 
		double downloaded)
{
	int i;
	int set = 0;

	/* Finished? */
	if (request_done >= request_num) {
		finished = 1;
		return 0;
	}
	request_done++;

	/* HTTP code */
	for (i=0; i<RESPONSES_COUNT_LEN; i++) {
		if (responses[i].http_code == http_code) {
			responses[i].count++;
			set = 1;
			break;

		} else if (responses[i].http_code == 0) {
			responses[i].http_code = http_code;
			responses[i].count     = 1;
			set = 1;
			break;
		}
	}

	if (! set) {
		finished = 1;
		fprintf (stderr, "FATAL ERROR: Run out of http_error space\n");
	}

	/* Was it an error? */
	if (http_code >= 400) {
		return 1;
	}

	/* Check downloaded */
	if (resp_check) {	
		if (resp_size == -1) {
			resp_size = downloaded;
		} else {
			if ((downloaded < resp_size * 0.90) ||
			    (downloaded > resp_size * 1.10))
			{
				return 1;
			}
		}
	}

	return 0;
}

static size_t
cb_write_data (void *ptr, size_t size, size_t nmemb, void *stream)
{
	long total;

	UNUSED(ptr);
	UNUSED(stream);

	total = (size * nmemb);
	tx_total += total;
	return total;
}

static size_t
cb_got_header (void *ptr, size_t size, size_t nmemb, void *stream)
{
	long total;

	UNUSED(ptr);
	UNUSED(stream);

	total = (size * nmemb);
	return total;
}

static void *
thread_routine (void *me)
{
	int          re;
	long         http_code;
	double       downloaded;
	CURLcode     error;
	int          is_error   = 0;
	cb_thread_t *thread     = (cb_thread_t *)me;
	cb_url_t    *url        = (cb_url_t *)urls.next;

	/* Wait until activated. Then, wait a sec until the rest of
	 * the its peers are ready as well.
	 */
	pthread_mutex_lock (&thread->start_mutex);
	sleep(1);

	/* The first thread reads the time
	 */
	if (time_start == 0) {
		time_start = get_time_msecs();
	}

	while (! finished) {
		time_msec_t requested_time;

		/* Configure curl, if needed
		 */
		if (thread->curl == NULL) {
			thread->curl = curl_easy_init();
			curl_easy_setopt (thread->curl, CURLOPT_NOPROGRESS, 1);
			curl_easy_setopt (thread->curl, CURLOPT_WRITEFUNCTION,  cb_write_data);
			curl_easy_setopt (thread->curl, CURLOPT_HEADERFUNCTION, cb_got_header);
			curl_easy_setopt (thread->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
# if 0
			curl_easy_setopt (thread->curl, CURLOPT_VERBOSE, 1);
#endif
		}
		
		curl_easy_setopt (thread->curl, CURLOPT_URL, url->url.buf);
			
		/* Request it
		 */
		requested_time = get_time_msecs();

		error = curl_easy_perform (thread->curl);
		switch (error) {
		case CURLE_OK:
			http_code  = 0;
			downloaded = 0;

			curl_easy_getinfo (thread->curl, CURLINFO_RESPONSE_CODE, &http_code);
			curl_easy_getinfo (thread->curl, CURLINFO_SIZE_DOWNLOAD, &downloaded);

			re = count_response (http_code, downloaded);
			if (re != 0) {
				is_error = 1;
				request_fails++;
			}
			break;

		case CURLE_COULDNT_RESOLVE_HOST:
			finished = 1;
			is_error = 1;
			request_fails++;

			fprintf (stderr, "FATAL ERROR: %s\n", curl_easy_strerror(re));
			break;

		default:
			is_error = 1;
			request_fails++;

			if (verbose) {
				fprintf (stderr, "ERROR: %s (elapsed %.2fs)\n",
					 curl_easy_strerror(re), 
					 ((get_time_msecs() - requested_time)/1000.0f));
			}
		}

		/* Prepare for the next request
		 */
		if ((! keepalive) || (is_error)) {
			curl_easy_cleanup (thread->curl);
			thread->curl = NULL;
		}

		url = (cb_url_t *)((url->entry.next == &urls) ? urls.next : url->entry.next);
	}

	return NULL;
}

static ret_t
raise_fdlimit (int limit)
{
	int           re;
	struct rlimit rl;

	if (limit < 256)
		return ret_ok;

	rl.rlim_cur = limit;
	rl.rlim_max = limit;

	re = setrlimit (RLIMIT_NOFILE, &rl);
	return (re == 0) ? ret_ok : ret_error;
}

static ret_t
thread_launch (cherokee_list_t *threads, int num)
{
	int              i;
	int              re;
	cb_thread_t     *thread;
	cherokee_list_t *item;
	pthread_attr_t   attr;

	/* Create threads
	 */
	for (i=0; i<num; i++) {
		ALLOCATE (thread, cb_thread_t);

		INIT_LIST_HEAD (&thread->entry);
		cherokee_list_add (&thread->entry, threads);
		thread->curl = NULL;

		pthread_mutex_init (&thread->start_mutex, NULL);
		pthread_mutex_lock (&thread->start_mutex);

		pthread_attr_init (&attr);
		pthread_attr_setstacksize (&attr, THREAD_STACK_SIZE);

		re = pthread_create (&thread->pthread, &attr, thread_routine, thread);
		if (re != 0) {
			PRINT_ERROR_S ("Couldn't create pthread\n");

			free (thread);
			return ret_error;
		}
	}

	/* Activate threads
	 */
	list_for_each (item, threads) {
		pthread_mutex_unlock (&((cb_thread_t *)(item))->start_mutex);
	}

	return ret_ok;
}

static void
print_help (void)
{
	printf (APP_NAME "\n"
		"Usage: cherokee-benchmark [options] <URL> [<URL>]\n\n"
		"  -h              Print this help\n"
		"  -V              Print version and exit\n"
		"  -v              Verbose\n"
		"  -k              Use keep-alive connections\n"
		"  -c <NUM>        Concurrency level\n"
		"  -n <NUM>        Stop after no less than <NUM> requests\n\n"
		"Report bugs to http://bugs.cherokee-project.com/\n");
}

static ret_t
process_parameters (int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "hvkVc:n:")) != -1) {
		switch(c) {
		case 'k':
			keepalive = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'c':
			thread_num = atoi(optarg);
			break;
		case 'n':
			request_num = atol(optarg);
			break;
		case 'V':
			printf (APP_NAME " " APP_VERSION "\n" APP_COPY_NOTICE);
			exit (EXIT_OK);
		case 'h':
		case '?':
		default:
			print_help();
			return ret_eof;
		}
	}

	for (c=0; c<argc; c++) {
		cb_url_t *u;

		if (strncmp ("http", argv[c], 4) != 0) {
			continue;
		}

		ALLOCATE (u, cb_url_t);
		cherokee_buffer_init (&u->url);
		cherokee_buffer_add  (&u->url, argv[c], strlen(argv[c]));
		cherokee_list_add (&u->entry, &urls);
	}

	return ret_ok;
}

int
main (int argc, char **argv)
{
	int             i;
	ret_t           ret;
	cherokee_list_t threads;

	/* Initialize
	 */
	INIT_LIST_HEAD (&threads);
	INIT_LIST_HEAD (&urls);

	for (i=0; i<RESPONSES_COUNT_LEN; i++) {
		responses[i].http_code = 0;
		responses[i].count     = 0;
	}

	/* Check arguments
	 */
	ret = process_parameters (argc, argv);
	if (ret != ret_ok) {
		exit (EXIT_ERROR);
	}

	if (cherokee_list_empty (&urls)) {
		print_help();
		exit (EXIT_ERROR);		
	}

	/* Libraries initialization
	 */
	curl_global_init (CURL_GLOBAL_ALL);

	i = (int) (thread_num * 1.2f);
	ret = raise_fdlimit (i);
	if (ret != ret_ok) {
		fprintf (stderr, "WARNING: Couldn't raise fd limit to %d\n", i);
	}

	ret = thread_launch (&threads, thread_num);
	if (ret != ret_ok) {
		exit (EXIT_ERROR);
	}

	/* Main loop
	 */
	sleep(1);
	while (! finished) {
		sleep(1);
		print_update();
	}

	if (verbose) {
		print_error_codes();
	}
	
	return EXIT_OK;
}
