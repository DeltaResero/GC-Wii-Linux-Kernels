#ifndef _SPARC64_PIL_H
#define _SPARC64_PIL_H

/* To avoid some locking problems, we hard allocate certain PILs
 * for SMP cross call messages that must do a etrap/rtrap.
 *
 * A local_irq_disable() does not block the cross call delivery, so
 * when SMP locking is an issue we reschedule the event into a PIL
 * interrupt which is blocked by local_irq_disable().
 *
 * In fact any XCALL which has to etrap/rtrap has a problem because
 * it is difficult to prevent rtrap from running BH's, and that would
 * need to be done if the XCALL arrived while %pil==15.
 */
#define PIL_SMP_CALL_FUNC	1
#define PIL_SMP_RECEIVE_SIGNAL	2
#define PIL_SMP_CAPTURE		3
#define PIL_SMP_CTX_NEW_VERSION	4
#define PIL_DEVICE_IRQ		5
#define PIL_SMP_CALL_FUNC_SNGL	6
#define PIL_KGDB_CAPTURE	8

#endif /* !(_SPARC64_PIL_H) */
