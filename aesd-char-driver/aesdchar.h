/* aesdchar.h */
#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#define AESD_DEBUG 1  //Remove comment on this line to enable debug

#undef PDEBUG             /* undef it, just in case */
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

struct aesd_dev
{
    struct aesd_circular_buffer buffer; /* The circular buffer for history */
    struct aesd_buffer_entry working_entry; /* Buffer for the current incomplete write */
    struct mutex lock; /* Mutex for locking */
    struct cdev cdev; /* Char device structure */
};

#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
