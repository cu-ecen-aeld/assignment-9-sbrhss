/*
 * aesd_ioctl.h
 *
 *  Created on: Assignment 9
 *      Author: Assignment
 */

#ifndef AESD_IOCTL_H
#define AESD_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define AESDCHAR_IOC_MAGIC 0x16

#define AESDCHAR_IOCSEEKTO _IOWR(AESDCHAR_IOC_MAGIC, 0, struct aesd_seekto)

struct aesd_seekto {
    uint32_t write_cmd;  /* zero-referenced command number */
    uint32_t write_cmd_offset;  /* zero-referenced offset within command */
};

#endif /* AESD_IOCTL_H */
