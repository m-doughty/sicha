/* sicha fallback demo — watch the retry/fallback engine work, live,
 * with NO network and NO model: the wire is the public scripted
 * transport, and you script it from the keyboard.
 *
 * Three fake backends form the chain (primary → mirror → fallback).
 * Queue wire events, then fire a streaming request and watch the
 * engine classify failures, back off with jitter, honor Retry-After,
 * and advance down the chain:
 *
 *   o  200 + streaming SSE reply     r  connection reset (retry-same)
 *   5  http 500 (retry-same)         9  http 429 + Retry-After: 2
 *   t  slow headers -> first-byte timeout (advance)
 *   m  200 + malformed body (advance)
 *   a  http 401 (abort - auth error)
 *
 *   ENTER fire request · c cancel in flight · x clear queue · q quit
 *
 * An empty queue answers "connection refused", so ENTER with nothing
 * queued shows the full exhaustion path.
 *
 * `./sicha_fallback --selftest` runs a canonical scenario headless
 * (no tty) and verifies the routing outcome; `--demo-seconds N`
 * auto-queues a showcase scenario, fires it, and exits. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sicha.h"

#ifndef DEMO_SELFTEST_ONLY
#include <notcurses/notcurses.h>
#include <poll.h>
#endif

/* ------------------------------------------------------------------ */
/* The scripted wire                                                   */
/* ------------------------------------------------------------------ */

static const char SSE_REPLY[] =
	"data: {\"model\":\"demo-model\",\"choices\":[{\"delta\":"
	"{\"content\":\"the \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"quick \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"brown \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"fox \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"jumps \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"over \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"the \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"lazy \"}}]}\n\n"
	"data: {\"choices\":[{\"delta\":{\"content\":\"dog.\"},"
	"\"finish_reason\":\"stop\"}]}\n\n"
	"data: [DONE]\n\n";

/* Push one keyed wire event; returns its display letter or 0. */
static char queue_event(sicha_transport *script, char key)
{
	sicha_script_response r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	switch (key) {
	case 'o': /* healthy streaming reply, dribbled */
		r.status = SICHA_T_OK;
		r.http_status = 200;
		r.body = SSE_REPLY;
		r.body_len = SICHA_LEN_CSTR;
		r.chunk_size = 64;
		r.connect_delay_ms = 150;
		r.per_chunk_delay_ms = 120;
		break;
	case 'r': /* connection reset */
		r.status = SICHA_T_E_RESET;
		r.connect_delay_ms = 150;
		break;
	case '5': /* transient server error */
		r.status = SICHA_T_OK;
		r.http_status = 500;
		r.body = "{\"error\":\"upstream exploded\"}";
		r.body_len = SICHA_LEN_CSTR;
		r.connect_delay_ms = 150;
		break;
	case '9': { /* rate limited, with a Retry-After hint */
		static const sicha_header ra = { "Retry-After", "2" };

		r.status = SICHA_T_OK;
		r.http_status = 429;
		r.headers = &ra;
		r.n_headers = 1;
		r.body = "{\"error\":\"slow down\"}";
		r.body_len = SICHA_LEN_CSTR;
		r.connect_delay_ms = 100;
		break;
	}
	case 't': /* headers never arrive within the budget */
		r.status = SICHA_T_OK;
		r.http_status = 200;
		r.body = SSE_REPLY;
		r.body_len = SICHA_LEN_CSTR;
		r.first_byte_delay_ms = 10000;
		break;
	case 'm': /* 200 with garbage body */
		r.status = SICHA_T_OK;
		r.http_status = 200;
		r.body = "<html>a proxy ate your stream</html>";
		r.body_len = SICHA_LEN_CSTR;
		r.connect_delay_ms = 150;
		break;
	case 'a': /* auth error: abort class, ends the whole call */
		r.status = SICHA_T_OK;
		r.http_status = 401;
		r.body = "{\"error\":\"bad api key\"}";
		r.body_len = SICHA_LEN_CSTR;
		r.connect_delay_ms = 100;
		break;
	default:
		return 0;
	}
	if (sicha_script_push(script, &r) != SICHA_OK) {
		return 0;
	}
	return key;
}

/* ------------------------------------------------------------------ */
/* Client over three fake backends                                     */
/* ------------------------------------------------------------------ */

static const char *BACKEND_NAMES[3] = { "primary", "mirror",
	"fallback" };

static sicha_client *make_client(sicha_transport *script,
	uint32_t backoff_base_ms)
{
	sicha_backend_desc descs[3];
	sicha_retry_policy pol;
	sicha_status_override ov = { 429, SICHA_CLASS_RETRY_SAME };
	sicha_client_opts opts;
	sicha_client *client = NULL;
	static const char *urls[3] = { "http://primary.demo/v1",
		"http://mirror.demo/v1", "http://fallback.demo/v1" };

	for (int i = 0; i < 3; i++) {
		memset(&descs[i], 0, sizeof(descs[i]));
		descs[i].struct_size = (uint32_t)sizeof(descs[i]);
		descs[i].base_url = urls[i];
		descs[i].model = BACKEND_NAMES[i];
		descs[i].timeouts.first_byte_ms = 1500;
		descs[i].timeouts.total_ms = 20000;
	}
	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 2;
	pol.backoff_base_ms = backoff_base_ms;
	pol.backoff_cap_ms = backoff_base_ms * 4;
	pol.backoff_jitter_ms = backoff_base_ms / 2;
	/* 429 waits (Retry-After) instead of advancing, so the demo can
	 * show the header being honored */
	pol.overrides = &ov;
	pol.n_overrides = 1;
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = descs;
	opts.n_backends = 3;
	opts.retry = pol;
	opts.transport = script;
	opts.prng_seed = 7;
	if (sicha_client_create(&opts, &client) != SICHA_OK) {
		return NULL;
	}
	return client;
}

/* ------------------------------------------------------------------ */
/* Worker <-> UI handoff                                               */
/* ------------------------------------------------------------------ */

enum { LOG_LINES = 64, LOG_W = 160 };

static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;
static char log_lines[LOG_LINES][LOG_W];
static sicha_error_class log_class[LOG_LINES];
static int log_n;
static char stream_text[512];
static size_t stream_len;
static int cur_backend = -1;
static int io_done;
static sicha_status io_status;
static uint32_t io_attempts;
static int events_consumed;

static void log_push(sicha_error_class cls, const char *line)
{
	pthread_mutex_lock(&io_mu);
	if (log_n == LOG_LINES) {
		memmove(log_lines[0], log_lines[1],
			(LOG_LINES - 1) * LOG_W);
		memmove(&log_class[0], &log_class[1],
			(LOG_LINES - 1) * sizeof(log_class[0]));
		log_n--;
	}
	snprintf(log_lines[log_n], LOG_W, "%s", line);
	log_class[log_n] = cls;
	log_n++;
	pthread_mutex_unlock(&io_mu);
}

static int32_t on_delta(void *ud, const char *bytes, size_t len)
{
	(void)ud;
	pthread_mutex_lock(&io_mu);
	if (stream_len + len < sizeof(stream_text)) {
		memcpy(stream_text + stream_len, bytes, len);
		stream_len += len;
	}
	pthread_mutex_unlock(&io_mu);
	return 0;
}

static void on_attempt(void *ud, const sicha_attempt *a)
{
	char line[LOG_W];

	(void)ud;
	pthread_mutex_lock(&io_mu);
	cur_backend = (int)a->backend;
	events_consumed++;
	pthread_mutex_unlock(&io_mu);
	snprintf(line, sizeof(line),
		"#%u  %-8s try %u  %-11s http %-3d  %4ums  %s",
		a->attempt + 1, BACKEND_NAMES[a->backend],
		a->try_of_backend + 1,
		sicha_error_class_str(a->error_class),
		(int)a->http_status, (unsigned)a->latency_ms,
		a->message);
	log_push(a->error_class, line);
}

typedef struct worker_arg {
	sicha_client *client;
	sicha_cancel *cancel;
} worker_arg;

static void *worker_main(void *p)
{
	worker_arg *w = p;
	sicha_message msg = { SICHA_ROLE_USER, "demo", SICHA_LEN_CSTR,
		NULL };
	sicha_request req;
	sicha_callbacks cbs;
	sicha_result *r = NULL;
	sicha_status st;

	memset(&req, 0, sizeof(req));
	req.struct_size = (uint32_t)sizeof(req);
	req.messages = &msg;
	req.n_messages = 1;
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.on_delta = on_delta;
	cbs.on_attempt = on_attempt;

	st = sicha_chat_stream(w->client, &req, &cbs, w->cancel, &r);

	pthread_mutex_lock(&io_mu);
	io_done = 1;
	io_status = st;
	io_attempts = sicha_result_attempt_count(r);
	pthread_mutex_unlock(&io_mu);
	sicha_result_destroy(r);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Headless selftest                                                   */
/* ------------------------------------------------------------------ */

static int selftest(void)
{
	sicha_transport *script = sicha_script_create();
	sicha_client *client = make_client(script, 50); /* fast waits */
	worker_arg warg;
	int ok = 1;

	if (client == NULL) {
		fprintf(stderr, "selftest: client create failed\n");
		return 1;
	}
	/* primary: reset then 500 (budget of 2 spent) -> mirror:
	 * timeout (advance) -> fallback: ok */
	queue_event(script, 'r');
	queue_event(script, '5');
	queue_event(script, 't');
	queue_event(script, 'o');

	warg.client = client;
	warg.cancel = NULL;
	worker_main(&warg);

	ok = ok && io_done == 1;
	ok = ok && io_status == SICHA_OK;
	ok = ok && io_attempts == 4;
	ok = ok && cur_backend == 2;
	ok = ok && stream_len == 44 &&
		memcmp(stream_text,
			"the quick brown fox jumps over the lazy dog.",
			44) == 0;
	for (int i = 0; i < log_n; i++) {
		printf("%s\n", log_lines[i]);
	}
	printf("selftest: %s\n", ok ? "OK" : "FAILED");
	sicha_client_destroy(client);
	sicha_script_destroy(script);
	return ok ? 0 : 1;
}

#ifndef DEMO_SELFTEST_ONLY

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void class_color(struct ncplane *p, sicha_error_class c)
{
	switch (c) {
	case SICHA_CLASS_NONE:
		ncplane_set_fg_rgb8(p, 120, 230, 120);
		break;
	case SICHA_CLASS_RETRY_SAME:
		ncplane_set_fg_rgb8(p, 230, 210, 100);
		break;
	case SICHA_CLASS_ADVANCE:
		ncplane_set_fg_rgb8(p, 220, 140, 240);
		break;
	default:
		ncplane_set_fg_rgb8(p, 255, 120, 120);
		break;
	}
}

static int run_ui(int demo_seconds)
{
	struct notcurses_options nopts;
	struct notcurses *nc;
	struct ncplane *std;
	sicha_transport *script = sicha_script_create();
	sicha_client *client = make_client(script, 1000);
	pthread_t worker;
	worker_arg warg;
	sicha_cancel *cancel = NULL;
	int worker_active = 0;
	char queued[64];
	int n_queued = 0;
	char result_line[192] = "queue events, then ENTER";
	double t_start = now_ms();
	int auto_fired = 0;

	if (client == NULL) {
		fprintf(stderr, "client create failed\n");
		return 1;
	}
	memset(&nopts, 0, sizeof(nopts));
	nopts.flags = NCOPTION_SUPPRESS_BANNERS;
	nc = notcurses_core_init(&nopts, NULL);
	if (nc == NULL) {
		fprintf(stderr, "notcurses_init failed (need a tty)\n");
		return 1;
	}
	std = notcurses_stdplane(nc);

	for (;;) {
		struct timespec pace = { 0, 33 * 1000 * 1000 };
		struct pollfd pfd;
		int quit = 0;
		int fire = 0;

		pfd.fd = notcurses_inputready_fd(nc);
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
			for (;;) {
				struct timespec tiny = { 0, 1000 * 1000 };
				ncinput ni;
				uint32_t key = notcurses_get(nc, &tiny,
					&ni);

				if (key == 0) {
					break;
				}
				if (key == (uint32_t)-1) {
					quit = 1;
					break;
				}
				/* kitty-protocol terminals report releases
				 * (and repeats) distinctly; act on presses */
				if (ni.evtype == NCTYPE_RELEASE) {
					continue;
				}
				if (key == 'q') {
					quit = 1;
					break;
				}
				if (key == NCKEY_ENTER || key == '\r') {
					fire = !worker_active;
				} else if (key == 'c' && worker_active) {
					sicha_cancel_trigger(cancel);
				} else if (key == 'x' && !worker_active) {
					/* rebuild the wire: empty queue */
					sicha_client_destroy(client);
					sicha_script_destroy(script);
					script = sicha_script_create();
					client = make_client(script, 1000);
					n_queued = 0;
					pthread_mutex_lock(&io_mu);
					events_consumed = 0;
					cur_backend = -1;
					pthread_mutex_unlock(&io_mu);
					snprintf(result_line,
						sizeof(result_line),
						"queue cleared");
				} else if (!worker_active &&
					key < 0x80 &&
					n_queued <
						(int)sizeof(queued)) {
					char c = queue_event(script,
						(char)key);

					if (c != 0) {
						queued[n_queued++] = c;
					}
				}
			}
		}
		if (quit || (demo_seconds > 0 &&
			now_ms() - t_start > demo_seconds * 1000.0)) {
			break;
		}
		/* showcase mode: queue a story and fire it once */
		if (demo_seconds > 0 && !auto_fired && !worker_active) {
			const char *scenario = "r59to";

			for (const char *k = scenario; *k != '\0'; k++) {
				if (n_queued < (int)sizeof(queued) &&
					queue_event(script, *k) != 0) {
					queued[n_queued++] = *k;
				}
			}
			fire = 1;
			auto_fired = 1;
		}

		if (fire) {
			pthread_mutex_lock(&io_mu);
			io_done = 0;
			stream_len = 0;
			cur_backend = -1;
			events_consumed = 0;
			pthread_mutex_unlock(&io_mu);
			cancel = sicha_cancel_create();
			warg.client = client;
			warg.cancel = cancel;
			if (pthread_create(&worker, NULL,
				(void *(*)(void *))worker_main,
				&warg) == 0) {
				worker_active = 1;
				snprintf(result_line,
					sizeof(result_line),
					"running… (c cancels)");
			} else {
				sicha_cancel_destroy(cancel);
				cancel = NULL;
			}
		}

		pthread_mutex_lock(&io_mu);
		if (io_done && worker_active) {
			pthread_mutex_unlock(&io_mu);
			pthread_join(worker, NULL);
			worker_active = 0;
			pthread_mutex_lock(&io_mu);
			io_done = 0;
			snprintf(result_line, sizeof(result_line),
				"%s after %u attempt(s)",
				sicha_status_str(io_status), io_attempts);
			n_queued -= events_consumed > n_queued ?
				n_queued : events_consumed;
		}

		/* ---- draw (still holding io_mu for log/stream) ---- */
		ncplane_erase(std);
		{
			uint32_t rows, cols;
			int y = 0;

			ncplane_dim_yx(std, &rows, &cols);
			ncplane_set_fg_default(std);
			ncplane_cursor_move_yx(std, y++, 0);
			ncplane_putstr(std,
				"sicha fallback demo — the retry engine "
				"on a keyboard-scripted wire");
			y++;
			ncplane_cursor_move_yx(std, y++, 0);
			for (int b = 0; b < 3; b++) {
				int active = worker_active &&
					cur_backend == b;

				if (active) {
					ncplane_set_fg_rgb8(std, 120,
						200, 255);
				} else {
					ncplane_set_fg_rgb8(std, 150,
						150, 150);
				}
				ncplane_printf(std, "  %s[%d %s]%s",
					active ? ">" : " ", b,
					BACKEND_NAMES[b],
					active ? "<" : " ");
			}
			ncplane_set_fg_default(std);
			ncplane_cursor_move_yx(std, y++, 0);
			ncplane_printf(std,
				"  queue: %.*s%s", n_queued, queued,
				n_queued == 0 ?
					"(empty — refused connection)" :
					"");
			y++;
			/* attempt log, newest at the bottom */
			{
				int show = (int)rows - y - 5;
				int first = log_n > show ?
					log_n - show : 0;

				for (int i = first; i < log_n; i++) {
					if (y >= (int)rows - 5) {
						break;
					}
					class_color(std, log_class[i]);
					ncplane_cursor_move_yx(std, y++,
						2);
					ncplane_putnstr(std, cols - 3,
						log_lines[i]);
				}
			}
			ncplane_set_fg_rgb8(std, 120, 230, 120);
			ncplane_cursor_move_yx(std, (int)rows - 4, 0);
			ncplane_printf(std, "  stream: %.*s",
				(int)stream_len, stream_text);
			ncplane_set_fg_default(std);
			ncplane_cursor_move_yx(std, (int)rows - 3, 0);
			ncplane_printf(std, "  %s", result_line);
			ncplane_set_fg_rgb8(std, 140, 140, 140);
			ncplane_cursor_move_yx(std, (int)rows - 1, 0);
			ncplane_putstr(std,
				" o ok · r reset · 5 http500 · 9 429+RA"
				" · t timeout · m malformed · a 401 ·"
				" ENTER fire · c cancel · x clear · q");
		}
		pthread_mutex_unlock(&io_mu);
		notcurses_render(nc);
		nanosleep(&pace, NULL);
	}

	if (worker_active) {
		sicha_cancel_trigger(cancel);
		pthread_join(worker, NULL);
	}
	notcurses_stop(nc);
	sicha_cancel_destroy(cancel);
	sicha_client_destroy(client);
	sicha_script_destroy(script);
	printf("fallback demo session done\n");
	return 0;
}

#endif /* !DEMO_SELFTEST_ONLY */

int main(int argc, char **argv)
{
#ifndef DEMO_SELFTEST_ONLY
	setenv("TERMINFO_DIRS",
		"/usr/share/terminfo:/etc/terminfo:/lib/terminfo:"
		"/usr/lib/terminfo", 0);
#endif
	if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
		return selftest();
	}
#ifdef DEMO_SELFTEST_ONLY
	fprintf(stderr, "built selftest-only; run with --selftest\n");
	return 1;
#else
	{
		int demo_seconds = 0;

		if (argc > 2 && strcmp(argv[1], "--demo-seconds") == 0) {
			demo_seconds = atoi(argv[2]);
		}
		return run_ui(demo_seconds);
	}
#endif
}
