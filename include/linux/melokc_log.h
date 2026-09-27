#ifndef _MELOKC_LOG_H_
#define _MELOKC_LOG_H_

#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>

#define melokc_debug(fmt, ...) \
	if (strstr(current->comm, "TEST")) \
		pr_err("Melokc:[%s][%s]: " fmt, current->comm, __func__, ##__VA_ARGS__)

#define melokc_pr(fmt, ...) \
		pr_err("Melokc:[%s][%s]: " fmt, current->comm, __func__, ##__VA_ARGS__)

#endif
