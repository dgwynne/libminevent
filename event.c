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

#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>

#include "minevent.h"
#include "minevent-internal.h"
#include "heap.h"

#define SET(_v, _m)	((_v) |= (_m))
#define CLR(_v, _m)	((_v) &= ~(_m))
#define ISSET(_v, _m)	((_v) & (_m))

HEAP_HEAD(event_heap);
TAILQ_HEAD(event_list, event);

HEAP_PROTOTYPE(event_heap, event);

struct event_base {
	struct event_heap	 evb_heap; /* holds the timeouts */
	struct event_list	 evb_signals[NSIG];
	struct event_list	 evb_list; /* holds fds */
	unsigned int		 evb_list_len; /* number of fds */
	unsigned int		 evb_nevents;
	int			 evb_running;
	struct event_list	 evb_fire;

	const struct event_ops	*evb_ops;
	void			*evb_backend;
};

#define event_op_init(_evb)						\
	(*(_evb)->evb_ops->evo_init()
#define event_op_destroy(_evb, _backend)				\
	(*(_evb)->evb_ops->evo_init)(_backend)
#define event_op_dispatch(_evb, _ts)					\
	(*(_evb)->evb_ops->evo_dispatch)((_evb), (_ts))
#define event_op_event_add(_evb, _ev)					\
	(*(_evb)->evb_ops->evo_event_add)((_evb), (_ev))
#define event_op_event_del(_evb, _ev)					\
	(*(_evb)->evb_ops->evo_event_del)((_evb), (_ev))
#define event_op_signal_add(_evb, _s)					\
	(*(_evb)->evb_ops->evo_signal_add)((_evb), (_s))
#define event_op_signal_del(_evb, _s)					\
	(*(_evb)->evb_ops->evo_signal_del)((_evb), (_s))

static int	event_deadline(struct timespec *, const struct timeval *);

static inline int
event_heap_empty(struct event_base *evb)
{
	return (HEAP_EMPTY(event_heap, &evb->evb_heap));
}

static inline void
event_heap_insert(struct event_base *evb, struct event *ev,
    const struct timespec *deadline)
{
	ev->ev_deadline = *deadline;
	HEAP_INSERT(event_heap, &evb->evb_heap, ev);
}

static inline void
event_heap_remove(struct event_base *evb, struct event *ev)
{
	HEAP_REMOVE(event_heap, &evb->evb_heap, ev);
}

static inline struct event *
event_heap_first(struct event_base *evb)
{
	return (HEAP_FIRST(event_heap, &evb->evb_heap));
}

static inline struct event *
event_heap_cextract(struct event_base *evb, const struct event *now)
{
	return (HEAP_CEXTRACT(event_heap, &evb->evb_heap, now));
}

static inline struct event *
event_fire_first(struct event_base *evb)
{
	return (TAILQ_FIRST(&evb->evb_fire));
}

static inline void
event_fire_insert(struct event_base *evb, struct event *ev)
{
	TAILQ_INSERT_TAIL(&evb->evb_fire, ev, ev_fire);
}

static inline void
event_fire_remove(struct event_base *evb, struct event *ev)
{
	TAILQ_REMOVE(&evb->evb_fire, ev, ev_fire);
}

static struct event_base *_event_base = NULL;

#include <unistd.h>

struct event_base *
event_init(void)
{
	const struct event_ops *ops = EVENT_OPS_DEFAULT;
	struct event_base *evb;
	void *backend;
	int i;

	evb = malloc(sizeof(*evb));
	if (evb == NULL)
		return (NULL);

	backend = ops->evo_init();
	if (backend == NULL) {
		free(evb);
		return (NULL);
	}

	HEAP_INIT(event_heap, &evb->evb_heap);

	for (i = 0; i < NSIG; i++)
		TAILQ_INIT(&evb->evb_signals[i]);

	TAILQ_INIT(&evb->evb_list);
	evb->evb_list_len = 0;
	evb->evb_nevents = 0;
	TAILQ_INIT(&evb->evb_fire);

	evb->evb_running = 0;
	evb->evb_ops = ops;
	evb->evb_backend = backend;

	_event_base = evb;

	return (evb);
}

int
event_dispatch(void)
{
	struct event_base *evb = _event_base;
	struct event *ev;
	struct event now;
	struct timespec *ts;
	short event;
	int runs = 0;

	evb->evb_running = 1;
	for (;;) {
		if (++runs == 30)
			abort();

		if (event_monotime(&now.ev_deadline) == -1)
			return (-1);

		while ((ev = event_heap_cextract(evb, &now)) != NULL) {
			struct event_list *evl;

			switch (ISSET(ev->ev_event, EV_TYPE_MASK)) {
			case EV_IO:
				if (event_op_event_del(evb, ev) != 0)
					return (-1);

				event_list_remove(evb, ev);
				break;
			case EV_SIGNAL:
				evl = &evb->evb_signals[ev->ev_ident];
				TAILQ_REMOVE(evl, ev, ev_list);
				if (TAILQ_EMPTY(evl) &&
				    event_op_signal_del(evb, ev->ev_ident) != 0)
					return (-1);
				break;
			case EV_TIMEOUT:
				break;
			default:
				abort();
			}
			CLR(ev->ev_event, EV_ON_LIST|EV_ON_HEAP);
			evb->evb_nevents--;

			SET(ev->ev_fires, EV_TIMEOUT);
			if (!ISSET(ev->ev_event, EV_ON_FIRE)) {
				event_fire_insert(evb, ev);
				SET(ev->ev_event, EV_ON_FIRE);
			}
		}

		while ((ev = event_fire_first(evb)) != NULL) {
			event_fire_remove(evb, ev);
			CLR(ev->ev_event, EV_ON_FIRE);
			event = ev->ev_fires;
			ev->ev_fires = 0;

			(*ev->ev_fn)(ev->ev_ident, event, ev->ev_arg);
			if (!evb->evb_running)
				return (0);
		}

		if (evb->evb_nevents == 0)
			break;

		ev = event_heap_first(evb);
		if (ev != NULL) {
			ts = &now.ev_deadline;
			timespecsub(&ev->ev_deadline, ts, ts);
		} else
			ts = NULL;

		if (event_op_dispatch(evb, ts) == -1)
			return (-1);
	}

	return (0);
}

void
event_set(struct event *ev, int fd, short events,
    void (*fn)(int, short, void *), void *arg)
{
	ev->ev_base = _event_base;
	ev->ev_ident = fd;
	ev->ev_fn = fn;
	ev->ev_arg = arg;
	ev->ev_event = EV_INITIALIZED | EV_IO |
	    (events & (EV_READ|EV_WRITE|EV_PERSIST));
	ev->ev_fires = 0;
}

int
event_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *evb = _event_base;
	struct timespec deadline;
	int flags = EV_ON_LIST;
	int rv;

	if (tv != NULL) {
		if (event_deadline(&deadline, tv) == -1)
			return (-1);

		flags |= EV_ON_HEAP;
	} else if (ISSET(ev->ev_event, EV_ON_LIST|EV_ON_HEAP) == EV_ON_LIST)
		return (0);

	if (!ISSET(ev->ev_event, EV_ON_LIST)) {
		rv = event_op_event_add(evb, ev);
		if (rv != 0)
			return (rv);
		event_list_insert(evb, ev);
		evb->evb_nevents++;
	} else if (ISSET(ev->ev_event, EV_ON_HEAP))
		event_heap_remove(evb, ev);

	SET(ev->ev_event, flags);
	if (tv != NULL)
		event_heap_insert(evb, ev, &deadline);

	return (rv);
}

int
event_del(struct event *ev)
{
	struct event_base *evb = _event_base;
	int rv;

	if (ISSET(ev->ev_event, EV_ON_LIST)) {
		rv = event_op_event_del(evb, ev);
		if (rv != 0)
			return (rv);
		event_list_remove(evb, ev);
		evb->evb_nevents--;
	}

	if (ISSET(ev->ev_event, EV_ON_HEAP))
		event_heap_remove(evb, ev);

	if (ISSET(ev->ev_event, EV_ON_FIRE))
		event_fire_remove(evb, ev);

	CLR(ev->ev_event, EV_ON_LIST|EV_ON_HEAP|EV_ON_FIRE);

	return (0);
}

int
event_initialized(struct event *ev)
{
	return (ISSET(ev->ev_event, EV_INITIALIZED));
}

int
event_pending(struct event *ev, short events, struct timeval *tv)
{
	int flags = 0;

	flags = ISSET(ev->ev_event, EV_ON_LIST) ?
	    (ev->ev_event & EV_PENDING_MASK) : 0;

	flags &= events;

	if (ISSET(events, EV_TIMEOUT) && ISSET(flags, EV_ON_HEAP)) {
		if (tv != NULL) {
			struct timespec now, ts;

			(void)event_monotime(&now);
			timespecsub(&ev->ev_deadline, &now, &ts);
			(void)event_walltime(&now);
			timespecadd(&now, &ts, &ts);

			TIMESPEC_TO_TIMEVAL(tv, &ts);
		}

		flags |= EV_TIMEOUT;
	}

	return (flags);
}

void
evtimer_set(struct event *ev,
    void (*fn)(int, short, void *), void *arg)
{
	ev->ev_base = _event_base;
	ev->ev_ident = -1;
	ev->ev_fn = fn;
	ev->ev_arg = arg;
	ev->ev_event = EV_INITIALIZED | EV_TIMEOUT;
	ev->ev_fires = 0;
}

int
evtimer_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *evb = _event_base;
	struct timespec deadline;

	if (event_deadline(&deadline, tv) == -1)
		return (-1);

	if (!ISSET(ev->ev_event, EV_ON_HEAP)) {
		evb->evb_nevents++;
		SET(ev->ev_event, EV_ON_HEAP);
	} else
		event_heap_remove(evb, ev);

	event_heap_insert(evb, ev, &deadline);

	return (0);
}

int
evtimer_del(struct event *ev)
{
	struct event_base *evb = _event_base;

	if (!ISSET(ev->ev_event, EV_ON_HEAP|EV_ON_FIRE))
		return (0);

	evb->evb_nevents--;
	if (ISSET(ev->ev_event, EV_ON_HEAP))
		event_heap_remove(evb, ev);
	if (ISSET(ev->ev_event, EV_ON_FIRE))
		event_fire_remove(evb, ev);
	CLR(ev->ev_event, EV_ON_HEAP | EV_ON_FIRE);

	return (0);
}

int
evtimer_pending(struct event *ev, struct timeval *tv)
{
	int flags = 0;

	if (ISSET(ev->ev_event, EV_ON_HEAP)) {
		if (tv != NULL) {
			struct timespec now, ts;

			(void)event_monotime(&now);
			timespecsub(&ev->ev_deadline, &now, &ts);
			(void)event_walltime(&now);
			timespecadd(&now, &ts, &ts);

			TIMESPEC_TO_TIMEVAL(tv, &ts);
		}

		flags = EV_TIMEOUT | (ev->ev_event & EV_PERSIST);
	}

	return (flags);
}

int
evtimer_initialized(struct event *ev)
{
	return (ISSET(ev->ev_event, EV_INITIALIZED));
}

void
signal_set(struct event *ev, int signal,
    void (*fn)(int, short, void *), void *arg)
{
	assert(signal < NSIG);

	ev->ev_base = _event_base;
	ev->ev_ident = signal;
	ev->ev_fn = fn;
	ev->ev_arg = arg;
	ev->ev_event = EV_INITIALIZED | EV_SIGNAL | EV_PERSIST;
	ev->ev_fires = 0;
}

int
signal_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *evb = _event_base;
	struct timespec deadline;
	int flags = EV_ON_LIST;
	int rv;

	if (tv != NULL) {
		if (event_deadline(&deadline, tv) == -1)
			return (-1);

		flags |= EV_ON_HEAP;
	} else if (ISSET(ev->ev_event, EV_ON_LIST|EV_ON_HEAP) == EV_ON_LIST)
		return (0);

	if (!ISSET(ev->ev_event, EV_ON_LIST)) {
		struct event_list *evl = &evb->evb_signals[ev->ev_ident];

		if (TAILQ_EMPTY(evl)) {
			rv = event_op_signal_add(evb, ev->ev_ident);
			if (rv != 0)
				return (rv);
		}

		TAILQ_INSERT_TAIL(evl, ev, ev_list);
		evb->evb_nevents++;
	} else if (ISSET(ev->ev_event, EV_ON_HEAP))
		event_heap_remove(evb, ev);

	SET(ev->ev_event, flags);
	if (tv != NULL)
		event_heap_insert(evb, ev, &deadline);

	return (0);
}

int
signal_del(struct event *ev)
{
	struct event_base *evb = _event_base;

	if (!ISSET(ev->ev_event, EV_ON_LIST|EV_ON_FIRE))
		return (0);

	if (ISSET(ev->ev_event, EV_ON_LIST)) {
		struct event_list *evl = &evb->evb_signals[ev->ev_ident];

		if (TAILQ_FIRST(evl) == ev &&
		    TAILQ_NEXT(ev, ev_list) == NULL) {
			/* this is the last event on the list */
			int rv = event_op_signal_del(evb, ev->ev_ident);
			if (rv != 0)
				return (rv);
		}

		TAILQ_REMOVE(evl, ev, ev_list);
		evb->evb_nevents--;
	}

	if (ISSET(ev->ev_event, EV_ON_HEAP))
		event_heap_remove(evb, ev);

	if (ISSET(ev->ev_event, EV_ON_FIRE))
		event_fire_remove(evb, ev);

	CLR(ev->ev_event, EV_ON_LIST|EV_ON_HEAP|EV_ON_FIRE);

	return (0);
}

int
signal_pending(struct event *ev, struct timeval *tv)
{
	return (event_pending(ev, EV_SIGNAL|EV_TIMEOUT|EV_PERSIST, tv));
}

int
signal_initialized(struct event *ev)
{
	return (ISSET(ev->ev_event, EV_INITIALIZED));
}

void
event_fire_event(struct event_base *evb, struct event *ev, short event)
{
	SET(ev->ev_fires, ISSET(event, EV_READ|EV_WRITE|EV_TIMEOUT));

	if (ISSET(ev->ev_event, EV_ON_FIRE))
		return;

	if (!ISSET(ev->ev_event, EV_PERSIST) && ISSET(event, EV_PERSIST)) {
		if (event_op_event_del(evb, ev) != 0) {
			/*
			 * we werent able to remove the event from the
			 * backend, so it could fire again in the future.
			 */
			return;
		}

		if (ISSET(ev->ev_event, EV_ON_HEAP))
			event_heap_remove(evb, ev);

		event_list_remove(evb, ev);
		evb->evb_nevents--;

		CLR(ev->ev_event, EV_ON_LIST|EV_ON_HEAP);
	}

	SET(ev->ev_event, EV_ON_FIRE);
	event_fire_insert(evb, ev);
}

void
event_fire_signal(struct event_base *evb, int sig)
{
	struct event_list *evl;
	struct event *ev, *nev;

	assert(sig < NSIG);

	evl = &evb->evb_signals[sig];
	TAILQ_FOREACH_SAFE(ev, evl, ev_list, nev) {
		SET(ev->ev_fires, EV_SIGNAL);

		if (!ISSET(ev->ev_event, EV_ON_FIRE)) {
			event_fire_insert(evb, ev);
			SET(ev->ev_event, EV_ON_FIRE);
		}
	}
}

static int
event_deadline(struct timespec *deadline, const struct timeval *tv)
{
	struct timespec ts;
	struct timespec now;

	if (event_monotime(&now) == -1)
		return (-1);
	TIMEVAL_TO_TIMESPEC(tv, &ts);
	timespecadd(&ts, &now, deadline);

	return (0);
}

static inline int
event_heap_compare(const struct event *a, const struct event *b)
{
	if (a->ev_deadline.tv_sec > b->ev_deadline.tv_sec)
		return (1);
	if (a->ev_deadline.tv_sec < b->ev_deadline.tv_sec)
		return (-1);
	if (a->ev_deadline.tv_nsec > b->ev_deadline.tv_nsec)
		return (1);
	if (a->ev_deadline.tv_nsec < b->ev_deadline.tv_nsec)
		return (-1);

	return (0);
}

HEAP_GENERATE(event_heap, event, ev_heap, event_heap_compare);

void *
event_base_backend(struct event_base *evb)
{
	return (evb->evb_backend);
}

void
event_list_insert(struct event_base *evb, struct event *ev)
{
	evb->evb_list_len++;
	TAILQ_INSERT_TAIL(&evb->evb_list, ev, ev_list);
}

void
event_list_remove(struct event_base *evb, struct event *ev)
{
	TAILQ_REMOVE(&evb->evb_list, ev, ev_list);
	evb->evb_list_len--;
}

struct event *
event_list_first(struct event_base *evb)
{
	return (TAILQ_FIRST(&evb->evb_list));
}

struct event *
event_list_next(struct event *ev)
{
	return (TAILQ_NEXT(ev, ev_list));
}

unsigned int
event_list_len(struct event_base *evb)
{
	return (evb->evb_list_len);
}
