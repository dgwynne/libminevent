
/*	$OpenBSD$ */

/*
 * Copyright (c) 2016, 2017 David Gwynne <dlg@openbsd.org>
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

#ifndef _LIB_EVENT_H_
#define _LIB_EVENT_H_

#include <sys/queue.h>
#include <sys/time.h>
#include <time.h>

struct event_base;

struct _heap_entry {
	struct _heap_entry	*he_left;
	struct _heap_entry	*he_child;
	struct _heap_entry	*he_nextsibling;
};
#define HEAP_ENTRY()		struct _heap_entry

struct _heap {
	struct _heap_entry	*h_root;
};

#define HEAP_HEAD(_name)						\
struct _name {								\
	struct _heap		heap;					\
}

struct event {
	void			 *ev_cookie;
	struct event_base	 *ev_base;
	TAILQ_ENTRY(event)	  ev_list;
	TAILQ_ENTRY(event)	  ev_fire;
	HEAP_ENTRY()		  ev_heap;
	struct timespec		  ev_deadline;

	void			(*ev_fn)(int, short, void *);
	void			 *ev_arg;
	int			  ev_ident; /* fd/signal */
	short			  ev_event;
	short			  ev_fires;
};

#define EV_TIMEOUT		(1 << 4)
#define EV_SIGNAL		(2 << 4)

#define EV_READ			(1 << 8)
#define EV_WRITE		(1 << 9)
#define EV_PERSIST		(1 << 10)

#define EVENT_FD(_ev)		((_ev)->ev_ident)

struct event_base	*event_init(void);
int			 event_dispatch(void);

void			 event_set(struct event *, int, short,
			     void (*)(int, short, void *), void *);
int			 event_add(struct event *, const struct timeval *);
int			 event_del(struct event *);
int			 event_pending(struct event *, short,
			     struct timeval *);
int			 event_initialized(struct event *);

void			 evtimer_set(struct event *,
			     void (*)(int, short, void *), void *);
int			 evtimer_add(struct event *, const struct timeval *);
int			 evtimer_del(struct event *);
int			 evtimer_pending(struct event *, struct timeval *);
int			 evtimer_initialized(struct event *);

void			 signal_set(struct event *, int,
			     void (*)(int, short, void *), void *);
int			 signal_add(struct event *, const struct timeval *);
int			 signal_del(struct event *);
int			 signal_pending(struct event *, struct timeval *);
int			 signal_initialized(struct event *);

#endif /* _LIB_EVENT_H_ */
