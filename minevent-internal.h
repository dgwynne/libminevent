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

#ifndef _LIB_EVENT_INTERNAL_H_
#define _LIB_EVENT_INTERNAL_H_

#include "minevent.h"

#define EV_INITIALIZED	(1 << 0)
#define EV_ON_LIST	(1 << 1)
#define EV_ON_HEAP	(1 << 3)
#define EV_ON_FIRE	(1 << 2)

/*
 * internally we use the type as a field, but it is used by the API as flags.
 */
#define EV_TYPE_MASK	(0xf << 4)
#define EV_IO		(0 << 4)
/*
#define EV_TIMEOUT	(1 << 4)
#define EV_SIGNAL	(2 << 4)

#define EV_READ		(1 << 8)
#define EV_WRITE 	(1 << 9)
#define EV_PERSIST	(1 << 10)
*/

/* EV_TIMEOUT is handled separately */
#define EV_PENDING_MASK	(EV_SIGNAL | EV_READ | EV_WRITE | EV_PERSIST)

#define SET(_v, _m)	((_v) |= (_m))
#define CLR(_v, _m)	((_v) &= ~(_m))
#define ISSET(_v, _m)	((_v) & (_m))

struct event_ops {
	void		*(*evo_init)(void);
	void		 (*evo_destroy)(void *);
	int		 (*evo_dispatch)(struct event_base *,
			       const struct timespec *);

	int		 (*evo_event_add)(struct event_base *, struct event *);
	int		 (*evo_event_del)(struct event_base *, struct event *);
	int		 (*evo_signal_add)(struct event_base *, int);
	int		 (*evo_signal_del)(struct event_base *, int);
};

void	*event_base_backend(struct event_base *);

void	 event_fire_event(struct event_base *, struct event *, short);
void	 event_fire_signal(struct event_base *, int);

#if 1 && defined(EVENT_HAS_KQUEUE)
extern const struct event_ops event_kqueue_ops;
#ifndef EVENT_OPS_DEFAULT
#define EVENT_OPS_DEFAULT (&event_kqueue_ops)
#endif
#endif

extern const struct event_ops event_poll_ops;
#ifndef EVENT_OPS_DEFAULT
#define EVENT_OPS_DEFAULT (&event_poll_ops)
#endif

#define	event_walltime(_ts)	clock_gettime(CLOCK_REALTIME, (_ts))
#define	event_monotime(_ts)	clock_gettime(CLOCK_MONOTONIC, (_ts))

void	event_list_init(struct event_base *);
void	event_list_insert(struct event_base *, struct event *);
void	event_list_remove(struct event_base *, struct event *);
struct event *
	event_list_first(struct event_base *);
struct event *
	event_list_next(struct event *);
unsigned int
	event_list_len(struct event_base *);

#define EVENT_LIST_FOREACH(_ev, _evb)					\
	for ((_ev) = event_list_first((_evb);				\
	    (_ev) != NULL;						\
	    (_ev) = event_list_next((_ev))

#endif /* _LIB_EVENT_INTERNAL_H_ */
