/* sicha chat demo — a minimal notcurses chat client over the public
 * sicha API: a scrolling transcript, a line editor, live streaming
 * deltas, Esc-to-cancel (with the KoboldCpp abort assist when asked),
 * multi-turn history, and retry/attempt telemetry in the status line.
 *
 * Usage:
 *   ./sicha_chat <base_url> <model> [api_key]
 * or via SICHA_LIVE_BASE_URL / SICHA_LIVE_MODEL / SICHA_LIVE_API_KEY.
 * Flags:
 *   --kobold          set SICHA_BACKEND_KOBOLD_CANCEL_ASSIST
 *   --auto "message"  send one message unattended, exit after the
 *                     reply completes (pty smoke-test mode)
 *   --demo-seconds N  hard exit timer
 *
 * Keys: type + Enter to send · Esc cancels a running stream (twice:
 * quits) · Esc on an idle prompt quits.
 *
 * Threading pattern worth copying: sicha calls BLOCK and fire their
 * callbacks on the calling thread, and notcurses is single-threaded —
 * so the request runs on a worker thread whose callbacks only write
 * into a mutex-guarded handoff buffer; the UI thread drains it every
 * frame.  Cancellation is just sicha_cancel_trigger from the UI
 * thread. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <notcurses/notcurses.h>
#include <poll.h>

#include "sicha.h"

/* ------------------------------------------------------------------ */
/* Conversation history                                                */
/* ------------------------------------------------------------------ */

enum { MAX_TURNS = 64, MAX_MSGS = MAX_TURNS + 1 };

typedef struct turn {
	int32_t role;
	char *text;
} turn;

static turn turns[MAX_TURNS];
static int n_turns;

static void turns_push(int32_t role, const char *text, size_t len)
{
	if (n_turns == MAX_TURNS) {
		free(turns[0].text);
		free(turns[1].text);
		memmove(&turns[0], &turns[2],
			(MAX_TURNS - 2) * sizeof(turn));
		n_turns -= 2; /* drop the oldest user+assistant pair */
	}
	turns[n_turns].role = role;
	turns[n_turns].text = malloc(len + 1);
	if (turns[n_turns].text == NULL) {
		return;
	}
	memcpy(turns[n_turns].text, text, len);
	turns[n_turns].text[len] = '\0';
	n_turns++;
}

/* ------------------------------------------------------------------ */
/* Worker <-> UI handoff (the only shared state; all mutex-guarded)    */
/* ------------------------------------------------------------------ */

static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;
static char io_pending[65536];
static size_t io_pending_len;
static char io_status[256];
static int io_done;
static sicha_status io_result_status;
static sicha_result *io_result;

static int32_t on_delta(void *ud, const char *bytes, size_t len)
{
	(void)ud;
	pthread_mutex_lock(&io_mu);
	if (io_pending_len + len <= sizeof(io_pending)) {
		memcpy(io_pending + io_pending_len, bytes, len);
		io_pending_len += len;
	}
	pthread_mutex_unlock(&io_mu);
	return 0;
}

static void on_attempt(void *ud, const sicha_attempt *a)
{
	(void)ud;
	pthread_mutex_lock(&io_mu);
	snprintf(io_status, sizeof(io_status),
		"attempt %u · backend %u try %u · %s (%s, %ums)",
		a->attempt + 1, a->backend, a->try_of_backend,
		a->message, sicha_error_class_str(a->error_class),
		(unsigned)a->latency_ms);
	pthread_mutex_unlock(&io_mu);
}

typedef struct worker_arg {
	sicha_client *client;
	sicha_message msgs[MAX_MSGS];
	size_t n_msgs;
	sicha_cancel *cancel;
} worker_arg;

static void *worker_main(void *p)
{
	worker_arg *w = p;
	sicha_callbacks cbs;
	sicha_request req;
	sicha_result *r = NULL;
	sicha_status st;

	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.on_delta = on_delta;
	cbs.on_attempt = on_attempt;
	memset(&req, 0, sizeof(req));
	req.struct_size = (uint32_t)sizeof(req);
	req.messages = w->msgs;
	req.n_messages = w->n_msgs;
	req.set_mask = SICHA_SET_TEMPERATURE | SICHA_SET_MAX_TOKENS;
	req.temperature = 0.8;
	req.max_tokens = 512;

	st = sicha_chat_stream(w->client, &req, &cbs, w->cancel, &r);

	pthread_mutex_lock(&io_mu);
	io_result_status = st;
	io_result = r;
	io_done = 1;
	pthread_mutex_unlock(&io_mu);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* UI helpers                                                          */
/* ------------------------------------------------------------------ */

static size_t utf8_encode(uint32_t cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

enum { COL_USER = 0, COL_BOT, COL_DIM, COL_ERR };

static void set_color(struct ncplane *p, int which)
{
	switch (which) {
	case COL_USER:
		ncplane_set_fg_rgb8(p, 120, 200, 255);
		break;
	case COL_BOT:
		ncplane_set_fg_default(p);
		break;
	case COL_DIM:
		ncplane_set_fg_rgb8(p, 140, 140, 140);
		break;
	default:
		ncplane_set_fg_rgb8(p, 255, 120, 120);
		break;
	}
}

static void transcript_put(struct ncplane *tp, int color, const char *s,
	size_t len)
{
	set_color(tp, color);
	ncplane_putnstr(tp, len, s);
}

/* ------------------------------------------------------------------ */
/* Main UI                                                             */
/* ------------------------------------------------------------------ */

typedef struct app_cfg {
	const char *base_url;
	const char *model;
	const char *api_key;
	int kobold;
	const char *auto_msg;
	int demo_seconds;
} app_cfg;

static sicha_client *make_client(const app_cfg *cfg)
{
	sicha_backend_desc d;
	sicha_client_opts opts;
	sicha_client *client = NULL;

	memset(&d, 0, sizeof(d));
	d.struct_size = (uint32_t)sizeof(d);
	d.base_url = cfg->base_url;
	d.model = cfg->model;
	d.api_key = cfg->api_key;
	if (cfg->kobold) {
		/* the abort assist stops abandoned generations, and
		 * KoboldCpp is happy to report streaming usage (the
		 * include_usage hang lesson is gateway-specific) */
		d.flags |= SICHA_BACKEND_KOBOLD_CANCEL_ASSIST |
			SICHA_BACKEND_STREAM_USAGE;
	}
	d.timeouts.connect_ms = 10000;
	d.timeouts.first_byte_ms = SICHA_INFINITE; /* local prefill */
	d.timeouts.idle_ms = 120000;
	d.timeouts.total_ms = 600000;
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	if (sicha_client_create(&opts, &client) != SICHA_OK) {
		return NULL;
	}
	return client;
}

static int run_ui(const app_cfg *cfg)
{
	struct notcurses_options nopts;
	struct notcurses *nc;
	struct ncplane *std;
	struct ncplane *tp = NULL; /* scrolling transcript */
	sicha_client *client;
	pthread_t worker;
	worker_arg warg;
	sicha_cancel *cancel = NULL;
	int worker_active = 0;
	char input[4096];
	size_t input_len = 0;
	char status[256] = "type a message · enter sends · esc quits";
	int esc_while_streaming = 0;
	int auto_sent = 0;
	double auto_done_at = -1.0;
	double t_start = now_ms();
	uint32_t last_rows = 0;
	uint32_t last_cols = 0;
	int exit_code = 0;

	client = make_client(cfg);
	if (client == NULL) {
		fprintf(stderr, "invalid backend configuration\n");
		return 1;
	}
	memset(&nopts, 0, sizeof(nopts));
	nopts.flags = NCOPTION_SUPPRESS_BANNERS;
	nc = notcurses_core_init(&nopts, NULL);
	if (nc == NULL) {
		fprintf(stderr, "notcurses_init failed (need a tty)\n");
		sicha_client_destroy(client);
		return 1;
	}
	std = notcurses_stdplane(nc);

	for (;;) {
		struct timespec pace = { 0, 33 * 1000 * 1000 };
		struct pollfd pfd;
		int quit = 0;
		int want_send = 0;

		/* Input: gate on the input fd's readiness, then drain with
		 * short-timeout gets.  Never call notcurses_get with a
		 * NULL timespec — terminal chatter can signal readiness
		 * without a dequeueable event and block the UI. */
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
				if (ni.evtype == NCTYPE_RELEASE) {
					continue;
				}
				if (key == NCKEY_ESC) {
					if (worker_active) {
						sicha_cancel_trigger(cancel);
						if (esc_while_streaming) {
							quit = 1;
						}
						esc_while_streaming = 1;
					} else {
						quit = 1;
					}
					continue;
				}
				if (worker_active) {
					continue; /* only esc while busy */
				}
				if (key == NCKEY_ENTER || key == '\r' ||
					key == '\n') {
					want_send = input_len > 0;
					continue;
				}
				if (key == NCKEY_BACKSPACE ||
					key == 0x7F || key == 0x08) {
					while (input_len > 0 &&
						((uint8_t)input[
							input_len - 1] &
							0xC0) == 0x80) {
						input_len--;
					}
					if (input_len > 0) {
						input_len--;
					}
					continue;
				}
				/* Text entry, the Selkie way: ni.id is the
				 * BASE key (kitty protocol reports Shift+1
				 * as id '1' + shift modifier) — the
				 * character actually produced lives in
				 * eff_text, a zero-terminated utf32 array
				 * that respects modifiers (Shift+1 → '!',
				 * Shift+a → 'A').  Fall back to id for
				 * terminals that never fill eff_text. */
				if (!nckey_synthesized_p(key) &&
					!(ni.modifiers &
						(NCKEY_MOD_CTRL |
							NCKEY_MOD_ALT))) {
					uint32_t one[2] = { key, 0 };
					const uint32_t *cps =
						ni.eff_text[0] != 0 ?
						ni.eff_text : one;

					for (size_t k = 0; k <
						NCINPUT_MAX_EFF_TEXT_CODEPOINTS
						&& cps[k] != 0; k++) {
						uint32_t cp = cps[k];
						char enc[4];
						size_t n;

						if (cp < 0x20 ||
							cp == 0x7F ||
							cp >= 0x110000) {
							continue;
						}
						n = utf8_encode(cp, enc);
						if (input_len + n <
							sizeof(input)) {
							memcpy(input +
								input_len,
								enc, n);
							input_len += n;
						}
					}
				}
			}
		}
		if (quit || (cfg->demo_seconds > 0 &&
			now_ms() - t_start >
				cfg->demo_seconds * 1000.0)) {
			break;
		}

		/* auto mode: send once, exit shortly after completion */
		if (cfg->auto_msg != NULL && !auto_sent &&
			!worker_active) {
			size_t n = strlen(cfg->auto_msg);

			if (n >= sizeof(input)) {
				n = sizeof(input) - 1;
			}
			memcpy(input, cfg->auto_msg, n);
			input_len = n;
			want_send = 1;
			auto_sent = 1;
		}
		if (auto_done_at > 0 && now_ms() >= auto_done_at) {
			break;
		}

		/* (re)build layout on first frame / resize */
		{
			uint32_t rows, cols;

			ncplane_dim_yx(std, &rows, &cols);
			if (rows != last_rows || cols != last_cols) {
				if (tp == NULL) {
					struct ncplane_options po;

					memset(&po, 0, sizeof(po));
					po.y = 0;
					po.x = 0;
					po.rows = rows > 2 ? rows - 2 : 1;
					po.cols = cols;
					tp = ncplane_create(std, &po);
					ncplane_set_scrolling(tp, true);
				} else {
					ncplane_resize_simple(tp,
						rows > 2 ? rows - 2 : 1,
						cols);
				}
				last_rows = rows;
				last_cols = cols;
			}
		}

		if (want_send && tp != NULL) {
			/* record the turn, paint it, launch the worker */
			turns_push(SICHA_ROLE_USER, input, input_len);
			transcript_put(tp, COL_USER, "\nyou: ", 6);
			transcript_put(tp, COL_USER, input, input_len);
			transcript_put(tp, COL_BOT, "\nbot: ", 6);
			input_len = 0;
			esc_while_streaming = 0;

			memset(&warg, 0, sizeof(warg));
			warg.client = client;
			for (int i = 0; i < n_turns &&
				warg.n_msgs < MAX_MSGS; i++) {
				warg.msgs[warg.n_msgs].role =
					turns[i].role;
				warg.msgs[warg.n_msgs].content =
					turns[i].text;
				warg.msgs[warg.n_msgs].content_len =
					SICHA_LEN_CSTR;
				warg.n_msgs++;
			}
			cancel = sicha_cancel_create();
			warg.cancel = cancel;
			pthread_mutex_lock(&io_mu);
			io_pending_len = 0;
			io_done = 0;
			io_result = NULL;
			snprintf(io_status, sizeof(io_status),
				"streaming… (esc cancels)");
			pthread_mutex_unlock(&io_mu);
			if (pthread_create(&worker, NULL, worker_main,
				&warg) == 0) {
				worker_active = 1;
			} else {
				snprintf(status, sizeof(status),
					"failed to start worker thread");
				sicha_cancel_destroy(cancel);
				cancel = NULL;
			}
		}

		/* drain the handoff buffer into the transcript */
		{
			char local[sizeof(io_pending)];
			size_t local_len;
			int done;

			pthread_mutex_lock(&io_mu);
			local_len = io_pending_len;
			memcpy(local, io_pending, local_len);
			io_pending_len = 0;
			done = io_done;
			snprintf(status, sizeof(status), "%s", io_status);
			pthread_mutex_unlock(&io_mu);

			if (local_len > 0 && tp != NULL) {
				transcript_put(tp, COL_BOT, local,
					local_len);
			}
			if (done && worker_active) {
				sicha_result *r;
				sicha_status st;

				pthread_join(worker, NULL);
				worker_active = 0;
				pthread_mutex_lock(&io_mu);
				r = io_result;
				st = io_result_status;
				io_done = 0;
				pthread_mutex_unlock(&io_mu);

				if (st == SICHA_OK) {
					size_t tlen = 0;
					const char *text =
						sicha_result_text(r, &tlen);

					turns_push(SICHA_ROLE_ASSISTANT,
						text, tlen);
					snprintf(status, sizeof(status),
						"done · finish=%s · "
						"tokens=%lld · esc quits",
						sicha_result_finish_reason_raw(r),
						(long long)
						sicha_result_total_tokens(r));
				} else {
					char line[192];

					snprintf(line, sizeof(line),
						"\n[%s after %u attempt(s)]",
						sicha_status_str(st),
						sicha_result_attempt_count(r));
					if (tp != NULL) {
						transcript_put(tp, COL_ERR,
							line, strlen(line));
					}
					snprintf(status, sizeof(status),
						"%s · type to try again",
						sicha_status_str(st));
					if (st != SICHA_E_CANCELLED) {
						exit_code = 1;
					}
					/* drop the failed user turn so a
					 * retry rebuilds the same prompt */
					if (n_turns > 0 &&
						st != SICHA_E_CANCELLED) {
						free(turns[--n_turns].text);
					}
				}
				if (tp != NULL) {
					transcript_put(tp, COL_BOT, "\n", 1);
				}
				sicha_result_destroy(r);
				sicha_cancel_destroy(cancel);
				cancel = NULL;
				if (cfg->auto_msg != NULL) {
					auto_done_at = now_ms() + 1200.0;
				}
			}
		}

		/* status + input rows on the stdplane */
		{
			uint32_t rows, cols;

			ncplane_dim_yx(std, &rows, &cols);
			set_color(std, COL_DIM);
			ncplane_cursor_move_yx(std, (int)rows - 2, 0);
			for (uint32_t x = 0; x < cols; x++) {
				ncplane_putchar(std, ' ');
			}
			ncplane_cursor_move_yx(std, (int)rows - 2, 0);
			ncplane_putstr(std,
				"— sicha chat demo · ");
			ncplane_putnstr(std, cols > 24 ? cols - 24 : 0,
				status);
			set_color(std, COL_USER);
			ncplane_cursor_move_yx(std, (int)rows - 1, 0);
			for (uint32_t x = 0; x < cols; x++) {
				ncplane_putchar(std, ' ');
			}
			ncplane_cursor_move_yx(std, (int)rows - 1, 0);
			ncplane_putstr(std, "> ");
			ncplane_putnstr(std, input_len, input);
			set_color(std, COL_DIM);
			ncplane_putstr(std, worker_active ? "" : "_");
		}
		notcurses_render(nc);
		nanosleep(&pace, NULL);
	}

	/* unwind an in-flight worker before tearing anything down */
	if (worker_active) {
		sicha_cancel_trigger(cancel);
		pthread_join(worker, NULL);
		pthread_mutex_lock(&io_mu);
		sicha_result_destroy(io_result);
		io_result = NULL;
		pthread_mutex_unlock(&io_mu);
		sicha_cancel_destroy(cancel);
	}
	notcurses_stop(nc);
	for (int i = 0; i < n_turns; i++) {
		free(turns[i].text);
	}
	sicha_client_destroy(client);
	printf("sicha chat session: %d turn(s)%s\n", n_turns,
		exit_code != 0 ? " (last request failed)" : "");
	return exit_code;
}

int main(int argc, char **argv)
{
	app_cfg cfg;
	int argi = 1;

	/* a vendored ncurses may have a baked-in terminfo path from its
	 * build machine, and a terminal-specific $TERMINFO (kitty's,
	 * say) only covers its own entry; system fallback dirs rescue
	 * both cases (e.g. tmux inside kitty). */
	setenv("TERMINFO_DIRS",
		"/usr/share/terminfo:/etc/terminfo:/lib/terminfo:"
		"/usr/lib/terminfo", 0);

	memset(&cfg, 0, sizeof(cfg));
	cfg.base_url = getenv("SICHA_LIVE_BASE_URL");
	cfg.model = getenv("SICHA_LIVE_MODEL");
	cfg.api_key = getenv("SICHA_LIVE_API_KEY");
	{
		int n_pos = 0;

		while (argi < argc) {
			if (strcmp(argv[argi], "--kobold") == 0) {
				cfg.kobold = 1;
				argi++;
			} else if (strcmp(argv[argi], "--auto") == 0 &&
				argi + 1 < argc) {
				cfg.auto_msg = argv[argi + 1];
				argi += 2;
			} else if (strcmp(argv[argi],
					"--demo-seconds") == 0 &&
				argi + 1 < argc) {
				cfg.demo_seconds = atoi(argv[argi + 1]);
				argi += 2;
			} else {
				/* positionals override env, in order:
				 * base_url, model, api_key */
				if (n_pos == 0) {
					cfg.base_url = argv[argi];
				} else if (n_pos == 1) {
					cfg.model = argv[argi];
				} else {
					cfg.api_key = argv[argi];
				}
				n_pos++;
				argi++;
			}
		}
	}
	if (cfg.api_key != NULL && cfg.api_key[0] == '\0') {
		cfg.api_key = NULL;
	}
	if (cfg.base_url == NULL || cfg.model == NULL) {
		fprintf(stderr,
			"usage: sicha_chat <base_url> <model> [api_key]\n"
			"       [--kobold] [--auto \"message\"]"
			" [--demo-seconds N]\n"
			"or set SICHA_LIVE_BASE_URL / SICHA_LIVE_MODEL.\n");
		return 2;
	}
	return run_ui(&cfg);
}
