#ifndef B43_SDIO_H_
#define B43_SDIO_H_

#ifdef CONFIG_B43_SDIO

#include <linux/ssb/ssb.h> /* for struct ssb_bus */
#include "b43.h" /* for struct b43_wldev */

int b43_sdio_init(void);
void b43_sdio_exit(void);

/*
 * We use this workaround to get access to b43_wldev from the SDIO
 * interrupt handler, and to get access to ssb_bus from the SDIO
 * driver. (b43_wldev is available only at interrupt registration time).
 */
struct b43_sdio_dev_wrapper {
	struct ssb_bus *ssb;
	struct b43_wldev *wldev;
};

#else /* CONFIG_B43_SDIO */

static inline int b43_sdio_init(void)
{
	return 0;
}
static inline void b43_sdio_exit(void)
{
}

#endif /* CONFIG_B43_SDIO */
#endif /* B43_SDIO_H_ */
