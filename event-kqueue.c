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

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>

#include "minevent.h"
#include "minevent-internal.h"

static void	*event_kq_init(void);
static void	 event_kq_destroy(void *);
static int	 event_kq_dispatch(struct event_base *,
		     const struct timespec *);
static int	 event_kq_event_add(struct event_base *, struct event *);
static int	 event_kq_event_del(struct event_base *, struct event *);
static int	 event_kq_signal_add(struct event_base *, int);
static int	 event_kq_signal_del(struct event_base *, int);

const struct event_ops event_kqueue = {
	event_kq_init,
	event_kq_destroy,
	event_kq_dispatch,
	event_kq_event_add,
	event_kq_event_del,
	event_kq_signal_add,
	event_kq_signal_del,
};

struct event_kq {
	int		 evkq_fd;

	struct kevent	*evkq_events;
	int		 evkq_eventslen;
	int		 evkq_nevents;
};

static void *
event_kq_init(void)
{
	struct event_kq *evkq;
	int fd;

	evkq = malloc(sizeof(*evkq));
	if (evkq == NULL)
		return (NULL);

	fd = kqueue();
	if (fd == -1) {
		free(evkq);
		return (NULL);
	}

	evkq->evkq_fd = fd;
	evkq->evkq_events = NULL;
	evkq->evkq_nevents = 0;

	return (evkq);
}

static void
event_kq_destroy(void *backend)
{
	struct event_kq *evkq = backend;

	free(evkq->evkq_events);
	close(evkq->evkq_fd);
	free(evkq);
}

static void
event_kq_fire_event(struct event_base *evb, short flags, struct event *ev)
{
	if (ISSET(ev->ev_event, EV_READ|EV_WRITE) != (EV_READ|EV_WRITE))
		SET(flags, ISSET(ev->ev_event, EV_PERSIST));
	else
		SET(flags, EV_PERSIST);

	event_fire_event(evb, ev, flags);
}

static int
event_kq_dispatch(struct event_base *evb, const struct timespec *ts)
{
	struct event_kq *evkq = event_base_backend(evb);
	struct kevent *kevs, *kev;
	int nevents;
	int i;

	nevents = evkq->evkq_nevents;
	if (nevents > evkq->evkq_eventslen) {
		kevs = reallocarray(evkq->evkq_events, nevents, sizeof(*kevs));
		if (kevs == NULL)
			return (-1);

		evkq->evkq_events = kevs;
		evkq->evkq_eventslen = nevents;
	} else
		kevs = evkq->evkq_events;

	nevents = kevent(evkq->evkq_fd, NULL, 0, kevs, nevents, ts);
	if (nevents == -1)
		return (-1);

	for (i = 0; i < nevents; i++) {
		kev = &kevs[i];

		switch (kev->filter) {
		case EVFILT_READ:
			event_kq_fire_event(evb, EV_READ, kev->udata);
			break;
		case EVFILT_WRITE:
			event_kq_fire_event(evb, EV_WRITE, kev->udata);
			break;
		case EV_SIGNAL:
			event_fire_signal(evb, kev->ident);
			break;
		}
	}

	return (0);
}

static int
event_kq_event_add(struct event_base *evb, struct event *ev)
{
	struct event_kq *evkq = event_base_backend(evb);
	struct kevent *kev, kevs[2];
	int flags = EV_ADD;
	int nchanges = 0;

	if (ISSET(ev->ev_event, EV_READ|EV_WRITE) != (EV_READ|EV_WRITE) &&
	    !ISSET(ev->ev_event, EV_PERSIST))
		SET(flags, EV_ONESHOT);

	if (ISSET(ev->ev_event, EV_READ)) {
		kev = &kevs[nchanges++];

		EV_SET(kev, EVENT_FD(ev), EVFILT_READ, flags, NOTE_EOF, 0, ev);
	}

	if (ISSET(ev->ev_event, EV_WRITE)) {
		kev = &kevs[nchanges++];

		EV_SET(kev, EVENT_FD(ev), EVFILT_WRITE, flags, 0, 0, ev);
	}

	/* commit */
	if (kevent(evkq->evkq_fd, kevs, nchanges, NULL, 0, NULL) == -1)
		return (-1);

	evkq->evkq_nevents += nchanges;

	return (0);
}

static int
event_kq_event_del(struct event_base *evb, struct event *ev)
{
	struct event_kq *evkq = event_base_backend(evb);
	struct kevent *kev, kevs[2];
	int nchanges = 0;

	if (ISSET(ev->ev_event, EV_READ)) {
		kev = &kevs[nchanges++];

		EV_SET(kev, EVENT_FD(ev), EVFILT_READ, EV_DELETE, 0, 0, ev);
	}

	if (ISSET(ev->ev_event, EV_WRITE)) {
		kev = &kevs[nchanges++];

		EV_SET(kev, EVENT_FD(ev), EVFILT_WRITE, EV_DELETE, 0, 0, ev);
	}

	/* commit */
	if (kevent(evkq->evkq_fd, kevs, nchanges, NULL, 0, NULL) == -1)
		return (-1);

	evkq->evkq_nevents -= nchanges;

	return (0);
}

static int
event_kq_signal_add(struct event_base *evb, int s)
{
	struct event_kq *evkq = event_base_backend(evb);
	struct kevent kev[1];
	int rv;

	EV_SET(&kev[0], s, EV_SIGNAL, EV_ADD, 0, 0, NULL);

	rv = kevent(evkq->evkq_fd, kev, 1, NULL, 0, NULL);
	if (rv == -1)
		return (-1);

	evkq->evkq_nevents++;

	return (0);
}

static int
event_kq_signal_del(struct event_base *evb, int s)
{
	struct event_kq *evkq = event_base_backend(evb);
	struct kevent kev[1];
	int rv;

	EV_SET(&kev[0], s, EV_SIGNAL, EV_DELETE, 0, 0, NULL);

	rv = kevent(evkq->evkq_fd, kev, 1, NULL, 0, NULL);
	if (rv == -1)
		return (-1);

	evkq->evkq_nevents--;

	return (0);
}
