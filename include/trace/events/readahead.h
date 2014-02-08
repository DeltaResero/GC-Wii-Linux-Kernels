#undef TRACE_SYSTEM
#define TRACE_SYSTEM readahead

#if !defined(_TRACE_READAHEAD_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_READAHEAD_H

#include <linux/tracepoint.h>

#define show_pattern_name(val)						   \
	__print_symbolic(val,						   \
			{ RA_PATTERN_INITIAL,		"initial"	}, \
			{ RA_PATTERN_SUBSEQUENT,	"subsequent"	}, \
			{ RA_PATTERN_CONTEXT,		"context"	}, \
			{ RA_PATTERN_THRASH,		"thrash"	}, \
			{ RA_PATTERN_MMAP_AROUND,	"around"	}, \
			{ RA_PATTERN_FADVISE,		"fadvise"	}, \
			{ RA_PATTERN_RANDOM,		"random"	}, \
			{ RA_PATTERN_ALL,		"all"		})

/*
 * Tracepoint for guest mode entry.
 */
TRACE_EVENT(readahead,
	TP_PROTO(struct address_space *mapping,
		 pgoff_t offset,
		 unsigned long req_size,
		 unsigned int ra_flags,
		 pgoff_t start,
		 unsigned int size,
		 unsigned int async_size,
		 unsigned int actual),

	TP_ARGS(mapping, offset, req_size,
		ra_flags, start, size, async_size, actual),

	TP_STRUCT__entry(
		__field(	dev_t,		dev		)
		__field(	ino_t,		ino		)
		__field(	pgoff_t,	offset		)
		__field(	unsigned long,	req_size	)
		__field(	unsigned int,	pattern		)
		__field(	pgoff_t,	start		)
		__field(	unsigned int,	size		)
		__field(	unsigned int,	async_size	)
		__field(	unsigned int,	actual		)
	),

	TP_fast_assign(
		__entry->dev		= mapping->host->i_sb->s_dev;
		__entry->ino		= mapping->host->i_ino;
		__entry->pattern	= ra_pattern(ra_flags);
		__entry->offset		= offset;
		__entry->req_size	= req_size;
		__entry->start		= start;
		__entry->size		= size;
		__entry->async_size	= async_size;
		__entry->actual		= actual;
	),

	TP_printk("readahead-%s(dev=%d:%d, ino=%lu, "
		  "req=%lu+%lu, ra=%lu+%d-%d, async=%d) = %d",
			show_pattern_name(__entry->pattern),
			MAJOR(__entry->dev),
			MINOR(__entry->dev),
			__entry->ino,
			__entry->offset,
			__entry->req_size,
			__entry->start,
			__entry->size,
			__entry->async_size,
			__entry->start > __entry->offset,
			__entry->actual)
);

#endif /* _TRACE_READAHEAD_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
