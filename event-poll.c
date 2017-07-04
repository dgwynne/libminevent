/*	$OpenBSD$ */

/*
 * Copyright (c) 2017 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stddef.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

#include "minevent.h"
#include "minevent-internal.h"
#include "heap.h"

static void	*event_poll_init(void);
static void	 event_poll_destroy(void *);
static int	 event_poll_dispatch(struct event_base *,
		     const struct timespec *);
static int	 event_poll_event_add(struct event_base *, struct event *);
static int	 event_poll_event_del(struct event_base *, struct event *);
static int	 event_poll_signal_add(struct event_base *, int);
static int	 event_poll_signal_del(struct event_base *, int);

const struct event_ops event_poll_ops = {
	event_poll_init,
	event_poll_destroy,
	event_poll_dispatch,
	event_poll_event_add,
	event_poll_event_del,
	event_poll_signal_add,
	event_poll_signal_del,
};

struct event_poll_signals {
	void		(*evs_handlers[NSIG])(int);

	volatile sig_atomic_t
			  evs_signals[NSIG];
	volatile sig_atomic_t
			  evs_rescan;
	struct event	  evs_ev;

	int		  evs_pipe[2];

	unsigned int	  evs_refcnt;
};

static struct event_poll_signals *_evs;

struct event_pfd {
	HEAP_ENTRY()	 evpfd_heap;
	struct event	*evpfd_ev;
	unsigned int	 evpfd_idx;
	unsigned int	 evpfd_gen;
};
HEAP_HEAD(event_pfd_live);
HEAP_HEAD(event_pfd_free);

struct event_poll {
	struct pollfd	 *evp_pfds;
	size_t		  evp_pfdlen;
	int		  evp_nfds;
	struct event_pfd **
			  evp_evpfds;
	struct event_pfd_live
			  evp_live;
	struct event_pfd_free
			  evp_free;
	unsigned int	  evp_gen;

	struct event_poll_signals *
			  evp_signals;
};

static struct event_poll_signals *
		 event_poll_signals_create(struct event_base *);
static void	 event_poll_signals_destroy(struct event_poll_signals *);
static struct event_poll_signals *
		 event_poll_signals_take(struct event_base *);
static void	 event_poll_signals_rele(struct event_base *,
		     struct event_poll_signals *);
static void	 event_poll_signal(int);
static void	 event_poll_pipe(int, short, void *);
static int	 event_poll_signal_scan(struct event_base *,
		     struct event_poll_signals *);

HEAP_PROTOTYPE(event_pfd_live, event_pfd);
HEAP_PROTOTYPE(event_pfd_free, event_pfd);

static inline int
event_pfd_free_cmp(const struct event_pfd *a, const struct event_pfd *b)
{
	if (a->evpfd_idx > b->evpfd_idx)
		return (1);
	if (a->evpfd_idx < b->evpfd_idx)
		return (-1);

	return (0);
}

static inline int
event_pfd_live_cmp(const struct event_pfd *a, const struct event_pfd *b)
{
	if (a->evpfd_idx < b->evpfd_idx)
		return (1);
	if (a->evpfd_idx > b->evpfd_idx)
		return (-1);

	return (0);
}

HEAP_GENERATE(event_pfd_free, event_pfd, evpfd_heap, event_pfd_free_cmp);
HEAP_GENERATE(event_pfd_live, event_pfd, evpfd_heap, event_pfd_live_cmp);

#define evpfd_live_init(_evp)						\
	HEAP_INIT(event_pfd_live, &(_evp)->evp_live)			
#define evpfd_live_first(_evp) 						\
	HEAP_FIRST(event_pfd_live, &(_evp)->evp_live)
#define evpfd_live_remove(_evp, _e)					\
	HEAP_REMOVE(event_pfd_live, &(_evp)->evp_live, (_e));		
#define evpfd_live_insert(_evp, _e)					\
	HEAP_INSERT(event_pfd_live, &(_evp)->evp_live, (_e));		

#define evpfd_free_init(_evp)						\
	HEAP_INIT(event_pfd_free, &(_evp)->evp_free)
#define evpfd_free_first(_evp) 						\
	HEAP_FIRST(event_pfd_free, &(_evp)->evp_free)
#define evpfd_free_cextract(_evp, _k)					\
	HEAP_CEXTRACT(event_pfd_free, &(_evp)->evp_free, (_k))
#define evpfd_free_extract(_evp)					\
	HEAP_EXTRACT(event_pfd_free, &(_evp)->evp_free)
#define evpfd_free_insert(_evp, _e)					\
	HEAP_INSERT(event_pfd_free, &(_evp)->evp_free, (_e))

static void *
event_poll_init(void)
{
	struct event_poll *evp;

	evp = malloc(sizeof(*evp));
	if (evp == NULL)
		return (NULL);

	evp->evp_pfds = NULL;
	evp->evp_pfdlen = 0;
	evp->evp_nfds = 0;

	evp->evp_evpfds = NULL;
	evpfd_live_init(evp);
	evpfd_free_init(evp);
	evp->evp_gen = 0;

	evp->evp_signals = NULL;

	return (evp);
}

static void
event_poll_destroy(void *backend)
{
	struct event_poll *evp = backend;
	struct event_pfd *evpfd;
	unsigned int i;

	event_poll_signals_destroy(evp->evp_signals);
	for (i = 0; i < evp->evp_pfdlen; i++) {
		evpfd = evp->evp_evpfds[i];
		free(evpfd);
	}

	free(evp->evp_pfds);
	free(evp->evp_evpfds);
	free(evp);
}

static void
event_poll_pack(struct event_poll *evp)
{
	struct event_pfd *fevpfd, *levpfd;
	struct pollfd *fpfd, *lpfd;
	unsigned int gen = evp->evp_gen;

	while ((levpfd = evpfd_live_first(evp)) != NULL &&
	    (fevpfd = evpfd_free_cextract(evp, levpfd)) != NULL) {
		evpfd_live_remove(evp, levpfd);

		fpfd = &evp->evp_pfds[fevpfd->evpfd_idx];
		lpfd = &evp->evp_pfds[levpfd->evpfd_idx];

		fpfd->fd = lpfd->fd;
		fpfd->events = lpfd->events;

		fevpfd->evpfd_gen = gen;
		fevpfd->evpfd_ev = levpfd->evpfd_ev;
		fevpfd->evpfd_ev->ev_cookie = fevpfd;

		levpfd->evpfd_gen = gen;
		levpfd->evpfd_ev = NULL;

		evpfd_live_insert(evp, fevpfd);
		evpfd_free_insert(evp, levpfd);
	}
}

static int
event_poll_dispatch(struct event_base *evb, const struct timespec *ts)
{
	struct event_poll *evp = event_base_backend(evb);
	struct event_poll_signals *evs = evp->evp_signals;
	struct event_pfd *evpfd;
	nfds_t nfds;
	unsigned int gen;
	int len;
	unsigned int i;

	if (evs != NULL && evs->evs_rescan && event_poll_signal_scan(evb, evs))
		return (0);

	event_poll_pack(evp);

	nfds = event_list_len(evb);
	len = ppoll(evp->evp_pfds, nfds, ts, NULL);
	switch (len) {
	case -1:
		return (-1);
	case 0:
		return (0);
	}

	gen = ++evp->evp_gen;
	if (gen == 0) {
		/*
		 * the generation counter is used to detect when an evpfd has
		 * changed while processing events on the pfds. when it wraps,
		 * reset all the current evpfds to avoid false positives.
		 */
		for (i = 0; i < evp->evp_pfdlen; i++) {
			evpfd = evp->evp_evpfds[i];
			evpfd->evpfd_gen = ~0;
		}
	}

	for (i = 0; i < nfds; i++) {
		struct pollfd *pfd;
		struct event *ev;
		short event = 0;

		evpfd = evp->evp_evpfds[i];

		if (evpfd->evpfd_gen == gen)
			continue;

		pfd = &evp->evp_pfds[i];

		if (ISSET(pfd->revents, POLLHUP|POLLERR))
			SET(event, EV_READ|EV_WRITE);
		else {
			if (ISSET(pfd->revents, POLLIN))
				SET(event, EV_READ);
			if (ISSET(pfd->revents, POLLOUT))
				SET(event, EV_WRITE);
		}

		ev = evpfd->evpfd_ev;
		if (ISSET(ev->ev_event, event))
			event_fire_event(evb, ev, event | EV_PERSIST);

		if (pfd->revents != 0 && --len == 0) {
			/*
			 * event_dels may remove fds that have fired so this
			 * can be off. the worst that happens is we look a
			 * bit further into the list than absolutely
			 * necessary.
			 */
			break;
		}
	}

	return (0);
}

static int
event_poll_event_add(struct event_base *evb, struct event *ev)
{
	struct event_poll *evp = event_base_backend(evb);
	struct event_pfd *evpfd;
	struct pollfd *pfd;
	unsigned int nfds = evp->evp_nfds;
	unsigned int i;

	i = nfds++;
	if (nfds > evp->evp_pfdlen) {
		struct event_pfd **evpfds;
		struct pollfd *pfds;

		evpfd = malloc(sizeof(*evpfd));
		if (evpfd == NULL)
			return (-1);

		evpfds = reallocarray(evp->evp_evpfds, nfds, sizeof(*evpfds));
		if (evpfds == NULL) {
			free(evpfd);
			return (-1);
		}

		evp->evp_evpfds = evpfds;

		pfds = reallocarray(evp->evp_pfds, nfds, sizeof(*pfds));
		if (pfds == NULL) {
			free(evpfd);
			return (-1);
		}

		/* commit */
		evpfds[i] = evpfd;
		evpfd->evpfd_idx = i;

		evp->evp_pfds = pfds;
		evp->evp_pfdlen = nfds;
	} else
		evpfd = evpfd_free_extract(evp);

	ev->ev_cookie = evpfd;

	evpfd->evpfd_gen = evp->evp_gen;
	evpfd->evpfd_ev = ev;

	pfd = &evp->evp_pfds[evpfd->evpfd_idx];
	pfd->fd = EVENT_FD(ev);
	pfd->events = (ISSET(ev->ev_event, EV_READ) ? POLLIN : 0) |
	    (ISSET(ev->ev_event, EV_WRITE) ? POLLOUT : 0);

	evpfd_live_insert(evp, evpfd);
	evp->evp_nfds = nfds;

	return (0);
}

static int
event_poll_event_del(struct event_base *evb, struct event *ev)
{
	struct event_poll *evp = event_base_backend(evb);
	struct event_pfd *evpfd = ev->ev_cookie;

	evpfd_live_remove(evp, evpfd);
	evpfd->evpfd_gen = evp->evp_gen;
	evpfd->evpfd_ev = NULL;
	evpfd_free_insert(evp, evpfd);
	evp->evp_nfds--;

	ev->ev_cookie = NULL;

	return (0);
}

static int
event_poll_signal_add(struct event_base *evb, int s)
{
	struct event_poll_signals *evs;
	void (*handler)(int);

	evs = event_poll_signals_take(evb);
	if (evs == NULL)
		return (-1);

	handler = signal(s, event_poll_signal);
	if (handler == SIG_ERR) {
		event_poll_signals_rele(evb, evs);
		return (-1);
	}

	evs->evs_handlers[s] = handler;

	return (0);
}

static int
event_poll_signal_del(struct event_base *evb, int s)
{
	struct event_poll *evp = event_base_backend(evb);
	struct event_poll_signals *evs = evp->evp_signals;

	if (signal(s, evs->evs_handlers[s]) == SIG_ERR)
		return (-1);

	event_poll_signals_rele(evb, evs);

	return (0);
}

static struct event_poll_signals *
event_poll_signals_create(struct event_base *evb)
{
	struct event_poll_signals *evs;
	struct event *ev;

	evs = malloc(sizeof(*evs));
	if (evs == NULL)
		return (NULL);

	if (pipe2(evs->evs_pipe, O_NONBLOCK) == -1)
		goto free;

	evs->evs_rescan = 0;
	evs->evs_refcnt = 1;

	ev = &evs->evs_ev;
	event_set(ev, evs->evs_pipe[0], EV_READ|EV_PERSIST,
	    event_poll_pipe, evb);
	if (event_add(ev, NULL) != 0)
		goto close;

	_evs = evs;

	return (evs);
close:
	close(evs->evs_pipe[0]);
	close(evs->evs_pipe[1]);
free:
	free(evs);
	return (NULL);
}

static void
event_poll_pipe(int fd, short events, void *arg)
{
	struct event_base *evb = arg;
	char sigs[1024];
	ssize_t len, i;

	len = read(fd, sigs, sizeof(sigs));
	if (len == -1) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			/* try again later */
			return;
		default:
			abort();
		}
	}

	for (i = 0; i < len; i++)
		event_fire_signal(evb, sigs[i]);
}

static void
event_poll_signals_destroy(struct event_poll_signals *evs)
{
	void (*handler)(int);
	int i;

	if (evs == NULL)
		return;

	_evs = NULL; /* ugh */

	if (event_del(&evs->evs_ev) != 0) {
		/* event_poll_event_del cannot fail */
		abort();
	}

	for (i = 0; i < NSIG; i++) {
		handler = evs->evs_handlers[i];
		if (handler != SIG_ERR)
			signal(i, handler); /* XXX */
	}

	close(evs->evs_pipe[0]);
	close(evs->evs_pipe[1]);

	free(evs);
}

static struct event_poll_signals *
event_poll_signals_take(struct event_base *evb)
{
	struct event_poll *evp = event_base_backend(evb);
	struct event_poll_signals *evs;

	evs = evp->evp_signals;
	if (evs == NULL) {
		evs = event_poll_signals_create(evb);
		if (evs == NULL)
			return (NULL);

		evp->evp_signals = evs; /* cache, not a ref */

		return (evs); /* give the ref to the caller */
	}

	evs->evs_refcnt++;

	return (evs);
}

static void
event_poll_signals_rele(struct event_base *evb, struct event_poll_signals *evs)
{
	struct event_poll *evp = event_base_backend(evb);

	assert(evp->evp_signals == evs);

	if (--evs->evs_refcnt == 0) {
		evp->evp_signals = NULL;
		event_poll_signals_destroy(evs);
	}
}

static void
event_poll_signal(int s)
{
	struct event_poll_signals *evs = _evs;
	unsigned char c[1] = { s };

	if (evs == NULL)
		return;

	if (write(evs->evs_pipe[1], c, sizeof(c)) != sizeof(c)) {
		/* if we fail to write to the pipe, fall back to a flag */
		evs->evs_signals[s] = 1;
		evs->evs_rescan = 1;
	}
}

static int
event_poll_signal_scan(struct event_base *evb, struct event_poll_signals *evs)
{
	int rv = 0;
	int s;

	evs->evs_rescan = 0;

	for (s = 0; s < NSIG; s++) {
		if (evs->evs_signals[s]) {
			evs->evs_signals[s] = 0;
			event_fire_signal(evb, s);
			rv = 1;
		}
	}

	return (rv);
}
